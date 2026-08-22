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
 * SIGTERM/SIGINT end the run with the summary printed — that is how the harness
 * reaps it. Exit 2 means the run had no verdict to give and printed none.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
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

/* The harness ends this probe with SIGTERM on EVERY clean run: ds5_session_ab.sh
 * gives it 120 s more than the blocks can consume and then reaps it from
 * restore_all. Every line this programme prints comes after the loop, so with
 * the default disposition probe.log is empty precisely when the run SUCCEEDED —
 * the two-clock verdict of a good session lost every time. The handler does the
 * only async-signal-safe thing there is to do here, set a flag; the loop, which
 * never blocks longer than the receive timeout, does the printing. Installed
 * without SA_RESTART so the signal lands on the loop head even if that timeout
 * is ever removed again. */
static volatile sig_atomic_t g_stop_sig = 0;
static void on_stop(int s) { g_stop_sig = s; }

int main(int argc, char **argv)
{
    int seconds = 300, want_handle = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--handle") && i + 1 < argc) want_handle = (int)strtol(argv[++i], NULL, 0);
    }

    struct sigaction sa_stop;
    memset(&sa_stop, 0, sizeof sa_stop);
    sa_stop.sa_handler = on_stop;
    sigemptyset(&sa_stop.sa_mask);
    sigaction(SIGINT, &sa_stop, NULL);
    sigaction(SIGTERM, &sa_stop, NULL);
    /* HUP too: the harness is regularly started over ssh, and a session that
     * goes away while the probe is still running would otherwise take the
     * summary with it for the same reason TERM did. */
    sigaction(SIGHUP, &sa_stop, NULL);

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

    /* The deadline below can only be checked BETWEEN packets, so a monitor
     * channel that falls completely silent (session over, pad gone, bluetooth
     * idle) would park us in recvmsg for as long as that lasts — well past
     * --seconds. The summary lines are printed only after the loop, so such a
     * run leaves no two-clock verdict at all, and the probe keeps a monitor
     * socket open on the TV until someone notices. Same 200 ms wakeup as
     * ds5_sniff.c; it costs five idle syscalls a second and nothing else.
     *
     * Fatal like the bind and the stamp option above: unchecked, a rejected
     * timeout would put the probe straight back into that unbounded wait with
     * nothing in probe.log saying which of the two programmes ran. */
    struct timeval rcvto = { .tv_sec = 0, .tv_usec = 200000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof rcvto) < 0) {
        perror("SO_RCVTIMEO"); return 1;
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
    /* When the NOCP events themselves started and stopped, which is not the same
     * question as how long we ran — see the refusal below. */
    uint64_t first_evt = 0, last_evt = 0;
    enum { END_DEADLINE, END_SIGNAL, END_RXERR } why = END_DEADLINE;
    uint64_t t_start = real_ms();
    uint64_t t_end = t_start + (uint64_t)seconds * 1000ull;

    uint8_t buf[2048];
    char cbuf[CMSG_SPACE(sizeof(struct timeval)) + 64];
    for (;;) {
        if (g_stop_sig) { why = END_SIGNAL; break; }
        if (real_ms() >= t_end) break;
        struct iovec iov = { .iov_base = buf, .iov_len = sizeof buf };
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = cbuf; msg.msg_controllen = sizeof cbuf;
        ssize_t r = recvmsg(fd, &msg, 0);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recvmsg"); why = END_RXERR; break;
        }
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
        if (!first_evt) first_evt = u;
        last_evt = u;
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

    /* Rates are per minute of what was OBSERVED, never of what was declared. The
     * harness reaps this probe roughly two minutes before its own deadline, so
     * the declared window is routinely the longer of the two, and dividing by it
     * would understate every column by that ratio. */
    double obs = (double)(real_ms() - t_start) / 1000.0;
    if (obs < 0.001) obs = 0.001;
    double span = (last_evt > first_evt) ? (double)(last_evt - first_evt) / 1000.0 : 0.0;
    const char *why_s = why == END_SIGNAL ? "ended by signal"
                      : why == END_RXERR  ? "ended on a recvmsg error"
                                          : "ran to its declared deadline";

    /* The rig's standing rule is that a run which cannot prove its preconditions
     * refuses rather than reports, and two ways of failing it are only visible
     * here, after the loop.
     *
     * With no intervals at all the two columns describe nothing — yet the table
     * below would print a tidy "+0.0 user-kernel" at every edge and then blame
     * the kernel for not stamping, i.e. a null result dressed as a clean
     * instrument plus a false diagnosis of the instrument. And a session that
     * dies ten seconds into a five-minute probe leaves the monitor channel
     * silent for the rest: those intervals are real, but the window they would
     * be divided by is not, so every per-minute figure comes out diluted by
     * however long nothing happened. The EVENT SPAN is what separates those two
     * from a genuine measurement — not the run length, which is why being ended
     * by a signal is not itself a refusal: that is how the harness ends this
     * probe on every clean run. */
    if (n == 0 || span * 2.0 < obs) {
        printf("[clock] REFUSING a verdict: %llu inter-NOCP intervals, events spanning %.1f s "
               "of the %.1f s observed (%s, %d s declared)\n",
               (unsigned long long)n, span, obs, why_s, seconds);
        printf("[clock]   pad uplink %llu, our downlink %llu packets, %llu without a kernel "
               "stamp\n", (unsigned long long)rx_pkts, (unsigned long long)tx_pkts,
               (unsigned long long)lag_zero);
        printf("[clock]   two clocks can only be compared across a window in which both saw "
               "the same events — this run has no two-clock verdict.\n");
        return 2;
    }

    if (why == END_SIGNAL)
        printf("[clock] CUT SHORT: %s at %.1f s of the %d s declared — the columns below are "
               "per minute of the %.1f s actually observed.\n",
               g_stop_sig == SIGINT ? "SIGINT" : g_stop_sig == SIGHUP ? "SIGHUP" : "SIGTERM",
               obs, seconds, obs);
    /* A read error is not a refusal by itself -- a single transient recvmsg at
     * second 599 of a good 600 s run would otherwise throw the whole table away,
     * which is the same damage the refusal exists to prevent. It does belong on
     * the page, because it means the tail of the run is missing. */
    if (why == END_RXERR)
        printf("[clock] NOTE: the run ended on a recvmsg error at %.1f s of the %d s declared "
               "— everything below is what was collected up to that point.\n", obs, seconds);
    double mins = obs / 60.0;
    /* Event span next to the observed window, on the accepted path too: anything
     * short of full coverage dilutes every per-minute column below by exactly
     * that ratio, and a reader cannot see it from the numbers themselves. */
    printf("[clock] %.1f s observed, events spanning %.1f s of it, %llu inter-NOCP intervals, "
           "lag(user-kernel) avg %.2f ms max %llu ms, packets without a kernel stamp: %llu\n",
           obs, span, (unsigned long long)n, (double)lag_sum / (double)n,
           (unsigned long long)lag_max, (unsigned long long)lag_zero);
    printf("[clock] pad uplink %.0f pkt/s, our downlink %.0f pkt/s over %.1f s\n",
           (double) rx_pkts / obs, (double) tx_pkts / obs, obs);
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
