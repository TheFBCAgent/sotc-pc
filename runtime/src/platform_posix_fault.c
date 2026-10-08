/* SPDX-License-Identifier: GPL-2.0-or-later */
/* POSIX: the signal handler, faults, plat_is_readable, CPU features.
 *
 * The handler runs on the thread's sigaltstack (the emergency stack, see
 * platform.h). Linux builds the signal frame below the 128-byte red zone of
 * the interrupted stack, or on the altstack, so guest red zones survive. */
#ifndef _WIN32

#define _GNU_SOURCE
#include <cpuid.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include "log.h"
#include "platform.h"

/* ---- faults ------------------------------------------------------------ */

static plat_fault_handler g_handler;
static _Thread_local void *t_altstack; /* allocated by us, NULL if none */

void posix_install_altstack(void) {
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) return;
    stack_t ss = {0};
    ss.ss_size = PLAT_FAULT_STACK_SIZE;
    ss.ss_sp = malloc(ss.ss_size);
    if (ss.ss_sp && sigaltstack(&ss, NULL) == 0) t_altstack = ss.ss_sp;
    else free(ss.ss_sp);
}

bool plat_fault_thread_init(void) {
    posix_install_altstack();
    stack_t cur;
    return sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE);
}

void plat_fault_thread_exit(void) {
    stack_t cur;
    if (!t_altstack || sigaltstack(NULL, &cur) != 0 || (cur.ss_flags & SS_ONSTACK)) return;
    stack_t off = {0};
    off.ss_flags = SS_DISABLE;
    if (sigaltstack(&off, NULL) == 0) {
        free(t_altstack);
        t_altstack = NULL;
    }
}

bool plat_fault_on_emergency_stack(void) {
    stack_t cur;
    return sigaltstack(NULL, &cur) == 0 && (cur.ss_flags & SS_ONSTACK);
}

static void on_signal(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
    plat_fault f = {0};
    f.kind = sig == SIGSEGV || sig == SIGBUS ? PLAT_FAULT_ACCESS
           : sig == SIGILL                   ? PLAT_FAULT_ILLEGAL
                                             : PLAT_FAULT_OTHER;
    f.addr = (uint64_t)(uintptr_t)info->si_addr;
    /* Page-fault error code bit 1: the access was a write. */
    f.is_write = f.kind == PLAT_FAULT_ACCESS && (uc->uc_mcontext.gregs[REG_ERR] & 2);
    f.rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    f.rip_ptr = (uint64_t *)&uc->uc_mcontext.gregs[REG_RIP];
    static const int gpr_index[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                      REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                      REG_R12, REG_R13, REG_R14, REG_R15};
    for (int i = 0; i < 16; i++) f.gpr[i] = (uint64_t *)&uc->uc_mcontext.gregs[gpr_index[i]];
    f.rsp = (uint64_t *)uc->uc_mcontext.gregs[REG_RSP];
    f.xmm = uc->uc_mcontext.fpregs ? (void *)uc->uc_mcontext.fpregs->_xmm : NULL;
    if (g_handler && g_handler(&f)) return;
    signal(sig, SIG_DFL); /* let the default action produce a core dump */
}

void plat_install_fault_handler(plat_fault_handler handler) {
    g_handler = handler;
    posix_install_altstack();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}

bool plat_cpu_has_sse4a(void) {
    unsigned a, b, c, d;
    return __get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 6));
}

bool plat_is_readable(const void *addr, size_t len) {
    /* process_vm_readv on ourselves fails with EFAULT instead of faulting. */
    char buf[256];
    uintptr_t cur = (uintptr_t)addr, end = cur + len;
    while (cur < end) {
        size_t n = end - cur < sizeof(buf) ? end - cur : sizeof(buf);
        struct iovec local = {buf, n}, remote = {(void *)cur, n};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)n) return false;
        cur += n;
    }
    return true;
}

#endif
