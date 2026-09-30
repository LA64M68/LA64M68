#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "vm.h"
#include "debug.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* Unified vm backend: userfaultfd (Linux primary) when available, else the
 * portable SIGSEGV/SIGBUS + mprotect path. BSD tier-2 path uses the latter
 * unconditionally.
 */

typedef struct {
    size_t off;
    size_t len;
    la64m68_vm_fault_fn fn;
    void *ctx;
    int used;
} vm_trap;

struct la64m68_vm {
    uint8_t *base;
    size_t size;
    int backend;                 /* 0 = posix/sigsegv, 1 = uffd */
    vm_trap traps[LA64M68_VM_MAX_REGIONS];
#if defined(__linux__)
    int uffd;
    int wake_pipe[2];
    pthread_t thread;
    int thread_started;
#endif
};

int la64m68_vm_pagesize(void)
{
    static int ps;
    if (!ps) {
        long r = sysconf(_SC_PAGESIZE);
        ps = r > 0 ? (int)r : 4096;
    }
    return ps;
}

static vm_trap *vm_find_trap(la64m68_vm *vm, size_t off)
{
    for (int i = 0; i < LA64M68_VM_MAX_REGIONS; i++) {
        vm_trap *t = &vm->traps[i];
        if (t->used && off >= t->off && off < t->off + t->len)
            return t;
    }
    return NULL;
}

static size_t vm_align(la64m68_vm *vm, size_t *off, size_t *len)
{
    size_t ps = (size_t)la64m68_vm_pagesize();
    (void)vm;
    *off &= ~(ps - 1);
    *len = (*len + ps - 1) & ~(ps - 1);
    return ps;
}

/* ================= POSIX / SIGSEGV backend ================= */

#define LA64M68_VM_MAX_WINDOWS 8
#define LA64M68_VM_FAULT_RETRY 4      /* unresolved-fault bail-out threshold */
static la64m68_vm *vm_windows[LA64M68_VM_MAX_WINDOWS];
static int vm_sig_installed;

static la64m68_vm *vm_find_win(uintptr_t addr)
{
    for (int i = 0; i < LA64M68_VM_MAX_WINDOWS; i++) {
        la64m68_vm *vm = vm_windows[i];
        if (vm && addr >= (uintptr_t)vm->base &&
            addr < (uintptr_t)vm->base + vm->size)
            return vm;
    }
    return NULL;
}

