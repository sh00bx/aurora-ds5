// ds5_txd.c — root DS5 raw-ACL transport forwarder for the jailed Aurora app.
//
// The webOS app jail lets the app BIND an HCI_CHANNEL_RAW socket but DENIES the
// ACL write() (EPERM), so in-process raw-ACL injection is impossible. This root
// daemon performs the privileged work the app cannot:
//   1. Captures the on-air ACL template (conn handle + dest L2CAP CID) via
//      HCI_CHANNEL_MONITOR (works as root) from our own outgoing HID-output.
//   2. Publishes a readiness/template record into the jail-shared tmp file so the
//      app knows when to stop seeding via hidraw and start forwarding.
//   3. Receives each already-built, already-CRC-signed report from the app over a
//      local AF_UNIX SOCK_DGRAM socket and injects it as a raw HCI ACL packet
//      (root write is permitted) — bypassing webOS BT one-outstanding metering.
//
// IPC is one local datagram per report (~tens of microseconds, no buffering, no
// round trip) — far below the ~10ms audio grid. The app falls back to its normal
// hidraw write if the daemon is absent, so this never regresses.
//
// MULTI-CONTROLLER (2026-07-09) — co-op haptics parity for a 2nd DualSense:
//   The single controller the daemon tracked was defence (e) below; a 2nd DS5's
//   output fell back to the flow-controlled hidraw path (jittery). The state is
//   now an ARRAY of independent inject links (struct ds5_link g_links[MAX_LINKS]),
//   each with its OWN captured template, identity binding, credit window, tx-type
//   ring, audio FIFO and gap telemetry — so every pad gets the same raw-ACL
//   jitter-bypass. Routing:
//     * Inbound reports: the app may TAG a datagram with the target BT address
//       ([0xA5][0x5A][addr LSB-first][report]); the daemon injects it onto the
//       link bound to that address. An UNTAGGED datagram (legacy / single-pad /
//       USB) routes to the link that is bound when EXACTLY ONE is — resolved by
//       identity, not by slot index — and is refused otherwise.
//     * Capture: HID-output seen on a NEW handle binds a FREE link slot (was:
//       "ignore any other handle"). Beyond MAX_LINKS slots a further device is
//       ignored (never flip-flops a bound link).
//     * Readiness: the base template file carries that same single bound link and
//       reads INVALID whenever zero or more than one is bound (a 1-pad / untagged
//       app therefore reads it unchanged); each bound link ALSO publishes a
//       per-address file <tmpl>.<aabbccddeeff> that the tagged app polls.
//   Cross-controller resources that are physically shared stay shared: the one
//   radio's scan state (off while ANY link is bound), the HCI command path
//   (rate-limited), and the handle->bdaddr table.
//
// HID-FD BROKER (Option A — app becomes jail-node-independent):
//   The jail's /dev/hidraw* is a STATIC snapshot taken at jail-build (copynodall),
//   so a controller hot-plugged afterwards onto a hidraw minor the snapshot never
//   covered (e.g. /dev/hidraw5) simply does not exist inside the jail — the app's
//   open() returns ENOENT and the bridge dies. We (root, real /dev) open the node
//   the app names and hand the OPEN FD across the jail via SCM_RIGHTS over a second
//   AF_UNIX SOCK_STREAM socket. The app then holds a real kernel fd and every
//   read/write/ioctl works unchanged. Only used as a fallback when the app's own
//   open() fails, so the static-node path is untouched (no regression).
//
//   usage: ds5_txd [sock_path] [tmpl_path] [hidfd_sock_path]
//
// ALT-LATCH HARDENING (2026-06-27, review wf_a75a6ae7) — see ALTLATCH_AUDIT.md:
//   Identity-bind each inject template to the DS5 bdaddr and refuse to inject onto a
//   handle we cannot prove is the DS5, so a flapped+reused 12-bit ACL handle can no
//   longer carry our HID-output onto a foreign device (the Magic-Remote Left-Alt
//   latch). Defences, in layers:
//     (a) Fail CLOSED: a template is published VALID only once the bound handle's
//         bdaddr is known (learned from HCI events or HCIGETCONNLIST). Until then the
//         app keeps seeding via hidraw — safe, never blind-injects.
//     (b) Event-driven invalidation: DISCONN / reassign-to-different-bdaddr of a
//         bound handle drops THAT link's template instantly (covers BR/EDR + legacy
//         & enhanced LE connect events).
//     (c) Idle backstop: evaluated every monitor wakeup (not only on recv-timeout),
//         so it still fires under continuous unrelated BT traffic — a DS5 going
//         silent after a flap invalidates its link within IDLE_INVALIDATE_MS even if
//         the DISCONN event was dropped.
//     (d) Atomic check-and-inject: the bdaddr re-check and the write() happen under
//         one lock, closing the check->inject TOCTOU.
//     (e) Bounded-controller scope: only hci_dev=0 (the inject target) is tracked,
//         so a second ADAPTER's handle reuse can't pollute the table. (This is the
//         controller/adapter index gate — NOT a one-DualSense limit; up to
//         MAX_LINKS DualSenses on hci0 are tracked, each identity-bound.)
//   Plus hardening the two IPC surfaces the audit named: the report datagram socket
//   now requires SO_PEERCRED == jail uid, and the hid-fd broker only ever hands out
//   an allowlisted game pad's hidraw (PAD_ALLOW; DS5, DS4, Xbox, Switch Pro,
//   8BitDo), never a system HID device's node.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>   /* RLIMIT_RTPRIO + setpriority: RT scheduling in main() */
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>       /* mlockall: a major fault in the capture thread would
                             * inflate a measured NOCP gap (L17b, 2026-08-16) */
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/eventfd.h>    /* NOCP->main-loop kick: drain held audio the moment credits free */
#include <time.h>
#include <dirent.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI         1
#define HCI_CHANNEL_RAW     0
#define HCI_CHANNEL_MONITOR 2
#define HCI_DEV_NONE        0xffff
#define HCI_ACLDATA_PKT     0x02
#define MON_COMMAND_PKT     2
#define MON_EVENT_PKT       3
#define MON_ACL_TX_PKT      4
#define MON_ACL_RX_PKT      5
#define ACL_MAX_REPORT      4096

/* HCI event codes we parse off the MONITOR stream to track handle<->device. */
#define HCI_EV_CONN_COMPLETE    0x03
#define HCI_EV_DISCONN_COMPLETE 0x05
#define HCI_EV_CMD_COMPLETE     0x0e
#define HCI_EV_CMD_STATUS       0x0f
#define HCI_EV_NUM_COMP_PKTS    0x13
#define HCI_EV_MODE_CHANGE      0x14
#define HCI_EV_PTYPE_CHANGED    0x1d
#define HCI_EV_LE_META          0x3e
#define STALL_RESET_MS          150   /* credits stopped this long -> resync a link's outstanding */
#define DEMAND_IDLE_MS         1000   /* no app report for this long -> the link is IDLE, so a
                                       * stuck `outstanding` is teardown residue, not a stall
                                       * (telemetry gate only; see last_demand). 1s is far above
                                       * the ~10-21ms audio cadence AND above the ~1/s rumble-only
                                       * floor, but far below any idle stretch. */
#define AUDIO_IDLE_MS           150   /* no AUDIO report (0x36/0x39) for this long -> the game
                                       * paused its pad audio, and every NOCP spacing measured
                                       * across that pause is injection cadence, not the link.
                                       * Third strike of "presence filter != activity filter"
                                       * (2026-08-16, live): the user let go of R1, three
                                       * 438-968ms game-silence pauses passed the demand gate
                                       * (keepalives kept it fresh) and were read as link
                                       * blackouts. Under real load audio arrives every ~21ms
                                       * and KEEPS being injected through a genuine stall
                                       * (credits stay free), so audio recency is the one
                                       * signal whose sign differs between the two cases. */
/* Edges of the cumulative NOCP-gap histogram, in ms. Chosen to straddle the
 * usable slider range (0..200, default 60) offset by one report of audio, so
 * every plausible pad buffer has an edge within a few ms of its own starvation
 * threshold: B=40 reads at 60, B=60 at 80, B=100 at 120. Above 200 the pad
 * buffer stops being the binding constraint and one overflow bin is enough. */
#define GAPGE_N 10
static const unsigned GAPGE_EDGE[GAPGE_N] =
    { 30, 40, 50, 60, 70, 80, 100, 120, 160, 200 };

#define HCI_SUBEV_LE_CONN       0x01   /* LE Connection Complete                 */
#define HCI_SUBEV_LE_ENH_CONN   0x0a   /* Enhanced LE Connection Complete (BT5)  */

/* The single controller (adapter) we inject on and therefore track. MUST equal the
 * raw inject socket's hci_dev (see main()). The MONITOR header's index field carries
 * the controller number (hci0 -> index 0); ignoring other indices stops a second
 * adapter's handle reuse from polluting g_htab or falsely invalidating a link. */
#define TARGET_HCI_INDEX    0

/* Number of concurrent DualSense inject links. 2 = couch co-op; the code is
 * written to scale (bump this + rebuild). Each slot is an independent template +
 * credit window; the physical radio airtime they share is the real ceiling. */
#define MAX_LINKS           2

#define IDLE_INVALIDATE_MS  1500   /* drop a link's template after this much DS5 silence */
#define SESSION_IDLE_MS     30000  /* no OUTPUT traffic on any link for this long -> as far as
                                    * the RADIO is concerned the session is over and page scan
                                    * goes back on (see ds5_link.session_seen). Deliberately far
                                    * above any in-session output gap, and asymmetric: scan goes
                                    * off again on the first report. A false "idle" here only
                                    * re-enables scan — it never invalidates a template — so the
                                    * worst case is <=SCAN_MIN_GAP_MS of scan-on when output
                                    * resumes, not the template flap of the pre-RX-liveness era. */
#define IDLE_SCAN_MS        250    /* min gap between full g_htab sweeps for an idle pad's
                                    * handle (sniff-pin); cached handle is revalidated O(1)
                                    * on every wakeup in between */
#define DRAIN_CAP           1024   /* max reports drained per poll wakeup (anti-flood) */
/* Fallback when the launcher passes no usable jail uid: accept NOBODY but root.
 *
 * The jail uid is the only non-root peer allowed to inject on the ACL socket and
 * the only one the hid-fd broker serves — and webOS assigns it PER APP ID, so it
 * cannot be a compile-time constant. The launcher reads the live value off the
 * app's own jail directory and passes it as argv[4] (see main()).
 *
 * This used to be 6261, hardcoded. Renaming the app com.aurora.gamestream ->
 * com.aurora.ds5 moved the uid to 5895 and the daemon rejected every request;
 * it failed SILENTLY, because the app only ever learns "no fd" and then reports
 * the ENOENT of its own direct open(). Hence the startup line naming the
 * accepted uid, and the expected uid in the broker's rejection line.
 * (BOTH paths now log a rejection: the broker per connection, the ACL datagram
 * path rate-limited to 1/s — and the 10s stats line breaks the drop tally down
 * per reason, so a climbing `cred:` names this failure on its own.)
 *
 * Falling back to a *number* would be worse than falling back to nothing:
 * 6261 still belongs to the old app, which is still installed, so a missing
 * argv[4] would hand the gate to a different app instead of closing it. */
#define JAIL_UID_DEFAULT    0
#define DS5_VID             0x054c /* Sony      } primary device; the broker additionally */
#define DS5_PID             0x0ce6 /* DualSense } allows a small game-pad list, see PAD_ALLOW */

/* Tagged-datagram framing: [ACL_TAG_M0][kind][addr 6, LSB-first][report].
 * ACL_TAG_M0 (0xA5) is not a DS5 output report id (0x31/0x32/0x36), so a tagged
 * datagram is told apart from a legacy untagged one by byte 0 alone.
 *
 * `kind` separates the two things a tagged datagram can mean, because conflating
 * them double-sends every report across the readiness flip: the app keeps seeding
 * via hidraw for up to one readiness-poll interval AFTER the daemon has bound the
 * link, and an inject-kind datagram arriving in that window is injected while the
 * app also writes it to hidraw — the same frame on air twice.
 *   ACL_TAG_INJECT (0x5A): forward it. The app sends this only while it believes
 *                          the link is READY, i.e. while it is NOT writing hidraw.
 *   ACL_TAG_ASSERT (0x5B): identity assertion only, NEVER injected. The app sends
 *                          this alongside its hidraw seed while NOT ready, so the
 *                          capture thread can match the bytes it sees on air and
 *                          learn handle->bdaddr (see the assert ring). Because it
 *                          is never injected, it stays harmless once the link has
 *                          bound but the app has not yet noticed.
 * An old app that only sends 0x5A keeps working (its asserts inject once bound —
 * the double-send window); an old daemon drops 0x5B (byte 0 is not a report id),
 * so the app just stays on hidraw. Both degrade safely. */
#define ACL_TAG_M0          0xA5
#define ACL_TAG_INJECT      0x5A
#define ACL_TAG_ASSERT      0x5B
/* Control datagram (cred-gated like everything else): [A5][5C][code][value].
 * Code 0x01 = audio-FIFO depth override (value 0..FIFO_MAX, 0xFF clears). The
 * app raises the deep FIFO only after the HOST advertised the rate-servo
 * capability in HOST_CONFIG — a deep FIFO under a non-servo host (fixed 100/s
 * pacing vs the pad's 93.75/s drain) just parks depth*10.7ms of permanent
 * audio latency. The boot default therefore stays shallow (see ds5-tmpld.sh). */
#define ACL_TAG_CTRL        0x5C
#define ACL_CTRL_FIFO_DEPTH 0x01
/* Code 0x02 = idle lightbar colour, [A5][5C][02][R][G][B]. Lets the app pick the
 * colour the painter uses while NOBODY is feeding this pad, so the two states the
 * app can tell apart but the daemon cannot -- "connected, unused" vs "connected
 * and in use by the app's own SDL input" -- get different colours WITHOUT a
 * second writer on the pad. The daemon stays the only one painting an idle link;
 * the app merely selects. (While an app IS feeding us the host owns the bar and
 * the painter is silent anyway, so this never competes with a live session.) */
#define ACL_CTRL_IDLE_LB    0x02
/* Code 0x03 = clear the app's idle-lightbar selection, [A5][5C][03] (n>=3, no
 * payload). 0x02 used to OVERWRITE the daemon's one colour global, so a single
 * SDL open/close cycle in the app permanently discarded the operator's
 * DS5_IDLE_LIGHTBAR env config (including "off" — the app re-enabled a painter
 * the operator disabled). Boot and app state are now split (see
 * g_idle_lb_boot/_app): 0x02 keeps its exact 6-byte wire format but only sets
 * the APP SELECTION; the client sends 0x03 for "connected, unused" so the boot
 * default comes back instead of the app echoing a literal colour. */
#define ACL_CTRL_IDLE_LB_CLEAR 0x03
#define ACL_TAG_LEN         8

struct sockaddr_hci { unsigned short hci_family, hci_dev, hci_channel; };
struct hci_mon_hdr  { uint16_t opcode, index, len; } __attribute__((packed));

/* hidraw VID/PID query — used to confine the broker to the DS5's own node. */
struct hidraw_devinfo { uint32_t bustype; int16_t vendor; int16_t product; };
#ifndef HIDIOCGRAWINFO
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif

/* HCIGETCONNLIST: bluez-compatible connection-list ioctl (best-effort startup
 * seed of the handle->bdaddr table for a controller already connected before we
 * started monitoring; if the kernel lacks it we fall back to event parsing). */
typedef struct { uint8_t b[6]; } bdaddr_t;
struct hci_conn_info { uint16_t handle; bdaddr_t bdaddr; uint8_t type, out;
                       uint16_t state; uint32_t link_mode; };
struct hci_conn_list_req { uint16_t dev_id, conn_num; struct hci_conn_info ci[16]; };
#ifndef HCIGETCONNLIST
#define HCIGETCONNLIST _IOR('H', 212, int)
#endif

static int injectable(uint8_t id){ return id==0x31 || id==0x32 || id==0x36 || id==0x39; }

/* --- idle lightbar -------------------------------------------------------
 * While a pad is connected but NO app is feeding it, the lightbar is whatever
 * the firmware picked: full-brightness player blue, or the orange charge
 * indication if it was charging when it connected. We are the only writer on
 * that link in this state (the host owns the bar as soon as it feeds), so paint
 * a dim idle colour instead.
 *
 * Layout facts are the same public ones the host-side builder uses (Linux
 * hid-playstation.c / DualSense wiki): the BT output report is
 *   [0x31][seq<<4][0x10][47-byte common block][pad][crc32 LE]
 * and the CRC is computed over a 0xA2 transaction-seed byte followed by every
 * byte up to the checksum itself. */
#define DS5_BT_OUT_LEN     78
#define DS5_OUT_COMMON_LEN 47
#define DS5_OUT_CRC_SEED   0xA2

/* Dim idle colour, 0x00RRGGBB. Overridable via DS5_IDLE_LIGHTBAR (hex) so the
 * boot script can change it without a rebuild; 0 disables the painter. */
#define IDLE_LB_RGB_DEFAULT 0x000002u
/* Beat the pad's asynchronous switch into extended BT mode: a few paints close
 * together right after the link falls idle, then a slow self-healing keep-alive
 * (the firmware re-takes the bar on its own when charging state changes). */
#define IDLE_LB_BURST        3
#define IDLE_LB_BURST_MS     1000
#define IDLE_LB_KEEPALIVE_MS 30000
/* The painter's demand gate (tx_demand) only sees OUR unix-socket datagrams;
 * kernel/hidraw writers (the app seeding pre-readiness, a non-Aurora app like
 * Kodi setting the LED, hid-playstation itself) never stamp last_demand, and
 * painting over them raw-injects 0x31 frames interleaved with the kernel's
 * writes on the same L2CAP channel (the documented two-writer oscillation).
 * session_seen DOES see them (on-air MON_ACL_TX), so additionally require this
 * much on-air OUTPUT silence before painting. Residual limitation: a hidraw
 * app that writes once and then never again is indistinguishable from idle
 * without traffic and will still be repainted after the quiet window. */
#define IDLE_LB_HIDRAW_QUIET_MS 5000

/* Painter colour state is SPLIT so the app can never destroy the operator's
 * boot configuration (a single global here let one ctrl-0x02 overwrite it for
 * the rest of the daemon's life). Boot value 0 = painter disabled, operator
 * wins regardless of any app selection; an app selection of 0 means "paint
 * nothing" (the app owns the idle bar) and is honored. All three are
 * main-thread-only (env parse, ctrl handler and painter run on main). */
static uint32_t g_idle_lb_boot    = IDLE_LB_RGB_DEFAULT; /* DS5_IDLE_LIGHTBAR */
static uint32_t g_idle_lb_app     = 0;  /* ctrl-0x02 selection (iff _set) */
static int      g_idle_lb_app_set = 0;  /* cleared by ctrl-0x03 / restart */
static uint32_t idle_lb_effective(void){
    return g_idle_lb_boot==0 ? 0 : (g_idle_lb_app_set ? g_idle_lb_app : g_idle_lb_boot);
}

static uint32_t ds5_crc32_update(uint32_t crc, const uint8_t *d, size_t n){
    for(size_t i=0;i<n;i++){
        crc^=d[i];
        for(int b=0;b<8;b++) crc=(crc>>1)^(0xEDB88320u & (~(crc&1u)+1u));
    }
    return crc;
}

/* Build a CRC-signed BT output report that sets the lightbar to @rgb.
 * The lightbar-setup release (valid_flag2 bit1 + lightbar_setup=LIGHT_OUT) is
 * folded in on every paint: over BT the firmware ignores colour writes until it
 * has been released, and a fresh connect re-latches that gate. We only ever
 * paint an idle link, so there is no game whose colour this could fight. */
static void ds5_build_lightbar(uint8_t out[DS5_BT_OUT_LEN], uint32_t rgb, uint8_t seq){
    uint8_t common[DS5_OUT_COMMON_LEN];
    memset(common,0,sizeof common);
    common[1]  = 0x04;   /* valid_flag1: LIGHTBAR_CONTROL_ENABLE */
    common[38] = 0x02;   /* valid_flag2: LIGHTBAR_SETUP_CONTROL_ENABLE */
    common[41] = 0x02;   /* lightbar_setup: LIGHT_OUT */
    common[44] = (uint8_t)(rgb>>16);
    common[45] = (uint8_t)(rgb>>8);
    common[46] = (uint8_t)rgb;

    memset(out,0,DS5_BT_OUT_LEN);
    out[0]=0x31; out[1]=(uint8_t)(seq<<4); out[2]=0x10;
    memcpy(out+3,common,DS5_OUT_COMMON_LEN);
    uint8_t seed=DS5_OUT_CRC_SEED;
    uint32_t crc=ds5_crc32_update(0xFFFFFFFFu,&seed,1);
    crc=ds5_crc32_update(crc,out,DS5_BT_OUT_LEN-4);
    crc=~crc;
    out[74]=(uint8_t)(crc);        out[75]=(uint8_t)(crc>>8);
    out[76]=(uint8_t)(crc>>16);    out[77]=(uint8_t)(crc>>24);
}

/* Audio-class output reports: they carry the speaker/voice-coil payload, share
 * the audio credit budget and the elastic FIFO, and must never be deduped.
 * 0x36 (398 B) = one 10 ms Opus frame + one 64-byte coil block.
 * 0x39 (547 B) = the batched variant of the same stream: TWO of each per report
 * (~47/s instead of ~100/s). Both ride the same HID-interrupt channel, and the
 * ACL/L2CAP lengths are recomputed per send, so the captured template serves
 * either. Everything else (0x31 rumble/trigger/LED, 0x32 SetState) is
 * state-class and keeps the rumble path's latest-wins semantics. */
static int is_audio_report(uint8_t id){ return id==0x36 || id==0x39; }
static uint64_t now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000ull+ts.tv_nsec/1000000ull; }
static uint64_t now_us(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000ull+ts.tv_nsec/1000ull; }

/* stderr write-cost probe (2026-08-15). The EPISODE lines are written from the
 * INJECT thread and they land DURING the stall they describe. glibc leaves
 * stderr unbuffered, so each one is a write(2): if stderr is a pipe into
 * pmlog/console rather than the tmpfs log file, a blocked write adds jitter to
 * injection at the worst possible moment AND perturbs the very gap it records.
 * Nobody has ever measured whether it actually blocks here, so the first move is
 * a probe, not a rewrite — the cost of guessing wrong in either direction is a
 * rebuild of the primary instrument. Reported on the 10s status line as
 * logw=max_us/slow, slow = writes >=1ms. A p99 far under 1ms (the tmpfs-backed
 * expectation) closes the item; multi-ms blocks correlated with episodes promote
 * it ahead of every other A/B, because it then contaminates their measurement. */
static uint64_t g_logw_max_us=0;
static long     g_logw_n=0, g_logw_slow=0;
static inline void logw_note(uint64_t t0){
    uint64_t d=now_us()-t0;
    g_logw_n++;
    if(d>g_logw_max_us) g_logw_max_us=d;
    if(d>=1000) g_logw_slow++;
}

/* Read a small non-negative integer out of a ROOT-OWNED regular file; -1 if the
 * file is absent, a symlink, not root-owned, or unparsable (both callers range-
 * check, and -1 fails every range, so it needs no separate error path).
 *
 * The two live tunables below sit on predictable /tmp paths and steer the very
 * parameters that are carefully SO_PEERCRED-gated when they arrive as an
 * ACL_TAG_CTRL datagram, so they get an ownership gate of their own: O_NOFOLLOW
 * refuses a planted symlink and fstat() on the OPEN fd (never the path) closes the
 * swap race. This is defence in depth — the daemon's /tmp is the ROOT namespace's,
 * not the jail's, so the app cannot reach these files at all. The file channel
 * stays (rather than moving behind a second ctrl code) because the tunables must
 * also work with no app attached: boot default and operator tuning.
 *
 * `warned` latches one line per call site: a refused file is re-read every second,
 * so it must be neither silent (the operator's edit would just stop working) nor
 * repeated. */
static int read_root_int(const char *path, int *warned){
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){
        /* ENOENT is the normal case (no tunable set) and must stay quiet. Anything
         * else is a refusal the operator needs to see — above all the ELOOP that
         * O_NOFOLLOW raises on the planted symlink this gate exists for, which
         * would otherwise be the one refusal that logs nothing at all. */
        if(errno!=ENOENT && warned && !*warned){
            *warned=1;
            fprintf(stderr,"[txd] ignoring %s: open failed errno=%d (%s)\n",
                    path,errno,strerror(errno));
        }
        return -1;
    }
    struct stat st;
    if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_uid!=0){
        if(warned && !*warned){
            *warned=1;
            fprintf(stderr,"[txd] ignoring %s: not a root-owned regular file\n",path);
        }
        close(fd); return -1;
    }
    char b[32]; ssize_t n=read(fd,b,sizeof b-1);
    close(fd);
    if(n<=0) return -1;
    b[n]='\0';
    char *end=NULL; long v=strtol(b,&end,10);
    if(end==b || v<0 || v>100000) return -1;
    return (int)v;
}

/* Max outstanding raw-ACL TX packets (a credit window), PER LINK. The inject path
 * bypasses the webOS BT one-outstanding metering: keeping it at 1 was the original
 * jitter wall, but removing the bound ENTIRELY lets our output backlog the
 * controller's TX queue, so the baseband spends its slots draining TX and polls the
 * DS5 less -> its INPUT reports gap (controller lag while the video/WiFi stays
 * smooth). A small credit window is the fix: pipeline enough packets to keep
 * haptics tight, but never so many that TX starves the input poll. It is INHERENTLY
 * ADAPTIVE and prioritises input without a static rate cap -- when the link is idle
 * the controller confirms our packets fast (NOCP), credits free immediately and we
 * inject at full haptic rate; only under contention (input/video needing airtime)
 * do the credits lag, and output automatically backs off to whatever bandwidth is
 * left. Live-tunable via /tmp/ds5_inject_maxq (>=1; a large value approximates the
 * original unmetered path; the file must be ROOT-OWNED). Cached ~1/sec. It is PER
 * LINK, so two pads each keep an
 * independent window; the shared radio airtime is what actually arbitrates them.
 *
 * Default 12 (measured 2026-07-04 in-game): the DS5 speaker/haptics 0x36 audio
 * stream is ~94 frames/s, and a window of 3 filled constantly under contention
 * -> the drop-newest path fired ~180x/10s = audible audio/haptic dropouts. A
 * gap sweep (3/8/12/16) vs DS5 input-report rate showed input is BT-connection-
 * -interval bound (~22ms floor) and barely moves with the window: 3->16 cost
 * only ~5% input rate (496->469 reports/10s) while cutting audio drops ~97%
 * (180->5). 12 is the balance: drops ~14/10s, input 482/10s (~3% under floor). */
#define INJECT_MAXQ_DEFAULT 12
static int inject_maxq(void){
    static int q=INJECT_MAXQ_DEFAULT; static uint64_t last=0; static int warned=0;
    uint64_t n=now_us();
    if(last==0 || n-last>1000000ull){
        last=n;
        int v=read_root_int("/tmp/ds5_inject_maxq",&warned);
        if(v>=1 && v<=1000) q=v;
    }
    return q;
}

/* Elastic FIFO for 0x36 speaker/haptics audio (OPT-IN, default 0 = disabled =
 * legacy drop-newest behavior). When a link's credit window is transiently full
 * (BT airtime lost to input/video), the drop-newest path punches a ~10ms hole
 * in the audio -> audible "Aussetzer". With a FIFO of depth N>0, a full-window
 * audio frame is instead HELD and injected as soon as a credit frees (drained
 * on the next report arrival, ~10ms grid), converting a transient drop into a
 * few-ms delay. The window (inject_maxq) still bounds the controller's TX queue,
 * so input polling is unaffected -- the FIFO holds frames in OUR memory, not in
 * the controller. Sustained congestion overflows the FIFO -> drop-oldest, so
 * latency is bounded by N frames (~10.7ms each). Live-tunable via
 * /tmp/ds5_inject_fifo (0..FIFO_MAX; the file must be ROOT-OWNED). Cached ~1/sec.
 * Each link has its own FIFO. */
