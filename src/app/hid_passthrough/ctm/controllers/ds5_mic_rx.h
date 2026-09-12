#ifndef DS5_MIC_RX_H
#define DS5_MIC_RX_H

/* DS5 microphone uplink, TV half (port plan W3-02).
 *
 * With its microphone armed a Bluetooth DualSense sends its voice as Opus
 * packets inside input report 0x31 (flag byte bit 1 set, 71 bytes from report
 * byte 3: CELT SWB stereo, 10 ms, ~100/s). This app never sees those on
 * hidraw in a form it may use: hid-playstation must NOT be bound while the mic
 * is armed (the kernel parses the audio as pad state and fights the mute LED
 * until the TV watchdog reboots -- see ha-voice/MIC-SAFE-DESIGN-2026-07-18.md),
 * and without the driver there is no hidraw node at all. The root daemon
 * ds5_txd watches the air instead (HCI monitor, inbound ACL) and hands each
 * packet to us as a datagram on a per-pad AF_UNIX socket that THIS side binds:
 *
 *     /tmp/ds5_mic.<mac12hex>.sock       (jail view; the daemon sees it under
 *                                          /var/palm/jail/<app>/tmp/)
 *
 * mac12hex = the pad's BT address as 12 lowercase hex digits in display
 * order, the same spelling ds5_acl_tx.c uses for the per-address template.
 *
 * This module only forwards: it validates the datagram (magic, version, the
 * pad address, and that the sender is root via SCM_CREDENTIALS), numbers the
 * frames with a per-session 16-bit sequence starting at 0, and sends each as
 * CTMB_MSG_DS5_MIC. No decoding, and NOTHING here arms the microphone: the
 * daemon forwards only while its own root-owned lever /tmp/ds5_mic is set,
 * and the session starts this receiver only when the host advertised
 * CTMB_HOSTCFG_DS5_MIC in HOST_CONFIG (ds5_native_mic on the host). Arming
 * stays with the existing unbind -> arm -> capture -> disarm -> rebind path
 * (ha-voice/ptt_daemon.py) until that is made safe end to end. */

#include <stddef.h>
#include <stdint.h>

#include "ctm_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Datagram the daemon sends (little-endian, packed): 16-byte header + Opus.
 * Byte-identical definition in src/daemon/ds5_txd/ds5_txd.c (single-file
 * build; keep the two in lockstep -- they travel in one IPK). */
#define DS5_MIC_DGRAM_MAGIC0 'D'
#define DS5_MIC_DGRAM_MAGIC1 'S'
#define DS5_MIC_DGRAM_MAGIC2 '5'
#define DS5_MIC_DGRAM_MAGIC3 'M'
#define DS5_MIC_DGRAM_VERSION 1
#define DS5_MIC_DGRAM_HDR 16
/*  [0..3]  magic "DS5M"
 *  [4]     version (1)
 *  [5]     report flag byte (0x31 byte 1: seq<<4 | bits; bit1 = audio)
 *  [6]     report counter byte (0x31 byte 2)
 *  [7]     opus_len (bytes that follow the header; 71 on today's firmware)
 *  [8..13] pad BT address, LSB-first as on the air
 *  [14..15] daemon per-link counter, LE16 (telemetry only; the session numbers
 *           the frames itself so a daemon restart cannot skip the host's PLC) */
#define DS5_MIC_DGRAM_MAX (DS5_MIC_DGRAM_HDR + 255)

typedef struct ds5_mic_rx ds5_mic_rx_t;

/* Bind the per-pad socket and start the receiver thread. bt_mac is the pad's
 * "aa:bb:cc:dd:ee:ff"; NULL/unparsable (a USB pad) => NULL, nothing started.
 * The thread forwards until ds5_mic_rx_stop() or ctm_ctl_readers_run(c) says
 * the session is tearing down. */
ds5_mic_rx_t *ds5_mic_rx_start(ctm_controller_t *c, const char *bt_mac);

/* Stop + join the thread, unlink the socket, log the session's counters. NULL ok. */
void ds5_mic_rx_stop(ds5_mic_rx_t *r);

#ifdef __cplusplus
}
#endif

#endif /* DS5_MIC_RX_H */
