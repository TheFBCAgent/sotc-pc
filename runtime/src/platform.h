/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host OS abstraction: memory, guest TLS register, threads, sync, faults. */
#ifndef SOTC_PLATFORM_H
#define SOTC_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Guest code uses the SysV ABI; host functions it calls must too. */
#define SYSV __attribute__((sysv_abi))

/* Everything the guest can see must live below 1 TiB (40-bit GPU pointers). */
#define PLAT_GUEST_LIMIT 0x10000000000ull

enum { PLAT_PROT_NONE = 0, PLAT_PROT_R = 1, PLAT_PROT_W = 2, PLAT_PROT_X = 4 };

/* Map zeroed memory at exactly `addr` (fixed) or anywhere at/after `addr`
 * below PLAT_GUEST_LIMIT. Returns NULL on failure. */
void *plat_map(uint64_t addr, size_t size, int prot, bool fixed);
void plat_unmap(void *addr, size_t size);
/* Reserve address space without backing it (fixed address), then commit or
 * decommit page ranges inside it. Committed pages start zeroed. */
void *plat_reserve(uint64_t addr, size_t size);
bool plat_commit(void *addr, size_t size, int prot);
void plat_decommit(void *addr, size_t size);
bool plat_protect(void *addr, size_t size, int prot);
void plat_flush_icache(void *addr, size_t size);

/* Guest arena with aliasable "physical" memory (PS4 direct memory).
 * - plat_phys_init creates the backing store for `size` bytes of physical
 *   memory; views of the same offset share their contents.
 * - plat_arena_reserve reserves the arena; its free space is placeholders.
 * - plat_arena_map maps one unit at [addr, addr+size), which must lie in free
 *   arena space: a view of physical memory at `phys`, or private zeroed
 *   memory when phys < 0. Views need PLAT_VIEW_ALIGN-aligned addr/size/phys;
 *   `commit` is false when the physical pages are known to be committed
 *   already (re-mapping a piece of a split view), which keeps that fast.
 * - plat_arena_unmap releases exactly one unit back to free space. */
#define PLAT_VIEW_ALIGN 0x10000ull
bool plat_phys_init(uint64_t size);
void *plat_arena_reserve(uint64_t addr, uint64_t size);
bool plat_arena_map(uint64_t addr, uint64_t size, int prot, int64_t phys, bool commit);
void plat_arena_unmap(uint64_t addr, uint64_t size, bool is_view);
/* GPU bridge (M3): a host view of all physical memory, mapped on first call
 * (offset = physical address; NULL on failure), and the host protection of a
 * committed page (-1 if not committed; *run_end, optional, gets the end of
 * the run with that protection). Implemented in platform_*_mem.c. */
void *plat_phys_backing(void);
int plat_query_prot(const void *addr, uint64_t *run_end);

/* Guest TLS: PS4 code reads its thread control block through %fs:0. The
 * runtime rewrites those instructions to a %gs read set up per platform:
 * Linux points GS at the TCB; Windows stores the TCB in a TEB TLS slot. */
bool plat_tls_init(void);
uint32_t plat_tls_patch_disp(int32_t fs_disp); /* new disp32 for a rewritten fs:[disp] */
void plat_set_guest_tcb(void *tcb);            /* call on every guest thread */

/* Threads. Each guest thread is a native thread with its own stack. */
typedef struct plat_thread plat_thread;
typedef void (*plat_thread_fn)(void *arg);
plat_thread *plat_thread_create(plat_thread_fn fn, void *arg, size_t stack_size);
void plat_thread_join(plat_thread *t);
void plat_thread_detach(plat_thread *t);
void plat_thread_set_name(const char *name);
void plat_yield(void);
void plat_sleep_us(uint64_t us);
void plat_sleep_until_ns(uint64_t deadline_ns); /* until plat_monotonic_ns() >= deadline_ns */
uint64_t plat_thread_id(void);

/* Recursive mutex and condition variable. */
typedef struct plat_mutex plat_mutex;
typedef struct plat_cond plat_cond;
plat_mutex *plat_mutex_new(void);
void plat_mutex_free(plat_mutex *m);
void plat_mutex_lock(plat_mutex *m);
bool plat_mutex_trylock(plat_mutex *m);
void plat_mutex_unlock(plat_mutex *m);
plat_cond *plat_cond_new(void);
void plat_cond_free(plat_cond *c);
/* timeout_us == UINT64_MAX waits forever; returns false on timeout. */
bool plat_cond_wait(plat_cond *c, plat_mutex *m, uint64_t timeout_us);
void plat_cond_signal(plat_cond *c);
void plat_cond_broadcast(plat_cond *c);

/* Time. */
uint64_t plat_monotonic_ns(void);
uint64_t plat_realtime_ns(void);
uint64_t plat_tsc(void);
uint64_t plat_tsc_frequency(void);

/* Faults. The handler gets the faulting instruction pointer and a pointer to
 * the saved general-purpose register file; it returns true if it handled the
 * fault (the context may have been modified, e.g. rip advanced). */
typedef struct plat_fault {
    enum { PLAT_FAULT_ACCESS, PLAT_FAULT_ILLEGAL, PLAT_FAULT_OTHER } kind;
    uint64_t rip;
    uint64_t addr;       /* faulting data address for access faults */
    bool is_write;       /* access faults: the access was a write */
    uint64_t *rsp;
    void *xmm;           /* 16 x 16-byte xmm registers, or NULL */
    uint64_t *rip_ptr;   /* writable instruction pointer */
} plat_fault;
typedef bool (*plat_fault_handler)(plat_fault *f);
/* True if [addr, addr+len) is committed readable memory (for fault diagnostics). */
bool plat_is_readable(const void *addr, size_t len);
void plat_install_fault_handler(plat_fault_handler handler);
/* Per-thread emergency stack (AjncSystem #02): the handler runs on it, not on
 * the faulting stack, so faults on nearly full (fiber) stacks still get
 * reported. Guest threads call plat_fault_thread_init when they start and
 * plat_fault_thread_exit before they end; other threads get one lazily on
 * their first fault. Windows: a guard page plus the rest committed; POSIX:
 * the sigaltstack. */
#define PLAT_FAULT_STACK_SIZE 0x40000u
bool plat_fault_thread_init(void);
void plat_fault_thread_exit(void);
bool plat_fault_on_emergency_stack(void); /* true inside the fault handler's stack */

/* CPU features. */
bool plat_cpu_has_sse4a(void);

#endif