#define FIFO_MAX             16
#define FIFO_ENTRY_MAX       1024   /* == ASSERT_MAX: the worst-case on-air audio report
                                     * (0x36 = 398 B, batched 0x39 = 547 B; both fit)
                                     * report (kept in sync by hand — ASSERT_MAX is
                                     * defined later so it can't be referenced here). At
                                     * 256 a real >256B audio frame would skip the FIFO
                                     * entirely (n<=FIFO_ENTRY_MAX gate) and fall to
                                     * latest-wins drop, silently defeating the
                                     * anti-dropout buffering. 16 entries x 1KB per link
                                     * is negligible. */
#define INJECT_FIFO_DEFAULT  0
static int g_fifo_override=-1;   /* ACL_CTRL_FIFO_DEPTH; main thread only */

static int inject_fifo(void){
    static int d=INJECT_FIFO_DEFAULT; static uint64_t last=0; static int warned=0;
    uint64_t n=now_us();
    if(last==0 || n-last>1000000ull){
        last=n;
        int v=read_root_int("/tmp/ds5_inject_fifo",&warned);
        if(v>=0 && v<=FIFO_MAX) d=v;
    }
    /* Session override from the app (it knows whether the HOST runs the rate
     * servo that bounds a deep FIFO's parked latency) wins over the boot/file
     * default; cleared on daemon restart and via 0xFF. */
    if(g_fifo_override>=0 && g_fifo_override<=FIFO_MAX) return g_fifo_override;
    return d;
}

/* ---- deterministic gap injector (bench instrument, 2026-08-15) ------------- *
 * Real coex episodes arrive ~5.3/min and CLUSTER, so every pad-side experiment
 * (underrun rule, re-prime depth, buffer-byte semantics) costs hours per data
 * point and is confounded by which cluster it landed in. This holds every ACL
 * write for exactly G ms on command, so the same questions take a 10-minute
 * sweep with real repetitions.
 *
 * 🚨 SCOPE — READ BEFORE TRUSTING A RESULT. A write-hold emulates TX ABSENCE,
 * not credit starvation: the controller's own TX queue drains normally and then
 * sits EMPTY for the rest of the hold. So this rig validates PAD physics (how
 * the pad's jitter buffer reacts to a hole) and nothing else. It can never
 * exercise the stall-resync path, and — once the auto-flush lever exists — it
 * can never fire a flush or produce a Flush_Occurred, because there is nothing
 * queued to flush. Flush semantics are only observable on REAL episodes.
 *
 * Ingest, FIFO accounting and credit accounting all stay live, so the backlog
 * behaviour on release is the real one.
 *
 * Safety (a stray trigger during a user session is a self-inflicted dropout that
 * would be blamed on coex and would poison the ledger): root-owned file only,
 * ONE-SHOT via unlink-on-read, refused if older than 5 s, G clamped, and both
 * ends logged with their own marker so the ledger can select or exclude them. */
#define GAP_INJECT_PATH     "/tmp/ds5_gap_inject"
#define GAP_INJECT_MIN_MS   20
#define GAP_INJECT_MAX_MS   2000
#define GAP_INJECT_FRESH_S  5
/* Telemetry quiet time AFTER the hold. The host rate servo up-steps on fifo
 * depth and drops and decays at only 3us per feedback sample (~12s to shed a
 * full stretch), so publishing the injected backlog would bias every
 * measurement for minutes afterwards. Freezing the .st record freezes the app's
 * PACE_FEEDBACK with it — the app already treats a non-advancing seq as
 * "nothing new", so no app change is needed. */
#define GAP_INJECT_QUIET_MS 2000
static uint64_t g_gap_hold_until_us=0;   /* written by main only (inject path reads it) */
/* Read by the CAPTURE thread too (it tags synthetic gaps), so both ends use
 * relaxed atomics — same reasoning as ds5_link.last_demand. */
static uint64_t g_gap_quiet_until_us=0;
static long     g_gap_injections=0;

/* Arm from the control file. Called on a ~250ms tick from the main loop (a
 * bench trigger does not need report-rate latency, and this keeps the syscall
 * off the inject path). */
static void gap_inject_poll(void){
    static uint64_t last=0; static int warned=0;
    uint64_t n=now_us();
    if(last && n-last<250000ull) return;
    last=n;
    int fd=open(GAP_INJECT_PATH,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return;                     /* ENOENT is the normal state */
    struct stat st; char b[32]; ssize_t rd=-1;
    if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==0)
        rd=read(fd,b,sizeof b-1);
    close(fd);
    unlink(GAP_INJECT_PATH);             /* ONE-SHOT: consumed even if malformed */
    if(rd<=0){
        if(!warned){ warned=1; fprintf(stderr,"[txd] GAPINJECT ignored: not a readable root-owned file\n"); }
        return;
    }
    b[rd]='\0';
    /* Freshness against the WALL clock (the file carries an mtime, not a
     * monotonic stamp): a trigger left over from a previous bench session must
     * never fire into a user session hours later. */
    time_t age=time(NULL)-st.st_mtim.tv_sec;
    char *end=NULL; long g=strtol(b,&end,10);
    if(end==b || g<GAP_INJECT_MIN_MS || g>GAP_INJECT_MAX_MS){
        fprintf(stderr,"[txd] GAPINJECT refused: G=%ld outside %d..%d ms\n",g,GAP_INJECT_MIN_MS,GAP_INJECT_MAX_MS);
        return;
    }
    if(age<0 || age>GAP_INJECT_FRESH_S){
        fprintf(stderr,"[txd] GAPINJECT refused: trigger is %llds old (max %d)\n",
                (long long)age,GAP_INJECT_FRESH_S);
        return;
    }
    g_gap_hold_until_us=now_us()+(uint64_t)g*1000ull;
    __atomic_store_n(&g_gap_quiet_until_us,g_gap_hold_until_us+GAP_INJECT_QUIET_MS*1000ull,__ATOMIC_RELAXED);
    g_gap_injections++;
    fprintf(stderr,"[txd] GAPINJECT start G=%ldms n=%ld (SYNTHETIC — telemetry frozen for G+%dms)\n",
            g,g_gap_injections,GAP_INJECT_QUIET_MS);
}

/* True while a synthetic hold is in force. PURE predicate: it is called from the
 * inject path with g_lock held, so it must not log or do anything blocking —
 * the end marker is emitted by gap_inject_tick() instead. */
static int gap_hold_active(void){
    return g_gap_hold_until_us && now_us()<g_gap_hold_until_us;
}

/* Once per main-loop wakeup, outside every lock: close out a finished hold (the
 * ledger marker wants the real end time, not the next 250ms poll) and then let
 * the throttled file poll arm the next one. */
static void gap_inject_tick(void){
    if(g_gap_hold_until_us && now_us()>=g_gap_hold_until_us){
        g_gap_hold_until_us=0;
        fprintf(stderr,"[txd] GAPINJECT end n=%ld\n",g_gap_injections);
    }
    gap_inject_poll();
}

/* g_lock guards the per-link template/identity state below (the g_links[] fields
 * noted "g_lock"). It is held only for short, NON-BLOCKING work — never across
 * filesystem I/O. Publishing the template record (which does open/write/rename and
 * can block on the jail mount) is done by publish_all() OUTSIDE g_lock, serialized
 * by g_pub_lock, to keep the inject loop's critical section bounded (no audio-grid
 * stalls). */
static pthread_mutex_t g_lock     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_pub_lock = PTHREAD_MUTEX_INITIALIZER;

#define TXRING 64

/* ---- one inject link (per DualSense) --------------------------------------- *
 * A link owns everything that is per-controller: its captured ACL template, its
 * identity binding, its credit window + tx-type ring, its audio FIFO, and its gap
 * telemetry. Fields are annotated with their guard/owner exactly as the single
 * globals were before the array refactor:
 *   [g_lock]  read/written from BOTH threads (capture + inject) -> under g_lock.
 *   [inject]  touched only by the main (inject) poll loop -> no lock.
 *   [cap]     touched only by the capture thread -> no lock.
 *
 * INVARIANT: have==1  =>  the template was bound with a KNOWN bdaddr
 * (bound_known==1, bound_addr valid). Capture refuses to set have without a known
 * bdaddr (fail closed), so every reader can trust bound_addr when have. */
struct ds5_link {
    /* template + identity (g_lock) */
    uint8_t  hdr[8];         /* captured ACL/L2CAP header (handle @0..1, CID @6..7) */
    int      have;           /* template valid -> safe to inject */
    uint16_t nonce;          /* bumps each (re)bind; stamps FIFO entries as a generation */
    uint64_t last_seen;      /* last on-air HID-output / NOCP / inbound ACL for the bound
                              * handle = ELECTRICAL liveness (keeps the template valid) */
    uint64_t session_seen;   /* last OUTPUT activity on this link: our own injects plus the
                              * kernel path's on-air HID-output. Deliberately NOT refreshed
                              * by inbound ACL: a powered-on DS5 streams input reports
                              * forever, so last_seen can never fall stale and therefore can
                              * never mean "the session ended" — keying the shared radio's
                              * scan state on it left page scan suppressed until the pad
                              * disconnected. This field is what the scan reconciler uses;
                              * last_seen keeps its template-freshness job unchanged. */
    uint16_t handle;         /* 12-bit ACL handle the template is bound to */
    uint8_t  bound_addr[6];  /* device identity captured with the template (LSB-first) */
    int      bound_known;    /* bound_addr is valid */
    int      ever_bound;     /* bound_addr is meaningful for the per-address file path */
    int      assert_learned; /* identity came from a JAIL-supplied assert, not a kernel
                              * event (no CONN_COMPLETE at restart). Such a binding is
                              * NOT trusted to survive a later kernel connect on its
                              * handle — see the CONN/LE handlers — else a lie asserted
                              * for a victim's bdaddr could pre-authorise inject onto a
                              * reused handle (would defeat defence (b)). */
    /* credit window (g_lock) */
    int      outstanding;    /* our raw-ACL TX packets queued in the controller, not yet
                                confirmed on-air (via HCI Number_Of_Completed_Packets) */
    uint64_t last_nocp;      /* last NOCP time; stall backstop if credits stop returning */
    uint64_t last_nocp_k;    /* kernel SO_TIMESTAMP (CLOCK_REALTIME ms) of the last NOCP.
                                Since 1.4.34 the gap histogram bins deltas of THESE stamps:
                                the ledger's clock used to be now_ms() in this nice-19
                                capture thread, so during a real session (50 SCHED_RR client
                                threads) reader lag was indistinguishable from air — the one
                                in-situ condition the clock probe never covered. Kernel
                                stamps are taken at socket-queue time, immune to our own
                                scheduling. Deltas only ever pair two kernel stamps
                                (nocp_k_ok); clock domains are never mixed. */
    uint8_t  nocp_k_ok;      /* last_nocp_k holds a kernel stamp (else the next interval
                                falls back to the old monotonic delta) */
    uint64_t last_wr_err_log;/* rate limit for the structural-write-error line (inject) */
    uint64_t last_demand;    /* last report the APP handed us for this link. Written by the
                                inject thread; since the gap histogram moved into the NOCP
                                handler the CAPTURE thread reads it too, so both sides go
                                through __atomic_*_n RELAXED — a torn 64-bit read on this
                                32-bit ARM would only mis-gate a single gap, but the race is
                                free to remove. Telemetry only:
                                separates "idle with residue" from a real TX stall. Must be
                                DEMAND, not last successful inject — a credit stall blocks
                                injection, so keying on success would mute exactly the
                                stalls the episode metric exists to catch. */
    /* in-flight TX type ring (g_lock, same lifetime as outstanding). NOCP credits do
     * not say WHICH packet completed, so per-type accounting approximates FIFO:
     * injects push their type, each returned credit pops the oldest. rumble_fly is
     * what lets the credit window be type-aware — rumble bounded by its OWN occupancy
     * instead of the total (a window full of audio must never read as "rumble over
     * budget"). Errs OPEN under foreign kernel-path TX on the same handle (their
     * NOCPs pop our entries early -> rumble_fly under-counts -> rumble slightly more
     * permissive), which is the safe direction. */
    uint8_t  txtype[TXRING]; /* 1 = rumble (0x31/0x32), 0 = audio (0x36/0x39) */
    int      tx_head, tx_cnt;
    int      rumble_fly;     /* believed-in-flight rumble packets */
    /* L4 ghost accounting: packets the stall backstop WROTE OFF while the
     * controller may still have been holding them. Their NOCPs arrive later and
     * would otherwise free credits for packets we already stopped counting. */
    int      ghost;          /* written-off packets still awaiting their late NOCPs */
    uint64_t ghost_ts;       /* when the write-off happened (TTL base) */
    long     ghost_absorbed; /* lifetime: late credits correctly consumed by ghosts */
    long     ghost_expired;  /* lifetime: ghosts that never came back (genuinely lost) */
    /* audio elastic FIFO (inject) — see inject_fifo() */
    struct { uint8_t buf[FIFO_ENTRY_MAX]; int len; uint64_t ts; } fifo[FIFO_MAX];
    int      fifo_head, fifo_count;
    uint16_t fifo_gen;       /* nonce the held backlog was queued under (stale-drop) */
    /* per-link inject/drop totals (main thread only; published in the .st
     * telemetry record). The old records carried the daemon-GLOBAL counters,
     * so with two pads the host rate servo attributed one pad's drop storm to
     * BOTH links and throttled the healthy pad too. */
    long     inj_total, drop_total;
    /* drop_total BY REASON (main thread; drop_total stays their sum so every
     * historical reader keeps working). The host rate servo up-steps its stretch
     * by 20us per drop it sees — but an AGE-OUT is an intentional staleness shed
     * (the frame was already past the pad's buffer), while an OVERFLOW eviction is
     * the real "we could not keep up" signature. Feeding both into one counter
     * makes the servo punish our own correct sheds, slowing production further and
     * eroding pad depth (which nothing in the stack ever refills). Splitting them
     * is also what lets a maxq ladder have an abort criterion at all: "drop_total
     * must stay 0" self-aborts the moment a normal 150-400ms episode ages a frame
     * out, which is by design and not a failure. */
    long     drop_age;       /* FIFO age-out (> FIFO_MAX_AGE_MS): intended shed */
    long     drop_ovf;       /* FIFO overflow evict-to-fit: could not keep up */
    long     flush_events;   /* controller-side auto-flush (0 until 0x0C28 ships) */
    /* previous identity pending an invalid-publish (g_lock). When a slot is
     * re-bound to a DIFFERENT pad before the old pad's invalidation was
     * published (main-thread invalidate racing a capture rebind), the old
     * address is no longer in any slot and publish_all() would never write its
     * per-address file again — the evicted pad's client would keep forwarding
     * against a stale valid=1 record forever. link_bind stages the outgoing
     * address here; the next publish_all() emits one valid=0 record for it. */
    uint8_t  prev_addr[6];
    int      prev_valid;
    /* per-link link-policy pin (cap) */
    uint16_t policy_handle;  /* handle the last link-policy write was for */
    uint64_t last_policy;
    uint64_t last_unsniff;   /* last Exit_Sniff send (>=3s throttle, cap-only) */
    uint16_t unsniff_probe_h;/* handle we already probed for an UNOBSERVED sniff */
    uint16_t flush_handle;   /* handle the last Write_Automatic_Flush_Timeout was for */
    int      flush_sent_ms;  /* value we last wrote (0 = infinite/off); -1 = nothing written */
    uint64_t last_flush_cmd; /* send throttle, shares the >=3s command budget */
    uint64_t last_linkq;     /* L16 poll throttle (capture thread) */
    unsigned linkq_rot;      /* L16 round-robin index over the four status reads */
    /* Start of the CURRENT continuous-demand stretch (relaxed atomic, written by
     * the main thread next to last_demand, read by the capture thread's NOCP
     * gate). The old gate tested demand only at the END of a gap, so one idle
     * keepalive re-armed it and a ~5s idle gap landed in every histogram bin —
     * the documented `d_inj=2` / `30:2/…/200:2` contamination. Requiring
     * last_nocp >= demand_since makes demand hold ACROSS the gap. */
    uint64_t demand_since;
    /* Same pair again, but for AUDIO reports only (0x36/0x39) — the histogram's
     * real gate. Demand alone cannot discriminate game silence from a link
     * stall (see AUDIO_IDLE_MS); audio injection can, because it continues
     * through a genuine stall and stops in silence. Relaxed atomics, same
     * writer/reader pair as last_demand/demand_since. */
    uint64_t last_audio;
    uint64_t audio_since;
    /* L18 packet-type clamp (capture thread) — same believed/sent pattern as the
     * flush lever above: a handle we never wrote carries the stack default. */
    uint16_t ptype_handle;   /* handle the last Change_Connection_Packet_Type was for */
    int      ptype_sent;     /* mask we last wrote; -1 = nothing written */
    uint64_t last_ptype_cmd; /* send throttle, shares the >=3s command budget */
    /* per-link gap telemetry (g_lock) — the A/B acceptance signal.
     *
     * MOVED TO THE CAPTURE THREAD 2026-08-15. The histogram used to be derived in
     * the main poll loop as a high-watermark of (now - last_nocp), sampled once per
     * wakeup. That loop is DATAGRAM-clocked, and since the switch to batched 0x39
     * the app feeds it ~47/s — so every gap was quantized to ~21ms and the 30-49
     * bin systematically undercounted (the bins are absolute, while the underrun
     * rule itself moved from B+10.67 to B+21.33ms). The NOCP handler sees every
     * credit return at native rate and each arrival ENDS exactly one gap, so
     * binning (now - last_nocp) there needs no watermark and carries no
     * quantization. Written under the lock the NOCP handler already holds; the
     * main loop snapshots them in its existing locked block. */
    long     gap30, gap50, gap80; /* NOCP-gap histogram 30-49/50-79/>=80ms */
    /* Cumulative "at or above" histogram, GAPGE_EDGE[] in ms.
     *
     * The three bins above have fixed edges at 30/50/80, and the question we
     * actually ask of them does not: a pad buffer of B ms survives a gap iff
     * roughly g <= B + one report's worth of audio (21.33ms on batched 0x39,
     * 10.67 on 0x36). So the decision threshold moves with the slider and never
     * lands on a bin edge. Reading exposure for B=40 or B=60 out of 30/50/80
     * means interpolating inside a bin, which is exactly the guesswork that sent
     * a 2026-08-16 A/B to the ears instead of to a counter: every audible
     * counter in the stack read zero while the pad was audibly dropping out,
     * because the failure happens INSIDE the pad and nothing here can see it.
     *
     * Cumulative counts make that readable without arithmetic: at buffer B, look
     * up the edge nearest B+21 and the number there is how often this link
     * out-ran the pad. Lower B until it leaves zero and you have the floor —
     * with no microphone and no subjective judgement.
     *
     * The old three bins stay untouched: the phase-2/4 analysis tools parse
     * `gaps=%ld/%ld/%ld` positionally. */
    long     gapge[GAPGE_N];
    uint64_t gap_max;             /* largest single gap this binding (ms) */
    /* Gaps we caused ourselves with the bench injector are counted SEPARATELY.
     * They must not enter the production bins — the histogram is the primary
     * instrument for every experiment in this area, and a rig that quietly
     * inflates the band it is meant to measure is worse than no rig. Keeping the
     * measured length is what makes the injector self-validating: commanded G
     * should come back out here within a few ms. */
    long     gap_synth;           /* synthetic gaps observed */
    uint64_t gap_synth_ms;        /* length of the most recent one */
    uint64_t ep_start;            /* episode start (main thread; the >80ms detector) */
    uint16_t tele_gen;            /* nonce the histogram counts; reset on rebind */
    /* RX-continuity forensics (stall RCA): inbound ACL packets from THIS link
     * (capture thread, g_lock) and episode-start snapshots (inject thread).
     * During a TX stall the deltas discriminate the cause: DS5 RX flowing =
     * TX-path/scheduling only; DS5 RX dead but other links alive = this
     * BR/EDR link's RF; everything dead = radio-global (coex/scan grab). */
    uint64_t rx_pkts;
    uint64_t ep_rx0, ep_other0;
    /* idle lightbar painter (main thread only) */
    uint64_t lb_last_paint;
    int      lb_paints;      /* paints since this link last went idle */
    uint16_t lb_idle_gen;    /* nonce the burst was started under */
    uint8_t  lb_seq;
};
static struct ds5_link g_links[MAX_LINKS];
static uint64_t g_other_rx;   /* inbound ACL on handles we don't own (g_lock) */

static void txwin_reset(struct ds5_link *L){ L->tx_head=0; L->tx_cnt=0; L->rumble_fly=0; }
static void txwin_push(struct ds5_link *L, int rumble){
    if(L->tx_cnt==TXRING){            /* overflow (huge tuned maxq): age out oldest */
        if(L->txtype[L->tx_head]) L->rumble_fly--;
        L->tx_head=(L->tx_head+1)%TXRING; L->tx_cnt--;
    }
    L->txtype[(L->tx_head+L->tx_cnt)%TXRING]=(uint8_t)(rumble?1:0);
    L->tx_cnt++; if(rumble) L->rumble_fly++;
}
static void txwin_pop(struct ds5_link *L, int cnt){
    while(cnt-->0 && L->tx_cnt>0){
        if(L->txtype[L->tx_head]) L->rumble_fly--;
        L->tx_head=(L->tx_head+1)%TXRING; L->tx_cnt--;
    }
}
static const char *g_tmpl_path;

/* ---- handle<->device identity binding (anti cross-contamination) ----------- *
 * The compositor Left-Alt latch traced to raw-ACL injection landing on the WRONG
 * BT device (the Magic Remote) after the DS5 link flapped and its 12-bit ACL
 * handle was REUSED for another device: the kernel routes our inject purely by
 * that handle, write() still succeeds (handle valid, wrong device), so the old
 * EBADF guard never tripped. We close it by binding each template to the BD_ADDR it
 * was captured on (guaranteed to be a DualSense — only a DualSense receives an 0xA2
 * 0x31/0x32/0x36 HID output) and refusing to inject the instant that handle's
 * bdaddr stops matching. handle->bdaddr is learned from CONN_COMPLETE /
 * (Enhanced)LE_CONN_COMPLETE / DISCONN_COMPLETE events on the SAME monitor socket
 * we already hold, seeded by HCIGETCONNLIST. No address is hardcoded — the binding
 * self-configures, so swapping controllers needs no rebuild. The table is SHARED
 * across links (it is just the adapter's handle map). */
struct htab_ent { uint8_t addr[6]; uint8_t known; uint8_t mode; uint8_t from_assert; uint8_t mode_seen; };
/* mode: HCI Mode Change (0=active,2=sniff). mode_seen: that value was OBSERVED (Mode
 * Change, or a kernel connect that by definition starts active) rather than assumed.
 * Everything that learns a handle without a connect event -- the identity assert, the
 * kernel conn-list seed -- writes mode=0 because it has nothing better to write, and a
 * link that fell into sniff BEFORE this daemon started then looks active forever: the
 * exit below never fires, 3-slot audio never fits the sniff window, and the transport
 * silently delivers a third of the offered reports. Measured 2026-08-16: 15-21 of 46
 * reports/s, with the pad's small uplink flowing normally the whole time. from_assert:
 * this handle's address was
 * learned from a jail identity-assert, not a kernel connect event -> any link bound
 * off it inherits assert_learned taint (so a later kernel connect re-derives it).
 * The taint lives on the htab entry, not just the link, so it survives an
 * idle-invalidate + rebind from the same handle (a per-link flag was lost there). */
static struct htab_ent g_htab[4096];   /* indexed by handle & 0x0fff */

/* ---- identity-assert ring (restart fix, 2026-07-10) ------------------------ *
 * After a daemon RESTART with the DualSenses already connected there is no
 * CONN_COMPLETE to learn handle->bdaddr from and webOS's HCIGETCONNLIST returns
 * an EMPTY list (the vendor stack keeps connections out of the kernel's
 * accounting), so every link stayed fail-closed and the app ran the whole
 * session on the slow hidraw fallback ("restart latency"). The app now
 * DUAL-SENDS while a pad is not ready: each report goes to hidraw (seeding, as
 * before) AND as a tagged datagram asserting "these exact bytes belong to
 * bdaddr X". When the capture thread then sees a DS5 HID-output (0xA2 +
 * 0x31/0x32/0x36 — content-proven DualSense) on an UNKNOWN handle whose bytes
 * exactly match a recent assertion (incl. the app's CRC32 tail + seq nibble),
 * that handle is bound to the asserted address. The sender is SO_PEERCRED-gated
 * to the jail uid, and identical bytes asserted for TWO different addresses are
 * ambiguous -> no learn (wait for diverging content). Defences (a)-(e) hold: we
 * still never inject onto a handle we cannot tie to a specific DualSense.
 * Ring is g_lock-guarded (written by the inject thread, matched by capture). */
#define ASSERT_RING   16
#define ASSERT_TTL_MS 2000
#define ASSERT_MAX    1024   /* must fit a full 0x36 audio report (app caches up to
                              * 1024): in-session the ONLY reliably on-air seeds are
                              * the audio frames — 0x31 rumble is usually dedup'd
                              * away by the app — so audio must be assertable or a
                              * restart mid-session never learns (found live: 256
                              * skipped every 0x36 -> app stuck seeding forever). */
static struct { uint8_t addr[6]; uint16_t len; uint64_t ts; uint8_t buf[ASSERT_MAX]; } g_assert[ASSERT_RING];
static int g_assert_next = 0;          /* round-robin overwrite slot (g_lock) */

/* Match an on-air report against the live assertions (g_lock held). Returns the
 * entry index, -1 = none, -2 = ambiguous (same bytes asserted for two addrs). */
static int assert_match(const uint8_t *rep, int rl){
    int am=-1; uint64_t t=now_ms();
    for(int i=0;i<ASSERT_RING;i++){
        if(!g_assert[i].ts || t-g_assert[i].ts>ASSERT_TTL_MS) continue;
        if(g_assert[i].len!=(uint16_t)rl || memcmp(g_assert[i].buf,rep,(size_t)rl)!=0) continue;
        if(am>=0 && memcmp(g_assert[am].addr,g_assert[i].addr,6)!=0) return -2;
        if(am<0) am=i;
    }
    return am;
}

/* A single content match is not enough to bind an assert-derived identity: with two
 * pads seeding post-restart, pad Y's on-air frame can transiently match pad X's live
 * assertion when Y's OWN assertion is momentarily absent from the ring (best-effort,
 * EAGAIN-dropped, or evicted at ASSERT_RING=16). Binding on that one collision routes
 * X's INJECT datagrams onto Y's handle AND locks X out for the whole session (X's
 * address is then "already bound" -> assert_match ambiguity forever). Require the SAME
 * (handle -> asserted address) to recur across ASSERT_CONFIRM on-air frames: a genuine
 * pad, whose reports are asserted continuously, confirms within one extra frame (~10ms),
 * while a spurious match would need two aligned content+seq collisions in a row on one
 * handle. Capture-thread-only state (touched under g_lock in the unknown-identity path);
 * MAX_LINKS+1 candidate slots cover every simultaneously-seeding pad with a spare. */
#define ASSERT_CONFIRM 2
static struct { uint16_t hh; uint8_t addr[6]; uint8_t cnt; uint64_t ts; } g_acand[MAX_LINKS+1];
static int assert_confirm(uint16_t hh, const uint8_t addr[6]){
    uint64_t t=now_ms(); int slot=-1, oldest=0;
    for(int i=0;i<(int)(sizeof g_acand/sizeof g_acand[0]);i++){
        if(g_acand[i].ts && t-g_acand[i].ts<=ASSERT_TTL_MS && g_acand[i].hh==hh){ slot=i; break; }
        if(!g_acand[i].ts || g_acand[i].ts<g_acand[oldest].ts) oldest=i;
    }
    if(slot<0){ slot=oldest; g_acand[slot].hh=hh; g_acand[slot].cnt=0; memcpy(g_acand[slot].addr,addr,6); }
    if(memcmp(g_acand[slot].addr,addr,6)!=0){ memcpy(g_acand[slot].addr,addr,6); g_acand[slot].cnt=0; }
    g_acand[slot].ts=t;
    if(g_acand[slot].cnt<255) g_acand[slot].cnt++;
    return g_acand[slot].cnt>=ASSERT_CONFIRM;
}

/* ---- link lookup helpers (all assume g_lock held) -------------------------- */
static struct ds5_link *link_by_handle(uint16_t hh){
    for(int i=0;i<MAX_LINKS;i++)
        if(g_links[i].have && g_links[i].handle==hh) return &g_links[i];
    return NULL;
}
static struct ds5_link *link_by_addr(const uint8_t addr[6]){
    for(int i=0;i<MAX_LINKS;i++)
        if(g_links[i].have && g_links[i].bound_known &&
           memcmp(g_links[i].bound_addr,addr,6)==0) return &g_links[i];
    return NULL;
}
static struct ds5_link *free_slot(void){
    /* Prefer a virgin slot: an idle-invalidated slot still carries another
     * pad's sticky ever_bound identity, and overwriting it while a never-bound
     * slot sits free silently disables that pad's idle sniff-pin (the pin loop
     * keys on ever_bound + bound_addr) and defeats slot_for_addr's stickiness
     * when the pad returns. */
    for(int i=0;i<MAX_LINKS;i++) if(!g_links[i].have && !g_links[i].ever_bound) return &g_links[i];
    for(int i=0;i<MAX_LINKS;i++) if(!g_links[i].have) return &g_links[i];
    return NULL;
}
/* Bind-path slot choice: the live link for addr, else the slot that LAST held
 * this addr (keeps pad<->slot identity stable across flaps, so per-binding
 * telemetry and the idle sniff-pin stay attached to the right pad instead of
 * churning when free_slot() hands a returning pad its neighbour's old slot),
 * else any free slot. */
