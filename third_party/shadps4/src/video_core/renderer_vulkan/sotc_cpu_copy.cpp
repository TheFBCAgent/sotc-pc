// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 60): the CPU copy (SOTC_CPU_COPY, sessions 21-50), moved from
// vk_rasterizer.cpp unchanged. The Vulkan-free part is sotc_cpu_copy.h.

#include <deque>
#include <mutex>
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/platform.h" // shadow-pc (session 45): IRQs signalled by stage B
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h" // shadow-pc (session 45): fence packets
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/sotc_cpu_copy.h" // shadow-pc (session 21)
#include "video_core/renderer_vulkan/sotc_gpu_pipe.h" // shadow-pc (session 43)
#include "video_core/renderer_vulkan/sotc_wait_timer.h" // shadow-pc (session 49)
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include "sotc_debug.h" // shadow-pc: gpu/src/debug_dump.cpp
#include <chrono>
#include <unordered_set>
#include "video_core/renderer_vulkan/sotc_rasterizer_internal.h" // shadow-pc (session 60)

namespace Vulkan {

// shadow-pc (session 21): SOTC_CPU_COPY runs SotC's copy kernel on the CPU too (on by default
// since session 24; =0 off). The black boots with a cold pipeline cache (bad SRT pointers at
// ~2 s after cs 0xa402ddac compiles) happen with the copy off as well, see docs/NEXT.md.
// =2 (check) writes the CPU result but still marks the binding GPU-written, and DownloadMemory
// compares downloads with it (SOTC_GPU_STATS "check: ... equal/differ").
static int SotcCpuCopyMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_CPU_COPY");
        const int m = v && *v == '0' ? 0 : v && *v == '2' ? 2 : 1;
        LOG_INFO(Render_Vulkan, "SOTC_CPU_COPY {}", m == 0 ? "off" : m == 2 ? "check" : "on");
        return m;
    }();
    return mode;
}

// shadow-pc (session 22, debug): SOTC_CPU_COPY_LOG=1 logs every copy-kernel dispatch in the
// first 8 s after the first one (=N: from N s after it) (outcome, tick, sharps, control words), to match the walker's
// bad-pointer trails (sotcgpu.cpp, ResolveSrtTable) against CPU-copied destinations.
bool SotcCpuCopyLogOn() { // session 60: external (RecordDispatchDirect logs with it)
    static const int from = [] {
        const char* v = std::getenv("SOTC_CPU_COPY_LOG");
        const int n = v ? std::atoi(v) : 0;
        return n == 1 ? 0 : n > 1 ? n : -1;
    }();
    if (from < 0) {
        return false;
    }
    static const auto start = std::chrono::steady_clock::now();
    const auto t = std::chrono::steady_clock::now() - start;
    return t >= std::chrono::seconds(from) && t < std::chrono::seconds(from + 8);
}

static SotcCpuCopy::Sharp SotcCopySharp(const AmdGpu::Buffer& b) {
    return {
        .base = b.base_address,
        .stride = static_cast<u32>(b.stride),
        .num_records = b.num_records,
        .data_format = b.data_format,
        .num_format = b.num_format,
        .dst_sel_x = b.dst_sel_x,
        .swizzle = b.swizzle_enable != 0,
        .add_tid = b.add_tid_enable != 0,
    };
}

void Rasterizer::SotcCpuCopy(const Shader::Info& cs, const AmdGpu::ComputeProgram& cs_program) {
    if (cs.pgm_hash != Vulkan::SotcCpuCopy::ShaderHash || SotcCpuCopyMode() == 0) {
        return;
    }
    thread_local SotcWords words;
    SotcCopyResult r;
    if (SotcCpuCopyCompute(cs, cs_program, r, words)) {
        SotcCpuCopyCommit(r, words);
    }
}

static void SotcCpuCopyReject(Vulkan::SotcCpuCopy::Reject why, u32 extra = 0) {
    SotcDebug::Count(SotcDebug::Counter::CpuCopyRejected);
    static std::array<std::atomic<bool>, 16> logged{};
    if (!logged[static_cast<u32>(why)].exchange(true)) {
        LOG_INFO(Render_Vulkan, "SOTC_CPU_COPY: dispatch left to the GPU, reason {} ({})",
                 static_cast<u32>(why), extra);
    }
}
// Reasons past SotcCpuCopy::Reject, for the log only.
constexpr auto SotcCopyResources = static_cast<Vulkan::SotcCpuCopy::Reject>(12);
constexpr auto SotcCopyImage = static_cast<Vulkan::SotcCpuCopy::Reject>(13);
constexpr auto SotcCopyWriteFailed = static_cast<Vulkan::SotcCpuCopy::Reject>(14);

