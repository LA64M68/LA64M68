#include "vnic.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>

void la64m68_vnic_init(la64m68_vnic *n, la64m68_memory *guest_mem)
{
    memset(n, 0, sizeof(*n));
    n->guest_mem = guest_mem;
    n->link_up = 1;
    static const uint8_t defmac[6] = {0x02, 0x4c, 0x41, 0x36, 0x38, 0x01};
    memcpy(n->mac, defmac, 6);              /* locally-administered LA68 */
}

void la64m68_vnic_set_mac(la64m68_vnic *n, const uint8_t mac[6])
{
    memcpy(n->mac, mac, 6);
}

void la64m68_vnic_rx(la64m68_vnic *n, const void *frame, size_t len)
{
    if (!n->enabled || !n->rxbuf || len == 0 || len > 1518) return;
    const uint8_t *p = frame;
    for (size_t i = 0; i < len; i++)
        la64m68_mem_write8(n->guest_mem, n->rxbuf + (uint32_t)i, p[i]);
    n->rx_len = (uint16_t)len;
}

typedef struct {
    la64m68_memory base;
    la64m68_vnic *n;
} vnic_regs_mem;

static uint32_t nr_r(vnic_regs_mem *m, uint32_t addr)
{
    la64m68_vnic *n = m->n;
    switch (addr - LA64M68_VNIC_REG_BASE) {
    case VNIC_REG_CTRL:   return n->enabled;
    case VNIC_REG_STATUS: return (n->link_up ? 1 : 0) | (n->rx_len ? 2 : 0);
    case VNIC_REG_MAC0:
        return ((uint32_t)n->mac[0] << 24) | (n->mac[1] << 16) |
               (n->mac[2] << 8) | n->mac[3];
    case VNIC_REG_MAC4:   return (n->mac[4] << 8) | n->mac[5];
    case VNIC_REG_TXBUF:  return n->txbuf;
    case VNIC_REG_RXBUF:  return n->rxbuf;
    case VNIC_REG_RXSTAT: {
        uint16_t l = n->rx_len;
        n->rx_len = 0;                       /* read clears */
        return l;
    }
    default: return 0;
    }
}

static void nr_w(vnic_regs_mem *m, uint32_t addr, uint32_t val)
{
    la64m68_vnic *n = m->n;
    switch (addr - LA64M68_VNIC_REG_BASE) {
    case VNIC_REG_CTRL:  n->enabled = val & 1; break;
    case VNIC_REG_TXBUF: n->txbuf = val; break;
    case VNIC_REG_RXBUF: n->rxbuf = val; break;
    case VNIC_REG_TXLEN:
        if (n->tx && n->txbuf && val) {
            uint8_t frame[1518];
            size_t len = val > sizeof(frame) ? sizeof(frame) : val;
            for (size_t i = 0; i < len; i++)
                frame[i] = la64m68_mem_read8(n->guest_mem,
                                             n->txbuf + (uint32_t)i);
            n->tx(n->tx_ctx, frame, len);
        }
        break;
    default: break;
    }
}

static uint8_t  nr_r8 (void *c, uint32_t a) { return (uint8_t) nr_r(c, a); }
static uint16_t nr_r16(void *c, uint32_t a) { return (uint16_t)nr_r(c, a); }
static uint32_t nr_r32(void *c, uint32_t a) { return nr_r(c, a); }
static void     nr_w8 (void *c, uint32_t a, uint8_t v)  { nr_w(c, a, v); }
static void     nr_w16(void *c, uint32_t a, uint16_t v) { nr_w(c, a, v); }
static void     nr_w32(void *c, uint32_t a, uint32_t v) { nr_w(c, a, v); }

la64m68_memory *la64m68_vnic_regs_memory(la64m68_vnic *n)
{
    vnic_regs_mem *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->n = n;
    m->base.ctx = m;
    m->base.read8 = nr_r8;   m->base.read16 = nr_r16;  m->base.read32 = nr_r32;
    m->base.write8 = nr_w8;  m->base.write16 = nr_w16; m->base.write32 = nr_w32;
    return &m->base;
}

void la64m68_vnic_regs_destroy(la64m68_memory *m)
{
    free(m);
}
