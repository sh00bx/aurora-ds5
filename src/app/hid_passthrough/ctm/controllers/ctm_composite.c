/* Composite USB controllers: forward every HID interface. See ctm_composite.h. */

#include "ctm_composite.h"
#include "ctm_state.h" /* read_text_file, composite_usb_device_dir_by_busid */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One forwarded sibling HID interface. The primary is the pump's own fd; these
 * are the rest. Each input report is tagged with the interface's IN endpoint. */
typedef struct {
    ctm_composite_t *cp;
    int fd;
    uint8_t in_ep;
    uint8_t out_ep;
    uint8_t iface;
    pthread_t thread;
    int started;
} comp_iface_t;

#define CTM_COMPOSITE_MAX_SIBLINGS 15

struct ctm_composite {
    ctm_controller_t *owner;
    unsigned int vid;
    unsigned int pid;
    char usb_busid[64];
    char primary_path[64];

    comp_iface_t sib[CTM_COMPOSITE_MAX_SIBLINGS];
    int count;
    uint8_t primary_in_ep;   /* the primary hidraw's IN endpoint (input tag) */
    uint8_t primary_out_ep;  /* the primary hidraw's OUT endpoint (output route) */
    uint8_t primary_iface;   /* the primary hidraw's USB interface number */
};

/* Resolve the USB device dir (the one holding idVendor) for vid/pid via the
 * reliable /sys/class/input path. Mirrors grab_matching_evdev. 0 on success. */
static int resolve_usb_device_dir(unsigned int vid, unsigned int pid, char *out, size_t out_len)
{
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *e; int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (strncmp(e->d_name, "input", 5) != 0) continue;
        char attr[256], v[32] = {0}, p[32] = {0};
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", e->d_name);
        read_text_file(attr, v, sizeof(v));
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", e->d_name);
        read_text_file(attr, p, sizeof(p));
        if (!ctm_hex_equals(v, vid) || !ctm_hex_equals(p, pid)) continue;
        /* PATH_MAX, not a smaller guess: glibc's realpath writes into this
         * buffer bounded only by PATH_MAX, so anything shorter is an
         * environment-controlled overflow of this frame. */
        char link[256], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", e->d_name);
        if (!realpath(link, real)) continue;
        while (real[0]) {                       /* walk up to the USB device dir */
            char idf[PATH_MAX + 16];
            snprintf(idf, sizeof(idf), "%s/idVendor", real);
            if (access(idf, F_OK) == 0) { snprintf(out, out_len, "%s", real); rc = 0; break; }
            char *s = strrchr(real, '/'); if (!s || s == real) break; *s = '\0';
        }
    }
    closedir(d);
    return rc;
}

void ctm_composite_iface_endpoints(const char *ifdir, uint8_t *in_ep, uint8_t *out_ep)
{
    DIR *d = opendir(ifdir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "ep_", 3) != 0) continue;
        char epf[1200], val[16] = {0};
        snprintf(epf, sizeof(epf), "%s/%s/bEndpointAddress", ifdir, e->d_name);
        if (read_text_file(epf, val, sizeof(val)) != 0) continue;
        unsigned int a = (unsigned int)strtoul(val, NULL, 16);
        if (a & 0x80u) { if (in_ep && *in_ep == 0) *in_ep = (uint8_t)a; }
        else { if (out_ep && *out_ep == 0) *out_ep = (uint8_t)a; }
    }
    closedir(d);
}

int ctm_composite_find_hidraw_under(const char *ifdir, char *out, size_t outlen)
{
    DIR *d = opendir(ifdir); if (!d) return -1;
    struct dirent *e; int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (e->d_name[0] == '.') continue;
        char hp[1200]; snprintf(hp, sizeof(hp), "%s/%s/hidraw", ifdir, e->d_name);
        DIR *h = opendir(hp);
        if (h) {
            struct dirent *he;
            while ((he = readdir(h)) != NULL) {
                if (strncmp(he->d_name, "hidraw", 6) == 0) {
                    snprintf(out, outlen, "/dev/%s", he->d_name); rc = 0; break;
                }
            }
            closedir(h);
        }
    }
    closedir(d);
    return rc;
}

int ctm_composite_usb_device_dir(const ctm_composite_t *cp, char *out, size_t out_len)
{
    if (!cp) return -1;
    if (cp->usb_busid[0] &&
        composite_usb_device_dir_by_busid(cp->usb_busid, out, out_len) == 0) {
        return 0;
    }
    return resolve_usb_device_dir(cp->vid, cp->pid, out, out_len);
}

ctm_composite_t *ctm_composite_create(ctm_controller_t *owner,
                                      unsigned int vid, unsigned int pid,
                                      const char *usb_busid, const char *primary_path)
{
    ctm_composite_t *cp = (ctm_composite_t *)calloc(1, sizeof(*cp));
    if (!cp) return NULL;
    cp->owner = owner;
    cp->vid = vid;
    cp->pid = pid;
    snprintf(cp->usb_busid, sizeof(cp->usb_busid), "%s", usb_busid ? usb_busid : "");
    snprintf(cp->primary_path, sizeof(cp->primary_path), "%s", primary_path ? primary_path : "");
    return cp;
}

void ctm_composite_destroy(ctm_composite_t *cp)
{
    free(cp);
}

