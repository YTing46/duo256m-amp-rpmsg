// /*
//  * rpmsg_rtt_bench.c
//  * --------------------------------------------------------------------------
//  * 在大核 (C906B / Linux) 端量測與小核 (C906L / ThreadX) 之間的 RPMsg RTT。
//  *
//  * 一次 write(ping) + read(pong) = 一個 round trip。
//  * 兩個時間戳都取在「同一顆核」上 (CLOCK_MONOTONIC),不需要跨核時鐘同步。
//  *
//  * 編譯:把這支加進 rpmsg_userspace 的 Makefile,跟 mailbox_pingpong
//  *       用同一條 cross-compile 規則即可,例如:
//  *         $(CROSS)gcc -O2 -o rpmsg_rtt_bench rpmsg_rtt_bench.c
//  * 執行:在板子上以 root 執行 (SCHED_FIFO 需要 root):
//  *         ./rpmsg_rtt_bench
//  *       完成後會產生 rtt_samples.csv,再用 rtt_analyze.py 後處理。
//  * --------------------------------------------------------------------------
//  */
// #define _GNU_SOURCE
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <stdint.h>
// #include <errno.h>
// #include <time.h>
// #include <sched.h>
// #include <sys/types.h>
// #include <sys/stat.h>
// #include <fcntl.h>
// #include <sys/ioctl.h>
// #include <unistd.h>
// #include "rpmsg.h"

// /* ---------------- 可調參數 ---------------- */
// #define WARMUP      1000      /* 暖機次數,不計入統計 (吸收冷快取/缺頁/通道建立的離群值) */
// #define ITERS       100000    /* 每個 payload 大小的量測次數;板子慢的話可調小 */
// #define MAX_PAYLOAD 480       /* RPMsg 單一 buffer 上限約 496 (512 - 16 byte header),保守取 480 */

// /* 要掃的 payload 大小 (bytes)。
//  * 若你的 ThreadX 韌體只會 echo 固定 4 bytes,改成:  static const int sizes[] = {4};  */
// static const int sizes[] = {4, 8, 16, 32, 64, 128, 256, 480};
// #define NSIZES ((int)(sizeof(sizes) / sizeof(sizes[0])))

// static inline uint64_t now_ns(void)
// {
//     struct timespec ts;
//     clock_gettime(CLOCK_MONOTONIC, &ts);   /* 不要用 REALTIME,會被 NTP 校正而跳動 */
//     return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
// }

// static int cmp_u64(const void *a, const void *b)
// {
//     uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
//     return (x > y) - (x < y);
// }

// int main(void)
// {
//     /* 降低排程抖動。MilkV Duo 的 Linux 端基本上是單核,affinity 影響不大,
//      * 關鍵是 SCHED_FIFO + 跑的時候系統乾淨。 */
//     struct sched_param sp = { .sched_priority = 80 };
//     if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
//         fprintf(stderr, "warn: SCHED_FIFO 設定失敗 (需 root),改用一般優先權繼續\n");

//     /* 與 mailbox_pingpong 相同的 channel / endpoint 設定 */
//     struct rpmsg_endpoint_info ept_info = {"rpmsg-openamp-demo-channel", 0x1e, 0x1e};

//     int fd = open("/dev/rpmsg_ctrl0", O_RDWR);
//     if (fd < 0) { perror("open /dev/rpmsg_ctrl0"); return 1; }
//     if (ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept_info) < 0) { perror("RPMSG_CREATE_EPT_IOCTL"); return 1; }

//     int fd_ept = open("/dev/rpmsg0", O_RDWR);
//     if (fd_ept < 0) fd_ept = open("/dev/rpmsg1", O_RDWR);
//     if (fd_ept < 0) { perror("open /dev/rpmsgN"); return 1; }

//     uint64_t *samples = malloc(sizeof(uint64_t) * ITERS);
//     uint64_t *sorted  = malloc(sizeof(uint64_t) * ITERS);
//     uint8_t  *txbuf   = calloc(1, MAX_PAYLOAD);
//     uint8_t  *rxbuf   = calloc(1, MAX_PAYLOAD);
//     if (!samples || !sorted || !txbuf || !rxbuf) { perror("malloc"); return 1; }