static struct ds5_link *slot_for_addr(const uint8_t addr[6]){
    struct ds5_link *L=link_by_addr(addr);
    if(L) return L;
    for(int i=0;i<MAX_LINKS;i++)
        if(!g_links[i].have && g_links[i].ever_bound &&
           memcmp(g_links[i].bound_addr,addr,6)==0) return &g_links[i];
    return free_slot();
}
static int any_link_bound_locked(void){
    for(int i=0;i<MAX_LINKS;i++) if(g_links[i].have) return 1;
    return 0;
}
/* The ONE bound link, or NULL when zero or more than one is bound. "Primary" used
 * to be encoded as the slot INDEX g_links[0], which is not an identity: free_slot()
 * prefers a virgin slot, so a second pad binds g_links[1], and slot_for_addr() keeps
 * a returning pad on whatever slot it had — a single bound pad is routinely NOT slot
 * 0. Both the untagged inject path and the base readiness record resolve through
 * here instead, so they fail CLOSED on ambiguity and otherwise name a real pad
 * (whose address the inject path then passes as `expect`).
 *
 * `nbound` (may be NULL) receives how many were bound, so a caller can tell "no pad
 * yet" (normal: the app is still hidraw-seeding) from "two pads, refusing" — the
 * two fail identically here but mean very different things in the drop tally. */
static struct ds5_link *sole_bound_link_locked(int *nbound){
    struct ds5_link *only=NULL; int n=0;
    for(int i=0;i<MAX_LINKS;i++)
        if(g_links[i].have){ n++; only=&g_links[i]; }
    if(nbound) *nbound=n;
    return n==1 ? only : NULL;   /* >1 is ambiguous: an untagged sender cannot say which pad */
}

/* Bind (or rebind) link L to handle hh with the just-seen ACL header. Caller holds
 * g_lock and has verified g_htab[hh].known. A rebind (same handle, new CID, or slot
 * reuse) bumps the nonce so any FIFO audio held under the prior generation is
 * dropped, and resets the credit window so we never inherit phantom credits (or
 * per-type counts) from a flapped connection whose NOCPs were lost. */
static void link_bind(struct ds5_link *L, const uint8_t hdr8[8], uint16_t hh){
    if(L->ever_bound && memcmp(L->bound_addr,g_htab[hh].addr,6)!=0){
        /* Slot changes owner: stage the outgoing pad's address so the next
         * publish_all() writes its per-address file valid=0 (see prev_addr). */
        memcpy(L->prev_addr,L->bound_addr,6); L->prev_valid=1;
    }
    memcpy(L->hdr,hdr8,8); L->handle=hh; memcpy(L->bound_addr,g_htab[hh].addr,6);
    L->bound_known=1; L->have=1; L->ever_bound=1; L->nonce++;
    L->assert_learned=g_htab[hh].from_assert;   /* inherit the handle's taint: a rebind
                                                 * from an assert-sourced htab entry stays
                                                 * assert_learned even across idle-flaps */
    L->unsniff_probe_h=0xFFFF;  /* a new binding re-arms the unobserved-mode probe */
    L->outstanding=0; L->last_nocp=now_ms(); L->nocp_k_ok=0; txwin_reset(L);
    L->ghost=0;                 /* a fresh connection cannot owe credits for the old one */
    L->last_seen=now_ms();
    L->session_seen=now_ms();   /* a bind only happens off on-air HID-output = a session */
}

/* L1 auto-flush state, declared here because inject_one's PB class split reads
 * it (definition and rationale live with the command senders further down).
 * Capture-thread-written, main-thread-read — same discipline as g_cmd_dead.
 *   g_lmp_nonflush      -1 unanswered / 0 controller lacks LMP bit 54 / 1 has it
 *   g_flush_confirmed_ms the timeout the controller CONFIRMED on read-back.
 *                        The split keys off THIS, never off the write, so a
 *                        controller that accepted-and-ignored the command leaves
 *                        the packet boundary flags exactly as they were. */
static volatile int g_lmp_nonflush = -1;
static volatile int g_flush_confirmed_ms = 0;
static volatile int g_flush_readback_pending = 0;

/* ---- L4: ghost in-flight accounting ---------------------------------------
 *
 * The stall backstop presumes a link's outstanding packets lost once credits
 * have stopped for STALL_RESET_MS, zeroes `outstanding` and resumes injecting.
 * That is right when the link really flapped and wrong when the controller was
 * merely slow: it still holds those packets, and their NOCPs arrive later. Each
 * such late credit then decrements an `outstanding` that no longer counts them
 * and pops a ring entry belonging to a NEWER packet — so right after every
 * blackout we believe we have more credit than we do and over-inject into a
 * controller that is still draining. That transient burst is the mechanism that
 * seeds the follower stalls in a cluster.
 *
 * Ghost accounting closes it: the written-off count is remembered, and the
 * credits that arrive afterwards pay the ghosts down FIRST. Only what is left
 * over frees real credit.
 *
 * The TTL is not a tuning knob, it is a safety bound, and both directions are
 * failure modes. Too short and late credits go back to freeing new packets —
 * the bug we are fixing. Unbounded and a ghost that genuinely never returns
 * absorbs credits forever, permanently shrinking the window of a daemon that
 * never restarts, until injection stops entirely: session-fatal. So ghosts
 * expire, comfortably above STALL_RESET_MS (the window that justified writing
 * them off) and comfortably above the observed blackout tail (p90 ~500ms,
 * longest real episode seen 2.2s).
 *
 * Default OFF: this changes injection behaviour after every blackout, and it
 * ships untested. Arm with `echo 1000 > /tmp/ds5_ghost_ttl_ms` (root-owned).
 * Refreshed by the capture thread only (function-static cache); inject_one and
 * the NOCP handler read the published snapshot. */
#define GHOST_TTL_MIN_MS  200    /* must exceed STALL_RESET_MS to be worth anything */
#define GHOST_TTL_MAX_MS 5000
static volatile int g_ghost_ttl_ms = 0;
/* Published copy of the operator toggle. flush_ms_want() keeps function-static
 * cache state and is therefore CAPTURE-THREAD-ONLY; the inject thread's 10s
 * ledger line reads this snapshot instead of calling it, so the two threads
 * never race on those statics. */
static volatile int g_flush_want = 0;

/* Inject one output report as a raw-ACL frame onto link L under g_lock, honoring
 * the credit window. Critical section identical in scope to the legacy inline path
 * (one bounded, non-blocking write; TOCTOU handle re-check under the same lock).
 *
 * The window is TYPE-AWARE, derived from rep[0]:
 *   audio (0x36):        may fill maxq-1 credits. The last credit is RESERVED so
 *                        a fresh rumble frame always finds room the moment none
 *                        of its own are in flight — without the reserve, the
 *                        ~94/s audio stream keeps the window pinned at full
 *                        under contention and a rumble state change (e.g. the
 *                        OFF after a burst) is dropped for as long as the
 *                        contention lasts: the pad keeps buzzing with the last
 *                        applied intensity.
 *   rumble (0x31/0x32):  bounded by the full window AND by its OWN in-flight
 *                        count (rumble_fly) at maxq/2 — counting rumble by
 *                        its own occupancy, not the total, is what makes the
 *                        "rumble may only fill half the window" contract real.
 *
 * Stall backstop: window full with no NOCP for STALL_RESET_MS means the credits
 * are presumed lost (a flap ate the NOCPs) -> resync to empty AND RE-ARM
 * last_nocp. Without the re-arm a long radio blackout (NOCPs delayed, not
 * lost) keeps the stall condition true, so the window would reset every time it
 * refills (~130ms at audio rate) — i.e. unbounded injection into the controller
 * TX queue for the whole blackout, exactly the input-poll starvation the window
 * exists to prevent. Any BLOCKED caller may resync — audio at a full window,
 * rumble also when pinned at its own rcap: the stale-NOCP condition already
 * proves the whole LINK's credits stopped (were audio still flowing, its NOCPs
 * would keep last_nocp fresh), so this never zeroes accounting that is actually
 * live. Without the rumble trigger a rumble-only session (no 0x36 stream to hit
 * the window-full path) stayed wedged after a NOCP loss until the 1.5s idle
 * backstop rebound the link.
 *
 * `expect` is the target bdaddr the caller ROUTED to. The link is resolved by
 * address, then g_lock is dropped and re-taken here, so a capture-thread rebind of
 * this slot to a DIFFERENT DualSense could slip into that window; re-checking
 * L->bound_addr against `expect` under THIS lock closes it — a mismatch means the
 * slot moved, so we skip (return -2) rather than buzz the wrong pad. NULL still
 * skips the check, but NO caller passes it any more: the untagged path used to,
 * and that is precisely how its datagrams landed on a foreign pad, so it now
 * resolves the single bound link and passes THAT link's address.
 *
 * `from_app` separates APP-driven traffic (unix-socket datagrams, via
 * process_report/drain_fifo) from the daemon's OWN idle-lightbar paints: only
 * the former stamps session_seen. The painter's keep-alive period equals
 * SESSION_IDLE_MS, so letting its paints count as session output made an idle
 * pad's keep-alives self-refresh the session forever — page scan stayed
 * suppressed (or flip-flopped) while a connected-but-unused pad sat there,
 * exactly the stuck-scan regression session_seen exists to fix.
 *
 * Returns:  1 = injected (L->outstanding++),
 *           0 = credit window full (caller may queue or drop),
 *          -1 = template invalidated (foreign handle / EBADF; *reason set, caller
 *               must publish_all() + log),
 *          -2 = no live template (L->have==0) or the routed slot was rebound. */
