#ifndef LA64M68_VNIC_H
#define LA64M68_VNIC_H

#include "memory.h"
#include <stdint.h>
#include <stddef.h>

/* vNIC — virtual Zorro/PCI-style NIC. Guest side: register window +
 * shared tx/rx ring buffers in guest RAM. Host side: tx callback into
 * the host net backend (TAP bridge/MASQ later, null now).
 * cfg: vnic.mode={off,bridge,masq} default off.
 */

#define LA64M68_VNIC_REG_BASE   0x00F20000

#define VNIC_REG_CTRL     0x00    /* bit0 enable */
#define VNIC_REG_STATUS   0x04    /* bit0 link up, bit1 rx pending */
#define VNIC_REG_MAC0     0x08    /* mac[0..3] */
#define VNIC_REG_MAC4     0x0c    /* mac[4..5] in low word */
#define VNIC_REG_TXBUF    0x10    /* guest addr of tx frame */
#define VNIC_REG_TXLEN    0x14    /* write = transmit now */
#define VNIC_REG_RXBUF    0x18    /* guest addr of rx buffer */
#define VNIC_REG_RXSTAT   0x1c    /* read = rx len, 0 = none pending */

typedef struct la64m68_vnic {
    int enabled;
    int link_up;
    uint8_t mac[6];
    uint32_t txbuf, rxbuf;
    uint16_t rx_len;
    la64m68_memory *guest_mem;
    int (*tx)(void *ctx, const void *frame, size_t len);   /* host send */
    void *tx_ctx;
} la64m68_vnic;

void la64m68_vnic_init(la64m68_vnic *n, la64m68_memory *guest_mem);
void la64m68_vnic_set_mac(la64m68_vnic *n, const uint8_t mac[6]);
/* host -> guest: queue a received frame (len <= 1518) */
void la64m68_vnic_rx(la64m68_vnic *n, const void *frame, size_t len);
/* guest register window */
la64m68_memory *la64m68_vnic_regs_memory(la64m68_vnic *n);
void            la64m68_vnic_regs_destroy(la64m68_memory *m);

#endif
