#include "evkbd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

#include "logging.h"

typedef struct dev_fd_t {
    int fd;
    bool grabbed;
} dev_fd_t;

struct evkbd_t {
    SDL_mutex *lock;
    bool listening;
    /* Sticky, never cleared: an interrupt that lands between the worker deciding
     * to listen and evkbd_listen() setting `listening` would otherwise be
     * overwritten by it, and nobody is left to stop the loop afterwards. */
    bool interrupted;
    dev_fd_t fds[EVKBD_MAX_FDS];
    int nfds;
};

static inline bool has_bit(const uint8_t *bits, uint32_t bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

/*
 * webOS fills /dev/input with a zoo of synthetic devices, and several of them
 * claim the full keyboard capability bitmap: the Magic Remote ("LGE RCU"),
 * "CHECK INPUT", "IoT keypad", "Bluetooth-audio-source", and — most dangerous —
 * "Smart Remote RCU Input", the uinput node that networkinput/sendKeyCode writes
 * into. Grabbing any of those would take the remote away from the TV or swallow
 * our own injected keys, so a capability test alone is not a safe filter.
 *
 * What separates them from a real keyboard is identity: LG's virtual devices use
 * placeholder vendor ids (0x0000, 0x0001, 0x9998), while anything actually
 * plugged in carries a genuine USB/Bluetooth vendor. Requiring both a real vendor
 * and a full typing layout leaves exactly the physical keyboards.
 */
static bool vendor_is_synthetic(uint16_t vendor) {
    return vendor == 0x0000 || vendor == 0x0001 || vendor == 0x9998;
}

static bool is_keyboard(int fd, const char *dev_path) {
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        return false;
    }
    if (vendor_is_synthetic(id.vendor)) {
        return false;
    }

    uint8_t keycaps[(KEY_MAX / 8) + 1];
    memset(keycaps, 0, sizeof(keycaps));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keycaps)), keycaps) < 0) {
        return false;
    }

    /* A full typing layout: letters spread across all three rows, both ends of the
     * number row, and the keys no real keyboard omits. Partial keypads (the
     * Logitech receiver also exposes "Consumer Control" and "System Control"
     * nodes) fail this and are left alone. */
    static const uint16_t required[] = {
            KEY_A, KEY_L, KEY_Q, KEY_P, KEY_Z, KEY_M,
            KEY_1, KEY_0, KEY_ENTER, KEY_SPACE, KEY_BACKSPACE,
            KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_TAB, KEY_ESC,
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!has_bit(keycaps, required[i])) {
            return false;
        }
    }

    /* Leave combined keyboard/pointer devices to SDL and the evmouse path: a grab
     * here would take the pointer with it, and losing the cursor is a worse
     * outcome than losing F12. */
    uint8_t relcaps[(REL_MAX / 8) + 1];
    memset(relcaps, 0, sizeof(relcaps));
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relcaps)), relcaps) >= 0 &&
        (has_bit(relcaps, REL_X) || has_bit(relcaps, REL_Y))) {
        commons_log_info("EvKbd", "Skipping %s: reports pointer axes, leaving it to the mouse path", dev_path);
        return false;
    }

    char name[128] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        name[0] = '\0';
    }
    commons_log_info("EvKbd", "Keyboard: %s (%04x:%04x) '%s'", dev_path, id.vendor, id.product, name);
    return true;
}

static int keyboard_fds_find(dev_fd_t *fds) {
    DIR *dir = opendir("/dev/input");
    if (dir == NULL) {
        commons_log_warn("EvKbd", "Cannot open /dev/input: %d (%s)", errno, strerror(errno));
        return 0;
    }
    struct dirent *ent;
    int nfds = 0;
    while (nfds < EVKBD_MAX_FDS && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char dev_path[64];
        snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", ent->d_name);
        int fd = open(dev_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            /* ENXIO is a node with no device behind it; the jail keeps a static
             * event0..event31 regardless of what is plugged in, so this is the
             * common case and not worth a warning. */
            if (errno != ENXIO && errno != ENODEV) {
                commons_log_warn("EvKbd", "Failed to open %s: %d (%s)", dev_path, errno, strerror(errno));
            }
            continue;
        }
        if (!is_keyboard(fd, dev_path)) {
            close(fd);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].grabbed = false;
        nfds++;
    }
    closedir(dir);
    return nfds;
}

evkbd_t *evkbd_open_default(void) {
    evkbd_t *kbd = calloc(1, sizeof(evkbd_t));
    if (kbd == NULL) {
        return NULL;
    }
    kbd->lock = SDL_CreateMutex();
    kbd->nfds = keyboard_fds_find(kbd->fds);
    if (kbd->nfds <= 0) {
        SDL_DestroyMutex(kbd->lock);
        free(kbd);
        return NULL;
    }
    evkbd_set_grab(kbd, true);
    return kbd;
}

