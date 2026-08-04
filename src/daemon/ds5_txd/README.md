# ds5_txd — DS5 raw-ACL transport daemon (vendored)

Root helper that lets the jailed app push DS5 controller-audio (`0x36`/`0x39`) and
rumble reports onto the Bluetooth link as raw HCI ACL frames, bypassing webOS's
one-outstanding BT-HID flow control (~62 reports/s with 30–67 ms of air jitter).
The app talks to it over three AF_UNIX rendezvous points inside its own jail tmp;
`src/app/hid_passthrough/ctm/controllers/ds5_acl_tx.c` is the client half.

It ships inside this IPK and is started by
`deploy/webos/services/com.aurora.gamestream.ds5txd/` once that service has been
elevated out of its jail — see `src/app/platform/webos/ds5_service.c`. Before
1.3.0 it had to be copied to `/var/lib/webosbrew/` and launched from a boot hook
by hand, which is exactly what bundling it removes.

## Provenance

Upstream: <https://github.com/sh00bx/webos-ds5-raw-acl> (MIT).
Vendored verbatim from commit **`7bbe0ba`** (`daemon/ds5_txd.c`, 2209 lines) —
deliberately *not* upstream `main`, because `7bbe0ba` is the revision that
produced the binary that has been running on the TV, so bundling changes only how
the daemon gets there and not what it does.

Reference build (this is what the CMake rule reproduces byte for byte):

    arm-webos-linux-gnueabi-gcc -O2 -Wall -Wextra ds5_txd.c -o ds5_txd -lpthread
    # md5 c87110415dd38bfeea92ea27444f8e6f, 78412 bytes

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
