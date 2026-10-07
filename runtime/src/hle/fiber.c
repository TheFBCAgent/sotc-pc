/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceFiber: cooperative user-mode fibers on guest-supplied stacks.
 *
 * Semantics (error codes, checks, the SceFiber layout, the stack signature,
 * context-less fibers running on the thread stack, FPU defaults for SDK >= 3.50)
 * follow shadPS4's src/core/libraries/fiber/ (fiber.cpp, fiber.h,
 * fiber_context.cpp; GPL-2.0-or-later, Copyright 2024-2025 shadPS4 Emulator
 * Project). The context switch itself is a simpler design written for this
 * runtime: the suspended context is pushed onto its own stack and only the
 * stack pointer is kept (in SceFiber.context, or in the per-thread state).
 *
 * ABI notes
 *  - The switch routines are SYSV functions, so every C caller already treats
 *    xmm0-15, rax, rcx, rdx, rsi, rdi, r8-r11 as clobbered. They save exactly
 *    the SysV callee-saved set: rbx rbp r12-r15 rsp, plus MXCSR and the x87
 *    control word. MS-ABI host frames (which expect xmm6-15 to survive) can
 *    only reach a switch through a SYSV call, and clang saves xmm6-15 around
 *    such calls itself, so nothing more is needed.
 *  - Windows: the TEB's StackBase (gs:[8]), StackLimit (gs:[0x10]) and
 *    DeallocationStack (gs:[0x1478]) are part of each saved context, like
 *    SwitchToFiber does. Otherwise __chkstk in a host function with a large
 *    frame would probe from the thread's StackLimit down to the fiber stack.
 *
 * Per-thread state (tcb->fiber) points at a fiber_thread living in the
 * sceFiberRun frame on the thread stack. Fibers may be resumed by a different
 * thread than the one they last ran on, so code that lands after a switch
 * always re-reads it from the current TCB. */
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "../tls.h"
#include "hle.h"

#ifdef _WIN32
#include <intrin.h>
#endif

#define FIBER_ERROR_NULL ((int32_t)0x80590001)
#define FIBER_ERROR_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_ERROR_RANGE ((int32_t)0x80590003)
#define FIBER_ERROR_INVALID ((int32_t)0x80590004)
#define FIBER_ERROR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERROR_STATE ((int32_t)0x80590006)

#define FIBER_MAX_NAME 31
#define FIBER_CONTEXT_MIN_SIZE 512

enum { FIBER_RUN = 1, FIBER_IDLE = 2, FIBER_TERMINATED = 3 };
enum {
    FIBER_FLAG_CONTEXT_SIZE_CHECK = 0x10,
    FIBER_FLAG_SET_FPU_REGS = 0x100,
    FIBER_FLAG_REDLINE_HIT = 0x10000, /* runtime-only: red-line warning given */
};

/* Small-stack guard (the M2 gap: host HLE code runs on the game's 16 KiB job
 * fiber stacks, which have no guard page; an overflow silently writes into
 * whatever lies below). At initialization the lowest FIBER_REDLINE bytes above
 * the signature are filled with the size-check pattern. Every switch away
 * from a fiber checks them: a damaged red line means the fiber's stack (guest
 * frames plus HLE frames) came within FIBER_REDLINE bytes of its end, which
 * is logged once per fiber, before a real overflow corrupts memory. HLE entry
 * points that fibers reach also check the room left (fiber_check_room). The
 * fault handler itself no longer runs on fiber stacks (emergency stack, see
 * platform_win_fault.c). */
#define FIBER_REDLINE 512u
#define FIBER_HLE_ROOM 2048u /* warn when an HLE call starts with less than this */

static const uint32_t kFiberSignature0 = 0xdef1649c;
static const uint32_t kFiberSignature1 = 0xb37592a0;
static const uint32_t kFiberOptSignature = 0xbb40e64d;
static const uint64_t kFiberStackSignature = 0x7149f2ca7149f2caull;
static const uint64_t kFiberStackSizeCheck = 0xdeadbeefdeadbeefull;
#define FW_350 0x3500000u

