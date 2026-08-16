/* ds5_clock_probe — is the gap ledger measuring the air, or measuring us?
 *
 * Every NOCP gap this programme has ever reported is the distance between two
 * `now_ms()` calls made by ds5_txd's capture thread when it *processes* a
 * monitor packet. That clock includes the daemon's own scheduling delay. On
 * 2026-08-16 an interleaved A/B showed the >=70 ms tail collapsing to x0.30
 * when the TV was pinned from 3 to 4 online cores, while the >=60 ms band did
 * not move at all — the signature of a measurement artefact, not of the air.
 *
 * This probe reads the same monitor stream, but takes each packet's timestamp
 * TWICE: once from the kernel (SO_TIMESTAMP, stamped when the packet was
 * queued) and once from userspace at the moment we get around to looking at it.
 * Same events, same run, two clocks. Whatever tail exists in the userspace
 * column and not in the kernel column was never on the air.
 *
 * It does not reproduce the daemon's gates (outstanding>0, demand, audio
 * freshness), so its absolute rates are NOT comparable with gapge numbers.
 * The comparison between its two columns is what it is for, and that comparison
 * is internally valid: both columns describe the identical event sequence.
 *
 * Build:
 *   $SDK/bin/arm-webos-linux-gnueabi-gcc -O2 -Wall -Wextra ds5_clock_probe.c \
 *       -o ds5_clock_probe
 * Run (root, alongside a running rig):
 *   ./ds5_clock_probe --seconds 300 [--handle 0x00b]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI         1
#define HCI_CHANNEL_MONITOR 2
#define HCI_DEV_NONE        0xffff
#define MON_EVENT_PKT       3
#define MON_ACL_TX_PKT      4
#define MON_ACL_RX_PKT      5
#define HCI_EV_NUM_COMP_PKTS 0x13

struct sockaddr_hci { uint16_t hci_family; uint16_t hci_dev; uint16_t hci_channel; };
struct hci_mon_hdr  { uint16_t opcode, index, len; } __attribute__((packed));

static const int EDGE[] = { 30, 40, 50, 60, 70, 80, 100, 120, 160, 200 };
#define NEDGE ((int)(sizeof(EDGE)/sizeof(EDGE[0])))

static uint64_t tv_ms(const struct timeval *tv)
{ return (uint64_t)tv->tv_sec * 1000ull + (uint64_t)tv->tv_usec / 1000ull; }

static uint64_t real_ms(void)
{ struct timeval tv; gettimeofday(&tv, NULL); return tv_ms(&tv); }

int main(int argc, char **argv)
{
    int seconds = 300, want_handle = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--handle") && i + 1 < argc) want_handle = (int)strtol(argv[++i], NULL, 0);
    }

    int fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_hci a = { .hci_family = AF_BLUETOOTH,
                              .hci_dev = HCI_DEV_NONE,
                              .hci_channel = HCI_CHANNEL_MONITOR };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind monitor (root?)"); return 1; }

    /* The whole point. If the kernel does not stamp these skbs, sock_recv_timestamp
     * falls back to the time of OUR recvmsg — which is the very delay under test,
     * and the probe would silently agree with itself. The self-check below is what
     * tells the two cases apart. */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof one) < 0) {
        perror("SO_TIMESTAMP"); return 1;
    }

    /* The pad talks back, and on a TDD radio its uplink competes with our
     * downlink for the same slots. During play the DS5 streams stick, trigger
     * and motion state; sitting on a table it does not. That difference is the
     * one load a rig with nobody holding the controller can never reproduce, so
     * count it. */
    uint64_t rx_pkts = 0, tx_pkts = 0;
    uint64_t hist_k[NEDGE], hist_u[NEDGE];
    memset(hist_k, 0, sizeof hist_k); memset(hist_u, 0, sizeof hist_u);
    uint64_t last_k = 0, last_u = 0, n = 0, lag_sum = 0, lag_max = 0, lag_zero = 0;
    uint64_t gmax_k = 0, gmax_u = 0;
    uint64_t t_end = real_ms() + (uint64_t)seconds * 1000ull;

    uint8_t buf[2048];
    char cbuf[CMSG_SPACE(sizeof(struct timeval)) + 64];
    for (;;) {
        if (real_ms() >= t_end) break;
        struct iovec iov = { .iov_base = buf, .iov_len = sizeof buf };
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = cbuf; msg.msg_controllen = sizeof cbuf;
        ssize_t r = recvmsg(fd, &msg, 0);
        if (r < 0) { if (errno == EINTR) continue; perror("recvmsg"); break; }
        uint64_t u = real_ms();          /* userspace clock: when WE looked */
        if (r < (ssize_t)sizeof(struct hci_mon_hdr)) continue;

        uint64_t k = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMP) {
                struct timeval tv; memcpy(&tv, CMSG_DATA(c), sizeof tv); k = tv_ms(&tv);
            }
        if (!k) { lag_zero++; continue; }

        struct hci_mon_hdr *h = (struct hci_mon_hdr *)buf;
        if (h->opcode == MON_ACL_RX_PKT || h->opcode == MON_ACL_TX_PKT) {
            /* ACL header: handle in the low 12 bits of the first two bytes. */
            if (h->len >= 2) {
                const uint8_t *a = buf + sizeof *h;
                uint16_t hh = (uint16_t)((a[0] | (a[1] << 8)) & 0x0fff);
                if (want_handle < 0 || hh == (uint16_t) want_handle) {
                    if (h->opcode == MON_ACL_RX_PKT) rx_pkts++; else tx_pkts++;
                }
            }
            continue;
        }
        if (h->opcode != MON_EVENT_PKT) continue;
        const uint8_t *e = buf + sizeof *h;
        int el = h->len;
        if (el < 2 || e[0] != HCI_EV_NUM_COMP_PKTS) continue;
        const uint8_t *p = e + 2; int pl = el - 2;
        if (pl < 1) continue;
        int nh = p[0];
        if (pl < 1 + nh * 4) continue;
        int mine = (want_handle < 0);
        for (int i = 0; i < nh && !mine; i++) {
            uint16_t hh = (uint16_t)((p[1 + i * 4] | (p[2 + i * 4] << 8)) & 0x0fff);
            if (hh == (uint16_t)want_handle) mine = 1;
        }
        if (!mine) continue;

        uint64_t lag = u > k ? u - k : 0;
        lag_sum += lag; if (lag > lag_max) lag_max = lag;
        if (last_k) {
            uint64_t gk = k - last_k, gu = u - last_u;
            if (gk > gmax_k) gmax_k = gk;
            if (gu > gmax_u) gmax_u = gu;
            for (int b = 0; b < NEDGE; b++) {
                if (gk >= (uint64_t)EDGE[b]) hist_k[b]++;
                if (gu >= (uint64_t)EDGE[b]) hist_u[b]++;
            }
            n++;
        }
        last_k = k; last_u = u;
    }

    double mins = (double)seconds / 60.0;
    printf("[clock] %d s, %llu inter-NOCP intervals, lag(user-kernel) avg %.2f ms max %llu ms"
           ", packets without a kernel stamp: %llu\n",
           seconds, (unsigned long long)n, n ? (double)lag_sum / (double)n : 0.0,
           (unsigned long long)lag_max, (unsigned long long)lag_zero);
    printf("[clock] pad uplink %.0f pkt/s, our downlink %.0f pkt/s over %d s\n",
           (double) rx_pkts / seconds, (double) tx_pkts / seconds, seconds);
    printf("[clock] longest gap: kernel %llu ms   userspace %llu ms\n",
           (unsigned long long)gmax_k, (unsigned long long)gmax_u);
    printf("[clock] %6s %10s %10s %10s\n", "edge", "kernel/min", "user/min", "user-kernel");
    for (int b = 0; b < NEDGE; b++)
        printf("[clock] >=%3d %10.1f %10.1f %+10.1f\n", EDGE[b],
               (double)hist_k[b] / mins, (double)hist_u[b] / mins,
               ((double)hist_u[b] - (double)hist_k[b]) / mins);
    /* If the kernel is not stamping, both columns are the same number by
     * construction and the probe has proved nothing — say so rather than let a
     * tidy table imply a clean instrument. */
    if (lag_max == 0)
        puts("[clock] WARNING: lag was zero throughout — the kernel is NOT stamping these\n"
             "        packets, so both columns are the same clock and this run proves nothing.");
    return 0;
}
