#define _POSIX_C_SOURCE 200809L

/* LA64M68 splash screen.
 *
 * Display priority follows 0-POOL/RULES.md: VideoCore (the host framebuffer
 * behind vRTG) is the primary surface, Amiga Denise and Atari ST layouts are
 * the fallback export. Both are served from the same 320x200 / 16-colour
 * raster so the picture is identical everywhere -- the palette sits on 3-bit
 * channel steps precisely because the Atari ST cannot do more.
 *
 * The raster lives in assets/ next to the executable and is loaded relative
 * to the program root, so the tree stays relocatable and the C source stays
 * free of generated blobs. */

#include "vsplash.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t la64m68_vsplash_palette[LA64M68_VSPLASH_COLORS];

static uint8_t  g_chunky[LA64M68_VSPLASH_WIDTH * LA64M68_VSPLASH_HEIGHT];
static int      g_ready;

static int slurp(const char *path, void *buf, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    return got == want ? 0 : -1;
}

int la64m68_vsplash_load(const char *root)
{
    char path[1024];
    const char *base = root && *root ? root : ".";

    uint8_t pal[LA64M68_VSPLASH_COLORS * 2];
    snprintf(path, sizeof(path), "%s/assets/splash.pal", base);
    if (slurp(path, pal, sizeof(pal)) != 0) {
        la64m68_trace("splash: %s missing -> no splash", path);
        return -1;
    }
    for (int i = 0; i < LA64M68_VSPLASH_COLORS; i++)
        la64m68_vsplash_palette[i] = (uint16_t)((pal[i * 2] << 8) | pal[i * 2 + 1]);

    snprintf(path, sizeof(path), "%s/assets/splash.chunky", base);
    if (slurp(path, g_chunky, sizeof(g_chunky)) != 0) {
        la64m68_trace("splash: %s missing -> no splash", path);
        return -1;
    }
    g_ready = 1;
    la64m68_trace("splash: %dx%d, %d colours, 3-bit channel steps",
                  LA64M68_VSPLASH_WIDTH, LA64M68_VSPLASH_HEIGHT,
                  LA64M68_VSPLASH_COLORS);
    return 0;
}

int la64m68_vsplash_ready(void)
{
    return g_ready;
}

const uint8_t *la64m68_vsplash_chunky(void)
{
    return g_ready ? g_chunky : NULL;
}

/* Primary surface: VideoCore / vRTG want true colour. */
void la64m68_vsplash_render_rgb24(uint8_t *out, uint32_t stride)
{
    if (!g_ready || !out) return;
    for (int y = 0; y < LA64M68_VSPLASH_HEIGHT; y++) {
        uint8_t *row = out + (size_t)y * stride;
        for (int x = 0; x < LA64M68_VSPLASH_WIDTH; x++) {
            uint16_t c = la64m68_vsplash_palette[g_chunky[y * LA64M68_VSPLASH_WIDTH + x]];
            row[x * 3 + 0] = (uint8_t)(((c >> 8) & 0xf) * 17);  /* R */
            row[x * 3 + 1] = (uint8_t)(((c >> 4) & 0xf) * 17);  /* G */
            row[x * 3 + 2] = (uint8_t)((c & 0xf) * 17);         /* B */
        }
    }
}

/* 16-bit 1:4:4:4 (Amiga native style). */
void la64m68_vsplash_render_rgb16(uint16_t *out, uint32_t stride_words)
{
    if (!g_ready || !out) return;
    for (int y = 0; y < LA64M68_VSPLASH_HEIGHT; y++) {
        uint16_t *row = out + (size_t)y * stride_words;
        for (int x = 0; x < LA64M68_VSPLASH_WIDTH; x++)
            row[x] = la64m68_vsplash_palette[g_chunky[y * LA64M68_VSPLASH_WIDTH + x]];
    }
}
