/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows: the vectored exception handler, faults, plat_is_readable, CPU features. */
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

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *ctx = ep->ContextRecord;
    /* A fault inside the fault handler: report it raw and stop, rather than recursing. */
    static __declspec(thread) int depth;
    if (depth) {
        char msg[80] = "nested fault ";
        int n = 13;
        uint64_t vals[3] = {er->ExceptionCode, ctx->Rip, ctx->Rsp};
        for (int v = 0; v < 3; v++) {
            for (int sh = 60; sh >= 0; sh -= 4) msg[n++] = "0123456789abcdef"[(vals[v] >> sh) & 15];
            msg[n++] = v < 2 ? ' ' : '\n';
        }
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)n, NULL, NULL);
        TerminateProcess(GetCurrentProcess(), 4);
    }
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
    f.rip = ctx->Rip;
    f.rip_ptr = &ctx->Rip;
    f.rsp = (uint64_t *)ctx->Rsp;
    f.xmm = &ctx->Xmm0;
    depth++;
    bool handled = g_handler && g_handler(&f);
    depth--;
    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void plat_install_fault_handler(plat_fault_handler handler) {
    g_handler = handler;
    AddVectoredExceptionHandler(1, veh);
}

bool plat_cpu_has_sse4a(void) {
    unsigned a, b, c, d;
    return __get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 6));
}

#endif
