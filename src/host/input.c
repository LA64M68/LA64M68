#define _POSIX_C_SOURCE 200809L

#include "input.h"
#include "../core/debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __has_include
# if __has_include(<linux/input.h>)
#  define LA64M68_HAVE_EVDEV 1
# endif
#endif

#ifdef LA64M68_HAVE_EVDEV
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <errno.h>
#endif

/* evdev backend (Linux): opens keyboard/mouse/gamepad event devices,
 * nonblocking poll -> vHID queue. devices_csv = comma list of
 * /dev/input/eventN paths; NULL/"" = auto-scan /dev/input. */

#define MAX_DEVS 8

struct la64m68_input {
    la64m68_vhid *vhid;
    int grab;
    /* Ctrl+Alt+Pause focus chord (vHID feature) */
    int chord_ctrl, chord_alt, chord_active;
#ifdef LA64M68_HAVE_EVDEV
    int fds[MAX_DEVS];
    int nfds;
#endif
};

/* evdev KEY_* -> Amiga raw key code (subset; unmapped keys pass a
 * synthetic code in the high range so they stay visible in traces) */
static uint8_t keymap[KEY_CNT];

static void keymap_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    memset(keymap, 0x7f, sizeof(keymap));
#ifdef LA64M68_HAVE_EVDEV
    keymap[KEY_ESC] = 0x45; keymap[KEY_1] = 0x01; keymap[KEY_2] = 0x02;
    keymap[KEY_3] = 0x03;   keymap[KEY_4] = 0x04; keymap[KEY_5] = 0x05;
    keymap[KEY_6] = 0x06;   keymap[KEY_7] = 0x07; keymap[KEY_8] = 0x08;
    keymap[KEY_9] = 0x09;   keymap[KEY_0] = 0x0a;
    keymap[KEY_MINUS] = 0x0b;      keymap[KEY_EQUAL] = 0x0c;
    keymap[KEY_BACKSPACE] = 0x41;  keymap[KEY_TAB] = 0x42;
    keymap[KEY_Q] = 0x10; keymap[KEY_W] = 0x11; keymap[KEY_E] = 0x12;
    keymap[KEY_R] = 0x13; keymap[KEY_T] = 0x14; keymap[KEY_Y] = 0x15;
    keymap[KEY_U] = 0x16; keymap[KEY_I] = 0x17; keymap[KEY_O] = 0x18;
    keymap[KEY_P] = 0x19; keymap[KEY_LEFTBRACE] = 0x1a;
    keymap[KEY_RIGHTBRACE] = 0x1b; keymap[KEY_ENTER] = 0x44;
    keymap[KEY_LEFTCTRL] = 0x63;
    keymap[KEY_A] = 0x20; keymap[KEY_S] = 0x21; keymap[KEY_D] = 0x22;
    keymap[KEY_F] = 0x23; keymap[KEY_G] = 0x24; keymap[KEY_H] = 0x25;
    keymap[KEY_J] = 0x26; keymap[KEY_K] = 0x27; keymap[KEY_L] = 0x28;
    keymap[KEY_SEMICOLON] = 0x29;  keymap[KEY_APOSTROPHE] = 0x2a;
    keymap[KEY_GRAVE] = 0x00;      keymap[KEY_LEFTSHIFT] = 0x60;
    keymap[KEY_BACKSLASH] = 0x2b;
    keymap[KEY_Z] = 0x31; keymap[KEY_X] = 0x32; keymap[KEY_C] = 0x33;
    keymap[KEY_V] = 0x34; keymap[KEY_B] = 0x35; keymap[KEY_N] = 0x36;
    keymap[KEY_M] = 0x37; keymap[KEY_COMMA] = 0x38;
    keymap[KEY_DOT] = 0x39; keymap[KEY_SLASH] = 0x3a;
    keymap[KEY_RIGHTSHIFT] = 0x61; keymap[KEY_LEFTALT] = 0x64;
    keymap[KEY_SPACE] = 0x40;      keymap[KEY_CAPSLOCK] = 0x62;
    keymap[KEY_F1] = 0x50; keymap[KEY_F2] = 0x51; keymap[KEY_F3] = 0x52;
    keymap[KEY_F4] = 0x53; keymap[KEY_F5] = 0x54; keymap[KEY_F6] = 0x55;
    keymap[KEY_F7] = 0x56; keymap[KEY_F8] = 0x57; keymap[KEY_F9] = 0x58;
    keymap[KEY_F10] = 0x59;
    keymap[KEY_UP] = 0x4c; keymap[KEY_DOWN] = 0x4d;
    keymap[KEY_RIGHT] = 0x4e; keymap[KEY_LEFT] = 0x4f;
    keymap[KEY_DELETE] = 0x46;     keymap[KEY_HOME] = 0x47;
    keymap[KEY_END] = 0x48;        keymap[KEY_PAGEUP] = 0x49;
    keymap[KEY_PAGEDOWN] = 0x4a;
    keymap[KEY_RIGHTALT] = 0x65;   keymap[KEY_RIGHTCTRL] = 0x63;
    keymap[KEY_LEFTMETA] = 0x66;   keymap[KEY_RIGHTMETA] = 0x67;
#endif
}

#ifdef LA64M68_HAVE_EVDEV
static int dev_is_interesting(int fd)
{
    unsigned long evbits[(EV_MAX + 63) / 64] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return 0;
    int has_key = evbits[EV_KEY / 64] & (1ul << (EV_KEY % 64));
    int has_rel = evbits[EV_REL / 64] & (1ul << (EV_REL % 64));
    int has_abs = evbits[EV_ABS / 64] & (1ul << (EV_ABS % 64));
    return has_key || has_rel || has_abs;
}

