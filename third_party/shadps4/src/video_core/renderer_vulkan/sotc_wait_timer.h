// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 49): wall time stage B (sotc:GpuRecord) spends in Vulkan calls
// that enter the kernel, reported in the pipe: line. B was off-CPU ~30% (ioctl on the DRM fd).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace SotcWaitTimer {

enum Slot { Submit, Refresh, SemWait, Acquire, Present, PresentFence, SideFence, NumSlots };
inline constexpr const char* Names[NumSlots] = {"submit",  "refresh",       "semwait", "acquire",
                                                "present", "present-fence", "side-fence"};

inline thread_local bool on_b = false;

// Session 64: what stage A and B are blocked on right now, for the stall watchdog
// (Rasterizer::SotcStallWatch): a name and since when, nested (restored on exit).
struct WhereSlot {
    std::atomic<const char*> name{nullptr};
    std::atomic<std::uint64_t> since_ns{0};
};
inline WhereSlot b_where, a_where;
inline std::uint64_t NowNs();
struct Where {
    WhereSlot* slot;
    const char* prev_name;
    std::uint64_t prev_since;
    Where(WhereSlot& s, const char* name)
        : slot(&s), prev_name(s.name.load(std::memory_order_relaxed)),
          prev_since(s.since_ns.load(std::memory_order_relaxed)) {
        s.since_ns.store(NowNs(), std::memory_order_relaxed);
        s.name.store(name, std::memory_order_relaxed);
    }
    ~Where() {
        slot->since_ns.store(prev_since, std::memory_order_relaxed);
        slot->name.store(prev_name, std::memory_order_relaxed);
    }
};
/// Session 51: dispatches that submitted at once for a hot line (vk_rasterizer.cpp, session 10).
inline std::atomic<std::uint64_t> hot_flushes{0};
/// Session 52: written bindings over hot lines whose per-flip flush budget was spent.
inline std::atomic<std::uint64_t> hot_denied{0};
inline std::array<std::atomic<std::uint64_t>, NumSlots> ns{};
inline std::array<std::atomic<std::uint64_t>, NumSlots> calls{};

struct Scope {
    Slot slot;
    bool on;
    std::chrono::steady_clock::time_point start;
    const char* prev_where = nullptr;
    std::uint64_t prev_since = 0;
    explicit Scope(Slot s) : slot(s), on(on_b) {
        if (on) {
            start = std::chrono::steady_clock::now();
            prev_where = b_where.name.load(std::memory_order_relaxed);
            prev_since = b_where.since_ns.load(std::memory_order_relaxed);
            b_where.since_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
            b_where.name.store(Names[s], std::memory_order_relaxed);
        }
    }
    ~Scope() {
        if (on) {
            b_where.since_ns.store(prev_since, std::memory_order_relaxed);
            b_where.name.store(prev_where, std::memory_order_relaxed);
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
    Where where; // session 64 (guest threads' faults too: the last one wins, fine for a hint)
    explicit AScope(ASlot s) : slot(s), where(a_where, ANames[s]) {}
    ~AScope() {
        AddA(slot, NowNs() - start);
    }
};

// Session 58: every vkQueueSubmit by its caller (file:line through std::source_location
// defaults on Scheduler::Flush/Finish/Wait and Rasterizer::Flush/Finish), so the pipe: line
// says why B submits ~46 times per flip. "B" marks submits on sotc:GpuRecord.
struct SubmitSite {
    const char* file;
    std::uint32_t line;
    bool b;
    std::uint64_t count;
};
inline std::mutex submit_sites_mutex;
inline std::array<SubmitSite, 64> submit_sites{};
inline std::size_t num_submit_sites = 0;

inline void CountSubmit(const std::source_location& loc) {
    std::scoped_lock lk{submit_sites_mutex};
    for (std::size_t i = 0; i < num_submit_sites; ++i) {
        SubmitSite& s = submit_sites[i];
        if (s.line == loc.line() && s.b == on_b && s.file == loc.file_name()) {
            ++s.count;
            return;
        }
    }
    if (num_submit_sites < submit_sites.size()) {
        submit_sites[num_submit_sites++] = {loc.file_name(), loc.line(), on_b, 1};
    }
}

/// " total N: file:line[ B] Nx, ..." by count, and resets the counts.
inline std::string TakeSubmitSites() {
    std::scoped_lock lk{submit_sites_mutex};
    std::array<SubmitSite, 64> sorted = submit_sites;
    std::sort(sorted.begin(), sorted.begin() + num_submit_sites,
              [](const SubmitSite& a, const SubmitSite& b) { return a.count > b.count; });
    std::uint64_t total = 0;
    std::string out;
    for (std::size_t i = 0; i < num_submit_sites; ++i) {
        const SubmitSite& s = sorted[i];
        if (s.count == 0) {
            continue;
        }
        total += s.count;
        std::string_view file = s.file;
        if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
            file.remove_prefix(slash + 1);
        }
        out += fmt::format(" {}:{}{} {}x,", file, s.line, s.b ? " B" : "", s.count);
    }
    for (std::size_t i = 0; i < num_submit_sites; ++i) {
        submit_sites[i].count = 0;
    }
    return fmt::format(" total {}:{}", total, out);
}

} // namespace SotcWaitTimer
