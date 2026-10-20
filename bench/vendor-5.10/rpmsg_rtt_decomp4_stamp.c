/*
 * rpmsg_rtt_decomp4_stamp.c — 四段拆解,小核 turnaround 改用 s1/s3 時間戳
 *
 * 以 rpmsg_rtt_decomp4.c (Jeremy Lu) 為底,分段定義不變:
 *   A 大核送出      = b1 - t0
 *   workqueue 延遲   = b2b - b2
 *   callback+喚醒    = t1 - b2b
 *   小核 turnaround  = (s3 - s1) ticks 換算
 *   transport (推估) = RTT - A - (t1-b2) - turnaround
 *
 * 差別只在小核那一段:韌體不是回 8B {data, turnaround_ticks},而是
 * patches/threadx/0002 的 24B 帶戳回覆
 *   {uint32 DATA, uint32 magic=0xB47C0DE5, uint64 s1_ticks, uint64 s3_ticks}
 *   s1 = mailbox ISR 進入 (prvQueueISR), s3 = rpmsg_lite_send 之前
 * 小於 24B 的請求一律回 24B (patches/threadx/0003),所以這裡送 8B、收 24B。
 *
 * 需求:
 *   - 小核韌體 = ThreadX-to-RISC-V64 amp (49ad38f) + patches/threadx/0001-0003
 *   - 大核 cvi_rpmsg_shmem.c 已插 b1/b2/b2b 並 proc_create("cvi_rpmsg_probe")
 *     (patches/vendor-sdk/0008 起;0011 的 9 欄格式也相容,只讀前 3 欄)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rpmsg.h"

#define TIMEBASE_HZ   25000000u      /* 已確認 = 25 MHz */
#define NS_PER_TICK   (1000000000ull / TIMEBASE_HZ)
#define STAMP_MAGIC   0xB47C0DE5u
#define WARMUP        200
#define ITERS         10000
#define REPLY_TIMEOUT_MS 2000
#define PROC_PATH     "/proc/cvi_rpmsg_probe"
#define CSV_PATH      "rtt_decomp4_stamp.csv"

_Static_assert(1000000000u % TIMEBASE_HZ == 0, "tick period must be a whole number of ns");

typedef struct { uint32_t data; uint32_t pad; } tx_msg_t;    /* 8 bytes,同 decomp4 的請求大小 */

typedef struct {                                            /* 韌體 THE_MESSAGE (threadx 0002) */
    uint32_t data;
    uint32_t magic;
    uint64_t s1_ticks;
    uint64_t s3_ticks;
} stamp_t;

_Static_assert(sizeof(stamp_t) == 24, "stamp_t must match the 24B firmware echo");
_Static_assert(offsetof(stamp_t, magic) == 4, "magic offset");
_Static_assert(offsetof(stamp_t, s1_ticks) == 8, "s1 offset");
_Static_assert(offsetof(stamp_t, s3_ticks) == 16, "s3 offset");

/* decode_stamp() 回傳值 */
#define STAMP_OK         0
#define STAMP_SHORT     -1   /* read 長度不是 24B */
#define STAMP_BAD_MAGIC -2   /* 韌體沒帶戳 (不是 threadx 0002 以後的版本) */
#define STAMP_BACKWARD  -3   /* s3 < s1 */
#define STAMP_OVERFLOW  -4   /* (s3 - s1) 換成 ns 會溢位 */

static inline uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

/* 檢查一筆回覆並算出小核 turnaround (ns) */
static int decode_stamp(const stamp_t *rx, ssize_t n, uint64_t *turn_ns)
{
    if (n != (ssize_t)sizeof(*rx)) return STAMP_SHORT;
    if (rx->magic != STAMP_MAGIC)  return STAMP_BAD_MAGIC;
    if (rx->s3_ticks < rx->s1_ticks) return STAMP_BACKWARD;
    uint64_t ticks = rx->s3_ticks - rx->s1_ticks;
    if (ticks > UINT64_MAX / NS_PER_TICK) return STAMP_OVERFLOW;
    *turn_ns = ticks * NS_PER_TICK;
    return STAMP_OK;
}