typedef SYSV void (*fiber_entry)(uint64_t arg_on_initialize, uint64_t arg_on_run);

/* SceFiber: 256 bytes, 8-aligned, owned by the guest (shadPS4 layout). */
typedef struct fiber {
    uint32_t magic_start;
    _Atomic uint32_t state;
    fiber_entry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[FIBER_MAX_NAME + 1];
    void *context; /* saved stack pointer while suspended (own-stack fibers only) */
    uint32_t flags;
    void *context_start;
    void *context_end;
    uint32_t magic_end;
} fiber;
_Static_assert(sizeof(fiber) <= 256, "SceFiber is 256 bytes");

typedef struct fiber_info {
    uint64_t size;
    fiber_entry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[FIBER_MAX_NAME + 1];
    uint64_t size_context_margin;
    uint8_t pad[48];
} fiber_info;
_Static_assert(sizeof(fiber_info) == 128, "SceFiberInfo is 128 bytes");

typedef struct fiber_opt_param {
    uint32_t magic;
} fiber_opt_param;

/* Per-thread state while inside sceFiberRun; tcb->fiber points here. */
typedef struct fiber_thread {
    void *thread_sp;        /* saved context of the sceFiberRun caller */
    fiber *current;         /* fiber running on this thread */
    fiber *prev;            /* fiber just switched away from; set Idle on landing */
    uint64_t arg_on_run_to; /* delivered to the fiber being entered */
    uint64_t arg_on_return; /* delivered back to sceFiberRun */
    uint64_t teb[3];        /* thread's StackBase, StackLimit, DeallocationStack */
} fiber_thread;

/* ---- context switch -------------------------------------------------------
 * sotc_fiber_swap(save, sp): push the callee-saved context, store rsp in *save
 *   (unless save is NULL: the current context is discarded), then pop the
 *   context saved at sp and return into it.
 * sotc_fiber_start(save, top, f, teb, fpu): push and store the context as
 *   above, move to the empty stack `top` (0: just below the context saved
 *   now), install teb[3] and fpu (mxcsr | fpucw << 32), call sotc_fiber_main(f).
 * Restoring MXCSR keeps the current exception flags (bits 0-5), as the PS4
 * library does. Saved frame, from low to high address:
 *   [StackBase StackLimit DeallocationStack] (Windows) mxcsr:fpucw r15 r14 r13
 *   r12 rbx rbp ret */
SYSV void sotc_fiber_swap(void **save, void *sp);
SYSV void sotc_fiber_start(void **save, void *top, fiber *f, const uint64_t *teb, uint64_t fpu);
SYSV __attribute__((noreturn, used)) void sotc_fiber_main(fiber *f);

#ifdef _WIN32
#define TEB_PUSH "pushq %gs:0x1478\n pushq %gs:0x10\n pushq %gs:0x8\n"
#define TEB_POP "popq %gs:0x8\n popq %gs:0x10\n popq %gs:0x1478\n"
#define TEB_LOAD_REGS "movq (%rcx), %rax\n movq 8(%rcx), %r9\n movq 16(%rcx), %r10\n"
#define TEB_STORE_REGS "movq %rax, %gs:0x8\n movq %r9, %gs:0x10\n movq %r10, %gs:0x1478\n"
#define FN_DEF(n) ".def " n "; .scl 2; .type 32; .endef\n"
#else
#define TEB_PUSH ""
#define TEB_POP ""
#define TEB_LOAD_REGS ""
#define TEB_STORE_REGS ""
#define FN_DEF(n) ".type " n ", @function\n"
#endif

#define SAVE_CONTEXT                                                       \
    "pushq %rbp\n pushq %rbx\n pushq %r12\n pushq %r13\n pushq %r14\n"     \
    "pushq %r15\n subq $8, %rsp\n stmxcsr (%rsp)\n fnstcw 4(%rsp)\n" TEB_PUSH \
    "testq %rdi, %rdi\n jz 1f\n movq %rsp, (%rdi)\n1:\n"