static int inject_one(struct ds5_link *L, int rawfd, const uint8_t *rep, int n, int maxq, const char **reason, const uint8_t *expect, int from_app){
    uint8_t frame[1+8+1+ACL_MAX_REPORT];
    int r=-2;
    int is_rumble=!is_audio_report(rep[0]);
    int lim  = is_rumble ? maxq : (maxq>1 ? maxq-1 : 1);   /* audio leaves 1 credit reserved */
    int rcap = (maxq/2)>0 ? (maxq/2) : 1;
    pthread_mutex_lock(&g_lock);
    if(L->have && (!expect || memcmp(L->bound_addr,expect,6)==0)){
        if(from_app)
            L->session_seen=now_ms();   /* the app is driving this live link: session traffic
                                         * (stamped even when the credit window blocks us —
                                         * congestion is not the end of a session). Painter
                                         * paints (from_app==0) are NOT a session — see above. */
        uint16_t hh=(uint16_t)((L->hdr[0]|(L->hdr[1]<<8))&0x0fff);
        if(g_htab[hh].known && memcmp(g_htab[hh].addr,L->bound_addr,6)!=0){
            L->have=0; *reason="bound handle now foreign -> template INVALID"; r=-1;
        } else {
            /* Synthetic hold (bench instrument): present exactly as a full credit
             * window so everything downstream — FIFO hold, evict-to-fit, rumble
             * latest-wins — runs its REAL path. But skip the stall backstop: the
             * credits are not actually lost, and resyncing `outstanding` mid-hold
             * would corrupt the very accounting the rig measures against. */
            int held = gap_hold_active();
            int blocked = held || L->outstanding>=lim || (is_rumble && L->rumble_fly>=rcap);
            if(!held && blocked && L->last_nocp && now_ms()-L->last_nocp>STALL_RESET_MS){
                /* Remember what we are abandoning, so the credits for these
                 * packets cannot later be spent on newer ones. Capped at the
                 * ring size: more cannot physically be in flight, and an
                 * uncapped accumulation across repeated write-offs during one
                 * long blackout would absorb credits well past the real debt. */
                if(g_ghost_ttl_ms>0 && L->outstanding>0){
                    int g=L->ghost+L->outstanding;
                    L->ghost = g>TXRING ? TXRING : g;
                    L->ghost_ts=now_ms();
                }
                L->outstanding=0; txwin_reset(L);   /* credits presumed lost -> resync */
                L->last_nocp=now_ms();              /* re-arm: at most one resync per STALL_RESET_MS */
                L->nocp_k_ok=0;                     /* monotonic re-arm has no kernel twin: the
                                                     * next NOCP restarts the kernel-stamp chain
                                                     * instead of measuring across the write-off */
                blocked=0;
            }
            if(blocked){
                r=0;
            } else {
                uint16_t l2=(uint16_t)(1+n), acl=(uint16_t)(4+l2);
                frame[0]=HCI_ACLDATA_PKT; memcpy(frame+1,L->hdr,8);
                frame[3]=(uint8_t)(acl&0xff); frame[4]=(uint8_t)(acl>>8);
                frame[5]=(uint8_t)(l2&0xff);  frame[6]=(uint8_t)(l2>>8);
                /* L1 PB class split. Only once the controller has CONFIRMED a
                 * finite flush timeout by read-back, and only then, do the
                 * packet boundary flags start meaning anything: audio stays
                 * PB=2 (automatically flushable — stale audio is worthless,
                 * dropping it beats replaying it late), while 0x31/0x32 control
                 * go out PB=0 (non-flushable) because a flushed SetState can
                 * leave the pad routing audio to the wrong sink for ~1s.
                 * Untouched while disarmed: the template's own flags survive
                 * byte-for-byte, so the off state is the pre-L1 behaviour and
                 * not merely a similar one. */
                if(g_flush_confirmed_ms>0)
                    frame[2] = (uint8_t)((frame[2]&0xCF) | (is_audio_report(rep[0]) ? (2<<4) : (0<<4)));
                frame[9]=0xA2; memcpy(frame+10,rep,n);
                ssize_t wr=write(rawfd,frame,10+n);
                if(wr==(ssize_t)(10+n)){ r=1; L->outstanding++; txwin_push(L,is_rumble); }
                else if(wr<0 && errno==EBADF){ L->have=0; *reason="inject EBADF -> template INVALID (reconnect)"; r=-1; }
                else {
                    /* A short/failed write is otherwise indistinguishable from
                     * credit-window pacing (both r=0). EAGAIN/ENOBUFS are the
                     * expected transient pair; anything else (notably EMSGSIZE
                     * or EINVAL from an oversized frame) is a structural reject
                     * and must not hide behind the drop counter — the 0x39
                     * variant is 552 B of ACL data where 0x36 is 403, so a size
                     * ceiling would present exactly as "silent audio". Rate-
                     * limited to one line per second per link. */
                    if(wr<0 && errno!=EAGAIN && errno!=ENOBUFS && errno!=EINTR){
                        uint64_t t=now_ms();
                        if(t-L->last_wr_err_log>1000){
                            L->last_wr_err_log=t;
                            fprintf(stderr,"[txd] inject write failed: id=0x%02x len=%d errno=%d (%s)\n",
                                    rep[0], 10+n, errno, strerror(errno));
                        }
                    }
                    r=0;
                }
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* Audio-only elastic FIFO drain (single-threaded per link: touched only by the main
 * poll loop). fifo_gen stamps the backlog with the template nonce it was queued
 * under, so frames held across an invalidate->rebind (new session, new nonce) are
 * recognized as stale and dropped instead of being injected as a burst of
 * last-session audio at the head of the new one. */
static void fifo_clear(struct ds5_link *L){ L->fifo_head=0; L->fifo_count=0; }

/* Drain a link's audio backlog oldest-first while credits allow (main thread only).
 * Runs even when the FIFO tunable is 0 so disabling it flushes a residual
 * backlog instead of stranding it. Returns the number injected; on template
 * invalidation sets need_inval + reason (caller publishes) and drops the stale
 * backlog. */
#define FIFO_MAX_AGE_MS 150   /* > the ~107ms a full 16-deep FIFO represents, so the
                               * age gate never fires during a normal congestion drain —
                               * only on backlog stranded across a stream PAUSE (drain
                               * runs on datagram arrival; no arrivals = frames sit),
                               * which would otherwise replay as stale audio at the
                               * head of the resumed stream */
static long drain_fifo(struct ds5_link *L, int rawfd, int maxq, int *need_inval, const char **reason, const uint8_t *expect){
    long inj=0;
    while(L->fifo_count>0){
        int idx=L->fifo_head;
        if(now_ms()-L->fifo[idx].ts>FIFO_MAX_AGE_MS){
            L->fifo_head=(L->fifo_head+1)%FIFO_MAX; L->fifo_count--;
            L->drop_total++; L->drop_age++;         /* a real loss (host servo must see it) but an
                                                     * INTENDED one — split so the servo, and any
                                                     * maxq ladder's abort criterion, can tell it
                                                     * apart from overflow (see drop_age/drop_ovf) */
            continue;                               /* stale pre-pause audio: drop */
        }
        int r=inject_one(L,rawfd,L->fifo[idx].buf,L->fifo[idx].len,maxq,reason,expect,1 /* FIFO holds app audio */);
        if(r==1){ L->fifo_head=(L->fifo_head+1)%FIFO_MAX; L->fifo_count--; inj++; L->inj_total++; continue; }
        if(r==-1){ *need_inval=1; fifo_clear(L); }  /* template gone -> drop stale audio */
        else if(r==-2) fifo_clear(L);               /* no template: backlog is already stale */
        break;                                      /* credits full (0) -> retry next wakeup */
    }
    return inj;
}
static int      g_rawfd = -1;          /* main's raw HCI socket, shared for link-policy writes */

/* NOCP -> main-loop wakeup (2026-08-15). drain_fifo() is main-thread-only by
 * design (it owns the FIFO and the fifo_gen rebind check), and it used to run
 * ONLY from process_report — i.e. on the next datagram from the app. So a credit
 * freed by the controller mid-period left a held frame sitting until the next
 * report arrived: a systematic 0..21.33ms (mean ~10.7) tax on every congestion
 * recovery, paid exactly at the B+21.33ms underrun boundary where it decides
 * whether the recovery is audible. The capture thread only WRITES this eventfd
 * (never touches the FIFO), the main loop drains as before, so the ownership
 * rules and the rebind guard are unchanged. The datagram-clocked drain stays in
 * place as belt-and-braces: a lost kick can only cost the old latency, never
 * strand a frame. */
static int      g_kickfd = -1;

/* HCI command health guard + rate discipline (2026-07-05, reworked 07-06).
 * PROVEN on-air (raw-write + monitor-listen probe): the LG vendor stack holds
 * the adapter in user-channel mode — the controller answers every command
 * fine, but the responses bypass the kernel's command tracking. So EVERY
 * command we send via the kernel path occupies a 2s "command tx timeout"
 * slot; the kernel drains OUR queue at a fixed 1 command / 2s (0.5/s). Send
 * faster than that for long enough and the socket sndbuf overflows (~30min at
 * the old 1Hz scan war) -> permanent EAGAIN and our scan-offs arrive minutes
 * late (the night's "firmware wedge" + "dropouts got worse", both really this
 * arithmetic). The command path is SHARED across links (one adapter). Rules:
 *  (a) steady-state command rate stays well under the 0.5/s drain (10s
 *      link-policy refresh PER LINK + >=3s-ratelimited scan sends <= 0.43/s
 *      typical for 2 links); BURSTS (template flap = invalidate+rebind = up to 3
 *      commands) are absorbed by a hard unacked-queue cap (CMD_PEND_MAX): once
 *      that many commands await their Command Complete, further sends are refused
 *      and the reconcilers simply retry later — the sndbuf can never build more
 *      than ~CMD_PEND_MAX*2s of backlog no matter how hard the session flaps.
 *  (b) every sent command is queued with its timestamp and watched for a
 *      Command Complete on the MONITOR socket (sees responses even in
 *      user-channel mode). The no-response deadline SCALES with the queue
 *      depth we created ourselves — N unacked commands legitimately take
 *      ~2N s to drain, so a fixed deadline false-tripped on a mere flap
 *      burst — miss = cease commands and enter the DEAD state;
 *  (c) EAGAIN on a command write (= sndbuf already full) trips immediately;
 *  (d) DEAD is no longer terminal: every 60s ONE probe command is sent past
 *      the gate; a monitor-observed Command Complete for it clears the state
 *      (logged CMDRECOVER). A foreign completion of the same opcode inside
 *      the probe window can recover us spuriously — harmless, the guard
 *      re-trips on the next unanswered command if the path is really dead.
 * Raw-ACL injection bypasses the cmd queue and is unaffected.
 * All fields are capture-thread-only, no locking needed. */
static volatile int g_cmd_dead = 0;
static uint64_t g_cmd_dead_t = 0;    /* trip time; schedules the 60s recovery probes */
static uint16_t g_probe_op   = 0;    /* recovery probe awaiting Command Complete */
static volatile long g_scan_ctr = 0; /* foreign scan re-enables countered (fight-rate telemetry) */
#define CMD_PEND_MAX 8
static struct { uint16_t op; uint16_t arg; uint64_t deadline; } g_pend[CMD_PEND_MAX];
/* arg: command-specific context carried to the response handler. Used by
 * OP_EXIT_SNIFF (the target ACL handle) so a Command Status answer can repair
 * g_htab[].mode — Command Status carries no handle of its own. */
static int g_pend_n = 0;             /* commands written but with no Command Complete seen yet */
static void cmd_guard_trip(const char *why, unsigned detail){
    if(g_cmd_dead) return;
    g_cmd_dead=1; g_cmd_dead_t=now_ms(); g_pend_n=0;
    fprintf(stderr,"[txd] HCI COMMAND PATH DEAD (%s 0x%04x) -> ceasing HCI commands "
                   "(scan/sniff mgmt paused; ACL inject unaffected; recovery probe every 60s).\n",
            why,detail);
}
/* Single choke point for HCI commands: dead-gate, unacked-queue cap, EAGAIN
 * trip, pend bookkeeping. force=1 is reserved for the recovery probe.
 * Returns 0 = written (pend entry queued), -1 = not sent (caller retries). */
static int cmd_send(const uint8_t *cmd, size_t len, uint16_t op, uint16_t arg, int force){
    if(g_rawfd<0) return -1;
    if(g_cmd_dead && !force) return -1;
    if(g_pend_n>=CMD_PEND_MAX) return -1;          /* >=16s of drain already queued: refuse */
    if(write(g_rawfd,cmd,len)!=(ssize_t)len){
        if(errno==EAGAIN||errno==EWOULDBLOCK) cmd_guard_trip("write EAGAIN",op);
        else fprintf(stderr,"[txd] hci cmd 0x%04x write failed: %s\n",op,strerror(errno));
        return -1;
    }
    /* The no-response deadline is fixed at ENQUEUE from the entry's queue
     * position: with N entries already ahead, the kernel needs ~2s each before
     * this one even goes on air. Scaling by the CURRENT depth instead would
     * shrink the allowance as earlier entries complete while the survivor's age
     * keeps growing — the tail of a full burst would false-trip. */
    g_pend[g_pend_n].op=op;
    g_pend[g_pend_n].arg=arg;
    g_pend[g_pend_n].deadline=now_ms()+6000ull+2000ull*(uint64_t)(g_pend_n+1);
    g_pend_n++;
    return 0;
}

/* Disable sniff/hold/park on a DS5 ACL link (keep role-switch). Measured live
 * 2026-07-02: the stack lets the link fall into SNIFF (interval 124 slots =
 * 77.5 ms) every ~30 s; input collapses to ~13 Hz for ~0.5 s until the app's
 * 500 ms stopSniff poll recovers it — the user-visible input dropouts. Clearing
 * the sniff policy bit makes the LMP layer reject sniff requests from EITHER
 * side, so the mode never changes. Per-packet write on a SOCK_RAW datagram
 * socket is atomic, so racing with main's ACL injects is safe. Pinned per link. */
#define OP_WRITE_LINK_POLICY 0x080d
#define LINK_POLICY_ACTIVE   0x0001    /* role switch only; sniff/hold/park off */
static int send_link_policy(uint16_t handle){
    uint8_t cmd[8]={ 0x01 /*HCI_COMMAND_PKT*/,
        OP_WRITE_LINK_POLICY&0xff, OP_WRITE_LINK_POLICY>>8, 4,
        (uint8_t)(handle&0xff), (uint8_t)((handle>>8)&0x0f),
        LINK_POLICY_ACTIVE&0xff, LINK_POLICY_ACTIVE>>8 };
    return cmd_send(cmd,sizeof cmd,OP_WRITE_LINK_POLICY,handle,0);
}

/* Exit an ESTABLISHED sniff mode. Write_Link_Policy only rejects FUTURE mode
 * requests — a link already sitting in sniff when the pin lands (a pad that
 * connected before the session and idled >30s) stays in sniff and keeps taxing
 * the active pad's airtime. Proven live 2026-07-10: Exit_Sniff on an active-mode
 * link answers Command Status 0x0c (disallowed), on a sniffed one it exits and
 * emits a Mode Change we track in g_htab[].mode. NOTE Exit_Sniff_Mode is
 * answered by Command STATUS (not Complete) — handle_hci_event pops pend on
 * both, or the cmdguard would false-trip DEAD on every exit. */
#define OP_EXIT_SNIFF 0x0804
static int send_exit_sniff(uint16_t handle){
    uint8_t cmd[6]={ 0x01,
        OP_EXIT_SNIFF&0xff, OP_EXIT_SNIFF>>8, 2,
        (uint8_t)(handle&0xff), (uint8_t)((handle>>8)&0x0f) };
    return cmd_send(cmd,sizeof cmd,OP_EXIT_SNIFF,handle,0);
}

/* ---- L1: let the CONTROLLER discard stale audio -----------------------------
 *
 * We already mark audio ACL frames PB=2 ("first fragment, automatically
 * flushable"), but that flag is inert: the default Automatic Flush Timeout is
 * infinite, so nothing is ever flushed. A frame queued in the controller behind
 * a blackout is therefore delivered LATE no matter how stale it has become —
 * and by the depth conservation law that late delivery is not a lost frame, it
 * is a permanent addition to the pad's standing buffer depth.
 *
 * Setting a finite timeout changes what a blackout costs. Stale audio is
 * dropped by the controller instead of replayed late, and — this is the part
 * that makes it worth a command — the spec requires flushed packets to be
 * credited back via Number_Of_Completed_Packets. Credits therefore return
 * DURING the blackout rather than after it, which also removes the
 * over-injection that follows a txwin_reset.
 *
 * Value 80ms, not the 30-40 the research suggested: at 30 the ~300/min benign
 * 30-49ms gaps would start flushing normal audio and collapse the buffer inside
 * a minute. 80 only reaches frames that were going to underrun anyway at B=60
 * (a gap must exceed B + one frame period).
 *
 * TWO HARD PRECONDITIONS, both enforced below rather than assumed:
 *
 *  1. The PB class split must be possible. A flush timeout is per-CONNECTION,
 *     not per-packet, so it would also eat the ~1/s 0x31 SetState — and a
 *     dropped SetState can leave the pad routing audio to the wrong sink for up
 *     to a second. The split (audio flushable, control non-flushable) needs the
 *     controller to support the non-flushable packet boundary flag, LMP feature
 *     bit 54. If it does not, we do NOT fall back to "flush everything": we
 *     refuse to arm at all.
 *
 *  2. The write must actually take. MediaTek controllers are known in this
 *     project to accept-and-ignore commands they do not implement (QoS_Setup
 *     did exactly that), so every write is followed by a read-back and the
 *     armed state is set from the READ, never from the write's status.
 *
 * Default OFF. Arm with `echo 80 > /tmp/ds5_flush_ms` (root-owned), disarm by
 * removing the file or writing 0 — which writes the infinite timeout back, so
 * an A/B can be reversed without restarting the daemon.
 */
#define OP_WRITE_AUTO_FLUSH    0x0c28
#define OP_READ_AUTO_FLUSH     0x0c27
#define OP_READ_LOCAL_FEATURES 0x1003
#define FLUSH_MS_MIN  50     /* below this the benign 30-49ms gap population starts
                              * flushing healthy audio -> buffer collapse (see above) */
#define FLUSH_MS_MAX 1000
#define FLUSH_SLOTS(ms) ((uint16_t)(((long)(ms)*8)/5))   /* 0.625ms units */

/* Operator toggle, cached ~1/s like every other live tunable. Out-of-range
 * values are refused loudly rather than clamped: silently flushing at a
 * different timeout than the operator asked for would corrupt an A/B.
 * CAPTURE-THREAD-ONLY (function-static cache); readers elsewhere use
 * g_flush_want. */
static int flush_ms_want(void){
    static int v=0; static uint64_t last=0; static int warned=0, badwarn=0;
    uint64_t n=now_us();
    if(last==0 || n-last>1000000ull){
        last=n;
        int r=read_root_int("/tmp/ds5_flush_ms",&warned);
        if(r<0)                    v=0;                      /* absent = off */
        else if(r==0)              v=0;                      /* explicit off */
        else if(r>=FLUSH_MS_MIN && r<=FLUSH_MS_MAX){ v=r; badwarn=0; }
        else if(!badwarn){
            badwarn=1;
            fprintf(stderr,"[txd] ignoring /tmp/ds5_flush_ms=%d: outside %d..%d ms "
                           "(below the floor a flush eats healthy audio)\n",
                    r,FLUSH_MS_MIN,FLUSH_MS_MAX);
        }
        g_flush_want=v;   /* publish for the inject thread's ledger line */
    }
    return v;
}

/* CAPTURE-THREAD-ONLY (function-static cache), publishes g_ghost_ttl_ms. */
static void ghost_ttl_refresh(void){
    static uint64_t last=0; static int warned=0, badwarn=0;
    uint64_t n=now_us();
    if(last && n-last<=1000000ull) return;
    last=n;
    int r=read_root_int("/tmp/ds5_ghost_ttl_ms",&warned);
    if(r<0 || r==0) g_ghost_ttl_ms=0;                     /* absent/0 = off */
    else if(r>=GHOST_TTL_MIN_MS && r<=GHOST_TTL_MAX_MS){ g_ghost_ttl_ms=r; badwarn=0; }
    else if(!badwarn){
        badwarn=1;
        fprintf(stderr,"[txd] ignoring /tmp/ds5_ghost_ttl_ms=%d: outside %d..%d ms "
                       "(below the floor it cannot outlive the write-off it corrects)\n",
                r,GHOST_TTL_MIN_MS,GHOST_TTL_MAX_MS);
    }
}

static int send_auto_flush(uint16_t handle, int ms){
    uint16_t slots = ms>0 ? FLUSH_SLOTS(ms) : 0;   /* 0 = no automatic flush */
    uint8_t cmd[8]={ 0x01,
        OP_WRITE_AUTO_FLUSH&0xff, OP_WRITE_AUTO_FLUSH>>8, 4,
        (uint8_t)(handle&0xff), (uint8_t)((handle>>8)&0x0f),
        (uint8_t)(slots&0xff), (uint8_t)(slots>>8) };
    return cmd_send(cmd,sizeof cmd,OP_WRITE_AUTO_FLUSH,handle,0);
}
static int send_read_auto_flush(uint16_t handle){
    uint8_t cmd[6]={ 0x01,
        OP_READ_AUTO_FLUSH&0xff, OP_READ_AUTO_FLUSH>>8, 2,
        (uint8_t)(handle&0xff), (uint8_t)((handle>>8)&0x0f) };
    return cmd_send(cmd,sizeof cmd,OP_READ_AUTO_FLUSH,handle,0);
}
static int send_read_local_features(void){
    uint8_t cmd[4]={ 0x01, OP_READ_LOCAL_FEATURES&0xff, OP_READ_LOCAL_FEATURES>>8, 0 };
    return cmd_send(cmd,sizeof cmd,OP_READ_LOCAL_FEATURES,0,0);
}

/* ---------------------------------------------------------------------------
 * L16 — air-side telemetry (read-only status reads).
 *
 * Why this exists. AMENDED 2026-08-16 late, after the gapge histogram was read
 * against a CONTINUOUS-audio workload (Ratchet's hoverboots) for the first time.
 *
 * The original motivation held that NOCP measures nothing real, because the
 * 30-79ms bands looked like BATCHED CREDIT REPORTING and the >=80ms band like
 * game-side SILENCE. That reading came from a workload with only sporadic
 * effects. Under continuous load it is wrong: a gap longer than B + 21.33ms
 * starves the pad, and the count at that edge PREDICTS THE EAR — B=40 gives
 * ~87 underruns/min (heard as dropouts), B=60 gives ~4.8 (heard as clean).
 * So NOCP does see the symptom. The buffer floor is ~58ms, set by the tail
 * between 60 and 80ms; gaps of 30-50ms really are the normal bundling cadence
 * (764/min against an injection every 21.8ms) and no buffer can go under them.
 *
 * What NOCP still cannot say is WHY that 60-80ms tail exists — and it is
 * INTERMITTENT (a whole 77s half of the same session had nothing above 80ms),
 * so there is a phase-shaped cause to find. Air retransmissions are the cheapest
 * hypothesis: they cost standing latency and add arrival jitter exactly where
 * the tail sits. These four reads are the smallest instrument that can see them.
 * Killing the tail is now the only remaining latency lever, worth ~20ms of
 * buffer if it succeeds.
 *
 * No lever is built on this yet, deliberately. The outcomes — including the one
 * that kills the hypothesis — are pre-registered in
 * workspace/ds5-linkq-preregistration-2026-08-16.md, written before this code.
 *
 * Default OFF. Arm with `echo 1000 > /tmp/ds5_linkq_ms` (root-owned).
 *
 * 🚨 The scan reconciler shares cmd_send's pending queue and the g_cmd_dead
 * guard, and scan-off is worth ~8x fewer blackouts in a live A/B. Telemetry is
 * garnish and yields to it: ONE command per tick, round-robin over the four,
 * and the tick is skipped outright while the queue is busy — so telemetry can
 * never occupy a slot a reconciler needs.
 */
#define OP_READ_LINK_QUALITY   0x1403
#define OP_READ_RSSI           0x1405
#define OP_READ_AFH_MAP        0x1406
#define OP_READ_FAILED_CONTACT 0x1408
#define LINKQ_MIN_MS   500      /* keeps the added rate under the reconcilers' 3s budget */
#define LINKQ_MAX_MS 10000
#define LINKQ_PEND_HEADROOM 3   /* of CMD_PEND_MAX=8 — leave the reconcilers their slots */

static volatile int g_linkq_ms = 0;          /* published toggle; 0 = off */
/* Samples; -1 (127 for dBm) = never seen. min/max are kept deliberately: a
 * vendor-defined link_quality that never MOVES is a MUTE INSTRUMENT, not a
 * quiet link, and reading a flat line as "no retransmissions" would repeat the
 * read-back mistake one level up. The pre-registration makes zero variance a
 * disqualifying result rather than a negative one. */
static volatile int g_lq_last=-1, g_lq_min=-1, g_lq_max=-1;
static volatile int g_rssi_last=127, g_rssi_min=127;
static volatile int g_afh_last=-1, g_afh_min=-1;
static volatile int g_fcc_base=-1, g_fcc_last=-1;
static volatile int g_linkq_off=0;           /* bitmask: opcodes that answered with an error */

/* The four reads share one shape: [handle] in, status+handle+payload out. */
static int send_status_read(uint16_t op, uint16_t handle){
    uint8_t cmd[6]={ 0x01, (uint8_t)(op&0xff), (uint8_t)(op>>8), 2,
        (uint8_t)(handle&0xff), (uint8_t)((handle>>8)&0x0f) };
    return cmd_send(cmd,sizeof cmd,op,handle,0);
}

/* CAPTURE-THREAD-ONLY (function-static cache), publishes g_linkq_ms. */
static void linkq_refresh(void){
    static uint64_t last=0; static int warned=0, badwarn=0;
    uint64_t n=now_us();
    if(last && n-last<=1000000ull) return;
    last=n;
    int r=read_root_int("/tmp/ds5_linkq_ms",&warned);
    int was=g_linkq_ms;
    if(r<0 || r==0)                              g_linkq_ms=0;
    else if(r>=LINKQ_MIN_MS && r<=LINKQ_MAX_MS){ g_linkq_ms=r; badwarn=0; }
    else if(!badwarn){
        badwarn=1;
        fprintf(stderr,"[txd] ignoring /tmp/ds5_linkq_ms=%d: outside %d..%d ms "
                       "(polling faster would compete with the scan reconciler)\n",
                r,LINKQ_MIN_MS,LINKQ_MAX_MS);
    }
    if(was && !g_linkq_ms){
        /* Clear on disarm so a second measurement never inherits the first
         * one's extremes — min/max are only meaningful within one armed run. */
        g_lq_last=g_lq_min=g_lq_max=-1; g_rssi_last=g_rssi_min=127;
        g_afh_last=g_afh_min=-1; g_fcc_base=g_fcc_last=-1; g_linkq_off=0;
        fprintf(stderr,"[txd] linkq: disarmed\n");
    } else if(!was && g_linkq_ms)
        fprintf(stderr,"[txd] linkq: armed, one read every %d ms, round-robin, read-only\n",
                g_linkq_ms);
}

/* ---------------------------------------------------------------------------
 * L18 — packet-type clamp (Change_Connection_Packet_Type, 0x040F).
 *
 * Why. 23k demand-gated NOCP gap records (sessions 08-15/08-16, out>=2) show
 * the 50-79ms band is STATIONARY (70-74/min across five arms of two nights),
 * decays smoothly (halving every ~5.4ms, no periodicity), and the pad's uplink
 * keeps flowing at baseline rate INSIDE the gaps — the piconet is alive, only
 * our downlink packet keeps failing to complete. That is the signature of air
 * retransmissions of a long fragile packet: 200-byte audio presumably rides
 * 2-DH3+ while the short uplink sails through. This band is exactly what
 * starves a 40ms pad buffer (threshold B+21.33ms), so compressing it is the
 * one remaining latency lever (~20ms of buffer if it works).
 *
 * The clamp forbids every 3-slot/5-slot type except 2-DH3 and the 1-slot
 * basics: allowed = DM1, DH1, 2-DH1, 2-DH3 (audio still fits: 2-DH3 carries
 * up to 367B). Shorter maximum packets mean a cheaper retransmit quantum and
 * fewer slots at risk per attempt. Bandwidth is not a concern at 9.2 KB/s.
 *
 * Pre-registered (workspace/ds5-l17-l18-preregistration-2026-08-16.md, written
 * before this code): the efficacy witness is the gapge DISTRIBUTION (decay
 * constant / >=60 rate under continuous load), NOT the 0x1D event — L1 taught
 * us this controller stores-and-reports settings it does not apply, so the
 * event is necessary but never sufficient. An unchanged distribution with a
 * confirming 0x1D closes L18 as accept-and-ignore.
 *
 * Default OFF. Arm with `echo 1 > /tmp/ds5_ptype` (root-owned); disarm writes
 * the full mask back so an A/B reverses without a daemon restart. Off state
 * sends nothing and is byte-identical to 1.4.29. */
#define OP_CHG_CONN_PTYPE 0x040f
#define PTYPE_ALL   0xcc18   /* DM1|DH1|DM3|DH3|DM5|DH5, all EDR allowed (stack default) */
#define PTYPE_CLAMP 0x321c   /* allow DM1|DH1|2-DH1|2-DH3; forbid 3-DH1/3-DH3/2-DH5/3-DH5;
                              * multi-slot basic-rate types not enabled */
static volatile int g_ptype_want = 0;    /* published toggle: 1 = clamp */
static volatile int g_ptype_seen = -1;   /* last mask a 0x1D event reported; -1 = never */
static volatile int g_ptype_ever = 0;    /* the toggle was armed at least once this run */
static volatile int g_ptype_refused = 0; /* controller rejected 0x040F: stop reconciling
                                          * (a 3s retry loop against a standing refusal
                                          * would eat the command budget forever) */

static int send_chg_ptype(uint16_t handle, uint16_t mask){
    uint8_t cmd[8]={ 0x01,
        OP_CHG_CONN_PTYPE&0xff, OP_CHG_CONN_PTYPE>>8, 4,
        (uint8_t)(handle&0xff), (uint8_t)((handle>>8)&0x0f),
        (uint8_t)(mask&0xff), (uint8_t)(mask>>8) };
    return cmd_send(cmd,sizeof cmd,OP_CHG_CONN_PTYPE,handle,0);
}

/* CAPTURE-THREAD-ONLY (function-static cache), publishes g_ptype_want. */
static void ptype_refresh(void){
    static uint64_t last=0; static int warned=0, badwarn=0;
    uint64_t n=now_us();
    if(last && n-last<=1000000ull) return;
    last=n;
    int r=read_root_int("/tmp/ds5_ptype",&warned);
    int was=g_ptype_want;
    if(r<0 || r==0)  g_ptype_want=0;
    else if(r==1){   g_ptype_want=1; g_ptype_ever=1; badwarn=0; }
    else if(!badwarn){
        badwarn=1;
        fprintf(stderr,"[txd] ignoring /tmp/ds5_ptype=%d: only 0/1 defined\n",r);
    }
    if(was!=g_ptype_want)
        fprintf(stderr,"[txd] L18 ptype: %s (mask 0x%04x) — efficacy is judged on the gapge "
                       "distribution, never on the 0x1D event alone\n",
                g_ptype_want?"CLAMP":"full",g_ptype_want?PTYPE_CLAMP:PTYPE_ALL);
}

/* ---------------------------------------------------------------------------
 * L17c — per-gap wall-clock log. The histogram proves THAT the tail exists;
 * correlating it with anything outside the daemon (WiFi off-channel scans in
 * `iw event -t`, coex windows) needs each gap as an EVENT with a wall-clock
 * stamp. Ring is CAPTURE-THREAD-ONLY (the NOCP handler and the flush both run
 * on the capture loop), so no locking beyond what the handler already holds.
 * Default OFF; arm with `echo 1 > /tmp/ds5_gaplog` (root-owned). 55ms floor:
 * below the audible edge at B=40 (61ms), above the bundling bulk. */
#define GAPLOG_MIN_MS 55
#define GAPLOG_RING   256
#define GAPLOG_MAX_BYTES (256*1024)
static volatile int g_gaplog_want=0;
static struct { uint64_t wall_ms; unsigned gap; int out; uint16_t h; }
    g_gaplog_ring[GAPLOG_RING];
static int  g_gaplog_n=0;                /* capture-thread-only */
static long g_gaplog_written=0, g_gaplog_lost=0;
/* Gap intervals binned WITHOUT a kernel stamp (SO_TIMESTAMP cmsg missing on the
 * monitor packet -> old monotonic clock used for that one interval). The clock
 * probe saw 0 missing stamps in 11448 intervals, so a nonzero here is an
 * instrument-health witness, not an expected code path. Capture-thread-only. */
static long g_ts_fallback=0;

static uint64_t now_wall_ms(void){
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    return (uint64_t)ts.tv_sec*1000ull+ts.tv_nsec/1000000ull;
}

/* CAPTURE-THREAD-ONLY (function-static cache), publishes g_gaplog_want. */
static void gaplog_refresh(void){
    static uint64_t last=0; static int warned=0;
    uint64_t n=now_us();
    if(last && n-last<=1000000ull) return;
    last=n;
    int r=read_root_int("/tmp/ds5_gaplog",&warned);
    int was=g_gaplog_want;
    /* 1 = armed at the historical default (55 ms). 20..500 = armed with THAT
     * threshold: the r36 arm's whole story happens below 55 ms (its per-report
     * cadence is 10.67 ms, so B+cadence thresholds start at ~45+10.67), and a
     * fixed compile-time floor left that region censored on 2026-08-17. The
     * floor is part of the record format contract, so a run's chosen value is
     * printed on arming and the analysis must slice with the same one. */
    g_gaplog_want = (r==1) ? GAPLOG_MIN_MS : (r>=20 && r<=500) ? r : 0;
    if(was!=g_gaplog_want && g_gaplog_want)
        fprintf(stderr,"[txd] gaplog: armed (/tmp/ds5_gaps.log, >=%dms, wall-clock ms)\n",
                g_gaplog_want);
    else if(was!=g_gaplog_want)
        fprintf(stderr,"[txd] gaplog: disarmed\n");
}

/* Drain the ring to /tmp/ds5_gaps.log. O_NOFOLLOW for the same reason as
 * write_record_atomic (root writing into the jail-shared tmp); a jail-planted
 * REGULAR file only pollutes telemetry we alone consume. Size-capped with a
 * loud truncation marker — silent truncation would read as "no gaps". */
static void gaplog_flush(void){
    if(g_gaplog_n==0) return;
    int fd=open("/tmp/ds5_gaps.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0644);
    if(fd<0){ g_gaplog_lost+=g_gaplog_n; g_gaplog_n=0; return; }
    struct stat st;
    if(fstat(fd,&st)==0 && st.st_size>GAPLOG_MAX_BYTES){
        close(fd);
        fd=open("/tmp/ds5_gaps.log",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0644);
        if(fd<0){ g_gaplog_lost+=g_gaplog_n; g_gaplog_n=0; return; }
        static const char m[]="G TRUNCATED older-entries-dropped\n";
        if(write(fd,m,sizeof m-1)<0){ /* the entries below still tell the story */ }
    }
    char buf[GAPLOG_RING*48]; int o=0;
    for(int i=0;i<g_gaplog_n && o<(int)sizeof buf-48;i++)
        o+=snprintf(buf+o,sizeof buf-o,"G %llu gap=%u out=%d h=0x%03x\n",
                    (unsigned long long)g_gaplog_ring[i].wall_ms,
                    g_gaplog_ring[i].gap,g_gaplog_ring[i].out,g_gaplog_ring[i].h);
    if(write(fd,buf,(size_t)o)==(ssize_t)o) g_gaplog_written+=g_gaplog_n;
    else g_gaplog_lost+=g_gaplog_n;
    close(fd);
    g_gaplog_n=0;
}

/* BR/EDR scan control (Write_Scan_Enable). Measured live 2026-07-05: the
 * controller's periodic page/inquiry scan blocks the DS5 ACL link for
 * 86-234ms on a ~1.28s grid (EPISODE detector) — audible speaker dropouts and
 * the input hiccups. Scan-off while a template is bound removed ~8x of the
 * blackouts and made input clean in the live A/B. Scan mode 2 (connectable,
 * not discoverable) is restored when NO link is bound so devices can (re)connect
 * outside sessions. Scan is a SHARED radio property: off while ANY link is bound.
 *
 * Implemented as a WANT/SENT reconciler instead of scattered direct sends:
 * every state change (bind, invalidate — from ANY thread via g_scan_restore —
 * or a foreign re-enable observed on the monitor) only updates the DESIRED
 * mode; the capture loop converges the actual mode toward it under one shared
 * >=3s limiter. This makes flap storms rate-safe by construction (a swallowed
 * transition is re-sent on the next pass, so the final state is always
 * reached) and keeps the whole command machinery capture-thread-only. */
#define OP_WRITE_SCAN_ENABLE 0x0c1a
#define SCAN_MIN_GAP_MS      3000
static uint8_t  g_scan_want = 0xff;  /* desired mode; 0xff = no opinion yet (pre-first-bind:
                                        never touch the TV's scan state before a session) */
static uint8_t  g_scan_sent = 0xff;  /* mode we last wrote; 0xff = unknown (force re-send) */
static uint64_t g_scan_tx   = 0;     /* last scan write time (the shared limiter) */
static volatile int g_scan_restore = 0; /* main-thread invalidations request a mode-2 restore
                                           here; the capture thread owns the send machinery */
static void scan_reconcile(void){
    if(g_scan_want==0xff || g_scan_want==g_scan_sent) return;
    uint64_t t=now_ms();
    if(g_scan_tx && t-g_scan_tx<SCAN_MIN_GAP_MS) return;
    uint8_t cmd[5]={ 0x01,
        OP_WRITE_SCAN_ENABLE&0xff, OP_WRITE_SCAN_ENABLE>>8, 1, g_scan_want };
    if(cmd_send(cmd,sizeof cmd,OP_WRITE_SCAN_ENABLE,0,0)==0){
        g_scan_tx=t; g_scan_sent=g_scan_want;
        fprintf(stderr,"[txd] scan_enable=%u (%s)\n",g_scan_want,
                g_scan_want?"restored":"off for session");
    }
}

/* Write `len` bytes to `path` atomically (temp+rename), never following a symlink.
 *
 * We are ROOT writing into the JAILED app's own tmp (the jail uid creates files there
 * too) under fully predictable names, so the temp name must be treated as hostile:
 * O_CREAT|O_EXCL|O_NOFOLLOW refuses to open anything we did not just create, and the
 * unlink() first is what makes O_EXCL usable (a planted symlink at "<path>.tmp"
 * survives the rename(), which only ever replaces `path`, so without the unlink the
 * clobber primitive would repeat on every publish). The mode comes from open() alone
 * — no fchmod, which used to chmod whatever the symlink pointed AT; main() pins the
 * umask so 0644 is what actually lands (the jail must be able to read these).
 * Failures stay silent by design: a publish must never abort the daemon. */
static void write_record_atomic(const char *path, const uint8_t *rec, size_t len){
    char tmp[700];
    if(snprintf(tmp,sizeof tmp,"%s.tmp",path)>=(int)sizeof tmp) return;   /* never write a truncated path */
    unlink(tmp);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0644);
    if(fd<0) return;
    ssize_t w=write(fd,rec,len);
    close(fd);
    if(w==(ssize_t)len) { if(rename(tmp,path)<0) unlink(tmp); } else unlink(tmp);
}

/* Write the 16-byte readiness/template record atomically (temp+rename) to `path`. */
static void publish_record(const char *path, uint8_t valid, uint16_t nonce, const uint8_t hdr[8]){
    uint8_t rec[16];
    rec[0]='D';rec[1]='S';rec[2]='5';rec[3]='T';rec[4]=1;rec[5]=valid?1:0;
    rec[6]=(uint8_t)(nonce&0xff); rec[7]=(uint8_t)(nonce>>8);
    if(hdr) memcpy(rec+8,hdr,8); else memset(rec+8,0,8);
    write_record_atomic(path,rec,16);
}

/* Build the per-address readiness filename "<base>.aabbccddeeff" (human MAC order,
 * lowercase, no colons) from an LSB-first bound_addr. */
static void per_addr_path(char *out, size_t n, const uint8_t addr[6]){
    snprintf(out,n,"%s.%02x%02x%02x%02x%02x%02x",g_tmpl_path,
             addr[5],addr[4],addr[3],addr[2],addr[1],addr[0]);
}

/* Startup hygiene: per-address readiness files from a PREVIOUS daemon run survive
 * in the jail tmp. One still saying valid=1 makes that tagged client keep
 * forwarding datagrams (which we must drop — no link) and never fall back to
 * hidraw seeding, so nothing ever goes on-air to capture OR assert-learn from:
 * the pad's output would be dead until a reconnect. Invalidate every
 * "<tmpl-base>.<12 lowercase hex>" sibling before serving. */
static void invalidate_stale_addr_files(void){
    char dir[600]; snprintf(dir,sizeof dir,"%s",g_tmpl_path);
    char *slash=strrchr(dir,'/');
    if(!slash) return;
    *slash='\0';
    const char *base=slash+1; size_t blen=strlen(base);
    DIR *dp=opendir(dir);
    if(!dp) return;
    struct dirent *e;
    while((e=readdir(dp))){
        const char *nm=e->d_name;
        if(strncmp(nm,base,blen)!=0 || nm[blen]!='.') continue;
        const char *hex=nm+blen+1; int hl=0;
        for(;hex[hl];hl++)
            if(!((hex[hl]>='0'&&hex[hl]<='9')||(hex[hl]>='a'&&hex[hl]<='f'))) break;
        char p[900];
        /* "<tmpl>.<mac>.st" telemetry sibling: a record from a previous daemon
         * run still reads valid=1 with a frozen seq — but a fresh consumer has
         * no "last seq" yet, so it would forward one phantom-backlog feedback
         * sample to the host servo. Remove instead of invalidate. */
        if(hl==12 && strcmp(hex+hl,".st")==0){
            snprintf(p,sizeof p,"%s/%s",dir,nm);
            unlink(p);
            fprintf(stderr,"[txd] stale per-address telemetry %s -> removed\n",nm);
            continue;
        }
        if(hex[hl] || hl!=12) continue;
        snprintf(p,sizeof p,"%s/%s",dir,nm);
        publish_record(p,0,0,NULL);
        fprintf(stderr,"[txd] stale per-address readiness %s -> invalidated\n",nm);
    }
    closedir(dp);
}

/* Publish the CURRENT readiness state of every link. g_pub_lock serializes
 * publishers; the g_lock snapshot is taken INSIDE the g_pub_lock section so the
 * last publisher to acquire g_pub_lock reads the latest committed state and writes
 * it LAST — the files therefore converge to the current state regardless of the
 * order two threads call this. Lock order is always g_pub_lock -> g_lock; this is
 * the only place that nests them and every caller has already released g_lock
 * before calling, so there is no inversion. File I/O (in publish_record()) still
 * never runs under g_lock. MUST be called WITHOUT g_lock held.
 *
 * The BASE file (g_tmpl_path) is what an untagged client reads (it is also what
 * service.js's /status parses), and it carries the SINGLE bound link — INVALID when
 * zero or more than one is bound, the same fail-closed rule the untagged inject path
 * applies. It used to mirror g_links[0] unconditionally, which lied in both
 * directions: a pad bound to slot 1 left the base record invalid while the transport
 * was up (untagged client stuck on hidraw forever), and a 2-pad session left it valid
 * while every untagged datagram was being refused (pad silent — the app stops seeding
 * hidraw the moment it reads valid=1). Each link that has ever bound also gets its
 * per-address file, which the tagged app polls for its own controller. */
static void publish_all(void){
    struct { uint8_t valid; uint16_t nonce; uint8_t hdr[8]; uint8_t addr[6]; int has_addr;
             uint8_t prev[6]; int prev_valid; } s[MAX_LINKS];
    int sole;
    pthread_mutex_lock(&g_pub_lock);
    pthread_mutex_lock(&g_lock);
    {   struct ds5_link *sl=sole_bound_link_locked(NULL);
        sole = sl ? (int)(sl-g_links) : -1; }
    for(int i=0;i<MAX_LINKS;i++){
        s[i].valid   = g_links[i].have ? 1 : 0;
        s[i].nonce   = g_links[i].nonce;
        memcpy(s[i].hdr,  g_links[i].hdr, 8);
        memcpy(s[i].addr, g_links[i].bound_addr, 6);
        s[i].has_addr = g_links[i].ever_bound;
        s[i].prev_valid = g_links[i].prev_valid;
        memcpy(s[i].prev, g_links[i].prev_addr, 6);
        g_links[i].prev_valid = 0;   /* consumed: this publisher writes it below */
    }
    pthread_mutex_unlock(&g_lock);
    if(sole>=0) publish_record(g_tmpl_path, 1, s[sole].nonce, s[sole].hdr);
    else        publish_record(g_tmpl_path, 0, 0, NULL);
    for(int i=0;i<MAX_LINKS;i++){
        /* Evicted identity first: valid=0 for a pad whose slot was taken over
         * (see ds5_link.prev_addr), unless that pad meanwhile lives in another
         * slot (then its own record below is authoritative). */
        if(s[i].prev_valid){
            int elsewhere=0;
            for(int j=0;j<MAX_LINKS;j++)
                if(s[j].has_addr && memcmp(s[j].addr,s[i].prev,6)==0){ elsewhere=1; break; }
            if(!elsewhere){
                char p[600]; per_addr_path(p,sizeof p,s[i].prev);
                publish_record(p, 0, 0, NULL);
            }
        }
        if(!s[i].has_addr) continue;
        char p[600]; per_addr_path(p,sizeof p,s[i].addr);
        publish_record(p, s[i].valid, s[i].nonce, s[i].valid ? s[i].hdr : NULL);
    }
    pthread_mutex_unlock(&g_pub_lock);
}

/* ---- jail-tmp remount self-heal ----------------------------------------- *
 * Both rendezvous sockets live at a PATH inside the app's jail tmp. When Aurora
 * (re)launches it mounts a FRESH tmpfs over /var/palm/jail/<app>/tmp, shadowing
 * the directory our socket node lives in: the bound socket stays alive in the
 * kernel (we hold the fd) but the PATH now resolves into the new, empty mount —
 * so the jailed app's sendto()/connect() by path gets ENOENT and silently falls
 * back to the flow-controlled hidraw write → DS5 audio/haptic dropouts. (The
 * tmpl FILE survives only because publish_record() re-open(O_CREAT)s it into
 * whatever mount is on top.) We give the sockets the same treatment: watch the
 * mount table via poll() on /proc/self/mountinfo — the kernel wakes us EXACTLY on
 * a mount/unmount, no time-based polling — and re-bind the node into the current
 * top mount whenever the path stops resolving to a socket. If the watch fd can't
 * be opened we degrade to a 500ms time-based poll (never lose self-heal). */

/* True iff a socket node is currently present at path (resolves in the live mount). */
static int node_alive(const char *path){
    struct stat st;
    return stat(path,&st)==0 && S_ISSOCK(st.st_mode);
}

/* Open the mount-change watch. poll() on this fd returns POLLPRI|POLLERR on every
 * mount-table change in our namespace; drain it once so the first poll blocks. */
static int open_mount_watch(void){
    int fd=open("/proc/self/mountinfo",O_RDONLY|O_CLOEXEC);
    if(fd<0){ perror("[txd] open mountinfo"); return -1; }
    char b[4096]; while(read(fd,b,sizeof b)>0){}   /* drain to EOF -> armed */
    return fd;
}

/* Re-arm the watch after an event: rewind and read to EOF (the mount list changed). */
static void rearm_mount_watch(int fd){
    if(lseek(fd,0,SEEK_SET)<0) return;
    char b[4096]; while(read(fd,b,sizeof b)>0){}
}

/* Create + bind a fresh AF_UNIX SOCK_DGRAM node at path (the report channel).
 * Non-blocking so the poll-driven recv drain terminates on EAGAIN. SO_PASSCRED so
 * each datagram carries the sender's SCM_CREDENTIALS for the peer-cred gate. -1 on
 * failure (e.g. parent dir momentarily absent mid-relaunch — caller retries). */
static int bind_unix_dgram(const char *path){
    int fd=socket(AF_UNIX,SOCK_DGRAM,0);
    if(fd<0) return -1;
    struct sockaddr_un ua; memset(&ua,0,sizeof ua);
    ua.sun_family=AF_UNIX; snprintf(ua.sun_path,sizeof ua.sun_path,"%s",path);
    unlink(path);
    if(bind(fd,(struct sockaddr*)&ua,sizeof ua)<0){ close(fd); return -1; }
    chmod(path,0666);   /* the jailed uid must be able to sendto it */
    int one=1; setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&one,sizeof one);  /* recv SCM_CREDENTIALS */
    int rcv=1<<20; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rcv,sizeof rcv);
    int fl=fcntl(fd,F_GETFL,0); if(fl>=0) fcntl(fd,F_SETFL,fl|O_NONBLOCK);
    return fd;
}

/* Create + bind + listen a fresh AF_UNIX SOCK_STREAM node at path (the broker
 * channel). Non-blocking listener so accept() can never block the broker thread on
 * a stale poll wakeup after a remount-rebind. -1 on failure. */
static int bind_unix_stream(const char *path){
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0) return -1;
    struct sockaddr_un ba; memset(&ba,0,sizeof ba);
    ba.sun_family=AF_UNIX; snprintf(ba.sun_path,sizeof ba.sun_path,"%s",path);
    unlink(path);
    if(bind(fd,(struct sockaddr*)&ba,sizeof ba)<0){ close(fd); return -1; }
    chmod(path,0666);   /* the jailed uid must be able to connect */
    int fl=fcntl(fd,F_GETFL,0); if(fl>=0) fcntl(fd,F_SETFL,fl|O_NONBLOCK);
    if(listen(fd,8)<0){ close(fd); return -1; }
    return fd;
}

/* ---- HID-FD broker ------------------------------------------------------- *
 * Hand the jailed app an open fd for a /dev/hidrawN node its static jail /dev
 * never received. One request per connection: the app writes the device path it
 * wants (newline-terminated); we authenticate the peer, validate the path, confirm
 * the node is a DS5's own hidraw, open it, and reply with a 1-byte status
 * ('O'=ok / 'E'=error) plus — on success — the open fd as SCM_RIGHTS ancillary. */

/* Defence in depth: only ever open /dev/hidraw<digits>, never an arbitrary path. */
static int valid_hidraw_path(const char *p){
    if(strncmp(p,"/dev/hidraw",11)!=0) return 0;
    const char *d=p+11;
    if(!*d) return 0;
    for(; *d; ++d) if(*d<'0'||*d>'9') return 0;
    return 1;
}

/* Broker allowlist: game controllers whose hidraw node may be handed into the
 * jail. Closes the audit's "second contamination route" — the broker must never
 * hand the app (or any local process) RW access to the Magic Remote's, a
 * keyboard's, or any other system HID node. Kept as an explicit VID/PID list
 * (pid 0 = whole vendor) rather than "anything that looks like a pad": every
 * entry here is reachable RW from the jail. */
static const struct { uint16_t vid, pid; } PAD_ALLOW[] = {
    {DS5_VID, DS5_PID}, /* Sony DualSense */
    {0x054c,  0x0df2},  /* Sony DualSense Edge */
    {0x054c,  0x05c4},  /* Sony DualShock 4 v1 */
    {0x054c,  0x09cc},  /* Sony DualShock 4 v2 */
    {0x054c,  0x0ba0},  /* Sony DS4 USB wireless dongle */
    {0x045e,  0x02e0},  /* Xbox One S pad (BT) */
    {0x045e,  0x02fd},  /* Xbox One S pad (BT, fw 3.x) */
    {0x045e,  0x0b05},  /* Xbox Elite Series 2 (BT) */
    {0x045e,  0x0b13},  /* Xbox Series X|S pad (BT) */
    {0x045e,  0x0b20},  /* Xbox One S pad (BLE fw 5.x) */
    {0x045e,  0x0b22},  /* Xbox Elite Series 2 (BLE fw 5.x) */
    {0x057e,  0x2009},  /* Nintendo Switch Pro Controller */
    {0x2dc8,  0x0000},  /* 8BitDo (controllers only as a vendor) */
};
static int is_allowed_pad_hidraw(int fd){
    struct hidraw_devinfo info;
    if(ioctl(fd,HIDIOCGRAWINFO,&info)<0) return 0;
    uint16_t v=(uint16_t)info.vendor, p=(uint16_t)info.product;
    for(size_t i=0;i<sizeof PAD_ALLOW/sizeof PAD_ALLOW[0];++i)
        if(PAD_ALLOW[i].vid==v && (PAD_ALLOW[i].pid==p || PAD_ALLOW[i].pid==0)) return 1;
    return 0;
}

/* The jail uid we accept, set once from argv[4] before any thread starts (see
 * main()); read-only afterwards, so the sockets' threads need no synchronisation. */
static uid_t g_jail_uid = (uid_t)JAIL_UID_DEFAULT;

/* True iff a connected peer's uid is allowed to drive us (jail app or root). */
static int uid_ok(uid_t u){ return u==g_jail_uid || u==0; }

/* Extract the sender's SCM_CREDENTIALS from a recvmsg() control buffer and check
 * the uid. Rejects (0) if no credentials are present. `peer_uid` (may be NULL)
 * receives the observed uid, or -1 when the datagram carried no credentials, so
 * the caller can NAME the rejected peer — a wrong jail uid is the misconfiguration
 * the argv[4] patch exists to make loud, and it is invisible from a bare 0. */
static int cred_ok(struct msghdr *mh, long *peer_uid){
    if(peer_uid) *peer_uid=-1;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(mh); c; c=CMSG_NXTHDR(mh,c)){
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS &&
           c->cmsg_len==CMSG_LEN(sizeof(struct ucred))){
            struct ucred uc; memcpy(&uc,CMSG_DATA(c),sizeof uc);
            if(peer_uid) *peer_uid=(long)uc.uid;
            return uid_ok(uc.uid);
        }
    }
    return 0;
}

