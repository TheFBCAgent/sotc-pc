/* SPDX-License-Identifier: GPL-2.0-or-later */
/* POSIX: the signal handler, faults, plat_is_readable, CPU features. */
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

void posix_install_altstack(void) {
    stack_t ss = {0};
    ss.ss_size = 64 * 1024;
    ss.ss_sp = malloc(ss.ss_size);
    sigaltstack(&ss, NULL);
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
