// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 49): wall time stage B (sotc:GpuRecord) spends in Vulkan calls
// that enter the kernel, reported in the pipe: line. B was off-CPU ~30% (ioctl on the DRM fd).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace SotcWaitTimer {

enum Slot { Submit, Refresh, SemWait, Acquire, Present, PresentFence, SideFence, NumSlots };
inline constexpr const char* Names[NumSlots] = {"submit",  "refresh",       "semwait", "acquire",
                                                "present", "present-fence", "side-fence"};

inline thread_local bool on_b = false;
inline std::array<std::atomic<std::uint64_t>, NumSlots> ns{};
inline std::array<std::atomic<std::uint64_t>, NumSlots> calls{};

struct Scope {
    Slot slot;
    bool on;
    std::chrono::steady_clock::time_point start;
    explicit Scope(Slot s) : slot(s), on(on_b) {
        if (on) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~Scope() {
        if (on) {
            ns[slot].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count(),
                               std::memory_order_relaxed);
            calls[slot].fetch_add(1, std::memory_order_relaxed);
        }
    }
};

} // namespace SotcWaitTimer
