/* Flydigi XInput gamepad via the kernel xpad driver. See ctm_xpad_evdev.h. */

#include "ctm_xpad_evdev.h"
#include "ctm_state.h" /* read_text_file, contains_ci, usb_busid_from_input_path */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <linux/input.h>

#ifndef BTN_SOUTH
#define BTN_SOUTH 0x130
#define BTN_EAST  0x131
#define BTN_NORTH 0x133
#define BTN_WEST  0x134
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223
#endif
#ifndef ABS_HAT0X
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#endif
#ifndef FF_RUMBLE
#define FF_RUMBLE 0x50
#endif
#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

struct ctm_xpad {
    ctm_controller_t *owner;
    int fd;                  /* evdev node; -1 when closed */
    pthread_t thread;
    int started;
    uint8_t in_ep;           /* IN endpoint of the xpad-claimed iface */
    uint8_t out_ep;          /* OUT endpoint of the xpad-claimed iface */
    int ff_effect_id;        /* evdev FF_RUMBLE effect slot; -1 = none yet */
};

typedef struct {
    uint16_t buttons;
    int8_t hat_x;
    int8_t hat_y;
    uint8_t lt;
    uint8_t rt;
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
} xpad_evdev_state_t;

static void xpad_sync_hat_buttons(xpad_evdev_state_t *st)
{
    st->buttons &= (uint16_t)~0x000fu;
    if (st->hat_x < 0) st->buttons |= 0x0004u;
    else if (st->hat_x > 0) st->buttons |= 0x0008u;
    if (st->hat_y < 0) st->buttons |= 0x0001u;
    else if (st->hat_y > 0) st->buttons |= 0x0002u;
}

static void xpad_apply_hat(xpad_evdev_state_t *st, uint16_t code, int value)
{
    if (code == ABS_HAT0X) {
        st->hat_x = (int8_t)value;
    } else if (code == ABS_HAT0Y) {
        st->hat_y = (int8_t)value;
    } else {
        return;
    }
    xpad_sync_hat_buttons(st);
}

static void xpad_map_button(xpad_evdev_state_t *st, uint16_t code, int value)
{
    uint16_t mask = 0;
    switch (code) {
    case BTN_SOUTH: mask = 0x1000; break;
    case BTN_EAST: mask = 0x2000; break;
    case BTN_WEST: mask = 0x8000; break;
    case BTN_NORTH: mask = 0x4000; break;
    case BTN_TL: mask = 0x0100; break;
    case BTN_TR: mask = 0x0200; break;
    case BTN_SELECT: mask = 0x0020; break;
    case BTN_START: mask = 0x0010; break;
    case BTN_MODE: mask = 0x0400; break;
    case BTN_THUMBL: mask = 0x0040; break;
    case BTN_THUMBR: mask = 0x0080; break;
    default:
        if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT) {
            static const uint16_t dpad_map[] = {0x0001, 0x0002, 0x0004, 0x0008};
            st->hat_x = 0;
            st->hat_y = 0;
            st->buttons &= (uint16_t)~0x000fu;
            if (value) st->buttons |= dpad_map[code - BTN_DPAD_UP];
            return;
        }
        break;
    }
    if (!mask) return;
    if (value) st->buttons |= mask;
    else st->buttons &= (uint16_t)~mask;
}

static void xpad_build_hid_report(const xpad_evdev_state_t *st, uint8_t *buf)
{
    memset(buf, 0, 20);
    buf[0] = 0x00;
    buf[1] = 0x14;
    buf[2] = (uint8_t)(st->buttons & 0xffu);
    buf[3] = (uint8_t)((st->buttons >> 8) & 0xffu);
    buf[4] = st->lt;
    buf[5] = st->rt;
    buf[6] = (uint8_t)(st->lx & 0xff);
    buf[7] = (uint8_t)((st->lx >> 8) & 0xff);
    buf[8] = (uint8_t)(st->ly & 0xff);
    buf[9] = (uint8_t)((st->ly >> 8) & 0xff);
    buf[10] = (uint8_t)(st->rx & 0xff);
    buf[11] = (uint8_t)((st->rx >> 8) & 0xff);
    buf[12] = (uint8_t)(st->ry & 0xff);
    buf[13] = (uint8_t)((st->ry >> 8) & 0xff);
}

