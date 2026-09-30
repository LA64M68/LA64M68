#ifndef LA64M68_VHID_H
#define LA64M68_VHID_H

#include "memory.h"
#include <stdint.h>

/* vHID — guest input device fed by the host (evdev backend later).
 * Guest side: register window with a small event queue: each event is
 * one 32-bit word: [kind:8][code:8][value:16].
 *   kind 1 = key (Amiga-style raw code, bit0 of value = pressed)
 *   kind 2 = mouse move (value packed dx:8|dy:8 signed)
 *   kind 3 = button (value bit0 = pressed, code = button index)
 */

#define LA64M68_VHID_REG_BASE   0x00F10000
#define VHID_QUEUE_DEPTH        32

#define VHID_REG_CTRL     0x00    /* bit0 enable */
#define VHID_REG_STATUS   0x04    /* bit0 events pending */
#define VHID_REG_POP      0x08    /* read: pop next event word */
#define VHID_REG_GRAB     0x0c    /* host grab request (window backend) */

/* Input/output focus switch (KVM-style) -- a vHID feature, not a daemon and
 * deliberately unnamed. Ctrl+Alt+Pause hands keyboard/mouse and the
 * presented frame between the host (Linux keeps everything) and LA64M68 (the
 * guest gets the input and its frame is shown).
 *
 * Default is the host side: the operator must never lose control of the
 * machine (0-POOL/RULES.md: safety before comfort). The chord is detected
 * before the forwarding gate so it works in both directions, and it is never
 * passed on -- the guest would otherwise see a Pause storm. A switch also
 * releases every key still held in the guest, so no Ctrl/Alt is left stuck
 * on the other side. */
#define LA64M68_VHID_FOCUS_HOST   0
#define LA64M68_VHID_FOCUS_GUEST  1

typedef struct la64m68_vhid {
    int enabled;
    uint32_t queue[VHID_QUEUE_DEPTH];
    int q_head, q_tail;
    int grab;
    int focus;                 /* LA64M68_VHID_FOCUS_* */
    uint32_t pressed[8];       /* held guest key codes (256-bit map) */
} la64m68_vhid;

void la64m68_vhid_init(la64m68_vhid *h);
/* host side feed */
void la64m68_vhid_push_key(la64m68_vhid *h, uint8_t code, int pressed);
void la64m68_vhid_push_move(la64m68_vhid *h, int dx, int dy);
void la64m68_vhid_push_button(la64m68_vhid *h, uint8_t idx, int pressed);
/* focus switch */
int  la64m68_vhid_focus(const la64m68_vhid *h);
void la64m68_vhid_set_focus(la64m68_vhid *h, int focus);
int  la64m68_vhid_toggle_focus(la64m68_vhid *h);   /* returns the new focus */
/* guest register window */
la64m68_memory *la64m68_vhid_regs_memory(la64m68_vhid *h);
void            la64m68_vhid_regs_destroy(la64m68_memory *m);

#endif