__asm__(".att_syntax prefix\n" /* clang-cl defaults top-level asm to Intel syntax */
        ".text\n"
        ".globl sotc_fiber_swap\n" FN_DEF("sotc_fiber_swap")
        ".p2align 4\n"
        "sotc_fiber_swap:\n"
        SAVE_CONTEXT
        "movq %rsi, %rsp\n"
        TEB_POP
        "subq $8, %rsp\n"
        "stmxcsr (%rsp)\n"
        "movl (%rsp), %eax\n"
        "andl $0x3f, %eax\n"
        "addq $8, %rsp\n"
        "movl (%rsp), %ecx\n"
        "andl $0xffffffc0, %ecx\n"
        "orl %eax, %ecx\n"
        "movl %ecx, (%rsp)\n"
        "ldmxcsr (%rsp)\n"
        "fldcw 4(%rsp)\n"
        "addq $8, %rsp\n"
        "popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %rbx\n popq %rbp\n"
        "ret\n"

        ".globl sotc_fiber_start\n" FN_DEF("sotc_fiber_start")
        ".p2align 4\n"
        "sotc_fiber_start:\n"
        SAVE_CONTEXT
        "testq %rsi, %rsi\n"
        "jnz 2f\n"
        "leaq -128(%rsp), %rsi\n"
        "2:\n"
        TEB_LOAD_REGS
        "movq %rsi, %rsp\n"
        "andq $-16, %rsp\n"
        TEB_STORE_REGS
        "pushq %r8\n"
        "ldmxcsr (%rsp)\n"
        "fldcw 4(%rsp)\n"
        "addq $8, %rsp\n"
        "movq %rdx, %rdi\n"
        "xorl %ebp, %ebp\n"
        "call sotc_fiber_main\n"
        "ud2\n");

/* ---- helpers --------------------------------------------------------------- */

static fiber_thread *thread_state(void) {
    guest_tcb *tcb = tls_current();
    return tcb ? (fiber_thread *)tcb->fiber : NULL;
}

static bool fiber_valid(const fiber *f) {
    return f->magic_start == kFiberSignature0 && f->magic_end == kFiberSignature1;
}

static bool cas_state(fiber *f, uint32_t from, uint32_t to) {
    return atomic_compare_exchange_strong(&f->state, &from, to);
}

static void copy_name(char *dst, const char *src) {
    strncpy(dst, src, FIBER_MAX_NAME);
    dst[FIBER_MAX_NAME] = 0;
}

static uint64_t current_fpu(void) {
    uint32_t mxcsr;
    uint16_t fpucw;
    __asm__ volatile("stmxcsr %0\n fnstcw %1" : "=m"(mxcsr), "=m"(fpucw));
    return mxcsr | ((uint64_t)fpucw << 32);
}

/* After landing on a stack (resume or fresh start): the fiber we left is now
 * off its stack and may be run again. */
static fiber_thread *landed(void) {
    fiber_thread *t = thread_state();
    if (t->prev) {
        atomic_store(&t->prev->state, FIBER_IDLE);
        t->prev = NULL;
    }
    return t;
}

/* Enter fiber `f` (already in Run state) on the current thread, saving the
 * current context in *save (NULL: discard it). Returns when the saved context
 * is resumed. */
