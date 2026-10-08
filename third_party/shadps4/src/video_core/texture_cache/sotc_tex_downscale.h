// SPDX-FileCopyrightText: Copyright 2026 shadow-pc
// SPDX-License-Identifier: GPL-2.0-or-later
//
// shadow-pc patch (session 16, SOTC_TEX_DOWNSCALE: on by default under 8 GiB of VRAM budget,
// =0 off, =1 on): sampled-only textures are created one mip level smaller, so the game's ~2 GB
// of textures fit a 6 GB card with the arena and its targets. VRAM over budget let WDDM move hot GPU-written allocations into system memory, and the
// GPU then ran each frame over PCIe at 2-4x the cost (docs/NEXT.md, session 16).
//
// The image's ImageInfo keeps the guest's size, levels and layout (cache matching, page
// tracking and detiling are unchanged); ImageInfo::sotc_mip_skip says how many top levels the
// Vulkan image leaves out. Host level k holds guest mip k + skip. Kept free of Vulkan so
// gpu/tests/test_tex_downscale.cpp can test it.
//
// Sampling by derivatives picks the same guest mip as before wherever the guest picked mip >= 1
// (the host image is half the size, so its LOD is one lower); only magnified mip 0 and explicit
// LODs get one level blurrier. Texel fetches and size/LOD queries use guest units, so textures a
// shader reads that way are never downscaled (ImageResource::is_texel_access).

#pragma once

#include <algorithm>

#include "common/types.h"

namespace VideoCore::SotcTexDownscale {

/// Smallest top level (largest side, in texels) that is downscaled.
constexpr u32 MIN_SIZE = 512;

/// Device memory budgets below this turn downscaling on by default.
constexpr u64 DEFAULT_ON_BELOW = 8ull << 30;

/// Whether downscaling is on: SOTC_TEX_DOWNSCALE ("0" off, anything else on) wins; unset or
/// empty, it is on when the device memory budget is under DEFAULT_ON_BELOW.
inline bool DefaultOn(u64 budget_bytes, const char* env) {
    if (env && *env) {
        return *env != '0';
    }
    return budget_bytes < DEFAULT_ON_BELOW;
}

/// What a texture binding asks for, as the texture cache sees it.
struct Request {
    bool sampled_only;   ///< a Texture binding (not storage, render or depth target, video out)
    bool texel_access;   ///< the shader fetches texels or queries the size or LOD
    bool is_depth;       ///< a depth format
    bool is_volume;      ///< a 3D texture
    u32 num_samples;
    u32 width, height;   ///< guest mip 0
    u32 levels;          ///< guest mip count
};

/// Mip levels the host image leaves out for this request: 1 or 0.
inline u32 MipSkip(const Request& r) {
    if (!r.sampled_only || r.texel_access || r.is_depth || r.is_volume || r.num_samples > 1) {
        return 0;
    }
    if (r.levels < 2 || std::max(r.width, r.height) < MIN_SIZE) {
        return 0;
    }
    return 1;
}

/// Host extent of a guest dimension (Vulkan's mip rule: halve, round down, at least 1).
inline u32 HostDim(u32 guest, u32 skip) {
    return std::max(guest >> skip, 1u);
}

struct LevelRange {
    u32 base;
    u32 count;
    bool operator==(const LevelRange&) const = default;
};

/// Maps a view's guest level range to host levels. Guest levels below the skip all read host
/// level 0; the range keeps at least one level and stays inside the host image.
inline LevelRange MapLevels(u32 base, u32 count, u32 guest_levels, u32 skip) {
    const u32 host_levels = guest_levels > skip ? guest_levels - skip : 1;
    const u32 end = base + std::max(count, 1u);
    u32 host_base = base > skip ? base - skip : 0;
    host_base = std::min(host_base, host_levels - 1);
    u32 host_end = end > skip ? end - skip : 0;
    host_end = std::clamp(host_end, host_base + 1, host_levels);
    return {host_base, host_end - host_base};
}

/// Maps a view's minimum LOD (guest levels, 1/256 units) to host levels.
inline u32 MapMinLod(u32 min_lod, u32 skip) {
    const u32 shift = skip * 256;
    return min_lod > shift ? min_lod - shift : 0;
}

} // namespace VideoCore::SotcTexDownscale