bool Rasterizer::SotcCpuCopyCompute(const Shader::Info& cs,
                                    const AmdGpu::ComputeProgram& cs_program, SotcCopyResult& out,
                                    SotcWords& dst_words) {
    namespace C = Vulkan::SotcCpuCopy;
    if (cs.pgm_hash != C::ShaderHash || SotcCpuCopyMode() == 0) {
        return false;
    }
    const bool log_on = SotcCpuCopyLogOn();
    u32 words[2]{};
    AmdGpu::Buffer ctl{}, src{}, dst{};
    const auto trace = [&](const char* what, u32 code = 0) {
        if (log_on) {
            LOG_INFO(Render_Vulkan,
                     "cpu-copy-log: {} {} dim {}x{}x{} ctl {:#x} [{:#x} {:#x}] src {:#x} "
                     "n{} dst {:#x} n{}",
                     what, code, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                     u64{ctl.base_address}, words[0], words[1], u64{src.base_address},
                     u32{src.num_records}, u64{dst.base_address}, u32{dst.num_records});
        }
    };
    const auto reject = [&](C::Reject why, u32 extra = 0) {
        trace("reject", static_cast<u32>(why));
        SotcCpuCopyReject(why, extra);
        return false;
    };
    // Handles 0-2 as in the IR (ctl, src, dst); later ones may only be special read-only
    // buffers (the flattened user data).
    const auto extra_ok = [&] {
        for (size_t i = 3; i < cs.buffers.size(); ++i) {
            if (!cs.buffers[i].IsSpecial() || cs.buffers[i].is_written) {
                return false;
            }
        }
        return true;
    };
    if (cs.buffers.size() < 3 || cs.buffers[0].is_written || cs.buffers[1].is_written ||
        !cs.buffers[2].is_written || cs.buffers[0].IsSpecial() || cs.buffers[1].IsSpecial() ||
        cs.buffers[2].IsSpecial() || !extra_ok()) {
        return reject(SotcCopyResources, static_cast<u32>(cs.buffers.size()));
    }
    ctl = cs.buffers[0].GetSharp(cs);
    src = cs.buffers[1].GetSharp(cs);
    dst = cs.buffers[2].GetSharp(cs);
    if (!ctl.Valid() || !src.Valid() || !dst.Valid() || ctl.base_address == 0 ||
        src.base_address == 0 || dst.base_address == 0) {
        return reject(C::Reject::Sharp);
    }
    // Session 47: on stage A, inputs a pending CPU copy writes are read from its overlay (and a
    // pending GPU write waits for B first); SotcPipeOverlay is null elsewhere.
    // The GPU reads what the arena holds at this point in the batch: guest memory, unless the
    // GPU wrote the bytes (or the texel-buffer path copies an image in).
    const auto read_input = [&](VAddr addr, void* to, u64 bytes) {
        if (const u8* o = SotcPipeOverlay(addr, bytes)) {
            std::memcpy(to, o, bytes);
            return true;
        }
        if (buffer_cache.IsRangeGpuWritten(addr, bytes)) {
            return false;
        }
        memory->CopySparseMemory(addr, static_cast<u8*>(to), bytes);
        return true;
    };
    if (!read_input(ctl.base_address, words, sizeof(words))) {
        trace("gpu-written ctl");
        SotcDebug::Count(SotcDebug::Counter::CpuCopyGpuWritten);
        return false;
    }
    const C::Dispatch d{
        .dim_x = cs_program.dim_x,
        .dim_y = cs_program.dim_y,
        .dim_z = cs_program.dim_z,
        .start_x = cs_program.start_x,
        .start_y = cs_program.start_y,
        .start_z = cs_program.start_z,
        .threads_x = cs_program.num_thread_x.full,
        .threads_y = cs_program.num_thread_y.full,
        .threads_z = cs_program.num_thread_z.full,
        .partial_x = cs_program.num_thread_x.partial,
        .partial_y = cs_program.num_thread_y.partial,
        .partial_z = cs_program.num_thread_z.partial,
    };
    const C::Plan plan =
        C::MakePlan(SotcCopySharp(src), SotcCopySharp(dst), ctl.GetSize(), words[0], words[1], d);
    if (plan.reject != C::Reject::None) {
        return reject(plan.reject);
    }
    const u64 src_bytes = u64{plan.src_dwords} * 4;
    // Session 49, opt-in (SOTC_COPY_FAST=1): SotcWords skips resize()'s zero-fill and Run
    // copies whole periods. Less CPU on both stages, yet fewer flips/s in 3 of 3 alternating
    // pairs (A's queued faults waited longer), so off by default.
    static const bool fast = [] {
        const char* v = std::getenv("SOTC_COPY_FAST");
        return v && v[0] == '1';
    }();
    const auto grow = [](SotcWords& v, size_t n) {
        const size_t old = v.size();
        v.resize(n);
        if (!fast && n > old) {
            std::fill(v.begin() + old, v.end(), 0u);
        }
    };
    thread_local SotcWords src_words;
    grow(src_words, plan.src_dwords);
    if (!read_input(src.base_address, src_words.data(), src_bytes)) {
        trace("gpu-written src");
        SotcDebug::Count(SotcDebug::Counter::CpuCopyGpuWritten);
        return false;
    }
    grow(dst_words, plan.count);
    (fast ? C::Run : C::RunSimple)(plan, src_words.data(), dst_words.data());
    out = {
        .src = src.base_address,
        .dst = dst.base_address,
        .src_bytes = src_bytes,
        .dst_vsize = dst.GetSize(),
        .count = plan.count,
    };
    trace("computed");
    return true;
}