static bool iface_is_xpad_gamepad_ff(const char *ifdir)
{
    char cls[8] = {0}, sub[8] = {0}, proto[8] = {0};
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/bInterfaceClass", ifdir);
    read_text_file(path, cls, sizeof(cls));
    snprintf(path, sizeof(path), "%s/bInterfaceSubClass", ifdir);
    read_text_file(path, sub, sizeof(sub));
    snprintf(path, sizeof(path), "%s/bInterfaceProtocol", ifdir);
    read_text_file(path, proto, sizeof(proto));
    return strcmp(cls, "ff") == 0 && strcmp(sub, "5d") == 0 && strtoul(proto, NULL, 16) == 0x01u;
}

static int find_xpad_iface_endpoints(ctm_composite_t *cp, uint8_t *in_ep, uint8_t *out_ep)
{
    if (in_ep) *in_ep = 0;
    if (out_ep) *out_ep = 0;
    char usbdir[512];
    if (ctm_composite_usb_device_dir(cp, usbdir, sizeof(usbdir)) != 0) return -1;
    const char *base = strrchr(usbdir, '/');
    base = base ? base + 1 : usbdir;
    size_t blen = strlen(base);
    DIR *d = opendir(usbdir);
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    uint8_t fallback_in = 0;
    uint8_t fallback_out = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, base, blen) != 0 || e->d_name[blen] != ':') continue;
        char ifdir[1024], clsf[1100], cls[8] = {0};
        snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, e->d_name);
        snprintf(clsf, sizeof(clsf), "%s/bInterfaceClass", ifdir);
        if (read_text_file(clsf, cls, sizeof(cls)) != 0) {
            continue;
        }
        const bool is_hid = strcmp(cls, "03") == 0;
        const bool is_xpad_ff = iface_is_xpad_gamepad_ff(ifdir);
        if (!is_hid && !is_xpad_ff) {
            continue;
        }
        char hidpath[64];
        if (is_hid && ctm_composite_find_hidraw_under(ifdir, hidpath, sizeof(hidpath)) == 0) {
            continue;
        }
        uint8_t in = 0, out = 0;
        ctm_composite_iface_endpoints(ifdir, &in, &out);
        if (in == 0) {
            continue;
        }
        if (is_xpad_ff) {
            if (in_ep) *in_ep = in;
            if (out_ep) *out_ep = out;
            rc = 0;
            break;
        }
        if (fallback_in == 0) {
            fallback_in = in;
            fallback_out = out;
        }
    }
    closedir(d);
    if (rc != 0 && fallback_in != 0) {
        if (in_ep) *in_ep = fallback_in;
        if (out_ep) *out_ep = fallback_out;
        rc = 0;
    }
    return rc;
}

static void xpad_stop_rumble(ctm_xpad_t *x)
{
    if (!x || x->fd < 0 || x->ff_effect_id < 0) {
        return;
    }
    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = (uint16_t)x->ff_effect_id;
    play.value = 0;
    (void)write(x->fd, &play, sizeof(play));
}

/* Parse Xbox 360 wired motor bytes from a host OUT report. Returns 0 only for
 * rumble payloads (00 08 [strong] [weak] ...). Short vendor handshakes on ep
 * 0x05 (e.g. 01 03 02) are ignored so they are not mistaken for motors. */
static int xpad_parse_rumble_motors(const uint8_t *payload, size_t len,
                                    uint8_t *strong_out, uint8_t *weak_out)
{
    if (!payload || !strong_out || !weak_out || len < 4) {
        return -1;
    }
    if (payload[0] == 0x01) {
        return -1;
    }
    if (payload[0] != 0x00 || payload[1] != 0x08) {
        return -1;
    }
    *strong_out = payload[2];
    *weak_out = payload[3];
    return 0;
}

