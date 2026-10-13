// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 60): the two-stage GPU pipeline's types, moved from vk_rasterizer.cpp
// unchanged; included by vk_rasterizer.cpp (Draw/Dispatch, ~Rasterizer) and sotc_pipe.cpp.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "common/unique_function.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/sotc_gpu_pipe.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Vulkan {

// shadow-pc (session 43): the two-stage GPU pipeline (docs/next/gpu-pipe.md). Build step 1:
// stage A (Draw/DrawIndirect/DispatchDirect/DispatchIndirect as Liverpool calls them) tests the
// registers, selects the pipeline and writes a packet (the register blocks written since the
// last packet, the CB/DB extent hints, the compute registers, the pipeline and a snapshot of
// each stage's user data); stage B (SotcPipeRun) replays the delta into its own register copy
// and runs the recording half (Record*, RunDrawFilter) with the snapshots installed.
// SOTC_GPU_PIPE=sync runs B at once on the same thread, after each packet.
// Session 44 (build step 2): SOTC_GPU_PIPE=1 runs B on its own thread (sotc:GpuRecord,
// SotcPipeThread). Every other call from Liverpool's thread into B's state drains first
// (SotcPipeDrain: Liverpool waits until B has run every packet), and so does a fault on
// Liverpool's thread, which is then handled inline as before (SotcFaultOnGpuSide).
// SOTC_GPU_PIPE_VERIFY=N: every Nth packet also carries Liverpool's whole register file; B
// compares its delta-built copy with it and logs the words that differ.
// Session 45 (build step 3): fences (EOP, EOS, ReleaseMem: a copy of the PM4 packet), submit-done,
// idle and the flip IRQ are packets too, so B signals them after recording what precedes them
// and A doesn't drain. A notes each fence's label write (SotcPendingFences) and decides a
// WaitRegMem on such a label from it (SotcPipeWaitMet).
// Session 47 (build step 4): the CPU copy (SOTC_CPU_COPY) is computed on A and committed by B
// in stream order (SotcCpuCopyCompute/Commit); until B has run the commit, A's reads of the
// destination (the walker's tables, windows, emulated loads, the next copy's inputs) see the
// values in A's overlay (SotcPendingRanges). A also notes every packet's written buffer
// bindings there: an A read of such a range waits for B (drain reason "pending"). With
// SOTC_GPU_PIPE_VERIFY, B re-runs the SRT walker for the stages of every Nth packet and
// compares the flattened user data with A's (SotcPipeVerifyWalk).
enum class SotcPipeKind : u32 {
    Draw, DrawIndirect, DispatchDirect, DispatchIndirect, Fence, Event, Stop
};

struct Rasterizer::SotcPipeArgs {
    SotcPipeKind kind;
    bool is_indexed = false;
    u32 index_offset = 0;
    VAddr arg_address = 0;
    u32 offset = 0;
    u32 stride = 0;
    u32 max_count = 0;
    VAddr count_address = 0;
    u16 vertex_sgpr_offset = 0;
    u16 instance_sgpr_offset = 0;
    u32 size = 0;
    bool verify = false; ///< the full register file follows the packet (SOTC_GPU_PIPE_VERIFY)
    SotcDrawFilter filter{};

    bool IsCompute() const {
        return kind == SotcPipeKind::DispatchDirect || kind == SotcPipeKind::DispatchIndirect;
    }
};

struct Rasterizer::SotcPipe {
    static constexpr size_t RingBytes = 16_MB;

    explicit SotcPipe(size_t depth, u32 verify_every_, bool threaded_)
        : ring{RingBytes, depth}, verify_every{verify_every_}, threaded{threaded_} {}

    SotcPacketRing ring;
    const u32 verify_every;
    const bool threaded;               ///< SOTC_GPU_PIPE=1: B is record_thread
    SotcPacketWriter writer;           ///< A
    std::vector<u32> scratch;          ///< A
    u64 submitted = 0;                 ///< A: packets pushed
    AmdGpu::Regs regs{};               ///< B's register copy
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent{};
    AmdGpu::CbDbExtent db_extent{};
    AmdGpu::ComputeProgram cs{};
    u64 packets = 0;                   ///< B
    u64 verify_diffs = 0;              ///< B
    SotcPendingFences pending_fences;  ///< A: label writes of fences B hasn't run

    // Drains (threaded). B stores `retired` (packets run) when it finds the ring empty, before
    // it sleeps; a draining A announces itself in drain_waiting (Dekker pair, seq_cst).
    alignas(64) std::atomic<u64> retired{0};
    std::atomic<bool> drain_waiting{false};
    /// B: packets run (release, after each one: its label write, its CPU-copy commit). Session
    /// 47: every packet (was fence_retired, after fences only).
    std::atomic<u64> progress{0};
    // Session 47, A: pending GPU writes and CPU-copy overlays (step 4), and how far A treats
    // B as done (progress, or `submitted` after a wait).
    SotcPendingRanges pending;
    u64 a_view = 0;
    u64 overlay_copies = 0;  ///< CPU copies computed on A
    u64 overlay_bytes = 0;
    u64 overlay_reads = 0;   ///< A reads served from an overlay
    u64 copy_fallbacks = 0;  ///< copy-kernel dispatches A left to B (then waits)
    std::atomic<u64> verify_walks{0};      ///< B: stages re-walked (SOTC_GPU_PIPE_VERIFY)
    std::atomic<u64> verify_walk_diffs{0}; ///< B: of them, with a flattened dword that differs
    std::atomic<u64> b_idle_ns{0}; ///< B: time asleep on an empty ring

    // Session 46: B's command queue (Liverpool::QueueCommand: guest-fault downloads, A's own
    // faults, flips, video-out registration), run between packets. B holds state_mutex while
    // awake; a drain's owner guard holds it on A (own_depth: A only, nested guards).
    std::mutex state_mutex;
    std::mutex command_mutex;
    std::deque<Common::UniqueFunction<void>> commands; ///< under command_mutex
    std::atomic<u32> num_commands{0};
    u32 own_depth = 0;              ///< A
    std::atomic<u64> commands_run{0}; ///< B
    u64 a_faults = 0;               ///< A: own faults queued to B
    u64 a_fault_ns = 0;             ///< A: time they took
    u64 report_commands = 0;
    // A's counters for the 10 s report (SotcPipeReport).
    static constexpr size_t NumReasons = size_t(SotcDrainReason::Count);
    std::array<u64, NumReasons> drains{};      ///< calls
    std::array<u64, NumReasons> drain_waits{}; ///< calls that found B busy
    std::array<u64, NumReasons> drain_ns{};    ///< time A waited
    u64 fences = 0;    ///< fence packets
    u64 events = 0;    ///< submit-done, idle and flip packets
    u64 waits_met = 0; ///< WaitRegMem met by a pending fence write
    u64 report_packets = 0;
    u64 report_b_idle_ns = 0;
    std::chrono::steady_clock::time_point report_start = std::chrono::steady_clock::now();

    std::thread record_thread; ///< B; joined in ~Rasterizer
};

// shadow-pc (session 60): defined in sotc_cpu_copy.cpp (SOTC_CPU_COPY_LOG; was file-static in
// vk_rasterizer.cpp, which RecordDispatchDirect still calls it from).
bool SotcCpuCopyLogOn();

} // namespace Vulkan
