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
/// Session 51: dispatches that submitted at once for a hot line (vk_rasterizer.cpp, session 10).
inline std::atomic<std::uint64_t> hot_flushes{0};
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

// Session 51: stage A's (Liverpool's thread) wall time by kind with SOTC_GPU_PIPE=1, and the
// guest threads' GPU page faults, reported in the pipe: line next to B's waits. Drains, A's
// queued faults and pending-overlay waits are in the first pipe: line already.
// A download a fault on a non-B thread waits for (async readback on): read-to-b until B starts
// the command, then read-async (B left it in flight, the finish wakes the thread) or read-on-b.
enum ASlot {
    AIdle,
    AVoLabel,
    ASpin,
    ARingFull,
    GuestRead,
    GuestWrite,
    ReadToB,
    ReadAsync,
    ReadOnB,
    NumASlots
};
inline constexpr const char* ANames[NumASlots] = {
    "idle",        "vo-label", "regmem-spin", "ring-full", "guest-read",
    "guest-write", "read-to-b", "read-async", "read-on-b"};
inline std::array<std::atomic<std::uint64_t>, NumASlots> a_ns{};
inline std::array<std::atomic<std::uint64_t>, NumASlots> a_calls{};

inline std::uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void AddA(ASlot slot, std::uint64_t ns, std::uint64_t calls = 1) {
    a_ns[slot].fetch_add(ns, std::memory_order_relaxed);
    a_calls[slot].fetch_add(calls, std::memory_order_relaxed);
}

struct AScope {
    ASlot slot;
    std::uint64_t start = NowNs();
    explicit AScope(ASlot s) : slot(s) {}
    ~AScope() {
        AddA(slot, NowNs() - start);
    }
};

} // namespace SotcWaitTimer