/* Send status byte + (optionally) one fd as SCM_RIGHTS on a stream conn.
 * MSG_NOSIGNAL is the per-call half of the SIGPIPE defence (main() ignores the
 * signal process-wide; this keeps the guarantee even if that ever regresses) —
 * a peer that dropped its read end must yield EPIPE here, not a dead daemon.
 * Returns the sendmsg() result; <0 means the fd never reached the app, which is
 * worth a line because the app only ever learns "no fd" and then reports the
 * ENOENT of its own direct open(). */
static int send_fd(int conn, int fd, char status){
    char c=status;
    struct iovec iov; iov.iov_base=&c; iov.iov_len=1;
    struct msghdr msg; memset(&msg,0,sizeof msg);
    msg.msg_iov=&iov; msg.msg_iovlen=1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    if(fd>=0){
        memset(cbuf,0,sizeof cbuf);
        msg.msg_control=cbuf; msg.msg_controllen=sizeof cbuf;
        struct cmsghdr *cm=CMSG_FIRSTHDR(&msg);
        cm->cmsg_level=SOL_SOCKET; cm->cmsg_type=SCM_RIGHTS; cm->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm),&fd,sizeof(int));
    }
    ssize_t sr=sendmsg(conn,&msg,MSG_NOSIGNAL);
    if(sr<0) fprintf(stderr,"[txd] broker reply '%c' not delivered errno=%d (%s)\n",status,errno,strerror(errno));
    return (int)sr;
}

static void *broker_thread(void *arg){
    prctl(PR_SET_NAME,(unsigned long)"ds5-brk",0,0,0);
    const char *path=(const char*)arg;
    /* A failed FIRST bind is not fatal: it is the same transient condition the
     * remount self-heal below already handles (parent dir momentarily absent, fd or
     * memory exhaustion at startup). Returning here killed the broker for the whole
     * process lifetime — with neither the daemon nor the supervisor noticing, since
     * ds5-tmpld.sh only watches the report socket. Start with sfd=-1 instead and let
     * the existing 500ms tick drive the retry (poll ignores a pollfd with fd<0, so
     * this waits, it never spins). */
    int sfd=bind_unix_stream(path);
    if(sfd<0) perror("[txd] bind broker (retrying every 500ms)");
    else fprintf(stderr,"[txd] hid-fd broker up: %s\n",path);
    int minfo=open_mount_watch();   /* self-heal the broker node across jail-tmp remounts */
    for(;;){
        /* Steady state: block on accept-ready + mount changes. A 500ms timeout
         * engages while a rebind is pending (node gone, dir not yet ready) or the
         * mount watch is unavailable (degrade to time-based polling). poll() ignores
         * a pollfd whose fd is < 0, so a dead minfo simply contributes nothing. */
        struct pollfd pf[2]={{sfd,POLLIN,0},{minfo,POLLPRI,0}};
        int to=(sfd<0 || minfo<0)?500:-1;
        int pr=poll(pf,2,to);
        if(pr<0){ if(errno==EINTR) continue; usleep(5000); continue; }
        if(pr==0 || pf[1].revents){
            if(minfo<0) minfo=open_mount_watch();        /* retry a previously-failed watch */
            else if(pf[1].revents) rearm_mount_watch(minfo);
            /* sfd<0 covers both a pending remount rebind AND a failed first bind
             * (the node can even exist without us owning it — bind_unix_stream
             * leaves it behind if listen() fails), so retry on either condition. */
            if(sfd<0 || !node_alive(path)){
                if(sfd>=0){ close(sfd); sfd=-1; }
                int ns=bind_unix_stream(path);
                if(ns>=0){ sfd=ns; fprintf(stderr,"[txd] hid-fd broker (re)bound: %s\n",path); }
            }
            continue;   /* re-poll with fresh fds; never accept() on a stale revents */
        }
        if(sfd<0 || !(pf[0].revents&POLLIN)) continue;
        int conn=accept(sfd,NULL,NULL);
        if(conn<0){ if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK) continue; usleep(5000); continue; }
        /* Authenticate the peer before doing any privileged work for it. */
        struct ucred uc; socklen_t ucl=sizeof uc;
        if(getsockopt(conn,SOL_SOCKET,SO_PEERCRED,&uc,&ucl)<0){ close(conn); continue; }
        if(!uid_ok(uc.uid)){ fprintf(stderr,"[txd] broker rejected peer uid=%u (accepting %u or root)\n",uc.uid,(unsigned)g_jail_uid); send_fd(conn,-1,'E'); close(conn); continue; }
        struct timeval tv={.tv_sec=2,.tv_usec=0};
        setsockopt(conn,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
        char req[80]; ssize_t n=recv(conn,req,sizeof req-1,0);
        if(n<=0){ close(conn); continue; }
        req[n]='\0';
        for(char *e=req+strlen(req); e>req && (e[-1]=='\n'||e[-1]=='\r'||e[-1]==' '); --e) e[-1]='\0';
        int hfd=-1;
        if(valid_hidraw_path(req)){
            hfd=open(req,O_RDWR|O_CLOEXEC);
            if(hfd<0) hfd=open(req,O_RDONLY|O_CLOEXEC);
            if(hfd>=0 && !is_allowed_pad_hidraw(hfd)){   /* only allowlisted pads leave the broker */
                fprintf(stderr,"[txd] broker refused non-allowlisted hidraw %s\n",req);
                close(hfd); hfd=-1;
            }
        }
        if(hfd>=0){
            /* Only claim the hand-off once it actually left: send_fd() logs the
             * failure itself, so a lone "handed fd" line now means the app has it. */
            if(send_fd(conn,hfd,'O')>0) fprintf(stderr,"[txd] broker handed fd for %s\n",req);
            close(hfd);   /* app now holds its own ref via SCM_RIGHTS */
        } else {
            send_fd(conn,-1,'E');
            fprintf(stderr,"[txd] broker open/verify failed for '%s' errno=%d\n",req,errno);
        }
        close(conn);
    }
    return NULL;   /* not reached (for(;;)); keeps the non-void return explicit */
}

/* Parse one HCI event packet off the MONITOR stream and maintain the
 * handle->bdaddr table; invalidate a bound link if its handle drops or is
 * reassigned to a different device. Called from capture_thread WITHOUT g_lock
 * held (it takes g_lock for the shared state, then publishes outside it). */
static void handle_hci_event(const uint8_t *e, int el, uint64_t kms){
    if(el < 2) return;
    uint8_t code=e[0]; const uint8_t *p=e+2; int pl=el-2;   /* skip [code][param_len] */
    const char *reason=NULL; int none_bound=0;
    if(code==HCI_EV_CMD_COMPLETE && pl>=3){                 /* ncmd,opcode(2),status... */
        uint16_t op=(uint16_t)(p[1]|(p[2]<<8));
        /* L1 answers. Both are read from the RESPONSE, never inferred from the
         * fact that we sent something — that distinction is the whole reason
         * these two commands exist (a controller that ignores the write still
         * returns success for it). */
        if(op==OP_READ_LOCAL_FEATURES && pl>=12){           /* ncmd,op(2),status,features(8) */
            if(p[3]==0x00){
                /* LMP feature bit 54 = Non-flushable Packet Boundary Flag. */
                int have=(p[4+6]&0x40)?1:0;
                if(g_lmp_nonflush!=have){
                    g_lmp_nonflush=have;
                    fprintf(stderr,"[txd] flush: local LMP bit54 (non-flushable PB) = %s%s\n",
                            have?"SUPPORTED":"ABSENT",
                            have?"" : " -> auto-flush stays DISARMED (a flush would eat 0x31 SetState)");
                }
            } else {
                g_lmp_nonflush=0;
                fprintf(stderr,"[txd] flush: Read_Local_Supported_Features failed status=0x%02x "
                               "-> auto-flush stays DISARMED\n",p[3]);
            }
        } else if(op==OP_READ_AUTO_FLUSH && pl>=8){         /* ncmd,op(2),status,handle(2),timeout(2) */
            g_flush_readback_pending=0;
            if(p[3]==0x00){
                uint16_t slots=(uint16_t)(p[6]|(p[7]<<8));
                int ms=(int)(((long)slots*5)/8);
                int was=g_flush_confirmed_ms;
                g_flush_confirmed_ms=ms;
                if(was!=ms)
                    fprintf(stderr,"[txd] flush: read-back handle=0x%03x -> %u slots = %d ms%s\n",
                            (unsigned)((p[4]|(p[5]<<8))&0x0fff),slots,ms,
                            ms? " (ARMED: audio PB=2 flushable, 0x31/0x32 PB=0 protected)"
                              : " (infinite: disarmed)");
            } else {
                g_flush_confirmed_ms=0;
                fprintf(stderr,"[txd] flush: read-back failed status=0x%02x -> treating as DISARMED\n",p[3]);
            }
        } else if(op==OP_READ_LINK_QUALITY || op==OP_READ_RSSI ||
                  op==OP_READ_AFH_MAP     || op==OP_READ_FAILED_CONTACT){
            /* L16. A command the controller does not implement is recorded once
             * and then never polled again — a retry storm on an unsupported
             * opcode would spend exactly the command budget this instrument
             * promised not to touch. */
            if(pl>=4 && p[3]!=0x00){
                int bit = op==OP_READ_LINK_QUALITY?1: op==OP_READ_RSSI?2:
                          op==OP_READ_AFH_MAP?4:8;
                if(!(g_linkq_off&bit)){
                    g_linkq_off|=bit;
                    fprintf(stderr,"[txd] linkq: 0x%04x returned status=0x%02x "
                                   "-> not polling it again this run\n",op,p[3]);
                }
            } else if(op==OP_READ_LINK_QUALITY && pl>=7){
                int q=p[6];
                g_lq_last=q;
                if(g_lq_min<0||q<g_lq_min) g_lq_min=q;
                if(g_lq_max<0||q>g_lq_max) g_lq_max=q;
            } else if(op==OP_READ_RSSI && pl>=7){
                int r=(int8_t)p[6];
                g_rssi_last=r;
                if(g_rssi_min==127||r<g_rssi_min) g_rssi_min=r;
            } else if(op==OP_READ_AFH_MAP && pl>=17){
                /* p[6]=mode, p[7..16]=79-bit map. Popcount = channels AFH still
                 * considers usable; a small number is coex having eaten the
                 * spectrum, which is a capacity finding independent of B. */
                int used=0;
                for(int k=0;k<10;k++){ unsigned v=p[7+k]; while(v){ used+=v&1; v>>=1; } }
                g_afh_last=used;
                if(g_afh_min<0||used<g_afh_min) g_afh_min=used;
            } else if(op==OP_READ_FAILED_CONTACT && pl>=8){
                int c=(int)(p[6]|(p[7]<<8));
                /* Reported as a delta from the first sample of this armed run:
                 * the counter is cumulative and we deliberately never reset it
                 * (Reset_Failed_Contact_Counter would clobber a value another
                 * stack user may be watching). */
                if(g_fcc_base<0) g_fcc_base=c;
                g_fcc_last=c;
            }
        } else if(op==OP_WRITE_AUTO_FLUSH && pl>=4 && p[3]!=0x00){
            /* Accepted-and-ignored is the case we fear, but an outright reject
             * must not leave a stale "armed" belief behind either. */
            g_flush_confirmed_ms=0;
            fprintf(stderr,"[txd] flush: Write_Automatic_Flush_Timeout rejected status=0x%02x\n",p[3]);
        }
        for(int i=0;i<g_pend_n;i++)                         /* pop the oldest matching pend */
            if(g_pend[i].op==op){
                g_pend_n--;
                memmove(&g_pend[i],&g_pend[i+1],(size_t)(g_pend_n-i)*sizeof g_pend[0]);
                break;
            }
        if(g_cmd_dead && g_probe_op && op==g_probe_op){     /* recovery probe answered */
            g_cmd_dead=0; g_probe_op=0; g_pend_n=0;
            fprintf(stderr,"[txd] CMDRECOVER: Command Complete 0x%04x on air -> HCI command path back up\n",op);
        }
        return;
    }
    if(code==HCI_EV_CMD_STATUS && pl>=4){                   /* status,ncmd,opcode(2) */
        /* Some commands (Exit_Sniff_Mode) are answered by Command STATUS only —
         * pop their pend entry here or the cmdguard would count them unanswered
         * and false-trip DEAD 6s after every exit-sniff. */
        uint16_t op=(uint16_t)(p[2]|(p[3]<<8));
        for(int i=0;i<g_pend_n;i++)
            if(g_pend[i].op==op){
                uint16_t arg=g_pend[i].arg;
                g_pend_n--;
                memmove(&g_pend[i],&g_pend[i+1],(size_t)(g_pend_n-i)*sizeof g_pend[0]);
                /* Exit_Sniff answered with 0x0c (Command Disallowed) proves the
                 * link is NOT sniffed — the tracked mode was stale (Mode Change
                 * or kernel connect lost on the monitor). Repair it
                 * authoritatively, or the pin loop would re-send Exit_Sniff
                 * every 3s forever: an active link never emits the Mode Change
                 * that would clear mode, and the futile loop eats ~2/3 of the
                 * 0.5/s kernel command drain for the rest of the session.
                 * ONLY 0x0c: any other error (controller busy, unspecified) on a
                 * genuinely sniffed link would wrongly clear mode, and with no
                 * further Mode Change event Exit_Sniff would never be retried —
                 * the inverse bug. Unknown errors leave mode untouched; the 3s
                 * throttle bounds the retry cost. */
                if(op==OP_EXIT_SNIFF && p[0]==0x0c){
                    pthread_mutex_lock(&g_lock);
                    g_htab[arg&0x0fff].mode=0;
                    pthread_mutex_unlock(&g_lock);
                }
                /* L18: an outright reject means the mask was never applied —
                 * the believed/sent state must not claim otherwise, or the A/B
                 * would compare two identical arms and read it as
                 * accept-and-ignore. */
                if(op==OP_CHG_CONN_PTYPE && p[0]!=0x00){
                    /* One refusal parks the lever for the whole run: with the
                     * converge-on-unknown rule a mere sent=-1 reset would turn
                     * into a 3s retry loop against a controller that will keep
                     * refusing (pre-registered outcome E1 = lever closed). */
                    g_ptype_refused=1;
                    fprintf(stderr,"[txd] L18 ptype: rejected status=0x%02x handle=0x%03x "
                                   "-> lever parked for this run (E1)\n",
                            p[0],arg&0x0fff);
                    /* ptype_* is capture-thread-owned, and so is this handler. */
                    for(int k=0;k<MAX_LINKS;k++)
                        if(g_links[k].ptype_handle==(arg&0x0fff)) g_links[k].ptype_sent=-1;
                }
                break;
            }
        return;
    }
    if(code==HCI_EV_PTYPE_CHANGED && pl>=5){                /* status,handle(2),ptype(2) */
        /* L18 witness W2: the controller REPORTS the new mask. Necessary but
         * never sufficient (L1: this chip stores-and-reports settings it does
         * not apply) — efficacy is judged on the gapge distribution. Logged on
         * every arrival: a mask the STACK changed behind our back is exactly
         * the kind of interference an A/B needs to know about. */
        uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
        int pt = p[0]==0x00 ? (int)(p[3]|(p[4]<<8)) : -1;
        if(p[0]==0x00) g_ptype_seen=pt;
        fprintf(stderr,"[txd] L18 ptype: Connection_Packet_Type_Changed handle=0x%03x "
                       "status=0x%02x mask=0x%04x\n",hh,p[0],pt<0?0:(unsigned)pt);
        return;
    }
    if(code==HCI_EV_MODE_CHANGE && pl>=6){                  /* status,handle(2),mode,interval(2) */
        if(p[0]!=0x00) return;
        uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
        pthread_mutex_lock(&g_lock);
        g_htab[hh].mode=p[3]; g_htab[hh].mode_seen=1;
        pthread_mutex_unlock(&g_lock);
        return;                                             /* mode never affects validity */
    }
    if(code==HCI_EV_CONN_COMPLETE && pl>=9){                /* status,handle(2),bdaddr(6),... */
        if(p[0]!=0x00) return;
        uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
        pthread_mutex_lock(&g_lock);
        memcpy(g_htab[hh].addr,p+3,6); g_htab[hh].known=1; g_htab[hh].mode=0; g_htab[hh].mode_seen=1; g_htab[hh].from_assert=0; /* kernel-proven, active */
        struct ds5_link *L=link_by_handle(hh);
        /* An assert-learned link is dropped on ANY kernel connect for its handle, even
         * when the addresses agree: the kernel is now the authority on what this handle
         * carries, and an agreeing address proves nothing when the address itself came
         * from the jail. The link re-binds from the next on-air DS5 output under the
         * now-trusted g_htab entry. See ds5_link.assert_learned. */
        if(L && (L->assert_learned || memcmp(p+3,L->bound_addr,6)!=0)){
            L->have=0;
            reason = L->assert_learned ? "kernel connect supersedes assert-learned identity (BR/EDR)"
                                       : "bound handle reassigned (BR/EDR)";
        }
        none_bound=!any_link_bound_locked();
        pthread_mutex_unlock(&g_lock);
    } else if(code==HCI_EV_DISCONN_COMPLETE && pl>=4){      /* status,handle(2),reason */
        if(p[0]!=0x00) return;
        uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
        pthread_mutex_lock(&g_lock);
        g_htab[hh].known=0;
        g_htab[hh].mode_seen=0;
        g_htab[hh].mode=0;   /* a dead link is not sniffed: a later occupant of this
                              * handle whose identity arrives via assert-learn/seed
                              * (paths that never see a Mode Change) must not inherit
                              * a stale mode==2 -> endless Exit_Sniff loop */
        struct ds5_link *L=link_by_handle(hh);
        if(L){ L->have=0; reason="bound handle disconnected"; }
        none_bound=!any_link_bound_locked();
        pthread_mutex_unlock(&g_lock);
    } else if(code==HCI_EV_LE_META && pl>=12){              /* subev,status,handle(2),role,atype,addr(6) */
        /* Legacy (0x01) and Enhanced (0x0A) LE Connection Complete share the same
         * prefix layout through peer_addr, so one offset path covers both. */
        if((p[0]!=HCI_SUBEV_LE_CONN && p[0]!=HCI_SUBEV_LE_ENH_CONN) || p[1]!=0x00) return;
        uint16_t hh=(uint16_t)((p[2]|(p[3]<<8))&0x0fff);
        pthread_mutex_lock(&g_lock);
        memcpy(g_htab[hh].addr,p+6,6); g_htab[hh].known=1; g_htab[hh].mode=0; g_htab[hh].mode_seen=1; g_htab[hh].from_assert=0; /* kernel-proven */
        struct ds5_link *L=link_by_handle(hh);
        if(L && (L->assert_learned || memcmp(p+6,L->bound_addr,6)!=0)){
            L->have=0;
            reason = L->assert_learned ? "kernel connect supersedes assert-learned identity (LE)"
                                       : "bound handle reassigned (LE)";
        }
        none_bound=!any_link_bound_locked();
        pthread_mutex_unlock(&g_lock);
    } else if(code==HCI_EV_NUM_COMP_PKTS && pl>=1){        /* TX credits returned: free the outstanding window */
        int nh=p[0];
        if(pl < 1+nh*4) return;
        int kick=0;
        uint64_t nowm=now_ms();
        pthread_mutex_lock(&g_lock);
        for(int i=0;i<nh;i++){
            uint16_t hh=(uint16_t)((p[1+i*4]|(p[2+i*4]<<8))&0x0fff);
            struct ds5_link *L=link_by_handle(hh);   /* credits are PER HANDLE -> per link */
            if(!L) continue;
            int cnt=(int)(p[3+i*4]|(p[4+i*4]<<8));
            /* Sub-episode gap histogram, at native NOCP resolution. THIS credit
             * return ends exactly one credit-starved stretch, so its length is
             * (now - last_nocp) — no high-watermark, no sampling, no ~21ms
             * quantization from the datagram-clocked main loop.
             *
             * Gated exactly as before: only while the window was actually
             * occupied (outstanding>0, read BEFORE the decrement below) and only
             * while the app is really feeding us. On stream teardown the
             * controller discards its queued packets without emitting the
             * matching NOCPs, so an ungated histogram counts idle as starvation —
             * that is the DEMAND_IDLE_MS gate, keyed on demand and never on
             * successful inject (a credit stall is precisely when injection
             * stops). */
            if(L->have && L->outstanding>0 && L->last_nocp && nowm>L->last_nocp){
                uint64_t ld=__atomic_load_n(&L->last_demand,__ATOMIC_RELAXED);
                uint64_t ds=__atomic_load_n(&L->demand_since,__ATOMIC_RELAXED);
                /* Demand must have held ACROSS the gap, not merely at its end:
                 * last_nocp >= demand_since means the gap began inside the
                 * current continuous-demand stretch. Without this, one idle
                 * keepalive re-arms the end-test and a ~5s idle gap lands in
                 * every cumulative bin (the `d_inj=2` / `30:2/…/200:2`
                 * signature). Continuous-load headline numbers are unaffected
                 * — the contamination only ever hit mixed/idle sessions. */
                uint64_t la=__atomic_load_n(&L->last_audio,__ATOMIC_RELAXED);
                uint64_t as=__atomic_load_n(&L->audio_since,__ATOMIC_RELAXED);
                /* The histogram arms only while AUDIO flowed across the whole
                 * gap: recent at the end (last_audio) and already flowing when
                 * the gap began (last_nocp >= audio_since). A genuine link
                 * stall passes both (injection continues on free credits);
                 * a game-silence pause fails the recency test. This gates the
                 * legacy gaps=/gmax counters too — a measurement change to
                 * note in the ledger, not a format change (silence-heavy
                 * sessions counted gaps before, and those numbers were the
                 * lie this gate removes). */
                if(ld && nowm>=ld && nowm-ld<DEMAND_IDLE_MS &&
                   ds && L->last_nocp>=ds &&
                   la && nowm>=la && nowm-la<AUDIO_IDLE_MS &&
                   as && L->last_nocp>=as){
                    /* The binned length pairs two KERNEL stamps when both ends
                     * have one; otherwise it degrades to the old monotonic
                     * delta for this one interval (never mixes domains). The
                     * gates above stay on the monotonic clock on purpose —
                     * demand/audio freshness is bookkeeping, not the
                     * measurement. */
                    uint64_t g;
                    if(kms && L->nocp_k_ok && kms>L->last_nocp_k) g=kms-L->last_nocp_k;
                    else { g=nowm-L->last_nocp; if(!kms) g_ts_fallback++; }
                    /* Was this OUR hole? The first NOCP after a synthetic hold
                     * necessarily lands after the hold has been released, so the
                     * test is the quiet window (hold + settle), not the hold
                     * itself. Everything measured in that window is bench data
                     * and is kept out of the production bins. */
                    if(now_us()<__atomic_load_n(&g_gap_quiet_until_us,__ATOMIC_RELAXED)){
                        L->gap_synth++; L->gap_synth_ms=g;
                    } else {
                        if(g>=80)      L->gap80++;
                        else if(g>=50) L->gap50++;
                        else if(g>=30) L->gap30++;
                        /* Same gate, same lock, same event — cumulative, so one
                         * gap lands in every bin it clears. */
                        for(int b=0;b<GAPGE_N;b++)
                            if(g>=GAPGE_EDGE[b]) L->gapge[b]++;
                        if(g>L->gap_max) L->gap_max=g;
                        /* L17c: the same production-binned gap as an EVENT with
                         * a wall-clock stamp, for offline correlation with
                         * radio-side logs. Ring + flush are capture-thread-only
                         * (this handler runs on the capture loop). */
                        if(g_gaplog_want && g>=(uint64_t)g_gaplog_want){
                            if(g_gaplog_n<GAPLOG_RING){
                                /* kms IS wall-clock (CLOCK_REALTIME domain) — the
                                 * queue-time stamp beats a flush-time now_wall_ms()
                                 * for correlating with iw-event epochs. */
                                g_gaplog_ring[g_gaplog_n].wall_ms=kms?kms:now_wall_ms();
                                g_gaplog_ring[g_gaplog_n].gap=(unsigned)g;
                                g_gaplog_ring[g_gaplog_n].out=L->outstanding;
                                g_gaplog_ring[g_gaplog_n].h=hh;
                                g_gaplog_n++;
                            } else g_gaplog_lost++;
                        }
                    }
                }
            }
            /* Late credits settle the written-off debt before they free
             * anything new. Expiry is evaluated here rather than on a timer:
             * this is the only place ghosts can be observed, and an expired
             * ghost must not silently keep absorbing. */
            int ttl=g_ghost_ttl_ms;
            if(L->ghost>0){
                /* ghost_ts is stamped by the INJECT thread, nowm read by this
                 * one; if that ordering ever put ghost_ts marginally ahead, an
                 * unsigned subtraction would wrap to "ancient" and expire the
                 * ghosts. Compare explicitly — and note the wrap direction was
                 * already the safe one (fall back to old behaviour), so this is
                 * about not lying in the counters. */
                if(ttl<=0 || (nowm>L->ghost_ts && nowm-L->ghost_ts>(uint64_t)ttl)){
                    L->ghost_expired+=L->ghost; L->ghost=0;   /* genuinely lost after all */
                } else {
                    int use = cnt<L->ghost ? cnt : L->ghost;
                    L->ghost-=use; cnt-=use; L->ghost_absorbed+=use;
                }
            }
            L->outstanding-=cnt; if(L->outstanding<0) L->outstanding=0;
            txwin_pop(L,cnt);   /* FIFO-approximate the per-type in-flight counts */
            /* Credits just freed: if this link is holding audio, wake the main
             * loop NOW instead of letting the backlog wait for the next app
             * datagram. Relaxed read of a main-thread-owned int — this is a HINT
             * (the main loop re-checks under its own ownership), so a stale value
             * can only cost a spurious wakeup or fall back to the old behaviour. */
            if(__atomic_load_n(&L->fifo_count,__ATOMIC_RELAXED)>0) kick=1;
            /* Refresh the stall timestamp ONLY for OUR handle's completions:
             * a global refresh would let any other device's NOCP chatter
             * (Magic Remote etc.) suppress the 150ms backstop exactly when
             * our credits are the ones wedged. */
            L->last_nocp=nowm;
            if(kms){ L->last_nocp_k=kms; L->nocp_k_ok=1; } else L->nocp_k_ok=0;
            /* NOCP for the bound handle also proves the LINK is alive:
             * the controller is completing OUR injections. Without this,
             * last_seen is only refreshed by kernel-path HID writes
             * (the tmpld seeder) -- if that write blocks >1.5s on the
             * one-outstanding flow control under full raw-inject load,
             * the idle backstop misfires mid-stream and the resulting
             * template-invalidate/rebind cycle stalls injection (an
             * audible speaker dropout). A real flap stops producing
             * NOCPs for this handle, and handle-reassignment is caught
             * by the CONN/DISCONN handlers, so the backstop's purpose
             * is preserved.
             * RESIDUAL RISK (accepted): if BOTH the DISCONN and the foreign
             * reuse-CONN for this 12-bit handle are lost on the monitor, and the
             * foreign device returns NOCPs for our injected ACL, this refresh
             * keeps the idle backstop from firing -> injection onto a non-DS5
             * until a later CONN/DISCONN corrects g_htab. It stays narrow because
             * injection is driven by the app, which is bound to the REAL hidraw
             * node: when the DS5 leaves, hidraw vanishes, the app tears the
             * controller down and stops forwarding, so injection (and these
             * NOCPs) stops on its own. Closing it fully needs an on-air identity
             * re-check, but HCIGETCONNLIST is empty on webOS and our own injects
             * aren't mirrored on MONITOR -> no cheap in-session identity signal. */
            L->last_seen=nowm;
        }
        pthread_mutex_unlock(&g_lock);
        /* Outside the lock on purpose: g_lock is held only for short non-blocking
         * work, and the eventfd write is the one syscall in this path. */
        if(kick && g_kickfd>=0){
            uint64_t one=1;
            ssize_t w=write(g_kickfd,&one,sizeof one);
            (void)w;   /* EAGAIN = counter saturated = a wakeup is already pending */
        }
        return;   /* NOCP never affects template validity */
    } else return;
    if(reason){
        publish_all(); fprintf(stderr,"[txd] %s -> template INVALID\n",reason);
        if(none_bound) g_scan_want=2;   /* restore scan only once NO link remains bound */
    }
}

