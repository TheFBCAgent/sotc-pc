// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/signals.h. shadow-pc owns the process fault
// handler (runtime); faults reach these handlers through sotcgpu_handle_fault, which
// calls DispatchAccessViolation with a synthesized EXCEPTION_POINTERS (RIP only).
#pragma once

#include <compare>
#include <mutex>
#include <set>
#include "common/singleton.h"
#include "common/types.h"

namespace Core {

using AccessViolationHandler = bool (*)(void* context, void* fault_address);
using IllegalInstructionHandler = bool (*)(void* context);

class SignalDispatch {
public:
    void RemoveHandlers() {
        std::scoped_lock lk{mutex};
        access_violation_handlers.clear();
        illegal_instruction_handlers.clear();
    }

    void RegisterAccessViolationHandler(const AccessViolationHandler& handler, u32 priority) {
        std::scoped_lock lk{mutex};
        access_violation_handlers.emplace(handler, priority);
    }

    void RegisterIllegalInstructionHandler(const IllegalInstructionHandler& handler,
                                           u32 priority) {
        std::scoped_lock lk{mutex};
        illegal_instruction_handlers.emplace(handler, priority);
    }

    bool DispatchAccessViolation(void* context, void* fault_address) const {
        for (const auto& entry : access_violation_handlers) {
            if (entry.handler(context, fault_address)) {
                return true;
            }
        }
        return false;
    }

    bool DispatchIllegalInstruction(void* context) const {
        for (const auto& entry : illegal_instruction_handlers) {
            if (entry.handler(context)) {
                return true;
            }
        }
        return false;
    }

private:
    template <typename T>
    struct HandlerEntry {
        T handler;
        u32 priority;

        std::strong_ordering operator<=>(const HandlerEntry& right) const {
            return priority <=> right.priority;
        }
    };
    std::mutex mutex;
    std::set<HandlerEntry<AccessViolationHandler>> access_violation_handlers;
    std::set<HandlerEntry<IllegalInstructionHandler>> illegal_instruction_handlers;
};

using Signals = Common::Singleton<SignalDispatch>;

} // namespace Core