static int dev_open(la64m68_input *i, const char *path)
{
    if (i->nfds >= MAX_DEVS) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (!dev_is_interesting(fd)) { close(fd); return -1; }
    i->fds[i->nfds++] = fd;
    la64m68_trace("input: opened %s", path);
    return 0;
}

static void autoscan(la64m68_input *i)
{
    for (int n = 0; n < 32 && i->nfds < MAX_DEVS; n++) {
        char p[64];
        snprintf(p, sizeof(p), "/dev/input/event%d", n);
        dev_open(i, p);
    }
}
#endif

la64m68_input *la64m68_input_create(la64m68_vhid *vhid,
                                    const char *devices_csv)
{
    la64m68_input *i = calloc(1, sizeof(*i));
    if (!i) return NULL;
    i->vhid = vhid;
    keymap_init();

#ifdef LA64M68_HAVE_EVDEV
    if (devices_csv && *devices_csv) {
        char *list = strdup(devices_csv);
        for (char *t = strtok(list, ","); t && i->nfds < MAX_DEVS;
             t = strtok(NULL, ","))
            dev_open(i, t);
        free(list);
    } else {
        autoscan(i);
    }
    la64m68_trace("input: evdev backend, %d device(s)", i->nfds);
#else
    (void)devices_csv;
    la64m68_trace("input: null backend (no evdev on this platform)");
#endif
    return i;
}

void la64m68_input_poll(la64m68_input *i)
{
#ifdef LA64M68_HAVE_EVDEV
    struct input_event ev[16];
    for (int d = 0; d < i->nfds; d++) {
        ssize_t n;
        while ((n = read(i->fds[d], ev, sizeof(ev))) > 0) {
            for (ssize_t k = 0; k < n / (ssize_t)sizeof(*ev); k++) {
                struct input_event *e = &ev[k];
                if (e->type == EV_KEY) {
                    /* Focus chord Ctrl+Alt+Pause. Evaluated BEFORE the
                     * forwarding gate so it works in both directions, and
                     * never passed on -- the guest would otherwise get a
                     * Pause storm. A bare Pause without the modifiers still
                     * reaches the guest normally. */
                    if (e->code == KEY_LEFTCTRL || e->code == KEY_RIGHTCTRL) {
                        i->chord_ctrl = !!e->value;
                    } else if (e->code == KEY_LEFTALT || e->code == KEY_RIGHTALT) {
                        i->chord_alt = !!e->value;
                    } else if (e->code == KEY_PAUSE &&
                               (i->chord_active ||
                                (i->chord_ctrl && i->chord_alt))) {
                        if (e->value == 1 && i->chord_ctrl && i->chord_alt) {
                            int f = la64m68_vhid_toggle_focus(i->vhid);
                            la64m68_input_grab(i, f == LA64M68_VHID_FOCUS_GUEST);
                            la64m68_trace("input: focus -> %s",
                                          f == LA64M68_VHID_FOCUS_GUEST
                                              ? "LA64M68" : "host");
                            i->chord_active = 1;
                        } else if (e->value == 0) {
                            i->chord_active = 0;
                        }
                        continue;
                    }

                    /* Host focus: Linux keeps its input, nothing is queued. */
                    if (la64m68_vhid_focus(i->vhid) != LA64M68_VHID_FOCUS_GUEST)
                        continue;

                    if (e->code == BTN_LEFT || e->code == BTN_RIGHT ||
                        e->code == BTN_MIDDLE) {
                        int idx = e->code == BTN_LEFT ? 0 :
                                  e->code == BTN_RIGHT ? 1 : 2;
                        la64m68_vhid_push_button(i->vhid, idx, !!e->value);
                    } else if (e->code == BTN_SOUTH || e->code == BTN_EAST) {
                        la64m68_vhid_push_button(i->vhid,
                            e->code == BTN_SOUTH ? 3 : 4, !!e->value);
                    } else if (e->code < KEY_CNT && e->value != 2) {
                        uint8_t c = keymap[e->code];
                        if (c == 0x7f) c = 0x70 | ((e->code >> 4) & 0xf);
                        la64m68_vhid_push_key(i->vhid, c, !!e->value);
                    }
                } else if (e->type == EV_REL) {
                    if (la64m68_vhid_focus(i->vhid) != LA64M68_VHID_FOCUS_GUEST)
                        continue;
                    if (e->code == REL_X)
                        la64m68_vhid_push_move(i->vhid, e->value, 0);
                    else if (e->code == REL_Y)
                        la64m68_vhid_push_move(i->vhid, 0, e->value);
                } else if (e->type == EV_ABS) {
                    /* gamepad axes pending: need per-device centering */
                }
            }
        }
    }
#else
    (void)i;
#endif
}

void la64m68_input_grab(la64m68_input *i, int grab)
{
    if (!i) return;
    i->grab = grab;
#ifdef LA64M68_HAVE_EVDEV
    for (int d = 0; d < i->nfds; d++)
        ioctl(i->fds[d], EVIOCGRAB, (void *)(long)(grab ? 1 : 0));
#endif
}

void la64m68_input_destroy(la64m68_input *i)
{
    if (!i) return;
#ifdef LA64M68_HAVE_EVDEV
    la64m68_input_grab(i, 0);
    for (int d = 0; d < i->nfds; d++)
        close(i->fds[d]);
#endif
    free(i);
}