/* 讀 /proc/cvi_rpmsg_probe 拿 b1 b2 b2b (0011 之後一行 9 欄,只取前 3 欄) */
static int read_probe(int pfd, uint64_t *b1, uint64_t *b2, uint64_t *b2b)
{
    char buf[256];
    unsigned long long v1, v2, v3;
    if (lseek(pfd, 0, SEEK_SET) < 0) return -1;
    ssize_t n = read(pfd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    if (sscanf(buf, "%llu %llu %llu", &v1, &v2, &v3) != 3) return -1;
    *b1 = v1; *b2 = v2; *b2b = v3;
    return 0;
}

static void report(const char *name, uint64_t *a, int n)
{
    uint64_t *s = malloc(sizeof(uint64_t) * n);
    if (!s) { perror("malloc"); return; }
    memcpy(s, a, sizeof(uint64_t) * n);
    qsort(s, n, sizeof(uint64_t), cmp_u64);
    double sum = 0; for (int i = 0; i < n; i++) sum += (double)a[i];
    printf("%-20s min=%-7llu p50=%-7llu p90=%-7llu p99=%-8llu max=%-9llu mean=%.0f\n",
           name,
           (unsigned long long)s[0],
           (unsigned long long)s[(size_t)n*50/100],
           (unsigned long long)s[(size_t)n*90/100],
           (unsigned long long)s[(size_t)n*99/100],
           (unsigned long long)s[n-1],
           sum / n);
    free(s);
}

/* 送一筆、等回覆、讀回 24B。回 0 成功,-1 I/O 錯,-2 逾時;*n 為 read 長度 */
static int ping(int fd, const tx_msg_t *tx, stamp_t *rx, ssize_t *n)
{
    struct pollfd pl = { .fd = fd, .events = POLLIN };
    if (write(fd, tx, sizeof(*tx)) < 0) return -1;
    int pr = poll(&pl, 1, REPLY_TIMEOUT_MS);
    if (pr == 0) return -2;
    if (pr < 0)  return -1;
    *n = read(fd, rx, sizeof(*rx));
    return (*n < 0) ? -1 : 0;
}

int main(void)
{
    struct sched_param sp = { .sched_priority = 80 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "warn: SCHED_FIFO 失敗 (%s),繼續\n", strerror(errno));

    struct rpmsg_endpoint_info ept_info = {"rpmsg-openamp-demo-channel", 0x1e, 0x1e};
    int fd = open("/dev/rpmsg_ctrl0", O_RDWR);
    if (fd < 0) { perror("open ctrl"); return 1; }
    if (ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept_info) < 0) { perror("CREATE_EPT"); return 1; }
    int fd_ept = open("/dev/rpmsg0", O_RDWR);
    if (fd_ept < 0) fd_ept = open("/dev/rpmsg1", O_RDWR);
    if (fd_ept < 0) { perror("open ept"); return 1; }

    int pfd = open(PROC_PATH, O_RDONLY);
    if (pfd < 0) { perror("open " PROC_PATH " (kernel 樁裝了嗎?)"); return 1; }

    uint64_t *A    = malloc(sizeof(uint64_t)*ITERS);  /* b1 - t0 */
    uint64_t *WQ   = malloc(sizeof(uint64_t)*ITERS);  /* b2b - b2 */
    uint64_t *CB   = malloc(sizeof(uint64_t)*ITERS);  /* t1 - b2b */
    uint64_t *RXC  = malloc(sizeof(uint64_t)*ITERS);  /* t1 - b2 (接收總和) */
    uint64_t *TURN = malloc(sizeof(uint64_t)*ITERS);  /* 小核 turnaround = s3 - s1 */
    uint64_t *TRAN = malloc(sizeof(uint64_t)*ITERS);  /* transport 推估 */
    uint64_t *RTT  = malloc(sizeof(uint64_t)*ITERS);
    tx_msg_t tx = {0, 0};
    stamp_t rx = {0, 0, 0, 0};
    ssize_t n;
    uint64_t turn;
    int rc, got = 0, bad_order = 0, bad_stamp = 0;
    int ret = 1;                                      /* 只有 CSV 完整寫出才算成功 */

    if (!A || !WQ || !CB || !RXC || !TURN || !TRAN || !RTT) { perror("malloc"); goto done; }

    fprintf(stderr, "暖機 %d 次...\n", WARMUP);
    for (int i = 0; i < WARMUP; i++) {
        rc = ping(fd_ept, &tx, &rx, &n);
        if (rc == -2) { fprintf(stderr, "小核沒回\n"); goto done; }
        if (rc == -1) { perror("warmup"); goto done; }
        rc = (i == 0) ? decode_stamp(&rx, n, &turn) : STAMP_OK;
        if (rc == STAMP_SHORT || rc == STAMP_BAD_MAGIC) {
            fprintf(stderr, "回覆沒有 s1/s3 時間戳 (%zd bytes, magic=0x%08x):"
                    "小核韌體要套 patches/threadx/0001-0003\n", n, rx.magic);
            goto done;
        }
    }

    fprintf(stderr, "量測 %d 次...\n", ITERS);
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        rc = ping(fd_ept, &tx, &rx, &n);
        uint64_t t1 = now_ns();
        if (rc == -2) { fprintf(stderr, "逾時 @%d\n", i); goto done; }
        if (rc == -1) { perror("ping"); goto done; }

        uint64_t b1,b2,b2b;
        if (read_probe(pfd,&b1,&b2,&b2b) != 0) { fprintf(stderr,"probe 讀取失敗\n"); goto done; }

        /* 基本健全性檢查:t0 <= b1 <= b2 <= b2b <= t1 */
        if (!(t0<=b1 && b1<=b2 && b2<=b2b && b2b<=t1)) { bad_order++; continue; }

        /* 時間戳壞掉,或 turnaround 超出 b1..b2 (s1/s3 必定落在 kick 送出與回覆中斷之間;
         * 超出代表 s1 是更早的 mailbox ISR 留下的) */
        if (decode_stamp(&rx, n, &turn) != STAMP_OK || turn > b2 - b1) { bad_stamp++; continue; }

        RTT[got]  = t1 - t0;
        A[got]    = b1 - t0;
        WQ[got]   = b2b - b2;
        CB[got]   = t1 - b2b;
        RXC[got]  = t1 - b2;
        TURN[got] = turn;
        TRAN[got] = (b2 - b1) - turn;                 /* = RTT - A - rxC - turnaround */
        got++;
    }

    printf("\n有效樣本 %d / %d (順序錯位丟棄 %d, 時間戳無效丟棄 %d)\n\n",
           got, ITERS, bad_order, bad_stamp);
    if (got == 0) { fprintf(stderr, "沒有有效樣本\n"); goto done; }
    report("RTT 全部",            RTT,  got);
    report("A 大核送出",          A,    got);
    report("workqueue 延遲",      WQ,   got);
    report("callback+喚醒",       CB,   got);
    report("  (接收總和 C)",      RXC,  got);
    report("小核 turnaround",     TURN, got);
    report("transport 推估",      TRAN, got);

    /* 先寫到同目錄下 mkstemp 建立的唯一暫存檔 (不會跟著既有的 symlink 走),
     * 完整寫出後才 rename,失敗時不會留下半截或覆寫舊的 CSV */
    char tmp[] = CSV_PATH ".XXXXXX";
    int tfd = mkstemp(tmp);
    if (tfd < 0) { perror("mkstemp"); goto done; }
    if (fchmod(tfd, 0644) != 0) perror("fchmod");
    FILE *csv = fdopen(tfd, "w");
    if (!csv) { perror("fdopen"); close(tfd); unlink(tmp); goto done; }
    fprintf(csv,"iter,rtt,A,workqueue,callback,rxC,turnaround,transport\n");
    for (int i=0;i<got;i++)
        fprintf(csv,"%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", i,
            (unsigned long long)RTT[i],(unsigned long long)A[i],
            (unsigned long long)WQ[i],(unsigned long long)CB[i],
            (unsigned long long)RXC[i],(unsigned long long)TURN[i],
            (unsigned long long)TRAN[i]);
    int werr = ferror(csv);
    if (fclose(csv) != 0 || werr) {
        fprintf(stderr, "寫入 %s 失敗\n", tmp);
        unlink(tmp);
        goto done;
    }
    if (rename(tmp, CSV_PATH) != 0) {
        perror("rename");
        unlink(tmp);
        goto done;
    }
    printf("\n原始資料已寫入 " CSV_PATH " (可用 rtt.py " CSV_PATH " 畫圖)\n");
    ret = 0;

done:
    free(A);free(WQ);free(CB);free(RXC);free(TURN);free(TRAN);free(RTT);
    close(pfd); ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL); close(fd_ept); close(fd);
    return ret;
}
