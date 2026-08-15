# ds5_txd — DS5 raw-ACL transport daemon (vendored)

Root helper that lets the jailed app push DS5 controller-audio (`0x36`/`0x39`) and
rumble reports onto the Bluetooth link as raw HCI ACL frames, bypassing webOS's
one-outstanding BT-HID flow control (~62 reports/s with 30–67 ms of air jitter).
The app talks to it over three AF_UNIX rendezvous points inside its own jail tmp;
`src/app/hid_passthrough/ctm/controllers/ds5_acl_tx.c` is the client half.

It ships inside this IPK and is started by
`deploy/webos/services/com.aurora.ds5.txd/` once that service has been
elevated out of its jail — see `src/app/platform/webos/ds5_service.c`. Before
1.3.0 it had to be copied to `/var/lib/webosbrew/` and launched from a boot hook
by hand, which is exactly what bundling it removes.

## Provenance

Upstream: <https://github.com/sh00bx/webos-ds5-raw-acl> (MIT).
Vendored from commit **`d1557da`** (`daemon/ds5_txd.c`) **plus two local patches**
— jail uid via `argv[4]`, and the daemon-hardening set (both under "Local patches"
below). It is no longer byte-identical to upstream — carry **both** forward on the
next re-vendor, or the fixes are gone without a conflict marker.

Until 2026-08-04 this was pinned to **`7bbe0ba`** on the reasoning that it was the
revision whose binary had been running on the TV, so bundling would change only
how the daemon gets there and not what it does. That reasoning was sound for the
transport, but it broke the idle lightbar: the *app* half of the 2026-08-03 deep
review is already deployed (`312d10a3`), and it speaks the `d1557da` protocol —
`0x02` with RGB `000000` means "the app owns the bar, do not paint", and the
selection is taken back with the 3-byte **`0x03`** clear. `7bbe0ba` has neither:
it keeps one `g_idle_lb_rgb`, so `000000` disables its painter permanently, and
its ctrl branch gates on `n>=4`, so the 3-byte clear is not even recognised as a
ctrl message. Observed live: the bar went dark on the first passthrough plug and
never came back (`[txd] ctrl: idle lightbar -> 000000` in `/tmp/ds5_txd.log`, old
log format). The two protocol halves have to ship in lockstep; pinning only one
of them is what "reviewed but never run" turned into here.

`d1557da` also carries the fix that the painter must neither count as session
traffic nor paint over a live session (`IDLE_LB_HIDRAW_QUIET_MS`), which is the
second writer the 08-03 review removed.

## Local patches

Two, both of which must survive a re-vendor. Checklist below; details follow.

### 1. Jail uid via `argv[4]` (2026-08-06)

The rule used to be "the vendored `.c` is **never edited here**", on the grounds
that everything app-id-specific reaches the daemon through argv. That held for
the three socket paths — and hid the one value it did *not* hold for.

`JAIL_UID` was compiled in as `6261`, the jail uid of `com.aurora.gamestream`,
and is the `SO_PEERCRED` / `SCM_CREDENTIALS` gate for both the hid-fd broker and
the ACL inject socket. webOS assigns a jail uid per app id, so renaming the app
to `com.aurora.ds5` moved it to `5895`; the daemon then rejected every request
from the app it exists to serve. It failed **silently**: the app only learns "no
fd" from the broker and reports the `ENOENT` of its own direct `open()`, so the
only trace was `[txd] broker rejected peer uid=5895` in the daemon's own log.
DS5 passthrough was dead with the host side provably healthy.

So the uid now arrives the same way the paths do — as `argv[4]`, read by
`ds5-tmpld.sh` off the app's jail directory, which follows any future rename by
itself. Two deliberate choices:

* **No usable `argv[4]` means root only** (`JAIL_UID_DEFAULT 0`), not a fallback
  number. Falling back to `6261` would hand the gate to a *different* app that is
  still installed on the device, i.e. open it instead of closing it.
* The startup line names the accepted uid and the broker's rejection line names
  the expected one, so the next app-id change fails loudly instead of silently.

Fix it upstream in `webos-ds5-raw-acl` and drop this patch when re-vendoring from
a commit that carries it.

### 2. Daemon hardening (2026-08-09)

Five independent changes, from the phase-5 refactor. Each is small and each is
load-bearing; re-check all five against any re-vendored source.

* **A local peer can no longer kill the daemon.** `SIGPIPE` is ignored at startup
  and every reply `sendmsg()` carries `MSG_NOSIGNAL`, so a client that closes its
  socket between request and reply gets an `EPIPE` return instead of taking the
  root process down with it. Verified off-device: the pre-patch `sendmsg(...,0)`
  really is fatal on both the reject and the `SCM_RIGHTS` paths.
* **Untagged datagrams route by identity, not by slot 0.** An untagged (legacy /
  USB) report now goes to the single bound link when exactly one is bound, and is
  refused otherwise; the base readiness record fails closed the same way. The old
  slot-0 rule left a lone pad in slot 1 permanently un-accelerated, and with two
  pads bound it reported ready while refusing every datagram — the pad went
  silent with nothing to fall back to. **Visible behaviour change:** in a 2-pad
  session the base record now reads `valid=0`, so a legacy untagged client and
  `service.js /status` report not-ready.
* **Per-reason drop counters** (`d_nolink`, `d_ambig`, `d_badlen`, `d_noninj`, …)
  in the 10 s stats line, so a refusal is diagnosable instead of one opaque total.
  Zero-links-bound is counted apart from the genuine ambiguity refusal: the
  former is the normal pre-bind state while the app is still hidraw-seeding.
* **A failed startup socket bind is no longer fatal** — it retries every 500 ms
  and logs once. Note the supervisor still kills and respawns after ~12 s of a
  missing socket (`ds5-tmpld.sh`), so this covers failures that clear inside
  ~10 s; the respawn remains the backstop for anything longer.