static void enter_fiber(void **save, fiber *f, uint64_t arg_on_run_to) {
    fiber_thread *t = thread_state();
    t->current = f;
    t->arg_on_run_to = arg_on_run_to;
    if (f->context) {
        void *sp = f->context;
        f->context = NULL;
        sotc_fiber_swap(save, sp);
        return;
    }
    uint64_t fpu = (f->flags & FIBER_FLAG_SET_FPU_REGS) ? (0x9fc0ull | (0x037full << 32))
                                                        : current_fpu();
    if (f->addr_context) {
        uint8_t *top = (uint8_t *)f->addr_context + f->size_context;
        uint64_t teb[3] = {(uint64_t)(uintptr_t)top, (uint64_t)(uintptr_t)f->addr_context,
                           (uint64_t)(uintptr_t)f->addr_context};
        sotc_fiber_start(save, top, f, teb, fpu);
    } else {
        /* Context-less: run on the thread stack, just below sceFiberRun's
         * saved context (0 = below the context being saved, for Run itself). */
        void *top = save == &t->thread_sp ? NULL : (uint8_t *)t->thread_sp - 128;
        sotc_fiber_start(save, top, f, t->teb, fpu);
    }
}

SYSV __attribute__((noreturn, used)) void sotc_fiber_main(fiber *f) {
    fiber_thread *t = landed();
    f->entry(f->arg_on_initialize, t->arg_on_run_to);
    log_fatal("fiber '%s': entry function returned", f->name);
}

static uint64_t redline_qwords(const fiber *f) {
    /* Leave at least half the stack outside the red line. */
    uint64_t bytes = f->size_context / 2 < FIBER_REDLINE ? f->size_context / 2 : FIBER_REDLINE;
    return bytes / 8 - 1; /* the first qword is the signature */
}

static void redline_arm(fiber *f) {
    uint64_t *p = (uint64_t *)f->addr_context + 1;
    for (uint64_t i = 0, n = redline_qwords(f); i < n; i++) p[i] = kFiberStackSizeCheck;
}

static void check_stack_overflow(fiber *f) {
    if (!f->addr_context) return;
    const uint64_t *base = (const uint64_t *)f->addr_context;
    if (*base != kFiberStackSignature)
        log_fatal("fiber '%s': stack overflow (context size %#llx)", f->name,
                  (unsigned long long)f->size_context);
    if (f->flags & FIBER_FLAG_REDLINE_HIT) return;
    uint64_t n = redline_qwords(f), i = n;
    while (i > 0 && base[i] == kFiberStackSizeCheck) i--; /* highest damaged qword */
    if (i == 0) return;
    f->flags |= FIBER_FLAG_REDLINE_HIT;
    log_warn("fiber '%s': stack use came within %llu bytes of the end of its %#llx-byte "
             "stack; deeper HLE calls on it would overflow", f->name,
             (unsigned long long)(i * 8), (unsigned long long)f->size_context);
}

/* Room left on the current fiber stack, or UINT64_MAX when not on one. */
static uint64_t fiber_room(void) {
    fiber_thread *t = thread_state();
    if (!t || !t->current || !t->current->addr_context) return UINT64_MAX;
    uint8_t *sp = (uint8_t *)__builtin_frame_address(0);
    uint8_t *lo = t->current->addr_context, *hi = lo + t->current->size_context;
    return sp >= lo && sp < hi ? (uint64_t)(sp - lo) : UINT64_MAX;
}

static void fiber_check_room(const char *where) {
    uint64_t room = fiber_room();
    if (room >= FIBER_HLE_ROOM) return;
    static atomic_int warned;
    if (!atomic_exchange(&warned, 1))
        log_warn("%s called on fiber '%s' with only %llu bytes of stack left", where,
                 thread_state()->current->name, (unsigned long long)room);
}

void hle_fiber_thread_exit(void) {
    guest_tcb *tcb = tls_current();
    fiber_thread *t = tcb ? (fiber_thread *)tcb->fiber : NULL;
    if (!t) return;
    if (t->current) {
        log_warn("pthread_exit inside fiber '%s'; the fiber is abandoned", t->current->name);
        t->current->context = NULL;
        atomic_store(&t->current->state, FIBER_IDLE);
    }
#ifdef _WIN32
    __asm__ volatile("movq %0, %%gs:0x8\n movq %1, %%gs:0x10\n movq %2, %%gs:0x1478"
                     :
                     : "r"(t->teb[0]), "r"(t->teb[1]), "r"(t->teb[2])
                     : "memory");
#endif
    tcb->fiber = NULL;
}

