// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (whole-draw memo plan, workstream W1; docs/next/draw-memo.md): the draw-memo probe,
// SOTC_DRAW_MEMO_PROBE=1 (debug only, off by default). For each record stage B runs (draw,
// indirect draw, dispatch, indirect dispatch) it asks, per memo layer, "would a memo keyed on
// this layer's inputs have hit?", and times the phase that layer would skip, split into the
// would-hit and the missing calls:
//   L1 buffers:      key = the stage's V# words          phase = BindBuffers (per stage)
//   L2 textures:     key = the stage's T# + S# words      phase = BindTextures (per stage)
//   L3 render state: key = the RT/DB registers (Regs())   phases = PrepareRenderState and
//                                                                  Rasterizer::BeginRendering
//   L4 barriers:     hit = L1, L2 and L3 all hit           phase = FlushBarriers
// L1 and L2 compare against the last call with the same Shader::Info* in a direct-mapped table
// (like SotcTexMemo); L3 against the last graphics record (like the session-38 memo). It also
// classifies each buffer binding (Info* + binding index) over the 10 s window: static, moving
// with a fixed stride (a ring candidate), moving forward with wraps (a ring with variable-sized
// allocations), irregular, one-shot. One `probe: draw-memo:` block is logged every 10 s.
//
// Threading: all state is stage B's (the recording thread: sotc:GpuRecord with SOTC_GPU_PIPE=1,
// Liverpool's thread without the pipe), so it is unlocked. The two counters other threads may
// touch (flips, cache-changing "dirty" events) are atomics. The key functions time themselves
// and their cycles are subtracted from the phases and records they run in.
// Off: every hook is one predictable branch on a constant global.
#pragma once

#include <atomic>
#include <cstdint>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace AmdGpu {
union Regs;
union CbDbExtent;
} // namespace AmdGpu
namespace Shader {
struct Info;
}

namespace SotcMemoProbe {

/// SOTC_DRAW_MEMO_PROBE, read once at static initialisation.
extern const bool g_on;
inline bool On() {
    return g_on;
}

enum class Kind : std::uint8_t { Draw, DrawInd, Disp, DispInd, Count };
enum Ph : std::uint8_t { RState, BeginRt, Buf, Tex, Barr, NumPh };

/// Cycles the probe spent in its own key functions (B only); phases and records subtract it.
inline std::uint64_t g_overhead = 0;
/// Cache-changing events (buffer upload, stream copy, image upload): a hit in a phase that saw
/// one would have missed a real memo whose generation tracks CPU writes.
inline std::atomic<std::uint64_t> g_dirty{0};
inline std::atomic<std::uint64_t> g_flips{0};

void BeginRecord(Kind kind);
void EndRecord();
void PhaseEnd(Ph ph, std::uint64_t t0, std::uint64_t ov0, std::uint64_t d0);

/// L3 key; call inside PrepareRenderState's phase (once per graphics record). `cb_extents` points
/// at the NUM_COLOR_BUFFERS extents (&CbExtent(0)).
void KeyRenderState(const AmdGpu::Regs& regs, std::uint32_t mrt_mask,
                    const AmdGpu::CbDbExtent* cb_extents, const AmdGpu::CbDbExtent& db_extent);
/// L1 key and the binding classification; call inside BindBuffers' phase.
void KeyBuffers(const Shader::Info& stage);
/// L2 key; call inside BindTextures' phase.
void KeyTextures(const Shader::Info& stage, const AmdGpu::Regs& regs);

inline void NoteDirty() {
    if (g_on) [[unlikely]] {
        g_dirty.fetch_add(1, std::memory_order_relaxed);
    }
}
inline void OnFlip() {
    if (g_on) [[unlikely]] {
        g_flips.fetch_add(1, std::memory_order_relaxed);
    }
}

/// One record (RAII at the top of Rasterizer::Record*).
struct Record {
    bool on;
    explicit Record(Kind kind) : on(g_on) {
        if (on) [[unlikely]] {
            BeginRecord(kind);
        }
    }
    ~Record() {
        if (on) [[unlikely]] {
            EndRecord();
        }
    }
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;
};

/// One timed phase; ends at End() or at the end of the scope. Outside a record it counts nothing.
struct Phase {
    std::uint64_t t0 = 0;
    std::uint64_t ov0 = 0;
    std::uint64_t d0 = 0;
    Ph ph;
    bool live;
    explicit Phase(Ph p) : ph(p), live(g_on) {
        if (live) [[unlikely]] {
            ov0 = g_overhead;
            d0 = g_dirty.load(std::memory_order_relaxed);
            t0 = __rdtsc();
        }
    }
    void End() {
        if (live) [[unlikely]] {
            live = false;
            PhaseEnd(ph, t0, ov0, d0);
        }
    }
    ~Phase() {
        End();
    }
    Phase(const Phase&) = delete;
    Phase& operator=(const Phase&) = delete;
};

} // namespace SotcMemoProbe
