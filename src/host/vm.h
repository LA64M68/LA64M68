#ifndef LA64M68_VM_H
#define LA64M68_VM_H

#include <stddef.h>
#include <stdint.h>

/* Virtual memory HAL: guest address window backed by the host MMU.
 *
 * Linux primary path: userfaultfd (vm_linux.c, later step).
 * Portable fallback:  mmap + mprotect + SIGSEGV/SIGBUS routing (vm_posix.c),
 * which also keeps the *BSD tier-2 path open.
 *
 * Contract: a vm window is one anonymous mapping. Regions inside it can be
 * protected; faults are routed to a registered callback. The callback must
 * resolve the fault (e.g. emulate MMIO, then re-protect or remap) and
 * return 0 to retry the faulting instruction, or non-zero to propagate.
 */

typedef struct la64m68_vm la64m68_vm;

enum {
    LA64M68_VM_PROT_NONE  = 0,
    LA64M68_VM_PROT_READ  = 1,
    LA64M68_VM_PROT_WRITE = 2,
    LA64M68_VM_PROT_RW    = 3
};

/* write: 1 = write fault, 0 = read fault, -1 = unknown (platform limit).
 * pc: faulting host instruction pointer (diagnostics only). */
typedef int (*la64m68_vm_fault_fn)(void *ctx, uintptr_t guest_addr,
                                   int write, void *pc);

int           la64m68_vm_pagesize(void);
la64m68_vm   *la64m68_vm_create(size_t size);
void          la64m68_vm_destroy(la64m68_vm *vm);
void         *la64m68_vm_base(la64m68_vm *vm);
size_t        la64m68_vm_size(la64m68_vm *vm);

/* Change host-side protection of a region (offsets relative to base,
 * page-granular). Returns 0 on success. */
int           la64m68_vm_protect(la64m68_vm *vm, size_t off, size_t len, int prot);

/* Register a fault callback for a region (guest offsets). First matching
 * region wins. max LA64M68_VM_MAX_REGIONS per window. */
#define LA64M68_VM_MAX_REGIONS 16
int           la64m68_vm_trap_region(la64m68_vm *vm, size_t off, size_t len,
                                     la64m68_vm_fault_fn fn, void *ctx);

#endif
