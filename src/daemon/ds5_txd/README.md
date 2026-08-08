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
Vendored from commit **`d1557da`** (`daemon/ds5_txd.c`) **plus one local patch**
(jail uid via `argv[4]`, see "Local patch" below). It is no longer byte-identical
to upstream — carry the patch forward on the next re-vendor, or the fix is gone
without a conflict marker.

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

## Local patch — jail uid via `argv[4]` (2026-08-06)

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

Reference build (this is what the CMake rule reproduces byte for byte):

    arm-webos-linux-gnueabi-gcc -O2 -Wall -Wextra ds5_txd.c -o ds5_txd -lpthread
    # md5 000d81dbec1ee824bbb621af17aaa51c, 78676 bytes   (+ daemon hardening phase)
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