/* Best-effort: seed/refresh the handle->bdaddr table from the kernel's current
 * connection list, so a controller already connected before we started monitoring
 * (or whose CONN_COMPLETE we missed) is still identity-bound. Uses a throwaway
 * unbound HCI socket (the bluez convention). Quiet on repeat: logs the
 * "unavailable" notice once and a seeded handle only when its mapping changes.
 * Safe to call repeatedly from capture_thread (takes g_lock itself). */
static void seed_conn_list(void){
    static int warned=0;
    int s=socket(AF_BLUETOOTH,SOCK_RAW,BTPROTO_HCI);
    if(s<0) return;
    struct hci_conn_list_req req; memset(&req,0,sizeof req);
    req.dev_id=TARGET_HCI_INDEX; req.conn_num=16;
    if(ioctl(s,HCIGETCONNLIST,&req)<0){
        if(!warned){ fprintf(stderr,"[txd] HCIGETCONNLIST unavailable (errno=%d) -> event-parse only\n",errno); warned=1; }
        close(s); return;
    }
    close(s);
    int n=req.conn_num>16?16:req.conn_num;
    /* Stage changed entries, then log AFTER releasing g_lock — seed_conn_list now runs
     * at runtime from capture_thread (the need_learn path), and a blocking fprintf
     * under g_lock could stall the inject thread waiting on the same lock (cf. #9). */
    struct { uint16_t hh; uint8_t a[6]; } chg[16]; int nchg=0;
    pthread_mutex_lock(&g_lock);
    for(int i=0;i<n;i++){
        uint16_t hh=req.ci[i].handle & 0x0fff;
        if(!g_htab[hh].known || memcmp(g_htab[hh].addr,req.ci[i].bdaddr.b,6)!=0){
            memcpy(g_htab[hh].addr,req.ci[i].bdaddr.b,6); g_htab[hh].known=1; g_htab[hh].mode=0; g_htab[hh].mode_seen=0; g_htab[hh].from_assert=0; /* kernel conn list carries no mode -> assumed, probe it */
            chg[nchg].hh=hh; memcpy(chg[nchg].a,req.ci[i].bdaddr.b,6); nchg++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    for(int i=0;i<nchg;i++){ const uint8_t*a=chg[i].a;
        fprintf(stderr,"[txd] seed handle=0x%03x addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
                chg[i].hh,a[5],a[4],a[3],a[2],a[1],a[0]); }
}

/* Capture thread: watch HCI_CHANNEL_MONITOR (root) for our outgoing HID-output
 * and keep each connection's handle+CID published — but only ever publish
 * VALID once the bound handle's bdaddr is known (fail closed). */
static void *capture_thread(void *arg){
    (void)arg;
    prctl(PR_SET_NAME,(unsigned long)"ds5-cap",0,0,0);
    int mfd=socket(AF_BLUETOOTH,SOCK_RAW,BTPROTO_HCI);
    if(mfd<0){ perror("[txd] socket monitor"); return NULL; }
    struct sockaddr_hci ma; memset(&ma,0,sizeof ma);
    ma.hci_family=AF_BLUETOOTH; ma.hci_dev=HCI_DEV_NONE; ma.hci_channel=HCI_CHANNEL_MONITOR;
    if(bind(mfd,(struct sockaddr*)&ma,sizeof ma)<0){ perror("[txd] bind monitor"); close(mfd); return NULL; }
    struct timeval tv={.tv_sec=0,.tv_usec=300000}; setsockopt(mfd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    /* Kernel queue-time stamps for the gap ledger (1.4.34). Proven on this exact
     * socket type by tools/ds5_clock_probe.c (0 missing stamps / 11448 intervals).
     * Failure is survivable: every interval then falls back to the monotonic
     * clock and g_ts_fallback witnesses it in the ledger. */
    { int one=1;
      if(setsockopt(mfd,SOL_SOCKET,SO_TIMESTAMP,&one,sizeof one)<0)
          perror("[txd] SO_TIMESTAMP (gap clock stays userspace)"); }
    uint8_t buf[2048];
    uint64_t last_learn=0;
    /* Idle sniff-pin handle cache (capture-thread-only). The pin needs the ACL
     * handle currently carrying an idle pad's address, but this loop wakes on
     * EVERY monitor packet (hundreds/s in-session) — a full g_htab sweep per
     * wakeup was ~36KB scanned under g_lock each time, all of it discarded by
     * the >=3s policy-send throttle downstream. Instead the found handle is
     * cached and revalidated O(1) (still known, address still matches) each
     * wakeup; the full sweep runs at most every IDLE_SCAN_MS. */
    uint64_t last_idle_scan=0;
    uint16_t idle_lh[MAX_LINKS]; uint8_t idle_ok[MAX_LINKS];
    memset(idle_lh,0,sizeof idle_lh); memset(idle_ok,0,sizeof idle_ok);
    for(;;){
        /* Idle backstop: evaluated EVERY wakeup (not only on recv-timeout) so it
         * still fires while other BT devices keep the monitor socket busy. After a
         * flap a DS5 stops emitting HID-output -> its last_seen ages out -> its link
         * is invalidated within IDLE_INVALIDATE_MS even if the DISCONN event was
         * lost. Snapshot each bound link's handle for the link-policy pin under the
         * same lock. */
        int idle_inval=0, none_bound=0, sess_active=0;
        int lb[MAX_LINKS], pin[MAX_LINKS]; uint16_t lh[MAX_LINKS]; uint8_t md[MAX_LINKS], mdseen[MAX_LINKS];
        pthread_mutex_lock(&g_lock);
        for(int i=0;i<MAX_LINKS;i++){
            /* Ghosts also expire without traffic. The NOCP path can only retire
             * them when a credit actually arrives, so a link that goes quiet
             * after a write-off would keep showing a stale live count — and
             * "ghost back to 0 after every episode" is the acceptance criterion
             * for this whole mechanism. A counter that cannot be trusted to
             * drain cannot serve as that check. */
            {   int gttl=g_ghost_ttl_ms; uint64_t tnow=now_ms();
                struct ds5_link *G=&g_links[i];
                if(G->ghost>0 && (gttl<=0 || (tnow>G->ghost_ts && tnow-G->ghost_ts>(uint64_t)gttl))){
                    G->ghost_expired+=G->ghost; G->ghost=0;
                }
            }
            if(g_links[i].have && now_ms()-g_links[i].last_seen>IDLE_INVALIDATE_MS){
                g_links[i].have=0; idle_inval=1;
            }
            if(g_links[i].have && g_links[i].session_seen &&
               now_ms()-g_links[i].session_seen<=SESSION_IDLE_MS) sess_active=1;
            lb[i]=g_links[i].have; lh[i]=g_links[i].handle; pin[i]=lb[i]; md[i]=0; mdseen[i]=0;
        }
        none_bound=!any_link_bound_locked();
        /* Idle sniff-pin (2026-07-10): while ANY pad is in-session, keep every
         * OTHER known DualSense (ever_bound identity, still connected per
         * g_htab) sniff-off too. Measured A/B: an idle unbound DS5 falling into
         * sniff steals the active pad's airtime on the sniff grid (~2.4 gap50/s
         * vs ~0 with both links active) — the pin trades the idle pad staying
         * in active mode (battery) for the active pad's haptic latency. Outside
         * a session nothing is pinned: an unused pad may sniff/park freely. */
        if(!none_bound){
            uint64_t ts=now_ms();
            int sweep=(ts-last_idle_scan>IDLE_SCAN_MS);
            if(sweep) last_idle_scan=ts;
            for(int i=0;i<MAX_LINKS;i++){
                if(pin[i] || !g_links[i].ever_bound) continue;
                if(idle_ok[i] && g_htab[idle_lh[i]].known &&
                   memcmp(g_htab[idle_lh[i]].addr,g_links[i].bound_addr,6)==0){
                    pin[i]=1; lh[i]=idle_lh[i]; continue;   /* cached handle still valid */
                }
                idle_ok[i]=0;                    /* stale (DISCONN/reassign/slot reuse) */
                if(!sweep) continue;             /* next sweep <=IDLE_SCAN_MS away — far
                                                  * inside the 3s policy-send throttle */
                for(int h=0;h<4096;h++)
                    if(g_htab[h].known && memcmp(g_htab[h].addr,g_links[i].bound_addr,6)==0){
                        pin[i]=1; lh[i]=(uint16_t)h; idle_lh[i]=(uint16_t)h; idle_ok[i]=1; break;
                    }
            }
        }
        for(int i=0;i<MAX_LINKS;i++) if(pin[i]){ md[i]=g_htab[lh[i]].mode; mdseen[i]=g_htab[lh[i]].mode_seen; }
        pthread_mutex_unlock(&g_lock);
        if(idle_inval){
            publish_all(); fprintf(stderr,"[txd] link idle -> template INVALID\n");
            if(none_bound) g_scan_want=2;
        }
        /* Main-thread invalidations (EBADF / foreign-handle caught in inject_one)
         * cannot touch the command machinery (g_pend/scan state is capture-thread-
         * -owned); they raise g_scan_restore instead and the restore lands here.
         * Skipped if a link is still bound — scan stays off. */
        if(g_scan_restore){ g_scan_restore=0; if(none_bound) g_scan_want=2; }
        /* Link-policy reconcile PER bound link: pin sniff off within ~300ms of a
         * bind (handle change) and refresh every 10s while bound; never more often
         * than every 3s so a flap storm cannot flood the 0.5/s kernel drain (bursts
         * beyond that are additionally absorbed by cmd_send's unacked-queue cap). */
        ghost_ttl_refresh();   /* capture-thread-only reader; publishes g_ghost_ttl_ms */
        linkq_refresh();       /* capture-thread-only reader; publishes g_linkq_ms */
        ptype_refresh();       /* capture-thread-only reader; publishes g_ptype_want */
        gaplog_refresh();      /* capture-thread-only reader; publishes g_gaplog_want */
        gaplog_flush();        /* drain the per-gap ring (capture-thread-only) */
        for(int i=0;i<MAX_LINKS;i++){
            struct ds5_link *L=&g_links[i];
            if(pin[i]){
                uint64_t t=now_ms();
                int need = (L->policy_handle!=lh[i]) || (t-L->last_policy>10000);
                if(need && (!L->last_policy || t-L->last_policy>3000) &&
                   send_link_policy(lh[i])==0){
                    if(!lb[i] && L->policy_handle!=lh[i])
                        fprintf(stderr,"[txd] L%d idle sniff-pin handle=0x%03x (protects the active pad's airtime)\n",i,lh[i]);
                    L->last_policy=t; L->policy_handle=lh[i];
                }
                /* The policy pin cannot EXIT an already-established sniff (it only
                 * rejects future requests) — kick a sniffed pinned link back to
                 * active explicitly. Mode Change updates g_htab[].mode, so this
                 * sends at most once per actual sniff episode (plus the >=3s
                 * throttle while the exit is in flight). */
                if(md[i]==2 && t-L->last_unsniff>3000 && send_exit_sniff(lh[i])==0){
                    L->last_unsniff=t;
                    fprintf(stderr,"[txd] L%d handle=0x%03x in sniff while pinned -> Exit_Sniff sent\n",i,lh[i]);
                }
                /* Believed state is not observed state. When this handle's mode was
                 * never witnessed -- we started after the connection, or learned the
                 * identity from an assert -- "active" is an assumption, and the branch
                 * above can never fire because the assumption says there is nothing to
                 * exit. Ask the controller ONCE per binding instead: on a link that is
                 * genuinely active the command is simply refused (Command Status 0x0c)
                 * and costs one slot of the >=3s budget, while on a sniffed one it is
                 * the difference between 15 and 46 reports per second. A real Mode
                 * Change answers either way and sets mode_seen, so this cannot loop. */
                else if(!mdseen[i] && L->unsniff_probe_h!=lh[i] &&
                        t-L->last_unsniff>3000 && send_exit_sniff(lh[i])==0){
                    L->unsniff_probe_h=lh[i]; L->last_unsniff=t;
                    fprintf(stderr,"[txd] L%d handle=0x%03x mode never observed -> Exit_Sniff probe\n",i,lh[i]);
                }
                /* L1 reconcile: one Write + one Read per bind (or per toggle
                 * change), sharing the same >=3s throttle as everything else on
                 * this budget. The feature probe is asked once and only when
                 * somebody actually wants the lever — an unused daemon sends
                 * nothing it did not send before this change. */
                int want=flush_ms_want();
                if(want && g_lmp_nonflush<0 && t-L->last_flush_cmd>3000 &&
                   send_read_local_features()==0)
                    L->last_flush_cmd=t;
                else if(g_lmp_nonflush==1 || (!want && L->flush_sent_ms>0)){
                    /* Disarming does not need the feature bit: writing the
                     * infinite timeout back must work even on a controller we
                     * later decided we cannot trust, or an A/B could not be
                     * reversed without a restart. */
                    int target = g_lmp_nonflush==1 ? want : 0;
                    /* What this HANDLE is believed to carry right now. A handle
                     * we have not written to is infinite by definition (that is
                     * the spec default for a fresh connection), so a disarmed
                     * lever costs zero commands per bind instead of one
                     * redundant "set infinite" on every reconnect. */
                    int believed = (L->flush_handle==lh[i]) ? L->flush_sent_ms : 0;
                    if(target!=believed &&
                       t-L->last_flush_cmd>3000 && send_auto_flush(lh[i],target)==0){
                        L->last_flush_cmd=t; L->flush_handle=lh[i]; L->flush_sent_ms=target;
                        g_flush_readback_pending=1;
                        fprintf(stderr,"[txd] L%d flush: Write_Automatic_Flush_Timeout handle=0x%03x "
                                       "-> %d ms (%u slots), verifying by read-back\n",
                                i,lh[i],target,(unsigned)(target?FLUSH_SLOTS(target):0));
                    } else if(g_flush_readback_pending && t-L->last_flush_cmd>3000 &&
                              send_read_auto_flush(lh[i])==0){
                        L->last_flush_cmd=t;
                    }
                }
                /* L18 reconcile: one Change_Connection_Packet_Type per bind or
                 * per toggle flip, same believed/sent pattern and the same >=3s
                 * budget as the flush lever.
                 *
                 * Believed-state RULES (learned live 2026-08-16, first armed
                 * night): the controller LATCHES the mask for the lifetime of
                 * the ACL connection, and there is no read-back command for it
                 * — so our ptype_sent is the only record of what the link
                 * carries, and erasing it wrongly means a session can run
                 * silently clamped (which poisons every OFF baseline).
                 *   - ptype state survives pin flaps (the idle-invalidate reset
                 *     used to erase it between the user's run and the disarm:
                 *     the restore then compared ALL==ALL and never fired,
                 *     while the link still carried the clamp).
                 *   - Once the lever has been armed this run (g_ptype_ever), an
                 *     UNKNOWN handle converges to the target with one command
                 *     instead of being assumed default — cheap insurance, only
                 *     paid by sessions that actually use the lever.
                 *   - A run that never arms keeps the zero-commands-per-bind
                 *     contract. Residual (accepted, documented): clamp armed in
                 *     run N + daemon restart + the same ACL connection
                 *     surviving into run N+1 which never arms — no record left
                 *     anywhere to know the mask; a pad reconnect clears it. */
                if(!g_ptype_refused){
                    int ptarget = g_ptype_want ? PTYPE_CLAMP : PTYPE_ALL;
                    int pbelieved = (L->ptype_handle==lh[i]) ? L->ptype_sent
                                    : (g_ptype_ever ? -1 : PTYPE_ALL);
                    if(ptarget!=pbelieved && t-L->last_ptype_cmd>3000 &&
                       send_chg_ptype(lh[i],(uint16_t)ptarget)==0){
                        L->last_ptype_cmd=t; L->ptype_handle=lh[i]; L->ptype_sent=ptarget;
                        fprintf(stderr,"[txd] L%d L18 ptype: handle=0x%03x -> 0x%04x (%s), "
                                       "witness = 0x1D event + gapge distribution\n",
                                i,lh[i],ptarget,g_ptype_want?"clamp":"full");
                    }
                }
                /* L16 poll: at most ONE read per tick, rotating over the four,
                 * and only while the command queue has room to spare. Both
                 * limits exist for the same reason — the scan reconciler and the
                 * sniff pin share this queue, and they are load-bearing while
                 * this instrument is merely curious. */
                if(g_linkq_ms && g_pend_n<LINKQ_PEND_HEADROOM &&
                   t-L->last_linkq>(uint64_t)g_linkq_ms){
                    static const uint16_t rot[4]={ OP_READ_LINK_QUALITY, OP_READ_RSSI,
                                                   OP_READ_AFH_MAP, OP_READ_FAILED_CONTACT };
                    /* Skip opcodes this controller has already refused. If all
                     * four are refused the loop falls through having sent
                     * nothing, which costs four bitmask tests per tick and
                     * leaves last_linkq untouched — cheap enough not to warrant
                     * a separate all-dead latch. */
                    for(int k=0;k<4;k++){
                        uint16_t op=rot[L->linkq_rot&3];
                        int bit=1<<(L->linkq_rot&3);
                        L->linkq_rot++;
                        if(g_linkq_off&bit) continue;
                        /* Stamped whether or not the send took: a refused write
                         * means the command path is busy, and the right response
                         * from a curious instrument is to wait a full interval,
                         * not to retry on the next capture pass. */
                        L->last_linkq=t;
                        send_status_read(op,lh[i]);
                        break;
                    }
                }
            /* ptype state deliberately NOT reset here: pin loss is routinely
             * just idle-invalidate, the ACL connection (and its latched mask)
             * outlives it, and this reset is exactly what ate the disarm on
             * 08-16. A genuinely new handle is caught by the
             * ptype_handle==lh[i] check in the reconcile above. */
            } else { L->policy_handle=0xffff; L->flush_handle=0xffff; L->flush_sent_ms=-1; }
        }
        /* Scan follows the SESSION, not electrical liveness. The three restores above
         * are edge-triggered on a link INVALIDATION, and since RX liveness (v11) a pad
         * that stays powered on after the session keeps `have` set forever — so
         * none_bound never became true and page/inquiry scan stayed suppressed until
         * the pad disconnected (the documented contract at Write_Scan_Enable is
         * "restored when NO link is bound so devices can (re)connect outside
         * sessions"). Level-drive it from session_seen instead: off while output is
         * flowing, back to mode 2 after SESSION_IDLE_MS of output silence. The
         * template is NOT touched, so a resuming session just flips want back to 0
         * (>=SCAN_MIN_GAP_MS later) with no rebind churn. Never acts before the first
         * bind (want==0xff = no opinion on the TV's scan state). */
        if(g_scan_want!=0xff){
            uint8_t want=sess_active?0:2;
            if(want!=g_scan_want){
                g_scan_want=want;
                fprintf(stderr,"[txd] session %s -> scan want=%u\n",sess_active?"active":"idle",want);
            }
        }
        /* Converge the actual scan mode toward the desired one (shared limiter). */
        scan_reconcile();
        /* Command health: no ON-AIR Command Complete (monitor-observed) for the
         * OLDEST unacked command by its enqueue-time deadline (6s + 2s per queue
         * position — the kernel drains 1 cmd / 2s in user-channel mode, so a
         * burst legitimately takes that long; a fixed 6s deadline used to
         * false-trip on a mere template-flap burst of 3 commands). Miss = enter
         * the DEAD state before retries fill the sndbuf. */
        if(!g_cmd_dead && g_pend_n>0 && now_ms()>g_pend[0].deadline){
            cmd_guard_trip("no response to",g_pend[0].op);
        }
        /* Recovery probe: DEAD is not terminal. Every 60s send ONE command past
         * the gate — the currently-desired scan mode, so a successful recovery
         * also reconciles scan state. Pre-probe the pend queue holds only dead-era
         * junk (nothing else is sent while dead); drop it so 8 failed probes can
         * never wedge the queue cap and stop probing forever. */
        if(g_cmd_dead && now_ms()-g_cmd_dead_t>60000){
            g_cmd_dead_t=now_ms(); g_pend_n=0;
            uint8_t mode=(g_scan_want!=0xff)?g_scan_want:2;
            uint8_t cmd[5]={ 0x01,
                OP_WRITE_SCAN_ENABLE&0xff, OP_WRITE_SCAN_ENABLE>>8, 1, mode };
            if(cmd_send(cmd,sizeof cmd,OP_WRITE_SCAN_ENABLE,0,1)==0){
                g_probe_op=OP_WRITE_SCAN_ENABLE;
                g_scan_tx=now_ms(); g_scan_sent=mode;
            }
        }

        struct iovec iov={.iov_base=buf,.iov_len=sizeof buf};
        char cbuf[64];
        struct msghdr msg={0}; msg.msg_iov=&iov; msg.msg_iovlen=1;
        msg.msg_control=cbuf; msg.msg_controllen=sizeof cbuf;
        int r=(int)recvmsg(mfd,&msg,0);
        if(r<0){
            /* EAGAIN/EWOULDBLOCK is the normal 300ms SO_RCVTIMEO tick, EINTR a signal;
             * any other errno is a real socket error -> back off so we never busy-spin. */
            if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) usleep(50000);
            continue;
        }
        /* Queue-time stamp of THIS packet, wall-clock ms; 0 = cmsg missing. */
        uint64_t kms=0;
        for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg); c; c=CMSG_NXTHDR(&msg,c))
            if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SO_TIMESTAMP){
                struct timeval ktv; memcpy(&ktv,CMSG_DATA(c),sizeof ktv);
                kms=(uint64_t)ktv.tv_sec*1000ull+(uint64_t)ktv.tv_usec/1000ull;
            }
        if(r<(int)sizeof(struct hci_mon_hdr)) continue;
        struct hci_mon_hdr*h=(struct hci_mon_hdr*)buf;
        if(h->index!=TARGET_HCI_INDEX) continue;   /* track only the controller we inject on */
        uint8_t*d=buf+sizeof(struct hci_mon_hdr); int dl=h->len;
        if(r<(int)(sizeof(struct hci_mon_hdr)+dl)) continue;
        if(h->opcode==MON_COMMAND_PKT){
            /* Event-driven scan-off: the LG stack re-enables page/inquiry scan on
             * its own (measured: scan blackouts leak back within seconds). Instead
             * of a blind 1Hz reassert we watch the command stream: a foreign
             * Write_Scan_Enable with mode!=0 while we want it off just marks our
             * sent-state unknown — the reconciler re-sends mode 0 under the shared
             * >=3s limiter on the next pass. Zero commands from us unless someone
             * actually flips scan back on; our own writes while want==0 are mode=0
             * and can't retrigger (and our mode-2 restores run while want==2).
             * Payload: opcode(2LE), plen(1), params. */
            if(dl>=4 && (uint16_t)(d[0]|(d[1]<<8))==OP_WRITE_SCAN_ENABLE && d[3]!=0 &&
               g_scan_want==0){
                if(g_scan_sent==0) g_scan_ctr++;   /* count fights, not every burst packet */
                g_scan_sent=0xff;
            }
            continue;
        }
        if(h->opcode==MON_EVENT_PKT){ handle_hci_event(d,dl,kms); continue; }
        if(h->opcode==MON_ACL_RX_PKT){
            /* Inbound ACL from a BOUND handle proves that link is alive: a
             * connected DS5 streams input reports continuously (250+/s), so RX
             * liveness carries the template across output-idle spells (game
             * pauses, the host-side 0x36 idle gate). Without this, >1.5s
             * without OUTBOUND traffic invalidated a healthy link and restored
             * page scan MID-SESSION; the next output burst then hit a cold,
             * scanning link (observed 2026-07-20: EPISODE stalls up to 671ms,
             * lost rumble, rebind churn nonce++ with inj frozen). A real flap
             * stops RX too, so the backstop's flap purpose is preserved; lost-
             * DISCONN handle contamination is still caught by the CONN/DISCONN
             * handlers and the identity re-bind path, same as for NOCP refresh. */
            if(dl>=2){
                uint16_t hh=(uint16_t)((d[0]|(d[1]<<8))&0x0fff);
                pthread_mutex_lock(&g_lock);
                struct ds5_link *L=link_by_handle(hh);
                if(L){ L->last_seen=now_ms(); L->rx_pkts++; }
                else g_other_rx++;   /* foreign links (Magic-Remote LE etc.) — the coex canary */
                pthread_mutex_unlock(&g_lock);
            }
            continue;
        }
        if(h->opcode!=MON_ACL_TX_PKT) continue;
        if(dl<10) continue;
        uint8_t pb=(uint8_t)((d[1]>>4)&0x3); if(pb==1) continue;
        if(d[8]!=0xA2 || !injectable(d[9])) continue;
        uint16_t acl_len=(uint16_t)(d[2]|(d[3]<<8)), l2_len=(uint16_t)(d[4]|(d[5]<<8));
        if(acl_len!=(uint16_t)(dl-4)) continue;
        if(l2_len !=(uint16_t)(acl_len-4)) continue;

        uint16_t hh=(uint16_t)((d[0]|(d[1]<<8))&0x0fff);
        int did_capture=0, need_learn=0; unsigned cid=0; uint16_t nonce=0; int slot=-1;
        pthread_mutex_lock(&g_lock);
        /* Route this on-air HID-output to a link. Three cases:
         *   1. Already bound to a link on THIS handle -> refresh; re-capture only if
         *      the header (CID) actually changed (a same-handle re-bind).
         *   2. Not bound on this handle, identity known -> bind it: reuse a slot
         *      already holding this bdaddr (a flap/rehandle), else a FREE slot. This
         *      is the multi-controller change — a 2nd DS5's handle no longer gets
         *      ignored as "foreign"; it takes its own slot. Beyond MAX_LINKS slots a
         *      further device finds none free and is ignored (never flip-flops a
         *      bound link — the anti-contamination property for over-capacity).
         *   3. Identity unknown -> fail CLOSED: keep seeding via hidraw until learned. */
        struct ds5_link *L=link_by_handle(hh);
        if(L){
            L->last_seen=now_ms();
            /* On-air HID-OUTPUT is session traffic too (this is the kernel/hidraw path:
             * the app seeding before readiness flips, or hid-playstation itself). Only
             * a host drives output to a pad, so unlike inbound input reports this does
             * mean somebody is using the controller — a hidraw-only session must keep
             * scan off exactly as it does today. Our own raw injects are not mirrored
             * on MONITOR, so THIS path cannot self-refresh — but the raw path stamps
             * session_seen in inject_one() itself, which is why that stamp is gated
             * on from_app: the idle-lightbar painter's keep-alives (period ==
             * SESSION_IDLE_MS) would otherwise self-refresh the session forever. */
            L->session_seen=L->last_seen;
            int changed=(L->hdr[0]!=d[0]||L->hdr[1]!=d[1]||L->hdr[6]!=d[6]||L->hdr[7]!=d[7]);
            if(changed && g_htab[hh].known){
                link_bind(L,d,hh);
                did_capture=1; slot=(int)(L-g_links); cid=(unsigned)(d[6]|(d[7]<<8)); nonce=L->nonce;
            }
        } else if(g_htab[hh].known){
            /* Identity confirmed: bind the DS5 on this handle to a slot and go VALID.
             * By the 0xA2 + 0x31/0x32/0x36 content filter above, that device IS a
             * DualSense, so any later reassignment of this handle to a different
             * bdaddr is a contamination event we reject (event handlers + inject_one). */
            L=slot_for_addr(g_htab[hh].addr);  /* same device / prior slot / free slot */
            if(L){
                link_bind(L,d,hh);
                did_capture=1; slot=(int)(L-g_links); cid=(unsigned)(d[6]|(d[7]<<8)); nonce=L->nonce;
            }
            /* else: all slots hold OTHER devices -> ignore this over-capacity pad. */
        } else {
            /* Identity unknown. Before failing closed, try the app's identity
             * ASSERT (restart fix): these on-air bytes are a content-proven DS5
             * output; an exact match against a jail-asserted report ties handle
             * hh to that pad's bdaddr even though no HCI connect event exists. */
            int am=assert_match(d+9,dl-9);
            /* An address can live on exactly ONE handle. If the asserted address is
             * already bound elsewhere, these on-air bytes are NOT that pad's — they
             * are a second DualSense whose (independently sequenced) report happened
             * to be byte-identical to a live assertion. Binding here would drag the
             * asserting pad's link onto the wrong handle and cross-route its haptics.
             * Fail closed instead: the other pad keeps seeding until it asserts bytes
             * of its own. (The ambiguity check inside assert_match only covers the
             * case where BOTH pads asserted the same bytes.) */
            if(am>=0 && link_by_addr(g_assert[am].addr)) am=-2;
            /* Demand ASSERT_CONFIRM consecutive matches of this (handle -> address)
             * before trusting the identity; one collision keeps seeding (need_learn). */
            if(am>=0 && !assert_confirm(hh,g_assert[am].addr)) am=-1;
            if(am>=0){
                memcpy(g_htab[hh].addr,g_assert[am].addr,6); g_htab[hh].known=1;
                g_htab[hh].mode=0;          /* fresh identity, no Mode Change seen: never
                                             * inherit a stale sniff flag from a previous
                                             * occupant of this 12-bit handle */
                g_htab[hh].mode_seen=0;     /* ...and do not pretend we know it either */
                g_htab[hh].from_assert=1;   /* jail-sourced identity: taint the htab entry
                                             * so this and any rebind off hh stay
                                             * assert_learned until a kernel connect (defence
                                             * (b) must not be silenced by a lied addr) */
                L=slot_for_addr(g_htab[hh].addr);
                if(L){
                    link_bind(L,d,hh);   /* inherits assert_learned=1 via from_assert */
                    did_capture=2; slot=(int)(L-g_links); cid=(unsigned)(d[6]|(d[7]<<8)); nonce=L->nonce;
                }
            } else {
                /* Fail CLOSED: bdaddr unknown (no assert / ambiguous) -> do NOT
                 * publish valid. The app keeps seeding via hidraw (safe) until
                 * we learn the identity. */
                need_learn=1;
            }
        }
        pthread_mutex_unlock(&g_lock);
        if(did_capture){
            publish_all();
            /* Radio hygiene via the reconcilers (next loop pass, <=300ms): the
             * link-policy pin fires on the handle change, scan-off on want=0.
             * Direct sends here were unthrottled — a flap storm (bind/invalidate
             * cycling) could beat the 0.5/s kernel drain and overflow the sndbuf. */
            g_scan_want=0;
            if(slot>=0) g_links[slot].policy_handle=0xffff;   /* force a re-pin for this link */
            fprintf(stderr,"[txd] L%d template handle=0x%03x CID=0x%04x nonce=%u bound%s (sniff-off+noscan pending)\n",
                    slot,hh,cid,nonce,did_capture==2?" [identity from app assert]":"");
        }
        if(need_learn){
            /* Throttled refresh of the handle->bdaddr table for the case where the
             * DS5 was already connected before we started (no CONN_COMPLETE seen).
             * Once HCIGETCONNLIST fills g_htab[hh], the next report binds + goes valid. */
            uint64_t t=now_ms();
            if(t-last_learn>500){ last_learn=t; seed_conn_list(); }
        }
    }
}