void Rasterizer::SotcCpuCopyCommit(const SotcCopyResult& r, std::span<const u32> dst_words) {
    const bool log_on = SotcCpuCopyLogOn();
    const u64 dst_bytes = u64{r.count} * 4;
    const auto trace = [&](const char* what, u64 code = 0) {
        if (log_on) {
            LOG_INFO(Render_Vulkan, "cpu-copy-log: {} {} tick {} src {:#x} dst {:#x} n{}", what,
                     code, scheduler.CurrentTick(), r.src, r.dst, r.count);
        }
    };
    // Session 22: the CPU writes only pages a download (or an earlier CPU copy) already made
    // resident: copying into GPU-only buffers no one reads back cost 1.2 GB (session 21). A
    // table's first frame takes the GPU path; the walker's or a guest read downloads it. The
    // other pages stay GPU-written (gpu_only). =2 (check) copies only fully resident ones.
    // Session 47: with SOTC_GPU_PIPE=1 stage A computed the values (its overlay held them
    // until here); the residency split and the image check are B's state, so they run here.
    // Session 50: an in-flight readback of these bytes lands first (it would overwrite them).
    buffer_cache.SotcReadbackSettle(r.dst, dst_bytes,
                                    VideoCore::BufferCache::SotcSettleSite::CpuCopy);
    const auto cov = buffer_cache.SotcResident().Covered(r.dst, dst_bytes);
    if (cov.marked == 0 || (SotcCpuCopyMode() == 2 && !cov.All())) {
        trace("not resident", cov.marked);
        SotcDebug::Count(SotcDebug::Counter::CpuCopyNotResident);
        return;
    }
    if (texture_cache.IsMeta(r.src) || texture_cache.FindImageFromRange(r.src, r.src_bytes)) {
        if (sotc_copy_commit) {
            // A read guest memory where the GPU reads an image: its overlay may have been wrong.
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render_Vulkan, "pipe: cpu copy source became an image ({:#x} +{:#x})",
                            r.src, r.src_bytes);
            }
        }
        SotcCpuCopyReject(SotcCopyImage);
        return;
    }
    if (log_on && dst_bytes >= 1_MB) {
        // Session 22: how much of the destination is in the working set before a big copy
        // (4 KiB pages).
        LOG_INFO(Render_Vulkan, "cpu-copy-log: working set before: dst {}/{}",
                 SotcDebug::WorkingSetPages(r.dst, dst_bytes), dst_bytes >> 12);
    }
    // Resident runs: written here, and older GPU data in them is never downloaded over the
    // result (a destination byte still GPU-written would be). Other runs: left to the GPU.
    thread_local std::vector<std::pair<VAddr, u64>> gpu_only;
    gpu_only.clear();
    bool written = true;
    buffer_cache.SotcResident().ForEachRun(r.dst, dst_bytes, [&](VAddr a, u64 n, bool resident) {
        if (!resident) {
            gpu_only.emplace_back(a, n);
            return;
        }
        const u32* words = dst_words.data() + (a - r.dst) / 4;
        if (!written || !memory->TryWriteBacking(reinterpret_cast<void*>(a), words, n)) {
            written = false;
            gpu_only.emplace_back(a, n);
            return;
        }
        if (SotcCpuCopyMode() == 1) {
            buffer_cache.SotcForgetGpuWrites(a, n);
        }
    });
    if (!written) {
        // Pieces already written hold the values the GPU writes there anyway.
        SotcCpuCopyReject(SotcCopyWriteFailed);
    }
    // BindBuffers obtains the binding with its clamped V# size; the GPU writes [0, count) of it.
    if (SotcCpuCopyMode() == 1) {
        buffer_cache.SotcSetCpuMirroredWrite(r.dst, r.dst_vsize, gpu_only);
        if (!gpu_only.empty()) {
            u64 gpu_bytes = 0;
            for (const auto& [a, n] : gpu_only) {
                gpu_bytes += n;
            }
            SotcDebug::Count(SotcDebug::Counter::CpuCopyPartial);
            SotcDebug::Count(SotcDebug::Counter::CpuCopyGpuOnlyDwords, gpu_bytes / 4);
        }
    } else {
        buffer_cache.SotcCheckCpuCopy(r.dst, dst_words.data(), r.count);
    }
    trace(gpu_only.empty() ? "copied" : "copied in part", cov.marked);
    SotcDebug::Count(SotcDebug::Counter::CpuCopy);
    SotcDebug::Count(SotcDebug::Counter::CpuCopyDwords, r.count);
}
} // namespace Vulkan
