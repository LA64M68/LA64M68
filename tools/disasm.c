/* Disassembly harness over the Musashi disassembler (MIT, Karl Stenerud).
 *
 * A manual decode of the Kickstart retry loop around 0xf809cc reached its
 * limit: the loop exits only when a stack slot reaches 2000, and what feeds
 * it sits before the window we could read by hand. Rather than keep guessing
 * at 68k opcode bit fields, use the reference decoder and look.
 *
 * Usage:  disasm <rom> <start-hex> <end-hex>
 * The ROM is mapped at 0xF80000, the way Kickstart decodes it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned int m68k_disassemble(char *str_buff, unsigned int pc,
                              unsigned int cpu_type);

/* Musashi reads through its own accessors; the disassembler only fetches
 * opcode words, so a plain read callback over the ROM image is enough. */
static const unsigned char *g_rom;
static size_t g_len;

unsigned int m68k_read_memory_16(unsigned int addr);
unsigned int m68k_read_memory_32(unsigned int addr);
unsigned int m68ki_read_imm_16(unsigned int addr);
unsigned int m68ki_read_imm_16(unsigned int addr)
{
    unsigned int off = addr - 0x00f80000u;
    if (off + 1 >= g_len) return 0;
    return ((unsigned int)g_rom[off] << 8) | g_rom[off + 1];
}

unsigned int m68ki_read_imm_32(unsigned int addr);
unsigned int m68ki_read_imm_32(unsigned int addr)
{
    return (m68ki_read_imm_16(addr) << 16) | m68ki_read_imm_16(addr + 2);
}

unsigned int m68k_read_memory_16(unsigned int addr)
{ return m68ki_read_imm_16(addr); }

unsigned int m68k_read_memory_32(unsigned int addr)
{ return m68ki_read_imm_32(addr); }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s rom start-hex end-hex\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    g_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *rom = malloc(g_len);
    if (!rom || fread(rom, 1, g_len, f) != g_len) return 1;
    fclose(f);
    g_rom = rom;

    unsigned start = (unsigned)strtoul(argv[2], NULL, 16);
    unsigned end   = (unsigned)strtoul(argv[3], NULL, 16);

    char buf[128];
    for (unsigned pc = start; pc < end; ) {
        unsigned sz = m68k_disassemble(buf, pc, 4 /* 68020, matches 020+ EA */);
        if (!sz) sz = 2;
        printf("%08x: %s\n", pc, buf);
        pc += sz;
    }
    return 0;
}
