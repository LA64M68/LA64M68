#ifndef LA64M68_VRTG_H
#define LA64M68_VRTG_H

#include "memory.h"
#include <stdint.h>

/* vRTG — native generic RTG device (VESA-alike, no AmigaOS config).
 *
 * Display priority (locked): real PCI/Zorro card via PiS passthrough >
 * this vGPU > VideoCore direct > X11 window > Denise/AGA fallback.
 *
 * Guest side: framebuffer lives in guest RAM (zero-copy window) plus a
 * small register block for mode selection. Host side: present() calls
 * the host fb backend chosen via cfg (vrtg.backend).
 */

#define LA64M68_VRTG_REG_BASE   0x00F30000   /* register window (guest) */
#define LA64M68_VRTG_FB_BASE    0x00F40000   /* default framebuffer base */
#define LA64M68_VRTG_MAX_DIM    4096u        /* guest-settable mode bound */

/* register offsets (guest-visible, big-endian friendly via cpu bus) */
#define VRTG_REG_CTRL      0x00   /* bit0 enable */
#define VRTG_REG_WIDTH     0x04
#define VRTG_REG_HEIGHT    0x08
#define VRTG_REG_BPP       0x0c   /* 8/16/32 */
#define VRTG_REG_ADDR      0x10   /* framebuffer guest addr */
#define VRTG_REG_FLIP      0x14   /* write: present now */

typedef struct la64m68_vrtg {
    uint32_t fb_base;
    uint32_t width, height;
    int bpp;
    int enabled;
    la64m68_memory *guest_mem;         /* bus to read pixels through */
    void (*present)(void *ctx, const void *pixels,
                    uint32_t w, uint32_t h, int bpp);
    void *present_ctx;
} la64m68_vrtg;

void la64m68_vrtg_init(la64m68_vrtg *v, la64m68_memory *guest_mem,
                       uint32_t fb_base);
void la64m68_vrtg_set_mode(la64m68_vrtg *v, uint32_t w, uint32_t h, int bpp);
void la64m68_vrtg_present(la64m68_vrtg *v);

/* guest register window as la64m68_memory (plug into router) */
la64m68_memory *la64m68_vrtg_regs_memory(la64m68_vrtg *v);
void            la64m68_vrtg_regs_destroy(la64m68_memory *m);

#endif