void evkbd_close(evkbd_t *kbd) {
    if (kbd == NULL) {
        return;
    }
    SDL_LockMutex(kbd->lock);
    for (int i = 0; i < kbd->nfds; i++) {
        if (kbd->fds[i].grabbed) {
            ioctl(kbd->fds[i].fd, EVIOCGRAB, 0);
        }
        /* Closing the fd would release the grab anyway — the kernel ties it to the
         * open file description, which is also why a crash can never leave the TV
         * with a dead keyboard. */
        close(kbd->fds[i].fd);
    }
    kbd->nfds = 0;
    SDL_UnlockMutex(kbd->lock);
    SDL_DestroyMutex(kbd->lock);
    free(kbd);
}

void evkbd_set_grab(evkbd_t *kbd, bool grab) {
    if (kbd == NULL) {
        return;
    }
    SDL_LockMutex(kbd->lock);
    for (int i = 0; i < kbd->nfds; i++) {
        if (kbd->fds[i].grabbed == grab) {
            continue;
        }
        if (ioctl(kbd->fds[i].fd, EVIOCGRAB, grab ? 1 : 0) < 0) {
            commons_log_warn("EvKbd", "Failed to %s fd %d: %d (%s)", grab ? "grab" : "ungrab", kbd->fds[i].fd,
                             errno, strerror(errno));
            continue;
        }
        kbd->fds[i].grabbed = grab;
    }
    SDL_UnlockMutex(kbd->lock);
}

int evkbd_device_count(const evkbd_t *kbd) {
    return kbd != NULL ? kbd->nfds : 0;
}

static bool is_listening(evkbd_t *kbd) {
    if (SDL_LockMutex(kbd->lock) != 0) {
        return false;
    }
    bool listening = kbd->listening;
    SDL_UnlockMutex(kbd->lock);
    return listening;
}

/*
 * Retire a device that has gone away. Once the node is dead its fd reports
 * POLLHUP|POLLERR forever, which select() counts as readable, so leaving it in
 * the table spins this loop at 100% for the rest of the stream. The trigger is
 * not only a physical unplug: is_keyboard() accepts Bluetooth keyboards, and
 * those idle-disconnect on their own.
 */
static void drop_fd(evkbd_t *kbd, int index) {
    SDL_LockMutex(kbd->lock);
    int fd = kbd->fds[index].fd;
    if (kbd->fds[index].grabbed) {
        ioctl(fd, EVIOCGRAB, 0);
    }
    close(fd);
    for (int i = index; i + 1 < kbd->nfds; i++) {
        kbd->fds[i] = kbd->fds[i + 1];
    }
    kbd->nfds--;
    int remaining = kbd->nfds;
    SDL_UnlockMutex(kbd->lock);
    commons_log_info("EvKbd", "Device on fd %d is gone, dropped it (%d left)", fd, remaining);
}

void evkbd_listen(evkbd_t *kbd, evkbd_listener_t listener, void *userdata) {
    if (SDL_LockMutex(kbd->lock) != 0) {
        return;
    }
    if (kbd->listening || kbd->interrupted) {
        SDL_UnlockMutex(kbd->lock);
        return;
    }
    kbd->listening = true;
    SDL_UnlockMutex(kbd->lock);

    while (is_listening(kbd)) {
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = -1;
        for (int i = 0; i < kbd->nfds; i++) {
            FD_SET(kbd->fds[i].fd, &fds);
            if (kbd->fds[i].fd > maxfd) {
                maxfd = kbd->fds[i].fd;
            }
        }
        /* A short timeout rather than an indefinite wait so evkbd_interrupt() is
         * observed promptly without needing a wake-up pipe. */
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 20000};
        if (select(maxfd + 1, &fds, NULL, NULL, &timeout) <= 0) {
            continue;
        }
        for (int i = 0; i < kbd->nfds;) {
            if (!FD_ISSET(kbd->fds[i].fd, &fds)) {
                i++;
                continue;
            }
            struct input_event raw;
            ssize_t got = read(kbd->fds[i].fd, &raw, sizeof(raw));
            if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
                /* Do not advance: drop_fd() compacted the table, so fds[i] is
                 * now the entry that used to follow it. */
                drop_fd(kbd, i);
                continue;
            }
            i++;
            if (got != (ssize_t) sizeof(raw) || raw.type != EV_KEY) {
                continue;
            }
            evkbd_event_t event = {
                    .code = raw.code,
                    .pressed = raw.value != 0,
                    .repeat = raw.value == 2,
            };
            listener(&event, userdata);
        }
        if (kbd->nfds == 0) {
            commons_log_info("EvKbd", "No keyboards left, stopping listener");
            break;
        }
    }
}

void evkbd_interrupt(evkbd_t *kbd) {
    if (kbd == NULL || SDL_LockMutex(kbd->lock) != 0) {
        return;
    }
    kbd->interrupted = true;
    kbd->listening = false;
    SDL_UnlockMutex(kbd->lock);
}