/* Route + inject one inbound report onto its link (main/inject thread). report/n
 * is the raw HID-output (tag already stripped). Preserves the exact rumble-first /
 * audio-FIFO ordering the single-link path had, now scoped to link L. snap_nonce is
 * L's template generation this wakeup (stamps held FIFO frames). */
static void process_report(struct ds5_link *L, int rawfd, const uint8_t *report, int n,
                           uint16_t snap_nonce, int fdepth, int maxq, const uint8_t *expect,
                           long *injected, long *dropped, long *paced){
    int need_inval=0; const char *reason=NULL; int r;
    if(!is_audio_report(report[0])){
        /* Rumble FIRST, before the audio backlog: it is an independent stream (its
         * ordering vs audio does not matter), and draining held audio ahead of it
         * would hand every freed credit to audio within the same wakeup — starvation
         * by ordering, on top of the type-aware window in inject_one(). Rumble itself
         * stays latest-wins on a full window: a stale rumble is wrong. */
        r=inject_one(L,rawfd,report,n,maxq,&reason,expect,1);
        if(r==1){(*injected)++; L->inj_total++;}
        else if(r==-1)need_inval=1;
        else if(r==0)(*paced)++;         /* latest-wins drop */
        else {(*dropped)++; L->drop_total++;}   /* r==-2: no live template */
        if(!need_inval)(*injected)+=drain_fifo(L,rawfd,maxq,&need_inval,&reason,expect);
    } else {
        /* Audio: strict FIFO order — backlog first, then the fresh frame. On a full
         * window with the FIFO enabled the fresh frame is HELD (few-ms delay, drained
         * on the next arrival) rather than punching a ~10ms hole; FIFO-off stays
         * latest-wins. */
        (*injected)+=drain_fifo(L,rawfd,maxq,&need_inval,&reason,expect);
        if(need_inval){
            (*dropped)++; L->drop_total++;   /* template just died; fresh frame is moot */
        } else {
            r=inject_one(L,rawfd,report,n,maxq,&reason,expect,1);
            if(r==1){(*injected)++; L->inj_total++;}
            else if(r==-1){ need_inval=1; fifo_clear(L); }
            else if(r==0){
                if(fdepth>0 && n<=FIFO_ENTRY_MAX){
                    /* Evict-to-fit: fdepth can be LOWERED at runtime; a single-evict
                     * would balance every insert and keep the count at the old
                     * high-water forever under congestion, voiding the latency bound. */
                    while(L->fifo_count>=fdepth){ L->fifo_head=(L->fifo_head+1)%FIFO_MAX; L->fifo_count--;
                                                  (*dropped)++; L->drop_total++; L->drop_ovf++; }
                    int tail=(L->fifo_head+L->fifo_count)%FIFO_MAX;
                    memcpy(L->fifo[tail].buf,report,(size_t)n); L->fifo[tail].len=n;
                    L->fifo[tail].ts=now_ms(); L->fifo_count++;
                    L->fifo_gen=snap_nonce;   /* held under THIS binding */
                } else {
                    (*paced)++;              /* legacy latest-wins drop */
                }
            }
            else {(*dropped)++; L->drop_total++;}   /* r==-2: no live template */
        }
    }
    if(need_inval){
        publish_all(); fprintf(stderr,"[txd] %s\n",reason);
        /* Scan restore for MAIN-thread invalidations: this thread must not touch the
         * capture-owned command machinery, so it only raises the flag; the capture
         * loop sends the mode-2 restore (and ignores it if a link is still bound or a
         * new session rebinds first). */
        g_scan_restore=1;
    }
}

