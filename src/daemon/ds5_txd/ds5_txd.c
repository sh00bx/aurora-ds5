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
//       USB) routes to the PRIMARY link (g_links[0]) — byte-for-byte the old path.
//     * Capture: HID-output seen on a NEW handle binds a FREE link slot (was:
//       "ignore any other handle"). Beyond MAX_LINKS slots a further device is
//       ignored (never flip-flops a bound link).
//     * Readiness: the base template file still tracks the primary link (a 1-pad /
//       untagged app reads it unchanged); each bound link ALSO publishes a
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
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
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
#define HCI_EV_LE_META          0x3e
#define STALL_RESET_MS          150   /* credits stopped this long -> resync a link's outstanding */
#define DEMAND_IDLE_MS         1000   /* no app report for this long -> the link is IDLE, so a
                                       * stuck `outstanding` is teardown residue, not a stall
                                       * (telemetry gate only; see last_demand). 1s is far above
                                       * the ~10-21ms audio cadence AND above the ~1/s rumble-only
                                       * floor, but far below any idle stretch. */
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
#define JAIL_UID            6261   /* aurora jail uid — only sender allowed to inject */
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

static uint32_t g_idle_lb_rgb = IDLE_LB_RGB_DEFAULT;

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
 * original unmetered path). Cached ~1/sec. It is PER LINK, so two pads each keep an
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
    static int q=INJECT_MAXQ_DEFAULT; static uint64_t last=0;
    uint64_t n=now_us();
    if(last==0 || n-last>1000000ull){
        last=n;
        FILE *f=fopen("/tmp/ds5_inject_maxq","r");
        if(f){ int v=0; if(fscanf(f,"%d",&v)==1 && v>=1 && v<=1000) q=v; fclose(f); }
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
 * /tmp/ds5_inject_fifo (0..FIFO_MAX). Cached ~1/sec. Each link has its own FIFO. */
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
    static int d=INJECT_FIFO_DEFAULT; static uint64_t last=0;
    uint64_t n=now_us();
    if(last==0 || n-last>1000000ull){
        last=n;
        FILE *f=fopen("/tmp/ds5_inject_fifo","r");
        if(f){ int v=-1; if(fscanf(f,"%d",&v)==1 && v>=0 && v<=FIFO_MAX) d=v; fclose(f); }
    }
    /* Session override from the app (it knows whether the HOST runs the rate
     * servo that bounds a deep FIFO's parked latency) wins over the boot/file
     * default; cleared on daemon restart and via 0xFF. */
    if(g_fifo_override>=0 && g_fifo_override<=FIFO_MAX) return g_fifo_override;
    return d;
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
    uint64_t last_wr_err_log;/* rate limit for the structural-write-error line (inject) */
    uint64_t last_demand;    /* last report the APP handed us for this link (inject thread
                                writes, same thread snapshots -> no lock). Telemetry only:
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
    /* audio elastic FIFO (inject) — see inject_fifo() */
    struct { uint8_t buf[FIFO_ENTRY_MAX]; int len; uint64_t ts; } fifo[FIFO_MAX];
    int      fifo_head, fifo_count;
    uint16_t fifo_gen;       /* nonce the held backlog was queued under (stale-drop) */
    /* per-link inject/drop totals (main thread only; published in the .st
     * telemetry record). The old records carried the daemon-GLOBAL counters,
     * so with two pads the host rate servo attributed one pad's drop storm to
     * BOTH links and throttled the healthy pad too. */
    long     inj_total, drop_total;
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
    /* per-link gap telemetry (inject) — the A/B acceptance signal */
    long     gap30, gap50, gap80; /* NOCP-gap histogram 30-50/50-80/>=80ms */
    uint64_t ep_start, gap_hi;    /* episode + sub-episode high-watermark state */
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
struct htab_ent { uint8_t addr[6]; uint8_t known; uint8_t mode; uint8_t from_assert; };
/* mode: HCI Mode Change (0=active,2=sniff). from_assert: this handle's address was
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
static int count_links_bound_locked(void){
    int n=0;
    for(int i=0;i<MAX_LINKS;i++) if(g_links[i].have) n++;
    return n;
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
    L->outstanding=0; L->last_nocp=now_ms(); txwin_reset(L);
    L->last_seen=now_ms();
    L->session_seen=now_ms();   /* a bind only happens off on-air HID-output = a session */
}

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
 * `expect` (may be NULL) is the target bdaddr the caller ROUTED to (a tagged send).
 * The link is resolved by address, then g_lock is dropped and re-taken here, so a
 * capture-thread rebind of this slot to a DIFFERENT DualSense could slip into that
 * window; re-checking L->bound_addr against `expect` under THIS lock closes it — a
 * mismatch means the slot moved, so we skip (return -2) rather than buzz the wrong
 * pad. NULL (legacy untagged -> primary link) skips the check, unchanged.
 *
 * Returns:  1 = injected (L->outstanding++),
 *           0 = credit window full (caller may queue or drop),
 *          -1 = template invalidated (foreign handle / EBADF; *reason set, caller
 *               must publish_all() + log),
 *          -2 = no live template (L->have==0) or the routed slot was rebound. */