void ctm_xpad_apply_rumble(ctm_xpad_t *x, const uint8_t *payload, size_t len)
{
    if (!x || x->fd < 0 || !payload) {
        return;
    }
    uint8_t weak = 0;
    uint8_t strong = 0;
    if (xpad_parse_rumble_motors(payload, len, &strong, &weak) != 0) {
        return;
    }

    struct ff_effect eff;
    memset(&eff, 0, sizeof(eff));
    eff.type = FF_RUMBLE;
    eff.id = x->ff_effect_id;
    eff.u.rumble.weak_magnitude = (uint16_t)((unsigned)weak * 0xffffu / 255u);
    eff.u.rumble.strong_magnitude = (uint16_t)((unsigned)strong * 0xffffu / 255u);
    if (ioctl(x->fd, EVIOCSFF, &eff) < 0) {
        return;
    }
    if (x->ff_effect_id < 0) {
        x->ff_effect_id = eff.id;
    }

    if (!weak && !strong) {
        xpad_stop_rumble(x);
        return;
    }

    struct input_event play;
    memset(&play, 0, sizeof(play));
    gettimeofday(&play.time, NULL);
    play.type = EV_FF;
    play.code = (uint16_t)eff.id;
    play.value = 1;
    (void)write(x->fd, &play, sizeof(play));
}

int ctm_xpad_find_evdev(const char *usb_busid, char *path_out, size_t path_len)
{
    if (!usb_busid || !usb_busid[0] || !path_out || path_len == 0) return -1;
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *ent;
    int rc = -1;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char input_path[PATH_MAX], busid[64] = {0};
        snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", ent->d_name);
        usb_busid_from_input_path(input_path, busid, sizeof(busid));
        if (strcmp(busid, usb_busid) != 0) continue;

        char vendor[16] = {0}, product[16] = {0}, name[128] = {0};
        char attr[PATH_MAX];
        snprintf(attr, sizeof(attr), "%s/id/vendor", input_path);
        read_text_file(attr, vendor, sizeof(vendor));
        snprintf(attr, sizeof(attr), "%s/id/product", input_path);
        read_text_file(attr, product, sizeof(product));
        snprintf(attr, sizeof(attr), "%s/name", input_path);
        read_text_file(attr, name, sizeof(name));
        if (!ctm_hex_equals(vendor, 0x045e) || !ctm_hex_equals(product, 0x028e)) {
            if (!contains_ci(name, "x-box") && !contains_ci(name, "xbox")) continue;
        }

        DIR *input = opendir(input_path);
        if (!input) continue;
        struct dirent *child;
        while ((child = readdir(input)) != NULL) {
            if (strncmp(child->d_name, "event", 5) != 0) continue;
            snprintf(path_out, path_len, "/dev/input/%s", child->d_name);
            rc = 0;
            break;
        }
        closedir(input);
        if (rc == 0) break;
    }
    closedir(d);
    return rc;
}

static void xpad_send_report(ctm_xpad_t *x, const xpad_evdev_state_t *st)
{
    if (!x || x->in_ep == 0) return;
    uint8_t buf[20];
    /* Overlay gate (event-driven flavor): while the overlay owns input, send a
     * neutral report per event instead of the real state. Stick jitter fires
     * evdev events continuously, so a press held across the gate edge is
     * released host-side within a few events. */
    if (ui_should_block_input()) {
        xpad_evdev_state_t neutral;
        memset(&neutral, 0, sizeof(neutral));
        xpad_build_hid_report(&neutral, buf);
    } else {
        xpad_build_hid_report(st, buf);
    }
    if (ctm_ctl_send(x->owner, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, x->in_ep,
                     buf, sizeof(buf)) == 0) {
        ctm_ctl_note_input_report(x->owner);
    }
}

