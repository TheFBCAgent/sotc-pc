// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

// shadow-pc patch (after AjncSystem's SotC build, commit 609b4dd2cc, "runtime-indexed constant
// reads"): a ReadConst whose dword index is only known while the shader runs (a loop counter,
// i.e. a Phi) can't get a fixed slot in the flattened buffer. Upstream reads it from guest memory
// only with direct memory access on; otherwise it read flatbuf[0] (user data s0) for every index,
// SotC's blocky depth of field (docs/NEXT.md, session 8). The SRT walker now copies a window of the
// table into the flattened buffer and the shader indexes it: the ReadConst's flags hold the
// window's first dword | ReadConstDynamicWindowFlag, and the index is clamped to the window.
/// Host helper the walker calls to copy a window (guest memory through CopySparseMemory).
void PS4_SYSV_ABI SrtCopyWindow(u32* dst, u64 src, u64 bytes);
using PFN_SrtCopyWindow = void PS4_SYSV_ABI (*)(u32*, u64, u64);
/// shadow-pc (session 10): true if `code` is inside the generated SRT walker code.
bool SrtWalkerContains(const void* code);
constexpr u32 ReadConstDynamicWindowFlag = 1u << 31;
/// 16 KiB. SotC indexes tables up to ~2.5 KiB past their start (AjncSystem used 512 dwords,
/// too small for shader 0xf94d23d4, whose loops start at byte 1712).
constexpr u32 ReadConstDynamicWindowDwords = 4096;

/// shadow-pc (session 10): where the walker reads a table of `bytes` bytes at guest `ptr`
/// (bytes == 0: extent unknown). The video core's resolver returns an unprotected alias of the
/// table when its page is read-blocked by GPU tracking but none of its bytes are GPU-written, so
/// the walker's loads don't fault (and drain the GPU); otherwise `ptr`. Default: `ptr`.
/// `table` numbers the walker's tables (Info::srt_leaves).
u64 PS4_SYSV_ABI SrtResolveTable(u64 ptr, u64 bytes, u64 table);
using PFN_SrtResolveTable = u64 PS4_SYSV_ABI (*)(u64, u64, u64);
using SrtTableResolver = u64 (*)(u64 ptr, u64 bytes, u64 table);
void SetSrtTableResolver(SrtTableResolver resolver);

/// shadow-pc (session 10): one dword (two for a pointer) the walker copies from table `table`
/// at dword `src_dw` into flattened-buffer dword `dst_dw`. Recorded when the walker is
/// generated (not serialized: a walker loaded from the pipeline cache has none).
struct SrtLeaf {
    u16 table;
    u16 src_dw;
    u16 dst_dw;
    bool pointer;
};
/// The Info whose walker is running on this thread (Info::RefreshFlatBuf), as void*.
void SrtSetWalkInfo(const void* info);
const void* SrtWalkInfo();

/// Host helpers the walker calls through its third argument (`call [r12 + offset]`).
struct SrtHelpers {
    PFN_SrtCopyWindow copy_window;
    PFN_SrtResolveTable resolve_table;
    /// shadow-pc (session 27b): PushPtr's target for null table pointers (64 KiB of zeros). Read
    /// through the helpers too: an address baked into the code went stale in the pipeline cache
    /// once the host executable moved (Linux PIE; Windows loads the exe at a fixed base).
    const void* zero_page;
};
extern const SrtHelpers g_srt_helpers;

/// The helpers are an argument, not addresses baked into the code: walker code is cached.
using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/,
                                            const SrtHelpers* /*helpers*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