static void vm_sig_handler(int sig, siginfo_t *si, void *uap)
{
    (void)uap;
    uintptr_t addr = (uintptr_t)si->si_addr;
    la64m68_vm *vm = vm_find_win(addr);

    if (vm) {
        size_t off = addr - (uintptr_t)vm->base;
        vm_trap *t = vm_find_trap(vm, off);
        if (t && t->fn(t->ctx, off, -1, NULL) == 0) {
            /* The callback must re-protect or remap the page. If it does not,
             * the same instruction faults forever -- give up after a few
             * retries and take the default action instead of hanging. */
            static uintptr_t last;
            static int repeat;
            if (addr != last) { last = addr; repeat = 1; return; }
            if (++repeat < LA64M68_VM_FAULT_RETRY) return;
            repeat = 0;
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void vm_sig_install(void)
{
    if (vm_sig_installed) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = vm_sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    vm_sig_installed = 1;
}

static int vm_posix_register_window(la64m68_vm *vm)
{
    for (int i = 0; i < LA64M68_VM_MAX_WINDOWS; i++) {
        if (!vm_windows[i]) {
            vm_windows[i] = vm;
            vm_sig_install();
            return 0;
        }
    }
    return -1;
}

static void vm_posix_unregister_window(la64m68_vm *vm)
{
    for (int i = 0; i < LA64M68_VM_MAX_WINDOWS; i++)
        if (vm_windows[i] == vm) vm_windows[i] = NULL;
}

/* ================= userfaultfd backend (Linux) ================= */

#if defined(__linux__)

static int uffd_open(void)
{
    return (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
}

static int uffd_wp(la64m68_vm *vm, size_t off, size_t len, int wp)
{
    struct uffdio_writeprotect p = {
        .range = { .start = (uintptr_t)vm->base + off, .len = len },
        .mode = wp ? UFFDIO_WRITEPROTECT_MODE_WP : 0,
    };
    return ioctl(vm->uffd, UFFDIO_WRITEPROTECT, &p);
}

static void *vm_fault_thread(void *arg)
{
    la64m68_vm *vm = arg;
    struct pollfd fds[2] = {
        { .fd = vm->uffd, .events = POLLIN },
        { .fd = vm->wake_pipe[0], .events = POLLIN },
    };
    size_t pg = (size_t)la64m68_vm_pagesize();

    for (;;) {
        if (poll(fds, 2, -1) <= 0)
            continue;
        if (fds[1].revents & POLLIN)
            break;
        if (!(fds[0].revents & POLLIN))
            continue;

        struct uffd_msg msg;
        ssize_t n = read(vm->uffd, &msg, sizeof(msg));
        if (n != sizeof(msg) || msg.event != UFFD_EVENT_PAGEFAULT)
            continue;

        size_t off = (size_t)(msg.arg.pagefault.address - (uintptr_t)vm->base);
        int write = (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) ? 1 : 0;

        vm_trap *t = vm_find_trap(vm, off);
        if (t && t->fn(t->ctx, off, write, NULL) == 0) {
            uffd_wp(vm, off & ~(pg - 1), pg, 0);
        } else {
            la64m68_trace("vm: unhandled uffd fault off=%zx write=%d", off, write);
            uffd_wp(vm, off & ~(pg - 1), pg, 0);
        }
    }
    return NULL;
}

static int vm_uffd_setup(la64m68_vm *vm)
{
    vm->uffd = uffd_open();
    if (vm->uffd < 0)
        return -1;

    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    if (ioctl(vm->uffd, UFFDIO_API, &api) < 0 ||
        !(api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
        close(vm->uffd);
        return -1;
    }

    struct uffdio_register reg = {
        .range = { .start = (uintptr_t)vm->base, .len = vm->size },
        .mode = UFFDIO_REGISTER_MODE_WP,
    };
    if (ioctl(vm->uffd, UFFDIO_REGISTER, &reg) < 0) {
        close(vm->uffd);
        return -1;
    }

    if (pipe(vm->wake_pipe) < 0) {
        close(vm->uffd);
        return -1;
    }
    if (pthread_create(&vm->thread, NULL, vm_fault_thread, vm) != 0) {
        close(vm->wake_pipe[0]);
        close(vm->wake_pipe[1]);
        vm->wake_pipe[0] = vm->wake_pipe[1] = -1;
        close(vm->uffd);
        return -1;
    }
    vm->thread_started = 1;
    return 0;
}

#else
static int vm_uffd_setup(la64m68_vm *vm) { (void)vm; return -1; }
#endif

/* ================= public API ================= */

la64m68_vm *la64m68_vm_create(size_t size)
{
    if (!size) return NULL;
    size_t ps = (size_t)la64m68_vm_pagesize();
    size = (size + ps - 1) & ~(ps - 1);

    la64m68_vm *vm = calloc(1, sizeof(*vm));
    if (!vm) return NULL;

    vm->base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm->base == MAP_FAILED) { free(vm); return NULL; }
    vm->size = size;
#if defined(__linux__)
    vm->wake_pipe[0] = vm->wake_pipe[1] = -1;
#endif

    if (vm_uffd_setup(vm) == 0) {
        vm->backend = 1;
        la64m68_trace("vm: uffd window base=%p size=%zu", (void *)vm->base, size);
    } else {
        if (vm_posix_register_window(vm) < 0) {
            munmap(vm->base, vm->size);
            free(vm);
            return NULL;
        }
        vm->backend = 0;
        la64m68_trace("vm: posix/sigsegv window base=%p size=%zu",
                      (void *)vm->base, size);
    }
    return vm;
}

void la64m68_vm_destroy(la64m68_vm *vm)
{
    if (!vm) return;
#if defined(__linux__)
    if (vm->backend == 1) {
        if (vm->thread_started) {
            char b = 1;
            ssize_t w = write(vm->wake_pipe[1], &b, 1);
            (void)w;
            pthread_join(vm->thread, NULL);
        }
        if (vm->wake_pipe[0] >= 0) close(vm->wake_pipe[0]);
        if (vm->wake_pipe[1] >= 0) close(vm->wake_pipe[1]);
        close(vm->uffd);
    } else
#endif
        vm_posix_unregister_window(vm);
    munmap(vm->base, vm->size);
    free(vm);
}

void *la64m68_vm_base(la64m68_vm *vm) { return vm ? vm->base : NULL; }
size_t la64m68_vm_size(la64m68_vm *vm) { return vm ? vm->size : 0; }

int la64m68_vm_protect(la64m68_vm *vm, size_t off, size_t len, int prot)
{
    /* overflow-safe bounds check: off + len can wrap */
    if (!vm || len > vm->size || off > vm->size - len) return -1;
    vm_align(vm, &off, &len);

#if defined(__linux__)
    if (vm->backend == 1) {
        if (prot & LA64M68_VM_PROT_WRITE)
            return uffd_wp(vm, off, len, 0);
        return uffd_wp(vm, off, len, 1);
    }
#endif
    int p = PROT_NONE;
    if (prot & LA64M68_VM_PROT_READ)  p |= PROT_READ;
    if (prot & LA64M68_VM_PROT_WRITE) p |= PROT_WRITE;
    return mprotect(vm->base + off, len, p);
}

int la64m68_vm_trap_region(la64m68_vm *vm, size_t off, size_t len,
                           la64m68_vm_fault_fn fn, void *ctx)
{
    if (!vm || !fn || len > vm->size || off > vm->size - len) return -1;
    for (int i = 0; i < LA64M68_VM_MAX_REGIONS; i++) {
        if (!vm->traps[i].used) {
            vm->traps[i] = (vm_trap){ off, len, fn, ctx, 1 };
            la64m68_trace("vm: trap region off=%zx len=%zu", off, len);
            size_t a_off = off, a_len = len;
            vm_align(vm, &a_off, &a_len);
#if defined(__linux__)
            if (vm->backend == 1)
                return uffd_wp(vm, a_off, a_len, 1);
#endif
            /* POSIX backend: without PROT_NONE no fault ever reaches the
             * handler, so the registered callback would never run. */
            return mprotect(vm->base + a_off, a_len, PROT_NONE);
        }
    }
    return -1;
}