int ctm_composite_count(const ctm_composite_t *cp) { return cp ? cp->count : 0; }
uint8_t ctm_composite_primary_in_ep(const ctm_composite_t *cp) { return cp ? cp->primary_in_ep : 0; }
uint8_t ctm_composite_primary_out_ep(const ctm_composite_t *cp) { return cp ? cp->primary_out_ep : 0; }

int ctm_composite_out_fd_for_ep(const ctm_composite_t *cp, uint8_t ep, int fallback_fd)
{
    if (!cp || ep == 0 || ep == cp->primary_out_ep) return fallback_fd;
    for (int i = 0; i < cp->count; ++i) {
        if (cp->sib[i].out_ep == ep) return cp->sib[i].fd;
    }
    return fallback_fd;
}

int ctm_composite_feature_fd(const ctm_composite_t *cp, bool composite,
                             uint32_t request_id, int fallback_fd)
{
    if (!cp || !composite) return fallback_fd;
    uint8_t iface = (uint8_t)(request_id >> 24);
    if (iface == cp->primary_iface) return fallback_fd;
    for (int i = 0; i < cp->count; ++i) {
        if (cp->sib[i].iface == iface) return cp->sib[i].fd;
    }
    return fallback_fd;
}

void ctm_composite_open(ctm_composite_t *cp)
{
    if (!cp) return;
    char usbdir[512];
    if (ctm_composite_usb_device_dir(cp, usbdir, sizeof(usbdir)) != 0) {
        ctm_ctl_log(cp->owner, "composite: USB device dir unresolved vid=%04x pid=%04x busid=%s",
                    cp->vid, cp->pid, cp->usb_busid[0] ? cp->usb_busid : "-");
        return;
    }
    const char *base = strrchr(usbdir, '/'); base = base ? base + 1 : usbdir;
    size_t blen = strlen(base);
    DIR *d = opendir(usbdir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && cp->count < CTM_COMPOSITE_MAX_SIBLINGS) {
        if (strncmp(e->d_name, base, blen) != 0 || e->d_name[blen] != ':') continue;
        char ifdir[1024], clsf[1100], cls[8] = {0};
        snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, e->d_name);
        snprintf(clsf, sizeof(clsf), "%s/bInterfaceClass", ifdir);
        if (read_text_file(clsf, cls, sizeof(cls)) != 0 || strcmp(cls, "03") != 0) continue;
        char hidpath[64];
        if (ctm_composite_find_hidraw_under(ifdir, hidpath, sizeof(hidpath)) != 0) continue;
        uint8_t in_ep = 0, out_ep = 0, iface = 0xff;
        char numf[1100], num[8] = {0};
        snprintf(numf, sizeof(numf), "%s/bInterfaceNumber", ifdir);
        if (read_text_file(numf, num, sizeof(num)) == 0) iface = (uint8_t)strtoul(num, NULL, 16);
        ctm_composite_iface_endpoints(ifdir, &in_ep, &out_ep);
        if (strcmp(hidpath, cp->primary_path) == 0) {            /* the primary */
            cp->primary_in_ep = in_ep; cp->primary_out_ep = out_ep; cp->primary_iface = iface;
            ctm_ctl_log(cp->owner, "composite primary %s if=%u in_ep=0x%02x out_ep=0x%02x",
                        hidpath, iface, in_ep, out_ep);
            continue;
        }
        int fd = open(hidpath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) fd = open(hidpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        comp_iface_t *ci = &cp->sib[cp->count++];
        ci->cp = cp; ci->fd = fd; ci->started = 0;
        ci->in_ep = in_ep; ci->out_ep = out_ep; ci->iface = iface;
        ctm_ctl_log(cp->owner, "composite sibling %s if=%u in_ep=0x%02x out_ep=0x%02x",
                    hidpath, iface, in_ep, out_ep);
    }
    closedir(d);
}

/* Sibling reader: poll one interface's hidraw, forward input tagged with its IN
 * endpoint. When: one thread per sibling, while the session's readers run. */
static void *composite_reader_main(void *arg)
{
    comp_iface_t *ci = (comp_iface_t *)arg;
    ctm_controller_t *owner = ci->cp->owner;
    while (ctm_ctl_readers_run(owner)) {
        struct pollfd pfd; pfd.fd = ci->fd; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;
        for (;;) {
            uint8_t buf[CTM_MAX_REPORT];
            ssize_t n = read(ci->fd, buf, sizeof(buf));
            if (n > 0) {
                if (ctm_ctl_send(owner, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, ci->in_ep,
                                 buf, (size_t)n) != 0) break;
                ctm_ctl_note_input_report(owner);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
    }
    return NULL;
}

void ctm_composite_start_readers(ctm_composite_t *cp)
{
    if (!cp) return;
    for (int i = 0; i < cp->count; ++i) {
        if (pthread_create(&cp->sib[i].thread, NULL, composite_reader_main, &cp->sib[i]) == 0)
            cp->sib[i].started = 1;
    }
    if (cp->count > 0)
        ctm_ctl_log(cp->owner, "composite: forwarding %d sibling interface(s)", cp->count);
}

void ctm_composite_shutdown(ctm_composite_t *cp)
{
    if (!cp) return;
    for (int i = 0; i < cp->count; ++i) {
        if (cp->sib[i].started) { pthread_join(cp->sib[i].thread, NULL); cp->sib[i].started = 0; }
        if (cp->sib[i].fd >= 0) { close(cp->sib[i].fd); cp->sib[i].fd = -1; }
    }
    cp->count = 0;
}
