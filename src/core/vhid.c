#include "vhid.h"
#include <stdlib.h>
#include <string.h>

void la64m68_vhid_init(la64m68_vhid *h)
{
    memset(h, 0, sizeof(*h));
}

static void push(la64m68_vhid *h, int kind, uint8_t code, uint16_t val)
{
    int next = (h->q_tail + 1) % VHID_QUEUE_DEPTH;
    if (next == h->q_head) return;              /* full: drop the new event */
    h->queue[h->q_tail] = ((uint32_t)kind << 24) | (code << 16) | val;
    h->q_tail = next;
}

void la64m68_vhid_push_key(la64m68_vhid *h, uint8_t code, int pressed)
{
    if (pressed) h->pressed[code >> 5] |=  (1u << (code & 31));
    else         h->pressed[code >> 5] &= ~(1u << (code & 31));
    push(h, 1, code, pressed ? 1 : 0);
}

/* Release everything still held in the guest. A focus switch must never leave
 * Ctrl/Alt stuck on the side we are leaving. Bounded by the queue depth: with
 * more than VHID_QUEUE_DEPTH-1 keys held some releases would be dropped, which
 * is far outside any real chord. */
static void release_all(la64m68_vhid *h)
{
    for (unsigned c = 0; c < 256; c++)
        if (h->pressed[c >> 5] & (1u << (c & 31)))
            push(h, 1, (uint8_t)c, 0);
    memset(h->pressed, 0, sizeof(h->pressed));
}

int la64m68_vhid_focus(const la64m68_vhid *h)
{
    return h ? h->focus : LA64M68_VHID_FOCUS_HOST;
}

void la64m68_vhid_set_focus(la64m68_vhid *h, int focus)
{
    if (!h) return;
    focus = focus ? LA64M68_VHID_FOCUS_GUEST : LA64M68_VHID_FOCUS_HOST;
    if (h->focus == focus) return;
    release_all(h);
    h->focus = focus;
}

int la64m68_vhid_toggle_focus(la64m68_vhid *h)
{
    if (!h) return LA64M68_VHID_FOCUS_HOST;
    la64m68_vhid_set_focus(h, h->focus == LA64M68_VHID_FOCUS_HOST
                              ? LA64M68_VHID_FOCUS_GUEST
                              : LA64M68_VHID_FOCUS_HOST);
    return h->focus;
}

void la64m68_vhid_push_move(la64m68_vhid *h, int dx, int dy)
{
    push(h, 2, 0, ((uint16_t)(int8_t)dx << 8) | (uint8_t)(int8_t)dy);
}

void la64m68_vhid_push_button(la64m68_vhid *h, uint8_t idx, int pressed)
{
    push(h, 3, idx, pressed ? 1 : 0);
}

typedef struct {
    la64m68_memory base;
    la64m68_vhid *h;
} vhid_regs_mem;

static uint32_t hr_r(vhid_regs_mem *m, uint32_t addr)
{
    la64m68_vhid *h = m->h;
    switch (addr - LA64M68_VHID_REG_BASE) {
    case VHID_REG_CTRL:   return h->enabled;
    case VHID_REG_STATUS: return h->q_head != h->q_tail;
    case VHID_REG_GRAB:   return h->grab;
    case VHID_REG_POP: {
        if (h->q_head == h->q_tail) return 0xffffffff;
        uint32_t v = h->queue[h->q_head];
        h->q_head = (h->q_head + 1) % VHID_QUEUE_DEPTH;
        return v;
    }
    default: return 0;
    }
}

static void hr_w(vhid_regs_mem *m, uint32_t addr, uint32_t val)
{
    la64m68_vhid *h = m->h;
    switch (addr - LA64M68_VHID_REG_BASE) {
    case VHID_REG_CTRL: h->enabled = val & 1; break;
    case VHID_REG_GRAB: h->grab = val & 1; break;
    default: break;
    }
}

static uint8_t  hr_r8 (void *c, uint32_t a) { return (uint8_t) hr_r(c, a); }
static uint16_t hr_r16(void *c, uint32_t a) { return (uint16_t)hr_r(c, a); }
static uint32_t hr_r32(void *c, uint32_t a) { return hr_r(c, a); }
static void     hr_w8 (void *c, uint32_t a, uint8_t v)  { hr_w(c, a, v); }
static void     hr_w16(void *c, uint32_t a, uint16_t v) { hr_w(c, a, v); }
static void     hr_w32(void *c, uint32_t a, uint32_t v) { hr_w(c, a, v); }

la64m68_memory *la64m68_vhid_regs_memory(la64m68_vhid *h)
{
    vhid_regs_mem *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->h = h;
    m->base.ctx = m;
    m->base.read8 = hr_r8;   m->base.read16 = hr_r16;  m->base.read32 = hr_r32;
    m->base.write8 = hr_w8;  m->base.write16 = hr_w16; m->base.write32 = hr_w32;
    return &m->base;
}

void la64m68_vhid_regs_destroy(la64m68_memory *m)
{
    free(m);
}