static void *evdev_gamepad_thread_main(void *arg)
{
    ctm_xpad_t *x = (ctm_xpad_t *)arg;
    xpad_evdev_state_t st;
    memset(&st, 0, sizeof(st));
    xpad_send_report(x, &st);

    /* The readers gate: on link loss run_session exits with stop still 0;
     * teardown clears the gate before the join, so without it this loop never
     * terminates and the join wedges the session thread (no reconnect until
     * unplug). */
    while (ctm_ctl_readers_run(x->owner) && x->fd >= 0) {
        struct pollfd pfd;
        pfd.fd = x->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;
        for (;;) {
            struct input_event ev;
            ssize_t n = read(x->fd, &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev)) break;
            if (ev.type == EV_KEY) {
                xpad_map_button(&st, ev.code, ev.value);
                xpad_send_report(x, &st);
            } else if (ev.type == EV_ABS) {
                switch (ev.code) {
                case ABS_X: st.lx = (int16_t)ev.value; break;
                case ABS_Y: st.ly = (int16_t)(-(int)ev.value); break;
                case ABS_RX: st.rx = (int16_t)ev.value; break;
                case ABS_RY: st.ry = (int16_t)(-(int)ev.value); break;
                case ABS_Z: st.lt = (uint8_t)(ev.value > 255 ? ev.value >> 7 : ev.value); break;
                case ABS_RZ: st.rt = (uint8_t)(ev.value > 255 ? ev.value >> 7 : ev.value); break;
                case ABS_HAT0X:
                case ABS_HAT0Y:
                    xpad_apply_hat(&st, ev.code, ev.value);
                    break;
                default: continue;
                }
                xpad_send_report(x, &st);
            }
        }
    }
    return NULL;
}

ctm_xpad_t *ctm_xpad_create(ctm_controller_t *owner)
{
    ctm_xpad_t *x = (ctm_xpad_t *)calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->owner = owner;
    x->fd = -1;
    x->ff_effect_id = -1;
    return x;
}

void ctm_xpad_destroy(ctm_xpad_t *x)
{
    free(x);
}

bool ctm_xpad_active(const ctm_xpad_t *x) { return x && x->fd >= 0; }
uint8_t ctm_xpad_out_ep(const ctm_xpad_t *x) { return x ? x->out_ep : 0; }

int ctm_xpad_start(ctm_xpad_t *x, ctm_composite_t *cp, const char *usb_busid)
{
    if (!x || !cp || !usb_busid || !usb_busid[0]) {
        return -1;
    }
    if (find_xpad_iface_endpoints(cp, &x->in_ep, &x->out_ep) != 0 || x->in_ep == 0) {
        ctm_ctl_log(x->owner, "xpad interface endpoint not found busid=%s", usb_busid);
        return -1;
    }

    char evdev_path[64];
    if (ctm_xpad_find_evdev(usb_busid, evdev_path, sizeof(evdev_path)) != 0) {
        ctm_ctl_log(x->owner, "xpad evdev not found busid=%s", usb_busid);
        return -1;
    }

    int fd = open(evdev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ctm_ctl_log(x->owner, "xpad evdev open failed path=%s errno=%d", evdev_path, errno);
        return -1;
    }
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        ctm_ctl_log(x->owner, "xpad evdev grab failed path=%s errno=%d (continuing)",
                    evdev_path, errno);
    }
    x->ff_effect_id = -1;
    x->fd = fd;
    if (pthread_create(&x->thread, NULL, evdev_gamepad_thread_main, x) == 0) {
        x->started = 1;
        ctm_ctl_log(x->owner, "xpad evdev feeder started path=%s in_ep=0x%02x out_ep=0x%02x",
                    evdev_path, x->in_ep, x->out_ep);
        return 0;
    }
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
    x->fd = -1;
    return -1;
}

void ctm_xpad_stop(ctm_xpad_t *x)
{
    if (!x) return;
    if (x->started) {
        pthread_join(x->thread, NULL);
        x->started = 0;
    }
    if (x->fd >= 0) {
        xpad_stop_rumble(x);
        ioctl(x->fd, EVIOCGRAB, 0);
        close(x->fd);
        x->fd = -1;
        x->ff_effect_id = -1;
    }
}
