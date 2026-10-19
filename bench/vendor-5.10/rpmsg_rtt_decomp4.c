/*
 * rpmsg_rtt_decomp4.c — 四段拆解
 *   A 大核送出      = b1 - t0
 *   workqueue 延遲   = b2b - b2     (IRQ 到 kworker 被排到,最可疑)
 *   callback+喚醒    = t1 - b2b
 *   小核 turnaround  = 回傳的 turnaround_ticks 換算
 *   transport (推估) = RTT - A - (t1-b2) - turnaround
 *
 * 需求:
 *   - 小核 comm_main.c 已插 turnaround 樁 (8-byte 訊息)
 *   - 大核 cvi_rpmsg_shmem.c 已插 b1/b2/b2b 並 proc_create("cvi_rpmsg_probe")
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "rpmsg.h"

#define TIMEBASE_HZ   25000000u      /* 已確認 = 25 MHz */
#define WARMUP        200
#define ITERS         10000
#define REPLY_TIMEOUT_MS 2000
#define PROC_PATH     "/proc/cvi_rpmsg_probe"

typedef struct { uint32_t data; uint32_t turnaround_ticks; } msg_t;

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

/* 讀 /proc/cvi_rpmsg_probe 拿 b1 b2 b2b */
static int read_probe(int pfd, uint64_t *b1, uint64_t *b2, uint64_t *b2b)
{
    char buf[96];
    lseek(pfd, 0, SEEK_SET);
    int n = read(pfd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (sscanf(buf, "%llu %llu %llu",
                   (unsigned long long*)b1,
                   (unsigned long long*)b2,
                   (unsigned long long*)b2b) == 3) ? 0 : -1;
}

static void report(const char *name, uint64_t *a, int n)
{
    uint64_t *s = malloc(sizeof(uint64_t) * n);
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
    uint64_t *TURN = malloc(sizeof(uint64_t)*ITERS);  /* 小核 turnaround */
    uint64_t *TRAN = malloc(sizeof(uint64_t)*ITERS);  /* transport 推估 */
    uint64_t *RTT  = malloc(sizeof(uint64_t)*ITERS);
    msg_t tx = {0,0}, rx = {0,0};
    struct pollfd pl = { .fd = fd_ept, .events = POLLIN };

    fprintf(stderr, "暖機 %d 次...\n", WARMUP);
    for (int i = 0; i < WARMUP; i++) {
        if (write(fd_ept, &tx, sizeof(tx)) < 0) { perror("w"); goto done; }
        if (poll(&pl,1,REPLY_TIMEOUT_MS) <= 0) { fprintf(stderr,"小核沒回\n"); goto done; }
        if (read(fd_ept, &rx, sizeof(rx)) < 0) { perror("r"); goto done; }
    }

    fprintf(stderr, "量測 %d 次...\n", ITERS);
    int got = 0;
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        if (write(fd_ept, &tx, sizeof(tx)) < 0) { perror("w"); goto done; }
        if (poll(&pl,1,REPLY_TIMEOUT_MS) <= 0) { fprintf(stderr,"逾時 @%d\n",i); goto done; }
        if (read(fd_ept, &rx, sizeof(rx)) < 0) { perror("r"); goto done; }
        uint64_t t1 = now_ns();

        uint64_t b1,b2,b2b;
        if (read_probe(pfd,&b1,&b2,&b2b) != 0) { fprintf(stderr,"probe 讀取失敗\n"); goto done; }

        /* 基本健全性檢查:t0 <= b1 <= b2 <= b2b <= t1 */
        if (!(t0<=b1 && b1<=b2 && b2<=b2b && b2b<=t1)) continue; /* 偶發錯位就跳過該筆 */

        RTT[got]  = t1 - t0;
        A[got]    = b1 - t0;
        WQ[got]   = b2b - b2;
        CB[got]   = t1 - b2b;
        RXC[got]  = t1 - b2;
        TURN[got] = (uint64_t)rx.turnaround_ticks * 1000000000ull / TIMEBASE_HZ;
        uint64_t known = A[got] + RXC[got] + TURN[got];
        TRAN[got] = (RTT[got] > known) ? (RTT[got] - known) : 0;
        got++;
    }

    printf("\n有效樣本 %d / %d\n\n", got, ITERS);
    report("RTT 全部",            RTT,  got);
    report("A 大核送出",          A,    got);
    report("workqueue 延遲",      WQ,   got);
    report("callback+喚醒",       CB,   got);
    report("  (接收總和 C)",      RXC,  got);
    report("小核 turnaround",     TURN, got);
    report("transport 推估",      TRAN, got);

    FILE *csv = fopen("rtt_decomp4.csv","w");
    fprintf(csv,"iter,rtt,A,workqueue,callback,rxC,turnaround,transport\n");
    for (int i=0;i<got;i++)
        fprintf(csv,"%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", i,
            (unsigned long long)RTT[i],(unsigned long long)A[i],
            (unsigned long long)WQ[i],(unsigned long long)CB[i],
            (unsigned long long)RXC[i],(unsigned long long)TURN[i],
            (unsigned long long)TRAN[i]);
    fclose(csv);
    printf("\n原始資料已寫入 rtt_decomp4.csv\n");

done:
    free(A);free(WQ);free(CB);free(RXC);free(TURN);free(TRAN);free(RTT);
    close(pfd); ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL); close(fd_ept); close(fd);
    return 0;
}