* **`/tmp` tunables are ownership-gated.** `read_root_int()` opens with
  `O_NOFOLLOW|O_CLOEXEC` and `fstat()`s the **fd**, requiring a root-owned
  regular file, so a planted symlink or a swap between check and open is refused
  — and the refusal is logged once per call site rather than silently disabling
  the operator's tuning.

## Measurement phase (2026-08-15)

Everything here exists to make the *next* experiment trustworthy; none of it
changes how audio is delivered.

* **NOCP gaps are binned in the capture thread**, not sampled from the main poll
  loop. The loop is datagram-clocked, and batched `0x39` halved its rate to
  ~47/s — so the old `gaps=` histogram was quantized to ~21 ms and undercounted
  the 30-49 bin. Each NOCP arrival ends exactly one gap, so the handler bins
  `now - last_nocp` directly. **`gaps=` counts are therefore not comparable
  across this build boundary** — it is a resolution change, not a format change.
* **`drop_total` is split** into `drop_age` (intended staleness age-out),
  `drop_ovf` (FIFO overflow) and the remainder (template loss), reported as
  `drops=age/ovf/other`. `drop_total` is still their sum, so old readers are
  unaffected. Without this a maxq ladder has no usable abort criterion —
  "drop_total must stay 0" self-aborts on the first normal 150-400 ms episode,
  which ages a frame out *by design*.
* **A freed credit now wakes the main loop** (eventfd, `g_kickfd`). `drain_fifo`
  stays main-thread-owned; the capture thread only posts the fd. Previously a
  held frame waited for the next app datagram — a systematic 0..21.33 ms
  (mean ~10.7) on every congestion recovery. The datagram-clocked drain remains
  as belt-and-braces, so a lost kick costs latency, never a stranded frame.
* **`DS5Q` telemetry record is v2, 36 B.** First 24 bytes byte-identical to v1.
  Appended: `gap50`, `gap80`, `flush_events`, `nocp_age_ms`, `drop_age`,
  `drop_ovf` — all **wrapping u16**, compare with wrapping deltas. Daemon and app
  ship from different trees, so the reader accepts v1 *and* v2; rejecting an
  unknown version would silently stop `PACE_FEEDBACK` and drop the host rate
  servo to its blind fallback with nothing showing why.
* **`logw=max_us/slow/n`** on the stats line measures how long the inject
  thread's own `fprintf`s take. The EPISODE lines are written *during* the stall
  they describe; whether that write can actually block here was never measured.
  Probe first — if `slow` stays 0 the item is closed, if it does not, log I/O is
  perturbing the very gaps it records and must move off the inject path (to a
  dedicated low-priority writer — **not** the capture thread, which owns credit
  accounting and would turn log backpressure into apparent NOCP gaps).
* **Deterministic gap injector**: write a millisecond count to
  `/tmp/ds5_gap_inject` (root-owned) and every ACL write is held for that long.
  One-shot (unlinked on read), refused if the file is >5 s old or outside
  20..2000 ms, bracketed by `GAPINJECT start`/`end` lines, and its gaps are
  counted in a **separate** `synth=` counter so they never enter the production
  bins. The `.st` record is frozen for the hold plus 2 s so the injected backlog
  cannot leave the host rate servo stretched for the next ~12 s.

  🚨 **Scope limit — this rig cannot test everything.** A write-hold emulates TX
  *absence*, not credit starvation: the controller's TX queue drains and then
  sits empty. It validates pad-side physics (how the pad's jitter buffer reacts
  to a hole) and nothing else. It can never exercise the stall-resync path and,
  once `Write_Automatic_Flush_Timeout` exists, can never fire a flush — there is
  nothing queued to flush. Flush semantics are observable **only on real coex
  episodes**.

Still open from that phase and deliberately not done here: hardening
`ds5-tmpld.sh`'s `>>"$LOG"` append against a symlink plant on the predictable
`/tmp/ds5_txd.log` path. It is only a real primitive if `/tmp` is `1777` on the
device — check that first, because getting it wrong breaks logging outright, and
logging is what makes the rest of this observable.

Reference build (this is what the CMake rule reproduces byte for byte):

    arm-webos-linux-gnueabi-gcc -O2 -Wall -Wextra ds5_txd.c -o ds5_txd -lpthread
    # md5 b61d95378ccdb30a51fb8866bb9e33d3, 79160 bytes   (measurement phase, 1.4.23)
    # md5 a5524813257d2b8a0e67cadfc170da62, 78676 bytes   (+ daemon hardening phase)
    # md5 0939f76761c39ebe702f3aae6eb624fc, 78504 bytes   (d1557da + jail-uid patch)
    # md5 8056dddf23813cc4c6e975e7f89ea6f6, 78476 bytes   (d1557da, unpatched)
    # md5 c87110415dd38bfeea92ea27444f8e6f, 78412 bytes   (7bbe0ba, superseded)

Two traps if you ever build it by hand:

* Sourcing the buildroot SDK's `environment-setup` exports a `CFLAGS` that the
  upstream Makefile's `CFLAGS ?=` yields to, which silently drops `-O2` and
  produces a different, slower binary. Pass the flags explicitly, or
  `env -u CFLAGS make`.
* gcc records the source path it was handed in `STT_FILE`, so compiling with an
  absolute path changes `.strtab` and the md5. The CMake rule compiles from the
  source directory with a bare `ds5_txd.c` for that reason.

Upstream `main` has since moved on (`d1557da`, +130/−37) with the 2026-08-03 deep
review's fixes. Those are reviewed but have never run on the TV; pulling them in
is a separate, independently verifiable step.
