// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/libraries/kernel/threads/pthread.h: common/thread.cpp
// only names the current guest thread; sotcgpu's threads are never guest threads, so
// g_curthread stays null and these types only need the members thread.cpp names.
#pragma once

#include <string>

namespace Libraries::Fiber {
struct OrbisFiber {
    std::string name;
};
struct OrbisFiberContext {
    OrbisFiber* current_fiber;
};
} // namespace Libraries::Fiber

namespace Core {
struct Tcb {
    Libraries::Fiber::OrbisFiberContext* tcb_fiber;
};
} // namespace Core

namespace Libraries::Kernel {

struct Pthread {
    std::string name;
    Core::Tcb* tcb;
};

inline thread_local Pthread* g_curthread = nullptr;

} // namespace Libraries::Kernel