//     FILE *csv = fopen("rtt_samples.csv", "w");
//     if (!csv) { perror("fopen csv"); return 1; }
//     fprintf(csv, "payload_bytes,iter,rtt_ns\n");

//     printf("ITERS=%d  WARMUP=%d\n\n", ITERS, WARMUP);
//     printf("%-7s %9s %9s %9s %9s %10s %11s\n",
//            "bytes", "min", "p50", "p90", "p99", "max", "mean(ns)");

//     for (int s = 0; s < NSIZES; s++) {
//         int len = sizes[s] > MAX_PAYLOAD ? MAX_PAYLOAD : sizes[s];

//         /* 暖機 */
//         for (int i = 0; i < WARMUP; i++) {
//             if (write(fd_ept, txbuf, len) < 0) { perror("write warmup"); goto done; }
//             if (read (fd_ept, rxbuf, len) < 0) { perror("read warmup");  goto done; }
//         }

//         /* 量測:一次 write+read = 一個 round trip */
//         for (int i = 0; i < ITERS; i++) {
//             uint64_t t0 = now_ns();
//             if (write(fd_ept, txbuf, len) < 0) { perror("write"); goto done; }
//             if (read (fd_ept, rxbuf, len) < 0) { perror("read");  goto done; }
//             samples[i] = now_ns() - t0;
//         }

//         /* 原始資料落地,統計留給 Python 做,這裡只印即時摘要 */
//         for (int i = 0; i < ITERS; i++)
//             fprintf(csv, "%d,%d,%llu\n", len, i, (unsigned long long)samples[i]);

//         memcpy(sorted, samples, sizeof(uint64_t) * ITERS);
//         qsort(sorted, ITERS, sizeof(uint64_t), cmp_u64);
//         double sum = 0.0;
//         for (int i = 0; i < ITERS; i++) sum += (double)samples[i];

//         printf("%-7d %9llu %9llu %9llu %9llu %10llu %11.1f\n",
//                len,
//                (unsigned long long)sorted[0],
//                (unsigned long long)sorted[(size_t)ITERS * 50 / 100],
//                (unsigned long long)sorted[(size_t)ITERS * 90 / 100],
//                (unsigned long long)sorted[(size_t)ITERS * 99 / 100],
//                (unsigned long long)sorted[ITERS - 1],
//                sum / ITERS);
//     }

// done:
//     fclose(csv);
//     free(samples); free(sorted); free(txbuf); free(rxbuf);
//     ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL);
//     close(fd_ept);
//     close(fd);
//     printf("\n原始資料已寫入 rtt_samples.csv\n");
//     return 0;
// }

/*
 * rpmsg_rtt_bench.c  (v2: 進度輸出 + read 逾時 + 較合理的預設次數)
 * 一次 write(ping)+read(pong) = 一個 round trip,兩個時間戳都在大核取。
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "rpmsg.h"

/* ---------------- 可調參數 ---------------- */
#define WARMUP      200       /* 暖機次數,不計入統計 */
#define ITERS       10000     /* 每個 payload 大小的量測次數 */
#define MAX_PAYLOAD 480       /* RPMsg 單一 buffer 上限約 496,保守取 480 */
#define REPLY_TIMEOUT_MS 2000 /* 等小核回覆的逾時(ms),逾時即視為小核沒回 */

/* 若你的 ThreadX 韌體只 echo 固定 4 bytes,把下行改成: static const int sizes[] = {4}; */
static const int sizes[] = {4};
#define NSIZES ((int)(sizeof(sizes) / sizeof(sizes[0])))

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* 一次 round trip。回傳 0 成功,-1 I/O 錯誤,-2 小核逾時沒回 */
static int rtt_once(int fd, void *tx, void *rx, int len, uint64_t *out_ns)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint64_t t0 = now_ns();
    if (write(fd, tx, len) < 0) return -1;
    int pr = poll(&pfd, 1, REPLY_TIMEOUT_MS);
    if (pr == 0)  return -2;            /* 逾時:小核沒回 */
    if (pr < 0)   return -1;
    if (read(fd, rx, len) < 0) return -1;
    *out_ns = now_ns() - t0;
    return 0;
}