static int inject_one(struct ds5_link *L, int rawfd, const uint8_t *rep, int n, int maxq, const char **reason, const uint8_t *expect){
    uint8_t frame[1+8+1+ACL_MAX_REPORT];
    int r=-2;
    int is_rumble=!is_audio_report(rep[0]);
    int lim  = is_rumble ? maxq : (maxq>1 ? maxq-1 : 1);   /* audio leaves 1 credit reserved */
    int rcap = (maxq/2)>0 ? (maxq/2) : 1;
    pthread_mutex_lock(&g_lock);
    if(L->have && (!expect || memcmp(L->bound_addr,expect,6)==0)){
        L->session_seen=now_ms();   /* the app is driving this live link: session traffic
                                     * (stamped even when the credit window blocks us —
                                     * congestion is not the end of a session) */
        uint16_t hh=(uint16_t)((L->hdr[0]|(L->hdr[1]<<8))&0x0fff);
        if(g_htab[hh].known && memcmp(g_htab[hh].addr,L->bound_addr,6)!=0){
            L->have=0; *reason="bound handle now foreign -> template INVALID"; r=-1;
        } else {
            int blocked = L->outstanding>=lim || (is_rumble && L->rumble_fly>=rcap);
            if(blocked && L->last_nocp && now_ms()-L->last_nocp>STALL_RESET_MS){
                L->outstanding=0; txwin_reset(L);   /* credits presumed lost -> resync */
                L->last_nocp=now_ms();              /* re-arm: at most one resync per STALL_RESET_MS */
                blocked=0;
            }
            if(blocked){
                r=0;
            } else {
                uint16_t l2=(uint16_t)(1+n), acl=(uint16_t)(4+l2);
                frame[0]=HCI_ACLDATA_PKT; memcpy(frame+1,L->hdr,8);
                frame[3]=(uint8_t)(acl&0xff); frame[4]=(uint8_t)(acl>>8);
                frame[5]=(uint8_t)(l2&0xff);  frame[6]=(uint8_t)(l2>>8);
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
            L->drop_total++;                        /* age-out is a real loss: keep it visible to the host servo */
            continue;                               /* stale pre-pause audio: drop */
        }
        int r=inject_one(L,rawfd,L->fifo[idx].buf,L->fifo[idx].len,maxq,reason,expect);
        if(r==1){ L->fifo_head=(L->fifo_head+1)%FIFO_MAX; L->fifo_count--; inj++; L->inj_total++; continue; }
        if(r==-1){ *need_inval=1; fifo_clear(L); }  /* template gone -> drop stale audio */
        else if(r==-2) fifo_clear(L);               /* no template: backlog is already stale */
        break;                                      /* credits full (0) -> retry next wakeup */
    }
    return inj;
}
static int      g_rawfd = -1;          /* main's raw HCI socket, shared for link-policy writes */

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
 * We are ROOT writing into the JAILED app's own tmp (uid 6261 creates files there
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
 * The BASE file (g_tmpl_path) tracks the PRIMARY link (g_links[0]) so a legacy
 * single-pad / untagged app reads it unchanged. Each link that has ever bound also
 * gets its per-address file, which the tagged app polls for its own controller. */
static void publish_all(void){
    struct { uint8_t valid; uint16_t nonce; uint8_t hdr[8]; uint8_t addr[6]; int has_addr;
             uint8_t prev[6]; int prev_valid; } s[MAX_LINKS];
    pthread_mutex_lock(&g_pub_lock);
    pthread_mutex_lock(&g_lock);
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
    publish_record(g_tmpl_path, s[0].valid, s[0].nonce, s[0].valid ? s[0].hdr : NULL);
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
    chmod(path,0666);   /* the jailed uid (6261) must be able to sendto it */
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
    chmod(path,0666);   /* the jailed uid (6261) must be able to connect */
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

/* True iff a connected peer's uid is allowed to drive us (jail app or root). */
static int uid_ok(uid_t u){ return u==(uid_t)JAIL_UID || u==0; }

/* Extract the sender's SCM_CREDENTIALS from a recvmsg() control buffer and check
 * the uid. Rejects (0) if no credentials are present. */
static int cred_ok(struct msghdr *mh){
    for(struct cmsghdr *c=CMSG_FIRSTHDR(mh); c; c=CMSG_NXTHDR(mh,c)){
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS &&
           c->cmsg_len==CMSG_LEN(sizeof(struct ucred))){
            struct ucred uc; memcpy(&uc,CMSG_DATA(c),sizeof uc);
            return uid_ok(uc.uid);
        }
    }
    return 0;
}

/* Send status byte + (optionally) one fd as SCM_RIGHTS on a stream conn. */
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
    return (int)sendmsg(conn,&msg,0);
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
        if(!uid_ok(uc.uid)){ fprintf(stderr,"[txd] broker rejected peer uid=%u\n",uc.uid); send_fd(conn,-1,'E'); close(conn); continue; }
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
            send_fd(conn,hfd,'O');
            fprintf(stderr,"[txd] broker handed fd for %s\n",req);
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
static void handle_hci_event(const uint8_t *e, int el){
    if(el < 2) return;
    uint8_t code=e[0]; const uint8_t *p=e+2; int pl=el-2;   /* skip [code][param_len] */
    const char *reason=NULL; int none_bound=0;
    if(code==HCI_EV_CMD_COMPLETE && pl>=3){                 /* ncmd,opcode(2),status... */
        uint16_t op=(uint16_t)(p[1]|(p[2]<<8));
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
                break;
            }
        return;
    }
    if(code==HCI_EV_MODE_CHANGE && pl>=6){                  /* status,handle(2),mode,interval(2) */
        if(p[0]!=0x00) return;
        uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
        pthread_mutex_lock(&g_lock);
        g_htab[hh].mode=p[3];
        pthread_mutex_unlock(&g_lock);
        return;                                             /* mode never affects validity */
    }
    if(code==HCI_EV_CONN_COMPLETE && pl>=9){                /* status,handle(2),bdaddr(6),... */
        if(p[0]!=0x00) return;
        uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
        pthread_mutex_lock(&g_lock);
        memcpy(g_htab[hh].addr,p+3,6); g_htab[hh].known=1; g_htab[hh].mode=0; g_htab[hh].from_assert=0; /* kernel-proven, active */
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
        memcpy(g_htab[hh].addr,p+6,6); g_htab[hh].known=1; g_htab[hh].mode=0; g_htab[hh].from_assert=0; /* kernel-proven */
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
        pthread_mutex_lock(&g_lock);
        for(int i=0;i<nh;i++){
            uint16_t hh=(uint16_t)((p[1+i*4]|(p[2+i*4]<<8))&0x0fff);
            struct ds5_link *L=link_by_handle(hh);   /* credits are PER HANDLE -> per link */
            if(!L) continue;
            int cnt=(int)(p[3+i*4]|(p[4+i*4]<<8));
            L->outstanding-=cnt; if(L->outstanding<0) L->outstanding=0;
            txwin_pop(L,cnt);   /* FIFO-approximate the per-type in-flight counts */
            /* Refresh the stall timestamp ONLY for OUR handle's completions:
             * a global refresh would let any other device's NOCP chatter
             * (Magic Remote etc.) suppress the 150ms backstop exactly when
             * our credits are the ones wedged. */
            L->last_nocp=now_ms();
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
            L->last_seen=now_ms();
        }
        pthread_mutex_unlock(&g_lock);
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
            memcpy(g_htab[hh].addr,req.ci[i].bdaddr.b,6); g_htab[hh].known=1; g_htab[hh].mode=0; g_htab[hh].from_assert=0; /* kernel conn list; mode unknown -> assume active */
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
        int lb[MAX_LINKS], pin[MAX_LINKS]; uint16_t lh[MAX_LINKS]; uint8_t md[MAX_LINKS];
        pthread_mutex_lock(&g_lock);
        for(int i=0;i<MAX_LINKS;i++){
            if(g_links[i].have && now_ms()-g_links[i].last_seen>IDLE_INVALIDATE_MS){
                g_links[i].have=0; idle_inval=1;
            }
            if(g_links[i].have && g_links[i].session_seen &&
               now_ms()-g_links[i].session_seen<=SESSION_IDLE_MS) sess_active=1;
            lb[i]=g_links[i].have; lh[i]=g_links[i].handle; pin[i]=lb[i]; md[i]=0;
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
        for(int i=0;i<MAX_LINKS;i++) if(pin[i]) md[i]=g_htab[lh[i]].mode;
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
            } else L->policy_handle=0xffff;
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

        int r=(int)recv(mfd,buf,sizeof buf,0);
        if(r<0){
            /* EAGAIN/EWOULDBLOCK is the normal 300ms SO_RCVTIMEO tick, EINTR a signal;
             * any other errno is a real socket error -> back off so we never busy-spin. */
            if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) usleep(50000);
            continue;
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
        if(h->opcode==MON_EVENT_PKT){ handle_hci_event(d,dl); continue; }
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
             * on MONITOR, so this cannot self-refresh. */
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
        r=inject_one(L,rawfd,report,n,maxq,&reason,expect);
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
            r=inject_one(L,rawfd,report,n,maxq,&reason,expect);
            if(r==1){(*injected)++; L->inj_total++;}
            else if(r==-1){ need_inval=1; fifo_clear(L); }
            else if(r==0){
                if(fdepth>0 && n<=FIFO_ENTRY_MAX){
                    /* Evict-to-fit: fdepth can be LOWERED at runtime; a single-evict
                     * would balance every insert and keep the count at the old
                     * high-water forever under congestion, voiding the latency bound. */
                    while(L->fifo_count>=fdepth){ L->fifo_head=(L->fifo_head+1)%FIFO_MAX; L->fifo_count--; (*dropped)++; L->drop_total++; }
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
    const char *sock_path = argc>1?argv[1]:"/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl.sock";
    g_tmpl_path           = argc>2?argv[2]:"/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl_tmpl";
    const char *hidfd_path= argc>3?argv[3]:"/var/palm/jail/com.aurora.gamestream/tmp/ds5_hidfd.sock";

    /* Leave the MAIN thread's comm at its default (argv[0] basename "ds5_txd"): the
     * launcher and management scripts (enforce_single.sh, revert_until_up.sh, etc.)
     * identify the daemon with `pkill -x ds5_txd`, which matches the comm — renaming
     * main would silently break every one of them. Only the worker threads are named
     * (ds5-cap/ds5-brk, per audit O3) so the respawner can still target them by TID. */
    struct sched_param sp; memset(&sp,0,sizeof sp); sp.sched_priority=14;
    sched_setscheduler(0,SCHED_FIFO,&sp);

    /* Pin the file-creation mask: the readiness/telemetry records now take their
     * mode from open(...,0644) alone (write_record_atomic no longer fchmod()s —
     * that used to chmod a symlink's target), and the JAILED app (uid 6261) must be
     * able to read them, so an inherited umask must not strip o+r. */
    umask(0022);

    for(int i=0;i<MAX_LINKS;i++){ g_links[i].policy_handle=0xffff; g_links[i].last_nocp=now_ms(); }

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
     * recv buffer, SO_PASSCRED — see bind_unix_dgram). Self-healed across remounts. */
    int ufd=bind_unix_dgram(sock_path);
    if(ufd<0){ perror("[txd] bind unix"); return 1; }

    pthread_t cap; pthread_create(&cap,NULL,capture_thread,NULL);
    pthread_t brk; pthread_create(&brk,NULL,broker_thread,(void*)hidfd_path);
    {   /* Idle lightbar colour: DS5_IDLE_LIGHTBAR=RRGGBB (hex), "0"/"off" disables. */
        const char *e=getenv("DS5_IDLE_LIGHTBAR");
        if(e && *e){
            if(!strcasecmp(e,"off")) g_idle_lb_rgb=0;
            else { char *end=NULL; unsigned long v=strtoul(e,&end,16);
                   if(end && *end=='\0' && v<=0xFFFFFFul) g_idle_lb_rgb=(uint32_t)v;
                   else fprintf(stderr,"[txd] DS5_IDLE_LIGHTBAR='%s' not RRGGBB hex -> keeping %06x\n",e,g_idle_lb_rgb); }
        }
    }
    fprintf(stderr,"[txd] forwarder up (cmdguard, %d links): unix=%s tmpl=%s hidfd=%s idle_lb=%06x\n",MAX_LINKS,sock_path,g_tmpl_path,hidfd_path,g_idle_lb_rgb);

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
    uint64_t last_stat=now_ms(); uint32_t stat_seq=0;   /* queue-telemetry publish (v9) */
    for(;;){
        struct pollfd pfd[2]={{ufd,POLLIN,0},{minfo,POLLPRI,0}};
        int to=500;
        int pr=poll(pfd,2,to);
        if(pr<0){ if(errno==EINTR) continue; usleep(2000); continue; }

        /* Mount table changed (or retry tick while rebinding / watch down): if our
         * node's path stopped resolving to a socket, Aurora remounted its tmp under
         * us — bind a fresh node into the current top mount and re-publish readiness. */
        if(pr==0 || pfd[1].revents){
            if(minfo<0) minfo=open_mount_watch();           /* retry a previously-failed watch */
            else if(pfd[1].revents) rearm_mount_watch(minfo);
            if(!node_alive(sock_path)){
                if(ufd>=0){ close(ufd); ufd=-1; }
                int nu=bind_unix_dgram(sock_path);
                if(nu>=0){
                    ufd=nu;
                    publish_all();   /* re-publish current state into the new mount */
                    fprintf(stderr,"[txd] jail-tmp remount -> rebound ds5_acl.sock + re-published\n");
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
            struct { int have, qd; uint64_t ln, ld; uint16_t nonce; uint64_t rx; uint8_t addr[6]; } sn[MAX_LINKS];
            uint64_t nowm=now_ms(), other_now;
            pthread_mutex_lock(&g_lock);
            for(int i=0;i<MAX_LINKS;i++){
                sn[i].have=g_links[i].have; sn[i].qd=g_links[i].outstanding;
                sn[i].ln=g_links[i].last_nocp; sn[i].nonce=g_links[i].nonce;
                sn[i].rx=g_links[i].rx_pkts; sn[i].ld=g_links[i].last_demand;
                memcpy(sn[i].addr,g_links[i].bound_addr,6);
            }
            other_now=g_other_rx;
            pthread_mutex_unlock(&g_lock);
            for(int i=0;i<MAX_LINKS;i++){
                struct ds5_link *L=&g_links[i];
                link_nonce[i]=sn[i].nonce;
                /* Per-binding telemetry: a rebind (nonce bump) starts a FRESH gap
                 * histogram so the 10s status line measures THIS binding, not the
                 * slot's whole lifetime — cumulative counts across rebinds/address
                 * swaps were useless for A/B deltas. The outgoing counts are
                 * logged, so nothing is lost. Reset here (inject thread owns the
                 * histogram), not in link_bind (capture thread — would race). */
                if(L->tele_gen!=sn[i].nonce){
                    if(L->gap30||L->gap50||L->gap80)
                        fprintf(stderr,"[txd] L%d gap histogram reset on rebind (was %ld/%ld/%ld)\n",
                                i,L->gap30,L->gap50,L->gap80);
                    L->gap30=L->gap50=L->gap80=0; L->ep_start=0; L->gap_hi=0;
                    L->tele_gen=sn[i].nonce;
                }
                /* Stale-backlog gate: a rebind bumps the nonce, so audio still held
                 * from the previous binding must be dropped, not played into the new
                 * session (drain_fifo also clears on the no-template path, but that
                 * never runs when the invalidate->rebind happens between wakeups). */
                if(L->fifo_count>0 && L->fifo_gen!=sn[i].nonce) fifo_clear(L);
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
                 * gate, and for the same reason it is the right one: it says
                 * whether an APP is driving this pad. While nothing is, the bar
                 * belongs to the firmware — full-brightness blue, or the orange
                 * charge indication — and we repaint it dim. The moment demand
                 * returns the host owns the bar again and we stop touching it,
                 * so the two writers can never fight over a live link. */
                if(g_idle_lb_rgb && sn[i].have && !tx_demand){
                    if(L->lb_idle_gen!=sn[i].nonce){   /* fresh binding -> fresh burst */
                        L->lb_idle_gen=sn[i].nonce; L->lb_paints=0; L->lb_last_paint=0;
                    }
                    uint64_t iv = (L->lb_paints<IDLE_LB_BURST) ? IDLE_LB_BURST_MS : IDLE_LB_KEEPALIVE_MS;
                    if(!L->lb_last_paint || nowm-L->lb_last_paint>=iv){
                        uint8_t lbrep[DS5_BT_OUT_LEN];
                        const char *why=NULL;
                        ds5_build_lightbar(lbrep,g_idle_lb_rgb,L->lb_seq++);
                        /* inject_one takes g_lock itself and re-checks the binding
                         * against the address we snapshotted, so a rebind between
                         * the snapshot and here cannot paint the wrong pad. */
                        int lr=inject_one(L,g_rawfd,lbrep,DS5_BT_OUT_LEN,inject_maxq(),&why,sn[i].addr);
                        /* Log the first paint of each idle stretch: whether this
                         * fires at all is the one thing that cannot be inferred
                         * from the outside. It needs a captured template, and the
                         * template comes only from an on-air output report -- so on
                         * a fresh connect it depends on the TV's own stack having
                         * written to the pad at least once. */
                        if(L->lb_paints==0)
                            fprintf(stderr,"[txd] L%d idle lightbar -> %06x (r=%d%s%s)\n",
                                    i,g_idle_lb_rgb,lr,why?" ":"",why?why:"");
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
                        fprintf(stderr,"[txd] L%d EPISODE start t=%llu q=%d\n",i,(unsigned long long)nowm,sn[i].qd);
                    }
                } else if(L->ep_start){
                    /* RX-continuity verdict for the stall window: rx = this pad's
                     * inbound ACL during the episode, orx = every other link's.
                     * rx flowing while TX starves = TX-scheduling; rx dead but orx
                     * alive = this link's RF; both dead = radio-global.
                     * "(idle)" = the demand gate ended it, i.e. the app stopped feeding
                     * mid-episode. Reported, never hidden, but NOT a link verdict. */
                    fprintf(stderr,"[txd] L%d EPISODE end t=%llu dur=%dms rx=%llu orx=%llu%s\n",
                            i,(unsigned long long)nowm,(int)(nowm-L->ep_start),
                            (unsigned long long)(sn[i].rx-L->ep_rx0),
                            (unsigned long long)(other_now-L->ep_other0),
                            tx_demand?"":" (idle)");
                    L->ep_start=0;
                }
                /* Sub-episode gap histogram: the EPISODE lines only show >80ms, but a
                 * 56ms DS5 buffer already underruns on 50-80ms NOCP gaps. Track the
                 * high-watermark of each credit-starved stretch and bucket it when a
                 * NOCP resets the gap. Loop wakes ~94/s in-session -> ~10ms res.
                 * Same demand gate: an idle stretch would otherwise pile its whole
                 * duration into gap80 the moment traffic resumes. */
                uint64_t cur=(sn[i].have && sn[i].qd>0 && sn[i].ln && tx_demand)?(nowm-sn[i].ln):0;
                if(cur>L->gap_hi) L->gap_hi=cur;
                else if(L->gap_hi){
                    if(L->gap_hi>=80) L->gap80++;
                    else if(L->gap_hi>=50) L->gap50++;
                    else if(L->gap_hi>=30) L->gap30++;
                    L->gap_hi=0;
                }
            }
        }
        if(ufd>=0 && (pfd[0].revents&POLLIN)){
            int maxq=inject_maxq();     /* per-wakeup constants (fopen /tmp ~1/s, never under g_lock) */
            int fdepth=inject_fifo();
            for(int drained=0; drained<DRAIN_CAP; drained++){
                struct iovec iov={.iov_base=rep,.iov_len=sizeof rep};
                union { char b[CMSG_SPACE(sizeof(struct ucred))]; struct cmsghdr a; } cmsgu;
                struct msghdr mh; memset(&mh,0,sizeof mh);
                mh.msg_iov=&iov; mh.msg_iovlen=1; mh.msg_control=cmsgu.b; mh.msg_controllen=sizeof cmsgu.b;
                ssize_t n=recvmsg(ufd,&mh,0);
                if(n<0){ if(errno==EINTR) continue; break; }   /* EAGAIN -> drained */
                if(n==0) continue;                             /* zero-length datagram: skip, keep draining */
                if(mh.msg_flags & MSG_TRUNC){ dropped++; continue; } /* oversized: never inject a truncated frame */
                if(!cred_ok(&mh)){ dropped++; continue; }      /* peer-cred gate: jail uid only */

                /* Route by tag kind (see ACL_TAG_*): an INJECT datagram goes to the
                 * link bound to that address; an ASSERT datagram only feeds the
                 * identity ring and is never injected; an untagged one (legacy/USB)
                 * goes to the PRIMARY link (g_links[0]) — byte-for-byte the old
                 * single-pad path. */
                /* Control datagram: consumed here, never routed to a link. */
                if(n>=4 && rep[0]==ACL_TAG_M0 && rep[1]==ACL_TAG_CTRL){
                    if(rep[2]==ACL_CTRL_FIFO_DEPTH){
                        int nv=(rep[3]==0xFF)?-1:(int)rep[3];
                        if(nv<=FIFO_MAX && nv!=g_fifo_override){
                            g_fifo_override=nv;
                            fprintf(stderr,"[txd] ctrl: audio-FIFO depth override -> %d\n",nv);
                        }
                    } else if(rep[2]==ACL_CTRL_IDLE_LB && n>=6){
                        uint32_t nv=((uint32_t)rep[3]<<16)|((uint32_t)rep[4]<<8)|rep[5];
                        if(nv!=g_idle_lb_rgb){
                            g_idle_lb_rgb=nv;
                            /* Re-arm every link's burst so the new colour lands now
                             * instead of at the next 30 s keep-alive. */
                            for(int i=0;i<MAX_LINKS;i++){ g_links[i].lb_paints=0; g_links[i].lb_last_paint=0; }
                            fprintf(stderr,"[txd] ctrl: idle lightbar -> %06x\n",nv);
                        }
                    }
                    continue;
                }
                const uint8_t *report; int rlen; struct ds5_link *L; const uint8_t *expect=NULL;
                int tagged = (n>=(ssize_t)(ACL_TAG_LEN+1) && rep[0]==ACL_TAG_M0 &&
                              (rep[1]==ACL_TAG_INJECT || rep[1]==ACL_TAG_ASSERT));
                if(tagged){
                    int is_assert=(rep[1]==ACL_TAG_ASSERT);
                    report=rep+ACL_TAG_LEN; rlen=(int)(n-ACL_TAG_LEN); expect=rep+2;
                    if(rlen<=0 || rlen>ACL_MAX_REPORT){ dropped++; continue; }
                    if(!injectable(report[0])){ dropped++; continue; }
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
                    if(!L){ dropped++; continue; }             /* no link for this target (not ready) */
                } else {
                    /* Legacy untagged -> primary link. FAIL CLOSED as soon as identity
                     * matters: with more than one link bound, "the primary link" is not
                     * an identity — g_links[0] is simply whoever was on air first — so an
                     * untagged sender cannot know WHICH pad it is about to drive. That is
                     * how an arming/output report meant for the pad the sender unbound
                     * lands on the OTHER pad, whose kernel driver is still bound (two
                     * writers on one L2CAP channel = the documented oscillation). One
                     * bound link is unambiguous, so the single-pad/USB path is unchanged.
                     * Control datagrams are consumed above and stay untagged-friendly. */
                    pthread_mutex_lock(&g_lock);
                    int nbound=count_links_bound_locked();
                    pthread_mutex_unlock(&g_lock);
                    if(nbound>1){ dropped++; continue; }   /* ambiguous target: refuse */
                    report=rep; rlen=(int)n; L=&g_links[0];
                    if(rlen<=0 || rlen>ACL_MAX_REPORT){ dropped++; continue; }
                    if(!injectable(report[0])){ dropped++; continue; }  /* only DS5 output reports */
                }

                int idx=(int)(L-g_links);
                L->last_demand=now_ms();   /* stamp BEFORE process_report: demand exists even
                                              when the report is then paced away or blocked */
                process_report(L,rawfd,report,rlen,link_nonce[idx],fdepth,maxq,expect,&injected,&dropped,&paced);
            }
        }
        if(now_ms()-last_log>10000){
            char links[MAX_LINKS*160]; int lo=0; links[0]='\0';
            for(int i=0;i<MAX_LINKS;i++){
                struct ds5_link *L=&g_links[i];
                if(!L->ever_bound) continue;
                const uint8_t*a=L->bound_addr;
                lo+=snprintf(links+lo,sizeof links-lo,
                    " | L%d %02x:%02x:%02x:%02x:%02x:%02x have=%d q=%d rq=%d fifo=%d gaps=%ld/%ld/%ld",
                    i,a[5],a[4],a[3],a[2],a[1],a[0],L->have,L->outstanding,L->rumble_fly,
                    L->fifo_count,L->gap30,L->gap50,L->gap80);
                if(lo>=(int)sizeof links) break;
            }
            fprintf(stderr,"[txd] inj=%ld drop=%ld backoff=%ld maxq=%d fifo=%d scanctr=%ld pend=%d%s%s\n",
                injected,dropped,paced,inject_maxq(),inject_fifo(),g_scan_ctr,g_pend_n,
                g_cmd_dead?" CMDDEAD":"", links);
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
        if(now_ms()-last_stat>200){
            last_stat=now_ms();
            struct { int eb; uint8_t valid,q,fifo; uint8_t addr[6]; uint32_t inj,drop; } st[MAX_LINKS];
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
            }
            pthread_mutex_unlock(&g_lock);
            stat_seq++;
            int mq=inject_maxq(), fc_cap=inject_fifo();
            for(int i=0;i<MAX_LINKS;i++){
                if(!st[i].eb || !st[i].valid) continue;   /* publish only live links */
                uint8_t rec[24];
                rec[0]='D';rec[1]='S';rec[2]='5';rec[3]='Q';
                rec[4]=1; rec[5]=st[i].valid; rec[6]=st[i].q; rec[7]=st[i].fifo;
                rec[8]=(uint8_t)(mq&0xff); rec[9]=(uint8_t)((mq>>8)&0xff);
                rec[10]=(uint8_t)(fc_cap&0xff); rec[11]=(uint8_t)((fc_cap>>8)&0xff);
                uint32_t v=st[i].inj;
                rec[12]=v&0xff; rec[13]=(v>>8)&0xff; rec[14]=(v>>16)&0xff; rec[15]=(v>>24)&0xff;
                v=st[i].drop;
                rec[16]=v&0xff; rec[17]=(v>>8)&0xff; rec[18]=(v>>16)&0xff; rec[19]=(v>>24)&0xff;
                v=stat_seq;
                rec[20]=v&0xff; rec[21]=(v>>8)&0xff; rec[22]=(v>>16)&0xff; rec[23]=(v>>24)&0xff;
                char p[600], p2[620];
                per_addr_path(p,sizeof p,st[i].addr);
                snprintf(p2,sizeof p2,"%s.st",p);
                write_record_atomic(p2,rec,24);
            }
        }
    }
    return 0;
}
