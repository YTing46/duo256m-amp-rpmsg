/*
 * rpmsg_rtt_decomp.c  — Phase 1 拆解:RTT 與小核 turnaround
 * 訊息 8 bytes: {data, turnaround_ticks}。小核回送時填 turnaround_ticks。
 * 大核這側量 RTT,並用回傳的 turnaround 算出 (RTT - turnaround) = 大核軟體 + transport。
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

/* === 務必填成你 `od .../timebase-frequency` 讀到的實際值 === */
#define TIMEBASE_HZ   25000000u      /* 例: 25 MHz。錯了 turnaround 換算就會錯 */

#define WARMUP      200
#define ITERS       10000
#define REPLY_TIMEOUT_MS 2000

typedef struct { uint32_t data; uint32_t turnaround_ticks; } msg_t;  /* 8 bytes */

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

/* 一次 round trip:回 0 成功, -1 I/O 錯, -2 逾時。回傳 rtt 與小核 turnaround(ticks) */
static int rtt_once(int fd, msg_t *tx, msg_t *rx, uint64_t *rtt_ns, uint32_t *turn_ticks)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint64_t t0 = now_ns();
    if (write(fd, tx, sizeof(*tx)) < 0) return -1;
    int pr = poll(&pfd, 1, REPLY_TIMEOUT_MS);
    if (pr == 0) return -2;
    if (pr < 0)  return -1;
    if (read(fd, rx, sizeof(*rx)) < 0) return -1;
    *rtt_ns = now_ns() - t0;
    *turn_ticks = rx->turnaround_ticks;
    return 0;
}

static void report(const char *name, uint64_t *a, int n)
{
    uint64_t *s = malloc(sizeof(uint64_t) * n);
    memcpy(s, a, sizeof(uint64_t) * n);
    qsort(s, n, sizeof(uint64_t), cmp_u64);
    double sum = 0; for (int i = 0; i < n; i++) sum += (double)a[i];
    printf("%-22s min=%-8llu p50=%-8llu p90=%-8llu p99=%-9llu max=%-9llu mean=%.0f\n",
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

    uint64_t *rtt  = malloc(sizeof(uint64_t) * ITERS);
    uint64_t *turn = malloc(sizeof(uint64_t) * ITERS);
    uint64_t *rest = malloc(sizeof(uint64_t) * ITERS);   /* RTT - turnaround */
    msg_t tx = {0,0}, rx = {0,0};
    uint64_t r; uint32_t tk; int rc;

    fprintf(stderr, "暖機 %d 次...\n", WARMUP);
    for (int i = 0; i < WARMUP; i++) {
        rc = rtt_once(fd_ept, &tx, &rx, &r, &tk);
        if (rc == -2) { fprintf(stderr, "小核沒回 — 確認 remoteproc running、韌體已更新成 8-byte 版本\n"); goto done; }
        if (rc == -1) { perror("warmup"); goto done; }
    }

    fprintf(stderr, "量測 %d 次...\n", ITERS);
    for (int i = 0; i < ITERS; i++) {
        rc = rtt_once(fd_ept, &tx, &rx, &rtt[i], &tk);
        if (rc != 0) { fprintf(stderr, "中斷 rc=%d\n", rc); goto done; }
        turn[i] = (uint64_t)tk * 1000000000ull / TIMEBASE_HZ;   /* ticks -> ns */
        rest[i] = (rtt[i] > turn[i]) ? (rtt[i] - turn[i]) : 0;
    }

    FILE *csv = fopen("rtt_decomp.csv", "w");
    fprintf(csv, "iter,rtt_ns,turnaround_ns,rest_ns\n");
    for (int i = 0; i < ITERS; i++)
        fprintf(csv, "%d,%llu,%llu,%llu\n", i,
                (unsigned long long)rtt[i], (unsigned long long)turn[i],
                (unsigned long long)rest[i]);
    fclose(csv);

    printf("\nTIMEBASE_HZ=%u (請確認與板子相符)\n\n", TIMEBASE_HZ);
    report("RTT (全部)",            rtt,  ITERS);
    report("小核 turnaround",        turn, ITERS);
    report("大核軟體 + transport",   rest, ITERS);
    printf("\n(註: 晶片內 transport 應為 sub-µs,所以「大核軟體+transport」幾乎就是大核軟體)\n");
    printf("原始資料已寫入 rtt_decomp.csv\n");

done:
    free(rtt); free(turn); free(rest);
    ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL);
    close(fd_ept); close(fd);
    return 0;
}