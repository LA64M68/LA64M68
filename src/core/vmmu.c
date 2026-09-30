#include "vmmu.h"
#include "debug.h"

/* 68060 descriptor type field (bits 1:0). */
#define DESC_DT_MASK   0x3u
#define DESC_DT_BAD    0x0u   /* invalid */
#define DESC_DT_PAGE   0x1u   /* page descriptor */
#define DESC_DT_TBL4   0x2u   /* table pointer, 4-byte */
#define DESC_DT_TBL8   0x3u   /* table pointer, 8-byte (indirect) */

#define DESC_W         0x4u   /* write protect */
#define DESC_U         0x8u   /* used */
#define DESC_M         0x10u  /* modified */

void la64m68_vmmu_reset(la64m68_vmmu *m)
{
    *m = (la64m68_vmmu){0};
}

static int ttr_match(uint32_t ttr, uint32_t vaddr, int supervisor, uint32_t *phys)
{
    if (!(ttr & LA64M68_VMMU_TTR_E))
        return 0;
    if ((ttr & LA64M68_VMMU_TTR_S) && !supervisor)
        return 0;
    /* TA = bits 31:24, TAM = bits 23:16 with a 1 bit meaning "don't care".
     * The previous expression `~(ttr & 0x00ff0000) | 0x00ffffff` always
     * folded to 0xffffffff, so `~mask` was 0, the comparison was vacuous and
     * every enabled TTR matched the whole address space. */
    uint32_t base = ttr & 0xff000000u;
    uint32_t tam  = (ttr >> 16) & 0xffu;
    uint32_t sig  = (~tam << 24) & 0xff000000u;  /* significant TA bits */
    if ((vaddr & sig) != (base & sig))
        return 0;
    *phys = vaddr; /* stage 1: TTR maps the logical block 1:1 (no relocation) */
    return 1;
}

static int ttr_any(la64m68_vmmu *m, int supervisor, int instr,
                   uint32_t vaddr, uint32_t *phys)
{
    const uint32_t *ttr = instr ? m->ittr : m->dttr;
    return ttr_match(ttr[0], vaddr, supervisor, phys) ||
           ttr_match(ttr[1], vaddr, supervisor, phys);
}

int la64m68_vmmu_translate(la64m68_vmmu *m, la64m68_memory *mem,
                           uint32_t vaddr, int supervisor, int instr,
                           int write, uint32_t *phys, uint32_t *desc)
{
    if (!m || !mem || !phys)
        return LA64M68_VMMU_F_BUS;

    if (!(m->tc & LA64M68_VMMU_TC_E)) {
        *phys = vaddr;
        if (desc) *desc = 0;
        return LA64M68_VMMU_OK;
    }
    if (ttr_any(m, supervisor, instr, vaddr, phys)) {
        if (desc) *desc = LA64M68_VMMU_MMUSR_T;
        return LA64M68_VMMU_OK;
    }

    int is8k = !(m->tc & LA64M68_VMMU_TC_PS_4K);
    /* index widths: 4K = 7+7+6+12, 8K = 7+7+5+13 */
    int idx_bits = is8k ? 5 : 6;
    int pg_shift = is8k ? 13 : 12;

    uint32_t root = supervisor ? m->srp : m->urp;

    /* Level 1: root table (7-bit index, 4-byte descs). */
    uint32_t off = (root & 0xfffffe00u) | ((vaddr >> 25) & 0x7f) * 4;
    uint32_t d1 = la64m68_mem_read32(mem, off);
    if ((d1 & DESC_DT_MASK) != DESC_DT_TBL4)
        return LA64M68_VMMU_F_PAGE;

    /* Level 2: pointer table (7-bit index, 4-byte descs).
     * Descriptor table address field is bits 31:4. */
    off = (d1 & ~0xfu) | ((vaddr >> 18) & 0x7f) * 4;
    uint32_t d2 = la64m68_mem_read32(mem, off);
    if ((d2 & DESC_DT_MASK) != DESC_DT_TBL4)
        return LA64M68_VMMU_F_PAGE;

    /* Level 3: page table (idx_bits index, 8-byte desc -> low word at +4). */
    off = (d2 & ~0xfu) |
          ((vaddr >> pg_shift) & ((1u << idx_bits) - 1)) * 8;
    uint32_t d3h = la64m68_mem_read32(mem, off);
    uint32_t d3 = la64m68_mem_read32(mem, off + 4);
    uint32_t d3a = off + 4;          /* address the U/M writeback belongs to */
    (void)d3h;                       /* upper descriptor word: unused subset */

    if ((d3 & DESC_DT_MASK) != DESC_DT_PAGE) {
        /* Indirect (DT=3): descriptor at pointed-to table entry. The U/M
         * writeback has to follow the indirection, otherwise the pointer
         * entry gets clobbered with the page descriptor it points at. */
        if ((d3 & DESC_DT_MASK) == DESC_DT_TBL8) {
            d3a = d3 & ~0x3u;
            d3 = la64m68_mem_read32(mem, d3a);
            if ((d3 & DESC_DT_MASK) != DESC_DT_PAGE)
                return LA64M68_VMMU_F_PAGE;
        } else {
            return LA64M68_VMMU_F_PAGE;
        }
    }

    /* write-protect applies to every write (documented subset: we do not
     * implement the supervisor-bypass Ux/Sx permission overlays yet). */
    if (write && (d3 & DESC_W)) {
        if (desc) *desc = d3;
        return LA64M68_VMMU_F_PROT;
    }

    /* U/M status bits: real PMMU sets U on hit, M on write hit. */
    uint32_t set = DESC_U | (write ? DESC_M : 0);
    if ((d3 & set) != set)
        la64m68_mem_write32(mem, d3a, d3 | set);

    if (desc)
        *desc = d3 | set;

    *phys = (d3 & (0xffffffffu << pg_shift)) | (vaddr & ((1u << pg_shift) - 1));
    return LA64M68_VMMU_OK;
}
