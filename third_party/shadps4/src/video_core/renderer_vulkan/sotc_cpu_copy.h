// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 21, SOTC_CPU_COPY): SotC's copy kernel run on the CPU as well.
//
// The SRT walker's costliest own faults (~24 ms/frame at the altar, session 20) are descriptor
// tables written by one compute shader, 0x50784801, a 1-D copy/fill: for
//   i = WorkgroupId.x * 64 + LocalId.x,  i < min(dst.num_records, ctl[0]):
//     dst[i] = src[i mod ctl[1]]
// with formatted 32-bit loads/stores (stride 4) and the sharps in user data (src SGPR0-3,
// dst SGPR4-7, ctl SGPR8-11). Its stores cover the whole binding, so no store footprint can
// narrow it. When the source and control words are CPU data at record time (not GPU-written),
// the result is known then: the rasterizer writes it into guest memory and the dispatch's
// binding isn't marked GPU-written, so the walker reads the CPU copy instead of waiting for a
// download. The GPU dispatch still runs and writes the same values into the arena.
// Session 22: only destinations whose pages a download already wrote (so their backing is
// resident; VideoCore::SotcResidentPages) are copied, which replaced the 64 KiB cap.
// This header holds the Vulkan-free part (the plan and the copy); test: gpu/tests/test_cpu_copy.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace Vulkan::SotcCpuCopy {

constexpr std::uint64_t ShaderHash = 0x50784801;

/// The fields of a V# the copy depends on (AmdGpu::Buffer).
struct Sharp {
    std::uint64_t base = 0;
    std::uint32_t stride = 0;
    std::uint32_t num_records = 0;
    std::uint32_t data_format = 0; ///< AmdGpu::DataFormat (Format32 = 4)
    std::uint32_t num_format = 0;  ///< AmdGpu::NumberFormat (Uint 4, Sint 5, Float 7)
    std::uint32_t dst_sel_x = 0;   ///< AmdGpu::CompSwizzle (Red = 4)
    bool swizzle = false;
    bool add_tid = false;
};

struct Dispatch {
    std::uint32_t dim_x = 0, dim_y = 0, dim_z = 0;
    std::uint32_t start_x = 0, start_y = 0, start_z = 0;
    std::uint32_t threads_x = 0, threads_y = 0, threads_z = 0; ///< num_thread_*.full
    std::uint32_t partial_x = 0, partial_y = 0, partial_z = 0; ///< num_thread_*.partial
};

enum class Reject : std::uint8_t {
    None,
    Sharp,      ///< a sharp the CPU copy doesn't reproduce bit for bit (format, stride, ...)
    Dispatch,   ///< not 64x1x1 threads from group 0
    Empty,      ///< nothing to copy
    CtlSize,    ///< the control buffer is under 8 bytes (the shader reads 0s)
    ZeroPeriod, ///< ctl[1] == 0 (the shader's division yields an out-of-range index)
    SrcBounds,  ///< a source index past src.num_records (the shader reads 0s)
    Overlap,    ///< source and destination overlap (GPU threads race)
    NotResident, ///< (the rasterizer's check) destination pages no download wrote yet
};

struct Plan {
    Reject reject = Reject::None;
    std::uint32_t count = 0;     ///< destination dwords written, [0, count)
    std::uint32_t period = 0;    ///< ctl[1]
    std::uint32_t src_dwords = 0; ///< source dwords read, [0, src_dwords)
};

inline bool ExactDword(const Sharp& s) {
    return s.stride == 4 && s.data_format == 4 &&
           (s.num_format == 4 || s.num_format == 5 || s.num_format == 7) && s.dst_sel_x == 4 &&
           !s.swizzle && !s.add_tid;
}

/// What the kernel writes for these sharps, control words and dispatch, or why the CPU can't
/// reproduce it exactly. `ctl_bytes` is the control buffer's size.
inline Plan MakePlan(const Sharp& src, const Sharp& dst, std::uint64_t ctl_bytes,
                     std::uint32_t ctl0, std::uint32_t ctl1, const Dispatch& d) {
    Plan p;
    if (!ExactDword(src) || !ExactDword(dst) || src.num_format != dst.num_format) {
        p.reject = Reject::Sharp;
        return p;
    }
    if (d.threads_x != 64 || d.threads_y != 1 || d.threads_z != 1 || d.partial_x != 0 ||
        d.partial_y != 0 || d.partial_z != 0 || d.start_x != 0 || d.start_y != 0 ||
        d.start_z != 0) {
        p.reject = Reject::Dispatch;
        return p;
    }
    if (ctl_bytes < 8) {
        p.reject = Reject::CtlSize;
        return p;
    }
    const std::uint64_t threads = std::uint64_t{d.dim_x} * 64;
    const std::uint64_t count =
        std::min<std::uint64_t>({threads, dst.num_records, ctl0});
    if (count == 0 || d.dim_y == 0 || d.dim_z == 0) {
        p.reject = Reject::Empty;
        return p;
    }
    if (ctl1 == 0) {
        p.reject = Reject::ZeroPeriod;
        return p;
    }
    const std::uint64_t src_dwords = std::min<std::uint64_t>(count, ctl1);
    if (src_dwords > src.num_records) {
        p.reject = Reject::SrcBounds;
        return p;
    }
    const std::uint64_t src_end = src.base + src_dwords * 4;
    const std::uint64_t dst_end = dst.base + count * 4;
    if (src.base < dst_end && dst.base < src_end) {
        p.reject = Reject::Overlap;
        return p;
    }
    p.count = static_cast<std::uint32_t>(count);
    p.period = ctl1;
    p.src_dwords = static_cast<std::uint32_t>(src_dwords);
    return p;
}

/// dst[i] = src[i mod period] for i < count; src holds plan.src_dwords dwords.
/// Session 49: one copy of the period, then doubling copies of what dst already holds (the
/// per-dword loop was ~3.6% of stage B). RunSimple is the default loop
/// (Run with SOTC_COPY_FAST=1; see SotcCpuCopyCompute).
inline void Run(const Plan& p, const std::uint32_t* src, std::uint32_t* dst) {
    std::uint64_t done = std::min(p.count, p.period);
    std::memcpy(dst, src, done * 4);
    while (done < p.count) {
        const std::uint64_t n = std::min<std::uint64_t>(done, p.count - done); // whole periods
        std::memcpy(dst + done, dst, n * 4);
        done += n;
    }
}

inline void RunSimple(const Plan& p, const std::uint32_t* src, std::uint32_t* dst) {
    for (std::uint32_t i = 0, j = 0; i < p.count; ++i) {
        dst[i] = src[j];
        if (++j == p.period) {
            j = 0;
        }
    }
}

} // namespace Vulkan::SotcCpuCopy