/* ---- libSceFiber ------------------------------------------------------------ */

static SYSV int32_t ps4__sceFiberInitializeImpl(fiber *f, const char *name, fiber_entry entry,
                                                uint64_t arg_on_initialize, void *addr_context,
                                                uint64_t size_context,
                                                const fiber_opt_param *opt, uint32_t build_ver) {
    if (!f || !name || !entry) return FIBER_ERROR_NULL;
    if (((uintptr_t)f & 7) || ((uintptr_t)addr_context & 15)) return FIBER_ERROR_ALIGNMENT;
    if (opt && ((uintptr_t)opt & 7)) return FIBER_ERROR_ALIGNMENT;
    if (size_context && size_context < FIBER_CONTEXT_MIN_SIZE) return FIBER_ERROR_RANGE;
    if (size_context & 15) return FIBER_ERROR_INVALID;
    if (!addr_context != !size_context) return FIBER_ERROR_INVALID;
    if (opt && opt->magic != kFiberOptSignature) return FIBER_ERROR_INVALID;

    memset(f, 0, sizeof(*f));
    copy_name(f->name, name);
    f->entry = entry;
    f->arg_on_initialize = arg_on_initialize;
    f->addr_context = addr_context;
    f->size_context = size_context;
    f->flags = build_ver >= FW_350 ? FIBER_FLAG_SET_FPU_REGS : 0;
    f->magic_start = kFiberSignature0;
    f->magic_end = kFiberSignature1;
    if (addr_context) {
        f->context_start = addr_context;
        f->context_end = (uint8_t *)addr_context + size_context;
        *(uint64_t *)addr_context = kFiberStackSignature;
        redline_arm(f);
        /* Host code runs on fiber stacks too (HLE calls, logging). */
        static atomic_int warned;
        if (size_context <= 0x4000 && !atomic_exchange(&warned, 1))
            log_warn("fiber '%s': small context (%#llx bytes); HLE calls from fibers "
                     "must keep their stack use low", f->name, (unsigned long long)size_context);
    }
    atomic_store(&f->state, FIBER_IDLE);
    log_debug("fiber init '%s' entry %p stack %p+%#llx build %#x", f->name, (void *)entry,
              addr_context, (unsigned long long)size_context, build_ver);
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberOptParamInitialize(fiber_opt_param *opt) {
    if (!opt) return FIBER_ERROR_NULL;
    if ((uintptr_t)opt & 7) return FIBER_ERROR_ALIGNMENT;
    opt->magic = kFiberOptSignature;
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberFinalize(fiber *f) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!fiber_valid(f)) return FIBER_ERROR_INVALID;
    if (!cas_state(f, FIBER_IDLE, FIBER_TERMINATED)) return FIBER_ERROR_STATE;
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberRun(fiber *f, uint64_t arg_on_run_to, uint64_t *arg_on_return) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!fiber_valid(f)) return FIBER_ERROR_INVALID;
    guest_tcb *tcb = tls_current();
    if (!tcb || tcb->fiber) return FIBER_ERROR_PERMISSION;
    if (!cas_state(f, FIBER_IDLE, FIBER_RUN)) return FIBER_ERROR_STATE;

    fiber_thread t;
    memset(&t, 0, sizeof(t));
#ifdef _WIN32
    t.teb[0] = __readgsqword(0x8);
    t.teb[1] = __readgsqword(0x10);
    t.teb[2] = __readgsqword(0x1478);