int main(int argc,char**argv){
    /* FIRST statement: a peer that closes or shuts down its read end before we
     * finish replying must never take the ROOT daemon down with it. The broker
     * writes a status byte back on every accepted connection — including the
     * REJECTION path, so any local process could kill us by connecting and
     * shutting down. We have no use for SIGPIPE on any socket we own; the
     * write paths already handle EPIPE via their errno branches. This cannot be
     * inherited from the launcher either: libuv's uv__process_child_init resets
     * every disposition to SIG_DFL before exec. */
    signal(SIGPIPE, SIG_IGN);

    const char *sock_path = argc>1?argv[1]:"/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl.sock";
    g_tmpl_path           = argc>2?argv[2]:"/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl_tmpl";
    const char *hidfd_path= argc>3?argv[3]:"/var/palm/jail/com.aurora.gamestream/tmp/ds5_hidfd.sock";

    /* argv[4]: the jail uid allowed to drive us, passed by ds5-tmpld.sh which
     * reads it off the app's jail directory — an app-ID change then carries
     * through without a rebuild. Anything unparsable, out of range or 0 keeps
     * the built-in default: this is a security gate (every peer it admits gets
     * RW access to an allowlisted pad's hidraw), so a bad argument must never
     * widen it. Parsed before any thread exists, so g_jail_uid is settled by
     * the time a socket can accept. */
    if (argc > 4) {
        char *end = NULL;
        errno = 0;
        unsigned long v = strtoul(argv[4], &end, 10);
        if (errno == 0 && end && end != argv[4] && *end == '\0' && v > 0 && v <= 65534) {
            g_jail_uid = (uid_t)v;
        } else {
            fprintf(stderr, "[txd] unusable jail uid '%s' -> accepting root only\n", argv[4]);
        }
    }
    if (g_jail_uid) {
        fprintf(stderr, "[txd] accepting peer uid %u (and root)\n", (unsigned)g_jail_uid);
    } else {
        fprintf(stderr, "[txd] no jail uid given -> accepting root ONLY\n");
    }

    /* Leave the MAIN thread's comm at its default (argv[0] basename "ds5_txd"): the
     * launcher and management scripts (enforce_single.sh, revert_until_up.sh, etc.)
     * identify the daemon with `pkill -x ds5_txd`, which matches the comm — renaming
     * main would silently break every one of them. Only the worker threads are named
     * (ds5-cap/ds5-brk, per audit O3) so the respawner can still target them by TID. */
    /* Real-time scheduling, and say out loud whether we got it. This daemon is the
     * last hop before the air: it services the pad's NOCP credits and injects at
     * ~94/s, so every stretch it is descheduled for shows up as a hole in the
     * controller's audio. It has asked for SCHED_FIFO 14 since the first commit —
     * and, measured 2026-08-15, has been running SCHED_OTHER the whole time: ls-hubd
     * starts us with RLIMIT_RTPRIO 0, the call fails, and nobody noticed because the
     * result was thrown away. The gap histogram below was reporting the consequence
     * (190 of 193 stall episodes classified TX-scheduling, not RF) with nothing to
     * connect it to.
     *
     * Raise the limit first — CAP_SYS_NICE alone should be enough, but if the
     * capability is not effective in our start context the setrlimit is what makes
     * the difference — then fall back to the deepest niceness we can get, which is
     * what the app does when its own attempt is refused. */
    struct rlimit rl_rt;
    if(getrlimit(RLIMIT_RTPRIO,&rl_rt)==0 && rl_rt.rlim_max<14){
        struct rlimit want={.rlim_cur=14,.rlim_max=14};
        if(setrlimit(RLIMIT_RTPRIO,&want)!=0)
            fprintf(stderr,"[txd] RLIMIT_RTPRIO stays %llu (%s)\n",
                    (unsigned long long)rl_rt.rlim_max,strerror(errno));
    }
    struct sched_param sp; memset(&sp,0,sizeof sp); sp.sched_priority=14;
    if(sched_setscheduler(0,SCHED_FIFO,&sp)==0){
        fprintf(stderr,"[txd] sched: SCHED_FIFO prio 14\n");
    }else{
        int e=errno;
        errno=0;
        int nice_rc=setpriority(PRIO_PROCESS,0,-20);
        int nice_now=getpriority(PRIO_PROCESS,0);
        fprintf(stderr,"[txd] sched: SCHED_FIFO DENIED (%s) -> SCHED_OTHER nice %d%s"
                       " — audio gaps below are expected to be scheduling, not RF;"
                       " boost us from the game-mode guard\n",
                strerror(e), (nice_rc==0||errno==0)?nice_now:0,
                nice_rc==0?"":" (renice also refused)");
    }

    /* Pin the file-creation mask: the readiness/telemetry records now take their
     * mode from open(...,0644) alone (write_record_atomic no longer fchmod()s —
     * that used to chmod a symlink's target), and the JAILED app (the jail uid) must be
     * able to read them, so an inherited umask must not strip o+r. */
    umask(0022);

    for(int i=0;i<MAX_LINKS;i++){ g_links[i].policy_handle=0xffff; g_links[i].last_nocp=now_ms();
                                  g_links[i].ptype_handle=0xffff; g_links[i].ptype_sent=-1; }

    /* L17b: pin every mapping. The NOCP handler timestamps gaps in user space;
     * with 470/600MB of swap in use on the TV a major fault on the capture
     * thread's path would be measured as a LINK stall. Locking is cheap (the
     * daemon's footprint is small and static) and turns that artefact class off.
     * Refusal is loud but not fatal — the daemon measured fine for months
     * without it, it just could not PROVE the gaps were the radio's. */
    if(mlockall(MCL_CURRENT|MCL_FUTURE)==0)
        fprintf(stderr,"[txd] mlockall: resident\n");
    else
        fprintf(stderr,"[txd] mlockall REFUSED (%s) — swap pressure can inflate measured gaps\n",
                strerror(errno));

    publish_record(g_tmpl_path,0,0,NULL);   /* start INVALID so a stale boot file can't mislead the app */
    invalidate_stale_addr_files();          /* same for per-address files of a previous run */
    seed_conn_list();    /* identity-bind a controller already connected at startup */

    /* Root HCI raw socket for injection (write is permitted as root). Bound to the
     * SAME controller index we track on MONITOR (TARGET_HCI_INDEX). */
    int rawfd=socket(AF_BLUETOOTH,SOCK_RAW,BTPROTO_HCI);
    if(rawfd<0){ perror("[txd] socket raw"); return 1; }
    struct sockaddr_hci ra; memset(&ra,0,sizeof ra);
    ra.hci_family=AF_BLUETOOTH; ra.hci_dev=TARGET_HCI_INDEX; ra.hci_channel=HCI_CHANNEL_RAW;
    if(bind(rawfd,(struct sockaddr*)&ra,sizeof ra)<0){ perror("[txd] bind raw"); return 1; }
    int fl=fcntl(rawfd,F_GETFL,0); if(fl>=0) fcntl(rawfd,F_SETFL,fl|O_NONBLOCK);
    g_rawfd=rawfd;   /* published before the capture thread starts (policy writes) */

    /* AF_UNIX datagram socket the jailed app sends reports to (non-blocking, 1MB
     * recv buffer, SO_PASSCRED — see bind_unix_dgram). Self-healed across remounts.
     * A failed FIRST bind is the same transient condition the self-heal below already
     * handles (jail tmp momentarily absent mid-relaunch, fd exhaustion at startup), so
     * start with ufd=-1 and let the 500ms tick retry instead of exiting into the
     * supervisor's respawn loop. poll() ignores a pollfd with fd<0, so this waits. */
    int ufd=bind_unix_dgram(sock_path);
    if(ufd<0) perror("[txd] bind unix (retrying every 500ms)");

    /* Before the capture thread starts, so it never sees a half-set fd. On
     * failure g_kickfd stays -1: poll() ignores a negative fd, the capture side
     * skips the write, and the drain falls back to the datagram-clocked path —
     * i.e. exactly the old behaviour, never a broken one. */
    g_kickfd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(g_kickfd<0) fprintf(stderr,"[txd] eventfd failed errno=%d -> credit drain stays datagram-clocked\n",errno);

    pthread_t cap; pthread_create(&cap,NULL,capture_thread,NULL);
    pthread_t brk; pthread_create(&brk,NULL,broker_thread,(void*)hidfd_path);
    {   /* Idle lightbar BOOT colour: DS5_IDLE_LIGHTBAR=RRGGBB (hex), "0"/"off"
         * disables the painter for good — the app's ctrl-0x02 selection can
         * never override an operator-disabled painter (idle_lb_effective). */
        const char *e=getenv("DS5_IDLE_LIGHTBAR");
        if(e && *e){
            if(!strcasecmp(e,"off")) g_idle_lb_boot=0;
            else { char *end=NULL; unsigned long v=strtoul(e,&end,16);
                   if(end && *end=='\0' && v<=0xFFFFFFul) g_idle_lb_boot=(uint32_t)v;
                   else fprintf(stderr,"[txd] DS5_IDLE_LIGHTBAR='%s' not RRGGBB hex -> keeping %06x\n",e,g_idle_lb_boot); }
        }
    }
    fprintf(stderr,"[txd] forwarder up (cmdguard, %d links): unix=%s tmpl=%s hidfd=%s idle_lb=%06x\n",MAX_LINKS,sock_path,g_tmpl_path,hidfd_path,g_idle_lb_boot);

    /* Event loop: wait on forwarded reports (ufd) AND mount-table changes (minfo)
     * at once, on a 500ms tick.
     *
     * This used to be poll(-1) in steady state, with a tick only while a rebind
     * was pending or the mount watch was down. That cannot work for anything
     * that must act on state OTHER threads produce: a link is bound by the
     * CAPTURE thread, so with no app feeding us the main loop sat in poll(-1),
     * never woke, and never noticed the pad had connected at all. Gating the
     * tick on "a link is bound" does not fix it either -- observing that is
     * itself something only a wakeup can do. So: always tick. Two wakeups a
     * second doing a locked snapshot of MAX_LINKS is nothing, and under load
     * poll returns on data long before the timeout ever expires. */
    int minfo=open_mount_watch();   /* self-heal ds5_acl.sock across jail-tmp remounts */
    uint8_t rep[ACL_TAG_LEN+ACL_MAX_REPORT];   /* tag (optional) + report; inject framing in inject_one() */
    long injected=0, dropped=0, paced=0; uint64_t last_log=now_ms();
    /* Per-reason breakdown of `dropped` (which stays the total, inject-path drops
     * included). One shared counter could not tell a wrong jail uid from an
     * oversized datagram from a pad that never bound — all three present as
     * "drop climbing, no audio", and only the first is a misconfiguration. */
    long d_cred=0, d_trunc=0, d_badlen=0, d_noninj=0, d_nolink=0, d_ambig=0;
    uint64_t last_cred_log=0;   /* 1/s rate limit for the cred-rejection line */
    uint64_t last_stat=now_ms(); uint32_t stat_seq=0;   /* queue-telemetry publish (v9) */
    for(;;){
        /* pfd[2] is the NOCP kick (g_kickfd): the capture thread posts it when a
         * credit frees on a link that is holding audio, so the backlog drains on
         * the credit instead of waiting for the next app datagram. */
        struct pollfd pfd[3]={{ufd,POLLIN,0},{minfo,POLLPRI,0},{g_kickfd,POLLIN,0}};
        int to=500;
        int pr=poll(pfd,3,to);
        if(pr<0){ if(errno==EINTR) continue; usleep(2000); continue; }
        if(g_kickfd>=0 && (pfd[2].revents&POLLIN)){
            uint64_t kv; ssize_t kr=read(g_kickfd,&kv,sizeof kv);
            (void)kr;   /* level-triggered counter: one read clears it */
        }
        gap_inject_tick();   /* bench instrument: close/arm synthetic TX holds */

        /* Mount table changed (or retry tick while rebinding / watch down): if our
         * node's path stopped resolving to a socket, Aurora remounted its tmp under
         * us — bind a fresh node into the current top mount and re-publish readiness.
         * ufd<0 covers both a pending rebind AND a failed first bind, and it must be
         * checked SEPARATELY from node_alive: a node can exist at the path without us
         * owning it (a SIGKILLed instance's leftover, or one bind_unix_dgram left
         * behind after a later step failed), and keying on node_alive alone would then
         * poll a dead fd forever while the supervisor sees a live pid AND a live
         * socket — permanently and silently dead, worse than exiting to be respawned. */
        if(pr==0 || pfd[1].revents){
            if(minfo<0) minfo=open_mount_watch();           /* retry a previously-failed watch */
            else if(pfd[1].revents) rearm_mount_watch(minfo);
            if(ufd<0 || !node_alive(sock_path)){
                if(ufd>=0){ close(ufd); ufd=-1; }
                int nu=bind_unix_dgram(sock_path);
                if(nu>=0){
                    ufd=nu;
                    publish_all();   /* re-publish current state into the new mount */
                    fprintf(stderr,"[txd] ds5_acl.sock (re)bound + re-published: %s\n",sock_path);
                }
            }
        }

        /* Per-link radio-episode detector + sub-episode gap histogram (the A/B
         * acceptance signal). Outstanding credits with no NOCP for >80ms means the
         * controller is not getting airtime (WiFi-scan blackout, interference burst)
         * -- the invisible cause of "late but not lost" audio/input. Each link is
         * tracked independently so a 2-pad session shows which pad is starved. */
        uint16_t link_nonce[MAX_LINKS];
        {
            struct { int have, qd; uint64_t ln, ld, ss; uint16_t nonce; uint64_t rx; uint8_t addr[6];
                     int rst; long r30,r50,r80; } sn[MAX_LINKS];
            uint64_t nowm=now_ms(), other_now;
            pthread_mutex_lock(&g_lock);
            for(int i=0;i<MAX_LINKS;i++){
                sn[i].have=g_links[i].have; sn[i].qd=g_links[i].outstanding;
                sn[i].ln=g_links[i].last_nocp; sn[i].nonce=g_links[i].nonce;
                sn[i].rx=g_links[i].rx_pkts; sn[i].ld=g_links[i].last_demand;
                sn[i].ss=g_links[i].session_seen;   /* painter hidraw-quiet gate */
                memcpy(sn[i].addr,g_links[i].bound_addr,6);
                /* Per-binding telemetry: a rebind (nonce bump) starts a FRESH gap
                 * histogram so the 10s status line measures THIS binding, not the
                 * slot's whole lifetime — cumulative counts across rebinds/address
                 * swaps were useless for A/B deltas. The outgoing counts ride out in
                 * the snapshot and are logged after the unlock, so nothing is lost.
                 * The reset moved INSIDE the lock when the histogram became
                 * capture-thread-owned: doing it out here would race a NOCP that
                 * lands between the snapshot and the clear (silently dropping a bin,
                 * or worse resurrecting a previous binding's count). */
                sn[i].rst=0; sn[i].r30=sn[i].r50=sn[i].r80=0;
                if(g_links[i].tele_gen!=g_links[i].nonce){
                    sn[i].rst=1;
                    sn[i].r30=g_links[i].gap30; sn[i].r50=g_links[i].gap50; sn[i].r80=g_links[i].gap80;
                    g_links[i].gap30=g_links[i].gap50=g_links[i].gap80=0;
                    /* Cleared with the other bins: a rebind starts a fresh
                     * histogram, otherwise the A/B delta spans two bindings. */
                    memset(g_links[i].gapge,0,sizeof g_links[i].gapge);
                    g_links[i].gap_max=0;
                    g_links[i].gap_synth=0; g_links[i].gap_synth_ms=0;
                    g_links[i].tele_gen=g_links[i].nonce;
                }
            }
            other_now=g_other_rx;
            pthread_mutex_unlock(&g_lock);
            for(int i=0;i<MAX_LINKS;i++){
                struct ds5_link *L=&g_links[i];
                link_nonce[i]=sn[i].nonce;
                /* The counters themselves were already cleared under the lock above;
                 * only the logging and the main-thread-owned episode state happen
                 * here. */
                if(sn[i].rst){
                    if(sn[i].r30||sn[i].r50||sn[i].r80)
                        fprintf(stderr,"[txd] L%d gap histogram reset on rebind (was %ld/%ld/%ld)\n",
                                i,sn[i].r30,sn[i].r50,sn[i].r80);
                    L->ep_start=0;
                }
                /* Stale-backlog gate: a rebind bumps the nonce, so audio still held
                 * from the previous binding must be dropped, not played into the new
                 * session (drain_fifo also clears on the no-template path, but that
                 * never runs when the invalidate->rebind happens between wakeups). */
                if(L->fifo_count>0 && L->fifo_gen!=sn[i].nonce) fifo_clear(L);
                /* Credit-freed drain. The NOCP that returned a credit is what woke
                 * us (g_kickfd), so put the held frame on the air NOW instead of at
                 * the next app datagram — that wait was a systematic 0..21.33ms
                 * (mean ~10.7) added to every congestion recovery, right at the
                 * B+21.33ms underrun boundary. Unconditional rather than gated on
                 * the kick: once we are awake the drain is the same work the
                 * datagram path would do, and this way the 500ms tick also bounds
                 * how long a backlog can sit if a kick is ever missed.
                 * `expect` = this link's snapshotted address, so inject_one's
                 * re-check still skips a pad that the capture thread rebound
                 * between the snapshot and here. */
                if(L->fifo_count>0){
                    int kneed=0; const char *kreason=NULL;
                    injected+=drain_fifo(L,rawfd,inject_maxq(),&kneed,&kreason,sn[i].addr);
                    if(kneed){
                        publish_all();
                        fprintf(stderr,"[txd] %s\n",kreason?kreason:"template invalid (credit drain)");
                        g_scan_restore=1;
                    }
                }
                /* DEMAND GATE (2026-08-02). "queue non-empty + no NOCP" is only a STALL
                 * while the app is actually feeding us. On stream teardown the controller
                 * DISCARDS its queued packets without ever emitting the matching NOCPs, so
                 * `outstanding` never drains and the old condition latched: measured that
                 * evening, 155s of 204s total "stall time" was idle — including one 101s
                 * "stall" in which inject advanced by 18 packets. Gate both the episode and
                 * the histogram on recent demand so idle cannot masquerade as starvation.
                 * Keyed on demand, NOT on successful inject: a credit stall is precisely
                 * when injection stops, so the success signal would mute the real thing. */
                int tx_demand = sn[i].ld && (nowm - sn[i].ld) < DEMAND_IDLE_MS;
                /* Idle lightbar (2026-08-03). Same demand signal as the episode
                 * gate: it says whether OUR app is driving this pad. While
                 * nothing is, the bar belongs to the firmware — full-brightness
                 * blue, or the orange charge indication — and we repaint it dim.
                 * The moment demand returns the host owns the bar again and we
                 * stop touching it. But demand alone is blind to kernel/hidraw
                 * writers (see IDLE_LB_HIDRAW_QUIET_MS), so ALSO require on-air
                 * output silence via session_seen — which paints no longer
                 * self-refresh (inject_one's from_app). nowm predates the locked
                 * snapshot, so guard the unsigned subtraction against a
                 * session_seen the capture thread stamped in between. */
                uint32_t lbrgb=idle_lb_effective();
                if(lbrgb && sn[i].have && !tx_demand &&
                   nowm>sn[i].ss && nowm-sn[i].ss>=IDLE_LB_HIDRAW_QUIET_MS){
                    if(L->lb_idle_gen!=sn[i].nonce){   /* fresh binding -> fresh burst */
                        L->lb_idle_gen=sn[i].nonce; L->lb_paints=0; L->lb_last_paint=0;
                    }
                    /* A hidraw writer painted AFTER our last paint (the quiet gate
                     * just proved it stopped again): the bar shows the foreign
                     * colour, so re-arm the burst — otherwise the repaint waits
                     * out the remainder of the 30 s keep-alive. Mirrors the
                     * tx_demand re-arm below for the kernel-path writer class. */
                    if(sn[i].ss>L->lb_last_paint && L->lb_last_paint){
                        L->lb_paints=0; L->lb_last_paint=0;
                    }
                    uint64_t iv = (L->lb_paints<IDLE_LB_BURST) ? IDLE_LB_BURST_MS : IDLE_LB_KEEPALIVE_MS;
                    if(!L->lb_last_paint || nowm-L->lb_last_paint>=iv){
                        uint8_t lbrep[DS5_BT_OUT_LEN];
                        const char *why=NULL;
                        ds5_build_lightbar(lbrep,lbrgb,L->lb_seq++);
                        /* inject_one takes g_lock itself and re-checks the binding
                         * against the address we snapshotted, so a rebind between
                         * the snapshot and here cannot paint the wrong pad.
                         * from_app=0: a paint must never count as session output. */
                        int lr=inject_one(L,g_rawfd,lbrep,DS5_BT_OUT_LEN,inject_maxq(),&why,sn[i].addr,0);
                        /* Log the first paint of each idle stretch: whether this
                         * fires at all is the one thing that cannot be inferred
                         * from the outside. It needs a captured template, and the
                         * template comes only from an on-air output report -- so on
                         * a fresh connect it depends on the TV's own stack having
                         * written to the pad at least once. */
                        if(L->lb_paints==0)
                            fprintf(stderr,"[txd] L%d idle lightbar -> %06x (r=%d%s%s)\n",
                                    i,lbrgb,lr,why?" ":"",why?why:"");
                        L->lb_last_paint=nowm; L->lb_paints++;
                    }
                } else if(tx_demand){
                    /* App took over: re-arm so the next idle stretch bursts again
                     * (the host may have left the bar on its own colour). */
                    L->lb_paints=0; L->lb_last_paint=0;
                }
                if(sn[i].have && sn[i].qd>0 && sn[i].ln && tx_demand && nowm-sn[i].ln>80){
                    if(!L->ep_start){
                        L->ep_start=nowm; L->ep_rx0=sn[i].rx; L->ep_other0=other_now;
                        uint64_t t0=now_us();
                        fprintf(stderr,"[txd] L%d EPISODE start t=%llu q=%d\n",i,(unsigned long long)nowm,sn[i].qd);
                        logw_note(t0);   /* this write lands mid-stall — see g_logw_* */
                    }
                } else if(L->ep_start){
                    /* RX-continuity verdict for the stall window: rx = this pad's
                     * inbound ACL during the episode, orx = every other link's.
                     * rx flowing while TX starves = TX-scheduling; rx dead but orx
                     * alive = this link's RF; both dead = radio-global.
                     * "(idle)" = the demand gate ended it, i.e. the app stopped feeding
                     * mid-episode. Reported, never hidden, but NOT a link verdict. */
                    uint64_t t0=now_us();
                    fprintf(stderr,"[txd] L%d EPISODE end t=%llu dur=%dms rx=%llu orx=%llu%s%s\n",
                            i,(unsigned long long)nowm,(int)(nowm-L->ep_start),
                            (unsigned long long)(sn[i].rx-L->ep_rx0),
                            (unsigned long long)(other_now-L->ep_other0),
                            tx_demand?"":" (idle)",
                            /* An injected episode ends only AFTER the hold is
                             * released, so test the quiet window, not the hold. */
                            now_us()<g_gap_quiet_until_us?" SYNTH":"");
                    logw_note(t0);
                    L->ep_start=0;
                }
                /* (The sub-episode gap histogram used to be derived HERE, as a
                 * high-watermark of (nowm - last_nocp) sampled once per wakeup. It
                 * now lives in the capture thread's NOCP handler at native rate —
                 * see the gap30/50/80 field comment. Sampling it from this loop was
                 * quantized to the datagram cadence, which the batched 0x39 halved
                 * to ~47/s = ~21ms per bucket.) */
            }
        }
        if(ufd>=0 && (pfd[0].revents&POLLIN)){
            int maxq=inject_maxq();     /* per-wakeup constants (root-owned /tmp read ~1/s, never under g_lock) */
            int fdepth=inject_fifo();
            for(int drained=0; drained<DRAIN_CAP; drained++){
                struct iovec iov={.iov_base=rep,.iov_len=sizeof rep};
                union { char b[CMSG_SPACE(sizeof(struct ucred))]; struct cmsghdr a; } cmsgu;
                struct msghdr mh; memset(&mh,0,sizeof mh);
                mh.msg_iov=&iov; mh.msg_iovlen=1; mh.msg_control=cmsgu.b; mh.msg_controllen=sizeof cmsgu.b;
                ssize_t n=recvmsg(ufd,&mh,0);
                if(n<0){ if(errno==EINTR) continue; break; }   /* EAGAIN -> drained */
                if(n==0) continue;                             /* zero-length datagram: skip, keep draining */
                if(mh.msg_flags & MSG_TRUNC){ dropped++; d_trunc++; continue; } /* oversized: never inject a truncated frame */
                {   /* Peer-cred gate: jail uid only. Rate-limited to 1/s because a
                     * mismatched uid means EVERY report is refused (~94/s) — one line
                     * per second is enough to name the problem without drowning the log,
                     * and it mirrors the broker's rejection line so the two channels now
                     * fail the same way out loud. */
                    long puid;
                    if(!cred_ok(&mh,&puid)){
                        dropped++; d_cred++;
                        uint64_t t=now_ms();
                        if(t-last_cred_log>1000){
                            last_cred_log=t;
                            if(puid<0) fprintf(stderr,"[txd] report socket rejected a datagram with no SCM_CREDENTIALS (accepting uid %u or root)\n",(unsigned)g_jail_uid);
                            else       fprintf(stderr,"[txd] report socket rejected peer uid=%ld (accepting %u or root)\n",puid,(unsigned)g_jail_uid);
                        }
                        continue;
                    }
                }

                /* Route by tag kind (see ACL_TAG_*): an INJECT datagram goes to the
                 * link bound to that address; an ASSERT datagram only feeds the
                 * identity ring and is never injected; an untagged one (legacy/USB)
                 * goes to the link that is bound when EXACTLY ONE is — resolved by
                 * identity, not by slot number — and is refused otherwise. */
                /* Control datagram: consumed here, never routed to a link.
                 * n>=3 (not 4): the payload-less IDLE_LB_CLEAR is exactly 3 bytes. */
                if(n>=3 && rep[0]==ACL_TAG_M0 && rep[1]==ACL_TAG_CTRL){
                    if(rep[2]==ACL_CTRL_FIFO_DEPTH && n>=4){
                        int nv=(rep[3]==0xFF)?-1:(int)rep[3];
                        if(nv<=FIFO_MAX && nv!=g_fifo_override){
                            g_fifo_override=nv;
                            fprintf(stderr,"[txd] ctrl: audio-FIFO depth override -> %d\n",nv);
                        }
                    } else if(rep[2]==ACL_CTRL_IDLE_LB && n>=6){
                        /* App SELECTION only — never touches g_idle_lb_boot, so the
                         * operator's DS5_IDLE_LIGHTBAR (incl. "off") survives any
                         * app open/close cycle. Re-arm the bursts only when the
                         * EFFECTIVE colour moved (boot=off keeps the painter dead),
                         * so the change lands now, not at the next 30 s keep-alive. */
                        uint32_t nv=((uint32_t)rep[3]<<16)|((uint32_t)rep[4]<<8)|rep[5];
                        if(!g_idle_lb_app_set || nv!=g_idle_lb_app){
                            uint32_t was=idle_lb_effective();
                            g_idle_lb_app=nv; g_idle_lb_app_set=1;
                            uint32_t eff=idle_lb_effective();
                            if(eff!=was)
                                for(int i=0;i<MAX_LINKS;i++){ g_links[i].lb_paints=0; g_links[i].lb_last_paint=0; }
                            fprintf(stderr,"[txd] ctrl: idle lightbar app selection -> %06x (effective %06x)\n",nv,eff);
                        }
                    } else if(rep[2]==ACL_CTRL_IDLE_LB_CLEAR){
                        /* Drop the app selection; the boot default paints again.
                         * The client sends this for "connected, unused" instead of
                         * echoing a literal colour (see the 0x03 define). */
                        if(g_idle_lb_app_set){
                            uint32_t was=idle_lb_effective();
                            g_idle_lb_app_set=0;
                            uint32_t eff=idle_lb_effective();
                            if(eff!=was)
                                for(int i=0;i<MAX_LINKS;i++){ g_links[i].lb_paints=0; g_links[i].lb_last_paint=0; }
                            fprintf(stderr,"[txd] ctrl: idle lightbar app selection cleared -> boot %06x\n",eff);
                        }
                    }
                    continue;
                }
                const uint8_t *report; int rlen; struct ds5_link *L; const uint8_t *expect=NULL;
                uint8_t sole_addr[6];   /* backs `expect` on the untagged path (below) */
                int tagged = (n>=(ssize_t)(ACL_TAG_LEN+1) && rep[0]==ACL_TAG_M0 &&
                              (rep[1]==ACL_TAG_INJECT || rep[1]==ACL_TAG_ASSERT));
                if(tagged){
                    int is_assert=(rep[1]==ACL_TAG_ASSERT);
                    report=rep+ACL_TAG_LEN; rlen=(int)(n-ACL_TAG_LEN); expect=rep+2;
                    if(rlen<=0 || rlen>ACL_MAX_REPORT){ dropped++; d_badlen++; continue; }
                    if(!injectable(report[0])){ dropped++; d_noninj++; continue; }
                    if(is_assert){
                        /* Identity ASSERT: the app is seeding these exact bytes on
                         * hidraw right now; record them so the capture thread can
                         * match its on-air copy and learn handle->bdaddr (restart
                         * fix). Never injected — that is what keeps the readiness
                         * flip from putting the frame on air twice. */
                        if(rlen<=ASSERT_MAX){
                            pthread_mutex_lock(&g_lock);
                            g_assert[g_assert_next].ts=now_ms();
                            memcpy(g_assert[g_assert_next].addr,expect,6);
                            g_assert[g_assert_next].len=(uint16_t)rlen;
                            memcpy(g_assert[g_assert_next].buf,report,(size_t)rlen);
                            g_assert_next=(g_assert_next+1)%ASSERT_RING;
                            pthread_mutex_unlock(&g_lock);
                        }
                        continue;
                    }
                    pthread_mutex_lock(&g_lock);
                    L=link_by_addr(expect);
                    pthread_mutex_unlock(&g_lock);
                    if(!L){ dropped++; d_nolink++; continue; } /* no link for this target (not ready) */
                } else {
                    /* Legacy untagged -> the SINGLE bound link, resolved by identity.
                     * FAIL CLOSED as soon as identity matters: with more than one link
                     * bound an untagged sender cannot know WHICH pad it is about to
                     * drive. That is how an arming/output report meant for the pad the
                     * sender unbound lands on the OTHER pad, whose kernel driver is
                     * still bound (two writers on one L2CAP channel = the documented
                     * oscillation). Resolving to whoever holds SLOT 0 was wrong for a
                     * second reason: slot 0 is not an identity, and a single pad
                     * routinely sits in slot 1 (free_slot prefers a virgin slot,
                     * slot_for_addr keeps a returning pad on its old slot). Two real
                     * failure modes came out of that, neither of which was a wrong-pad
                     * write — inject_one() gates on L->have, so an unbound slot 0 was
                     * never written to. (a) ONE pad in slot 1: the BASE readiness record
                     * mirrored slot 0 and so read invalid, the client kept seeding
                     * hidraw and simply never got the raw-ACL bypass. (b) TWO pads
                     * bound: the base record read VALID off slot 0, so the client
                     * stopped seeding hidraw and sent — and the ambiguity refusal above
                     * then dropped every datagram, taking that pad off air with nothing
                     * to fall back to. Case (b) is what the fail-closed base record
                     * fixes. Carrying the resolved address in `expect` also
                     * arms inject_one()'s re-check, so a capture-thread rebind between
                     * resolve and inject skips instead of buzzing the wrong pad.
                     * Control datagrams are consumed above and stay untagged-friendly. */
                    int nbound;
                    pthread_mutex_lock(&g_lock);
                    L=sole_bound_link_locked(&nbound);
                    if(L) memcpy(sole_addr,L->bound_addr,6);
                    pthread_mutex_unlock(&g_lock);
                    /* nbound==0 is the NORMAL pre-bind state (the app is still
                     * hidraw-seeding), nbound>1 is a refusal — different counters. */
                    if(!L){ dropped++; if(nbound) d_ambig++; else d_nolink++; continue; }
                    expect=sole_addr;
                    report=rep; rlen=(int)n;
                    if(rlen<=0 || rlen>ACL_MAX_REPORT){ dropped++; d_badlen++; continue; }
                    if(!injectable(report[0])){ dropped++; d_noninj++; continue; }  /* only DS5 output reports */
                }

                int idx=(int)(L-g_links);
                /* stamp BEFORE process_report: demand exists even when the report is
                 * then paced away or blocked. Relaxed atomic because the capture
                 * thread's NOCP gap gate reads it (see the field comment).
                 * demand_since marks where the CURRENT continuous stretch began:
                 * re-stamped only when the previous demand is stale, so the gap
                 * gate can require demand to have held across a whole gap. */
                {   uint64_t dnow=now_ms();
                    uint64_t prev=__atomic_load_n(&L->last_demand,__ATOMIC_RELAXED);
                    if(!prev || dnow-prev>=DEMAND_IDLE_MS)
                        __atomic_store_n(&L->demand_since,dnow,__ATOMIC_RELAXED);
                    __atomic_store_n(&L->last_demand,dnow,__ATOMIC_RELAXED);
                    /* Audio recency is tracked separately: only 0x36/0x39 carry
                     * pad audio, and only audio can arm the gap histogram (see
                     * AUDIO_IDLE_MS — keepalives kept the demand gate open
                     * through game-silence pauses and silence was binned as
                     * link blackouts). */
                    if(report[0]==0x36 || report[0]==0x39){
                        uint64_t pa=__atomic_load_n(&L->last_audio,__ATOMIC_RELAXED);
                        if(!pa || dnow-pa>=AUDIO_IDLE_MS)
                            __atomic_store_n(&L->audio_since,dnow,__ATOMIC_RELAXED);
                        __atomic_store_n(&L->last_audio,dnow,__ATOMIC_RELAXED);
                    }
                }
                process_report(L,rawfd,report,rlen,link_nonce[idx],fdepth,maxq,expect,&injected,&dropped,&paced);
            }
        }
        if(now_ms()-last_log>10000){
            /* LEDGER FORMAT RULE: everything that existed here keeps its exact
             * spelling and position — `gaps=g30/g50/g80` above all. Every A/B
             * baseline in this project is a re-parse of these lines, so new
             * numbers are APPENDED, never substituted. (`gaps=` now counts at
             * native NOCP resolution instead of the old ~21ms-quantized sampling,
             * so it is not comparable across this build boundary — that is a
             * measurement change to note in the ledger, not a format change.) */
            /* 224 -> 416: the cumulative histogram adds up to ~120 chars per
             * link once it is populated, and snprintf would silently truncate
             * the tail fields rather than the new one. */
            char links[MAX_LINKS*416]; int lo=0; links[0]='\0';
            for(int i=0;i<MAX_LINKS;i++){
                struct ds5_link *L=&g_links[i];
                if(!L->ever_bound) continue;
                const uint8_t*a=L->bound_addr;
                /* Only present once the bench injector has actually been used, so
                 * an ordinary session's line is byte-for-byte the old one plus the
                 * new always-on fields. */
                char synth[48]; synth[0]='\0';
                if(L->gap_synth)
                    snprintf(synth,sizeof synth," synth=%ld/%llums",
                             L->gap_synth,(unsigned long long)L->gap_synth_ms);
                /* Self-describing edge:count pairs — the edges are a compile-time
                 * choice and a bare tuple would need the reader to have this file
                 * open. Emitted only once a bin is non-zero, so a quiet link's
                 * line does not grow a 100-character tail of zeros. */
                char gge[160]; int go=0; gge[0]='\0';
                if(L->gapge[0]){
                    go+=snprintf(gge+go,sizeof gge-go," gapge=");
                    for(int b=0;b<GAPGE_N && go<(int)sizeof gge;b++)
                        go+=snprintf(gge+go,sizeof gge-go,"%s%u:%ld",
                                     b?"/":"",GAPGE_EDGE[b],L->gapge[b]);
                }
                lo+=snprintf(links+lo,sizeof links-lo,
                    " | L%d %02x:%02x:%02x:%02x:%02x:%02x have=%d q=%d rq=%d fifo=%d gaps=%ld/%ld/%ld"
                    " gmax=%llu drops=%ld/%ld/%ld ghost=%d/%ld/%ld%s%s",
                    i,a[5],a[4],a[3],a[2],a[1],a[0],L->have,L->outstanding,L->rumble_fly,
                    L->fifo_count,L->gap30,L->gap50,L->gap80,
                    (unsigned long long)L->gap_max,
                    L->drop_age,L->drop_ovf,L->drop_total-L->drop_age-L->drop_ovf,
                    /* live / absorbed / expired. `live` must return to 0 after
                     * every episode — a live count that never drains is the
                     * session-fatal leak the TTL exists to prevent, and this is
                     * where it would show. */
                    L->ghost,L->ghost_absorbed,L->ghost_expired,
                    synth,gge);
                if(lo>=(int)sizeof links) break;
            }
            /* Measured too: this is the LONGEST single write the inject thread
             * makes, so if stderr can block at all it shows here first. */
            /* L16 air telemetry, present ONLY while armed so an ordinary
             * session's line stays byte-for-byte the old one (every A/B baseline
             * in this project is a re-parse of these lines).
             *   lq=last/min/max — link quality; min==max over a whole run means
             *                     the instrument is MUTE, not that the link is
             *                     clean. That reading is disqualifying, not
             *                     negative (see the pre-registration).
             *   rssi=last/min   — dBm. afh=last/min — usable channels of 79.
             *   fcc=delta       — expected to stay 0 while the flush timeout is
             *                     infinite; movement here is a real finding. */
            char linkq[80]; linkq[0]='\0';
            if(g_linkq_ms)
                snprintf(linkq,sizeof linkq," lq=%d/%d/%d rssi=%d/%d afh=%d/%d fcc=%d",
                         g_lq_last,g_lq_min,g_lq_max,g_rssi_last,g_rssi_min,
                         g_afh_last,g_afh_min,
                         (g_fcc_base>=0&&g_fcc_last>=0)?g_fcc_last-g_fcc_base:0);
            /* L18/L17c ledger fields, present ONLY while their toggles are live
             * (same append-only contract as linkq).
             *   ptype=want/0xseen — want is the toggle, seen the last mask a
             *                       0x1D event REPORTED (a report, not proof).
             *   gaplog=written/lost — lost>0 means the ring or /tmp write could
             *                       not keep up; the log is then incomplete
             *                       and a correlation run should say so. */
            char ptyp[56]; ptyp[0]='\0';
            if(g_ptype_want || g_ptype_seen>=0)
                snprintf(ptyp,sizeof ptyp," ptype=%d/0x%04x",
                         g_ptype_want,g_ptype_seen<0?0:(unsigned)g_ptype_seen);
            char gpl[48]; gpl[0]='\0';
            if(g_gaplog_want || g_gaplog_lost || g_gaplog_written)
                snprintf(gpl,sizeof gpl," gaplog=%ld/%ld",g_gaplog_written,g_gaplog_lost);
            /* Measurement-boundary marker (1.4.34): the gap ledger's clock is the
             * KERNEL's queue-time stamp. The number counts intervals that fell
             * back to the userspace clock (expected 0) — always printed, so any
             * analysis can see which clock a line was measured with. */
            char gck[24];
            snprintf(gck,sizeof gck," gclk=k/%ld",g_ts_fallback);
            uint64_t t0=now_us();
            fprintf(stderr,"[txd] inj=%ld drop=%ld cred:%ld trunc:%ld badlen:%ld noninj:%ld nolink:%ld ambig:%ld backoff=%ld maxq=%d fifo=%d scanctr=%ld pend=%d logw=%llu/%ld/%ld flush=%d/%d/%s%s%s%s%s%s%s\n",
                injected,dropped,d_cred,d_trunc,d_badlen,d_noninj,d_nolink,d_ambig,
                paced,inject_maxq(),inject_fifo(),g_scan_ctr,g_pend_n,
                (unsigned long long)g_logw_max_us,g_logw_slow,g_logw_n,
                /* want / confirmed-by-read-back / LMP bit54. The middle number is
                 * the only one that means the lever is live; want>0 with
                 * confirmed=0 is precisely the accept-and-ignore case. */
                g_flush_want, g_flush_confirmed_ms,
                g_lmp_nonflush<0?"?":(g_lmp_nonflush?"pb":"nopb"),
                g_cmd_dead?" CMDDEAD":"", linkq, ptyp, gpl, gck, links);
            logw_note(t0);
            last_log=now_ms();
        }
        /* Queue-telemetry publish (v9): every ~200ms of TRAFFIC (this loop only
         * wakes on datagrams, which is exactly when the queue can move) write a
         * small per-address stats record "<tmpl>.<mac>.st" the jailed app reads
         * and forwards to the host as CTMB pace feedback — the host's rate
         * servo needs to SEE the backlog (fifo>0 = frames parked behind a full
         * credit window) to shed it. Written temp+rename like every other record
         * (the reader re-open()s the path per poll, so the swap is transparent):
         * the O_EXCL|O_NOFOLLOW temp is what keeps this 5/s root write out of a
         * symlink planted in the jail tmp, and the reader never sees a half
         * record. A stale file after unbind stops advancing seq, which the app
         * treats as "nothing new". */
        /* Freeze the record across a synthetic hold (+settle): the injected
         * backlog would otherwise ride out as PACE_FEEDBACK and leave the host
         * rate servo stretched for ~12s, biasing everything measured after it.
         * A non-advancing seq is already the app's "nothing new" signal, so this
         * needs no app-side change. */
        if(now_ms()-last_stat>200 && now_us()>=g_gap_quiet_until_us){
            last_stat=now_ms();
            struct { int eb; uint8_t valid,q,fifo; uint8_t addr[6]; uint32_t inj,drop;
                     uint16_t g50,g80,flush,nage,dage,dovf; } st[MAX_LINKS];
            uint64_t stnow=now_ms();
            pthread_mutex_lock(&g_lock);
            for(int i=0;i<MAX_LINKS;i++){
                struct ds5_link *L=&g_links[i];
                st[i].eb=L->ever_bound; st[i].valid=L->have?1:0;
                int q=L->outstanding; if(q<0)q=0; if(q>255)q=255;
                st[i].q=(uint8_t)q;
                int fc=L->fifo_count; if(fc<0)fc=0; if(fc>255)fc=255;
                st[i].fifo=(uint8_t)fc;
                memcpy(st[i].addr,L->bound_addr,6);
                /* PER-LINK totals (not the process-wide injected/dropped): the
                 * host servo treats drop_total deltas as THIS pad's losses. */
                st[i].inj=(uint32_t)L->inj_total;
                st[i].drop=(uint32_t)L->drop_total;
                /* v2 fields. All are WRAPPING u16 counters — readers must use
                 * wrapping deltas, exactly like the u32s above. The gap bins are
                 * what finally lets the app arm on the stalls it cannot see
                 * itself: they happen BELOW the app, so its own PLC/fill counters
                 * stay at zero through every episode. */
                st[i].g50=(uint16_t)L->gap50; st[i].g80=(uint16_t)L->gap80;
                st[i].flush=(uint16_t)L->flush_events;
                st[i].dage=(uint16_t)L->drop_age; st[i].dovf=(uint16_t)L->drop_ovf;
                /* Age of the credit window's silence, right now: the one field
                 * that reports a stall IN PROGRESS rather than after it ended. */
                uint64_t na=(L->last_nocp && stnow>L->last_nocp)?(stnow-L->last_nocp):0;
                st[i].nage=(uint16_t)(na>65535?65535:na);
            }
            pthread_mutex_unlock(&g_lock);
            stat_seq++;
            int mq=inject_maxq(), fc_cap=inject_fifo();
            for(int i=0;i<MAX_LINKS;i++){
                if(!st[i].eb || !st[i].valid) continue;   /* publish only live links */
                /* DS5Q v2 (36 B). The first 24 bytes are BYTE-IDENTICAL to v1 and
                 * keep their meaning: a v1 reader that checks `rec[4]==1` simply
                 * stops parsing (it sees v2 and skips the tick), and a v2 reader
                 * accepts both. That matters more than usual here — daemon and app
                 * ship in separate trees and can be deployed out of step, and a
                 * silently mis-parsed record would take PACE_FEEDBACK down with it,
                 * dropping the host servo to its blind fallback with no counter
                 * anywhere showing why. */
                uint8_t rec[36];
                memset(rec,0,sizeof rec);
                rec[0]='D';rec[1]='S';rec[2]='5';rec[3]='Q';
                rec[4]=2; rec[5]=st[i].valid; rec[6]=st[i].q; rec[7]=st[i].fifo;
                rec[8]=(uint8_t)(mq&0xff); rec[9]=(uint8_t)((mq>>8)&0xff);
                rec[10]=(uint8_t)(fc_cap&0xff); rec[11]=(uint8_t)((fc_cap>>8)&0xff);
                uint32_t v=st[i].inj;
                rec[12]=v&0xff; rec[13]=(v>>8)&0xff; rec[14]=(v>>16)&0xff; rec[15]=(v>>24)&0xff;
                v=st[i].drop;
                rec[16]=v&0xff; rec[17]=(v>>8)&0xff; rec[18]=(v>>16)&0xff; rec[19]=(v>>24)&0xff;
                v=stat_seq;
                rec[20]=v&0xff; rec[21]=(v>>8)&0xff; rec[22]=(v>>16)&0xff; rec[23]=(v>>24)&0xff;
                /* --- v2 tail --- */
                rec[24]=st[i].g50&0xff;   rec[25]=(st[i].g50>>8)&0xff;    /* NOCP gaps 50-79ms */
                rec[26]=st[i].g80&0xff;   rec[27]=(st[i].g80>>8)&0xff;    /* NOCP gaps >=80ms  */
                rec[28]=st[i].flush&0xff; rec[29]=(st[i].flush>>8)&0xff;  /* controller flushes */
                rec[30]=st[i].nage&0xff;  rec[31]=(st[i].nage>>8)&0xff;   /* current NOCP silence, ms */
                rec[32]=st[i].dage&0xff;  rec[33]=(st[i].dage>>8)&0xff;   /* drops: intended age-out */
                rec[34]=st[i].dovf&0xff;  rec[35]=(st[i].dovf>>8)&0xff;   /* drops: FIFO overflow */
                char p[600], p2[620];
                per_addr_path(p,sizeof p,st[i].addr);
                snprintf(p2,sizeof p2,"%s.st",p);
                write_record_atomic(p2,rec,sizeof rec);
            }
        }
    }
    return 0;
}