int main(void)
{
    struct sched_param sp = { .sched_priority = 80 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "warn: SCHED_FIFO 失敗 (%s),改用一般優先權繼續\n", strerror(errno));

    struct rpmsg_endpoint_info ept_info = {"rpmsg-openamp-demo-channel", 0x1e, 0x1e};

    int fd = open("/dev/rpmsg_ctrl0", O_RDWR);
    if (fd < 0) { perror("open /dev/rpmsg_ctrl0"); return 1; }
    if (ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &ept_info) < 0) { perror("RPMSG_CREATE_EPT_IOCTL"); return 1; }

    int fd_ept = open("/dev/rpmsg0", O_RDWR);
    if (fd_ept < 0) fd_ept = open("/dev/rpmsg1", O_RDWR);
    if (fd_ept < 0) { perror("open /dev/rpmsgN"); return 1; }

    uint64_t *samples = malloc(sizeof(uint64_t) * ITERS);
    uint64_t *sorted  = malloc(sizeof(uint64_t) * ITERS);
    uint8_t  *txbuf   = calloc(1, MAX_PAYLOAD);
    uint8_t  *rxbuf   = calloc(1, MAX_PAYLOAD);
    if (!samples || !sorted || !txbuf || !rxbuf) { perror("malloc"); return 1; }

    FILE *csv = fopen("rtt_samples.csv", "w");
    if (!csv) { perror("fopen csv"); return 1; }
    fprintf(csv, "payload_bytes,iter,rtt_ns\n");

    printf("ITERS=%d  WARMUP=%d\n\n", ITERS, WARMUP);
    printf("%-7s %9s %9s %9s %9s %10s %11s\n",
           "bytes", "min", "p50", "p90", "p99", "max", "mean(ns)");
    fflush(stdout);

    for (int s = 0; s < NSIZES; s++) {
        int len = sizes[s] > MAX_PAYLOAD ? MAX_PAYLOAD : sizes[s];
        int rc;
        uint64_t r;

        fprintf(stderr, "[size %d B] 暖機中...\n", len); fflush(stderr);
        for (int i = 0; i < WARMUP; i++) {
            rc = rtt_once(fd_ept, txbuf, rxbuf, len, &r);
            if (rc == -2) {
                fprintf(stderr,
                  "\n小核在 %d ms 內沒有回覆。請確認:\n"
                  "  1) 有跑 remoteproc.sh,且 cat /sys/class/remoteproc/remoteproc0/state 顯示 running\n"
                  "  2) 小核韌體就是 echo-on-receive 的 ping-pong\n"
                  "  3) 先用原本的 mailbox_pingpong 確認通道本身會通\n",
                  REPLY_TIMEOUT_MS);
                goto done;
            }
            if (rc == -1) { perror("rtt_once warmup"); goto done; }
        }

        fprintf(stderr, "[size %d B] 量測 %d 次...\n", len, ITERS); fflush(stderr);
        for (int i = 0; i < ITERS; i++) {
            rc = rtt_once(fd_ept, txbuf, rxbuf, len, &samples[i]);
            if (rc != 0) { fprintf(stderr, "量測中斷 rc=%d\n", rc); goto done; }
        }

        for (int i = 0; i < ITERS; i++)
            fprintf(csv, "%d,%d,%llu\n", len, i, (unsigned long long)samples[i]);

        memcpy(sorted, samples, sizeof(uint64_t) * ITERS);
        qsort(sorted, ITERS, sizeof(uint64_t), cmp_u64);
        double sum = 0.0;
        for (int i = 0; i < ITERS; i++) sum += (double)samples[i];

        printf("%-7d %9llu %9llu %9llu %9llu %10llu %11.1f\n",
               len,
               (unsigned long long)sorted[0],
               (unsigned long long)sorted[(size_t)ITERS * 50 / 100],
               (unsigned long long)sorted[(size_t)ITERS * 90 / 100],
               (unsigned long long)sorted[(size_t)ITERS * 99 / 100],
               (unsigned long long)sorted[ITERS - 1],
               sum / ITERS);
        fflush(stdout);
    }

done:
    fclose(csv);
    free(samples); free(sorted); free(txbuf); free(rxbuf);
    ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL);
    close(fd_ept);
    close(fd);
    printf("\n原始資料已寫入 rtt_samples.csv\n");
    return 0;
}