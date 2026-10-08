/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows: the vectored exception handler, faults, plat_is_readable, CPU features.
 *
 * The handler runs on a per-thread emergency stack (AjncSystem fix #02).
 * Windows has no sigaltstack: the kernel always builds the exception frame
 * (CONTEXT, EXCEPTION_RECORD, machine frame: about 1.3 KiB) directly below the
 * faulting rsp, and ntdll's dispatcher plus the VEH run there too. A fault on
 * a nearly full 16 KiB fiber stack used to overflow while reporting and kill
 * the process silently. Only that unavoidable part stays on the faulting
 * stack now: veh() switches to the emergency stack before calling the
 * runtime's handler, so the GPU page-tracking handler, the memory retry and
 * the crash report (logging, stack scan) never touch the guest stack.
 *
 * The switch costs a handful of instructions, so even the fast path (handled
 * page-tracking faults) uses it; see runtime/tests/test_fault.c for timings.
 *
 * The red zone: because the kernel writes the frame directly below rsp, the
 * 128 bytes a SysV leaf function may keep there are clobbered by every fault
 * taken in guest code, handled or not, before any user-mode code runs. No
 * user-mode handler can prevent that; see docs/m3/s.md. */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cpuid.h>

#include "log.h"
#include "platform.h"

/* ---- faults ------------------------------------------------------------ */

bool plat_is_readable(const void *addr, size_t len) {
    uintptr_t cur = (uintptr_t)addr, end = cur + len;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)cur, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        cur = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

static plat_fault_handler g_handler;

/* ---- emergency stack ----------------------------------------------------
 * One per thread: a reserved block whose lowest page is a no-access guard and
 * the rest committed read-write. Guest threads get theirs when they start
 * (plat_fault_thread_init); any other thread gets one on its first fault. */
#define ESTACK_GUARD 0x1000u

static __declspec(thread) uint8_t *t_estack; /* base of the reservation */
static __declspec(thread) int t_depth;       /* > 0 while the handler runs */

static uint8_t *estack_alloc(void) {
    uint8_t *base = VirtualAlloc(NULL, PLAT_FAULT_STACK_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) return NULL;
    if (!VirtualAlloc(base + ESTACK_GUARD, PLAT_FAULT_STACK_SIZE - ESTACK_GUARD, MEM_COMMIT,
                      PAGE_READWRITE)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }
    return base;
}

bool plat_fault_thread_init(void) {
    if (!t_estack) t_estack = estack_alloc();
    return t_estack != NULL;
}

void plat_fault_thread_exit(void) {
    if (t_estack && !t_depth) {
        VirtualFree(t_estack, 0, MEM_RELEASE);
        t_estack = NULL;
    }
}

bool plat_fault_on_emergency_stack(void) {
    uint8_t *sp = (uint8_t *)__builtin_frame_address(0);
    return t_estack && sp >= t_estack && sp < t_estack + PLAT_FAULT_STACK_SIZE;
}

/* call_on_stack(fn, arg, top, limit): run fn(arg) with rsp = top, and the
 * TEB's StackBase/StackLimit set to [limit, top) so __chkstk in large host
 * frames probes the right stack. MS ABI. rbp-framed with SEH unwind codes, so
 * an unwinder can walk back out to the guest stack. */
typedef bool (*estack_fn)(void *arg);
bool call_on_stack(estack_fn fn, void *arg, void *top, void *limit);
__asm__(".att_syntax prefix\n"
        ".text\n"
        ".globl call_on_stack\n"
        ".def call_on_stack; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "call_on_stack:\n"
        ".seh_proc call_on_stack\n"
        "pushq %rbp\n"
        ".seh_pushreg %rbp\n"
        "movq %rsp, %rbp\n"
        ".seh_setframe %rbp, 0\n"
        ".seh_endprologue\n"
        "pushq %gs:0x8\n"       /* [rbp-8]  StackBase */
        "pushq %gs:0x10\n"      /* [rbp-16] StackLimit */
        "movq %r8, %gs:0x8\n"
        "movq %r9, %gs:0x10\n"
        "movq %r8, %rsp\n"
        "andq $-16, %rsp\n"
        "subq $32, %rsp\n"      /* shadow space */
        "movq %rcx, %rax\n"
        "movq %rdx, %rcx\n"
        "callq *%rax\n"
        "movq -16(%rbp), %rcx\n"
        "movq %rcx, %gs:0x10\n"
        "movq -8(%rbp), %rcx\n"
        "movq %rcx, %gs:0x8\n"
        "movq %rbp, %rsp\n"
        "popq %rbp\n"
        "retq\n"
        ".seh_endproc\n");

static bool dispatch(void *arg) {
    return g_handler((plat_fault *)arg);
}

/* A fault inside the fault handler: report it raw and stop, rather than
 * recursing. Small and allocation-free: it may run with little stack left. */
static __attribute__((noreturn)) void nested_fault(EXCEPTION_RECORD *er, CONTEXT *ctx) {
    char msg[80] = "nested fault ";
    int n = 13;
    uint64_t vals[3] = {er->ExceptionCode, ctx->Rip, ctx->Rsp};
    for (int v = 0; v < 3; v++) {
        for (int sh = 60; sh >= 0; sh -= 4) msg[n++] = "0123456789abcdef"[(vals[v] >> sh) & 15];
        msg[n++] = v < 2 ? ' ' : '\n';
    }
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)n, NULL, NULL);
    TerminateProcess(GetCurrentProcess(), 4);
    __builtin_unreachable();
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *ctx = ep->ContextRecord;
    plat_fault f = {0};
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        f.kind = PLAT_FAULT_ACCESS;
        f.addr = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
        /* ExceptionInformation[0]: 0 read, 1 write, 8 execute (DEP). */
        f.is_write = er->NumberParameters >= 1 && er->ExceptionInformation[0] == 1;
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        f.kind = PLAT_FAULT_ILLEGAL;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        f.kind = PLAT_FAULT_OTHER;
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH; /* C++ exceptions, debugger events, ... */
    }
    if (t_depth) nested_fault(er, ctx);
    if (!g_handler) return EXCEPTION_CONTINUE_SEARCH;
    f.rip = ctx->Rip;
    f.rip_ptr = &ctx->Rip;
    for (int i = 0; i < 16; i++) f.gpr[i] = &ctx->Rax + i; /* Rax..R15 are contiguous */
    f.rsp = (uint64_t *)ctx->Rsp;
    f.xmm = &ctx->Xmm0;

    t_depth++;
    bool handled;
    if (!t_estack) t_estack = estack_alloc();
    if (t_estack) {
        handled = call_on_stack(dispatch, &f, t_estack + PLAT_FAULT_STACK_SIZE,
                                t_estack + ESTACK_GUARD);
    } else {
        handled = g_handler(&f); /* out of memory: run where we are */
    }
    t_depth--;
    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void plat_install_fault_handler(plat_fault_handler handler) {
    static bool installed;
    g_handler = handler;
    plat_fault_thread_init();
    if (!installed) AddVectoredExceptionHandler(1, veh);
    installed = true;
}

bool plat_cpu_has_sse4a(void) {
    unsigned a, b, c, d;
    return __get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 6));
}

#endif