#endif
    tcb->fiber = &t;
    log_trace("fiber run '%s' arg %#llx", f->name, (unsigned long long)arg_on_run_to);
    enter_fiber(&t.thread_sp, f, arg_on_run_to);

    /* Back on the thread: some fiber called sceFiberReturnToThread. */
    fiber *last = t.current;
    t.current = NULL;
    atomic_store(&last->state, FIBER_IDLE);
    tcb->fiber = NULL;
    if (arg_on_return) *arg_on_return = t.arg_on_return;
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberSwitch(fiber *f, uint64_t arg_on_run_to, uint64_t *arg_on_run) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!fiber_valid(f)) return FIBER_ERROR_INVALID;
    fiber_thread *t = thread_state();
    if (!t) return FIBER_ERROR_PERMISSION;
    fiber_check_room("sceFiberSwitch");
    if (!cas_state(f, FIBER_IDLE, FIBER_RUN)) return FIBER_ERROR_STATE;

    fiber *cur = t->current;
    log_trace("fiber switch '%s' -> '%s' arg %#llx", cur->name, f->name,
              (unsigned long long)arg_on_run_to);
    t->prev = cur;
    /* A context-less fiber cannot be resumed: its stack is reused. */
    if (cur->addr_context) check_stack_overflow(cur);
    enter_fiber(cur->addr_context ? &cur->context : NULL, f, arg_on_run_to);

    t = landed();
    if (arg_on_run) *arg_on_run = t->arg_on_run_to;
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberGetSelf(fiber **out) {
    if (!out) return FIBER_ERROR_NULL;
    fiber_thread *t = thread_state();
    if (!t) return FIBER_ERROR_PERMISSION;
    *out = t->current;
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberReturnToThread(uint64_t arg_on_return, uint64_t *arg_on_run) {
    fiber_thread *t = thread_state();
    if (!t) return FIBER_ERROR_PERMISSION;
    fiber_check_room("sceFiberReturnToThread");
    fiber *cur = t->current;
    t->arg_on_return = arg_on_return;
    log_trace("fiber return to thread from '%s' arg %#llx", cur->name,
              (unsigned long long)arg_on_return);
    if (cur->addr_context) check_stack_overflow(cur);
    sotc_fiber_swap(cur->addr_context ? &cur->context : NULL, t->thread_sp);

    /* Resumed by a later sceFiberRun or sceFiberSwitch, maybe on another thread. */
    t = landed();
    if (arg_on_run) *arg_on_run = t->arg_on_run_to;
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberGetInfo(fiber *f, fiber_info *info) {
    if (!f || !info) return FIBER_ERROR_NULL;
    if (((uintptr_t)f & 7) || ((uintptr_t)info & 7)) return FIBER_ERROR_ALIGNMENT;
    if (info->size != sizeof(fiber_info)) return FIBER_ERROR_INVALID;
    if (!fiber_valid(f)) return FIBER_ERROR_INVALID;
    info->entry = f->entry;
    info->arg_on_initialize = f->arg_on_initialize;
    info->addr_context = f->addr_context;
    info->size_context = f->size_context;
    copy_name(info->name, f->name);
    info->size_context_margin = ~0ull; /* context size check is never enabled */
    if ((f->flags & FIBER_FLAG_CONTEXT_SIZE_CHECK) && f->addr_context) {
        const uint64_t *p = (const uint64_t *)f->context_start + 1;
        const uint64_t *end = (const uint64_t *)f->context_end;
        while (p < end && *p == kFiberStackSizeCheck) p++;
        info->size_context_margin = (uint64_t)((const uint8_t *)p - (const uint8_t *)f->context_start) - 8;
    }
    return SCE_OK;
}

static SYSV int32_t ps4_sceFiberRename(fiber *f, const char *name) {
    if (!f || !name) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!fiber_valid(f)) return FIBER_ERROR_INVALID;
    copy_name(f->name, name);
    return SCE_OK;
}

HLE_LIBRARY(fiber, "libSceFiber",
    HLE(_sceFiberInitializeImpl),
    HLE(sceFiberOptParamInitialize),
    HLE(sceFiberFinalize),
    HLE(sceFiberRun),
    HLE(sceFiberSwitch),
    HLE(sceFiberGetSelf),
    HLE(sceFiberReturnToThread),
    HLE(sceFiberGetInfo),
    HLE(sceFiberRename),
)
