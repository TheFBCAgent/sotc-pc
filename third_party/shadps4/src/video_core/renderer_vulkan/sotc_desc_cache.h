// SPDX-FileCopyrightText: Copyright 2026 shadow-pc
// SPDX-License-Identifier: GPL-2.0-or-later

// shadow-pc patch (session 55): the descriptor-set cache (SOTC_DESC_CACHE).
//
// Session 55 counted the temple's descriptor pushes (Pro mode, ~1.24M per 10 s, ~10.5
// descriptors each): almost none repeats the last push, but with the storage buffers' offsets
// left out only ~4.4k distinct sets remain per 10 s, ~80% of them already used in the 10 s
// before. Every buffer descriptor points into one of ~6 VkBuffers (arena, staging stream,
// GDS, null), only at another offset. So a pipeline whose set has at most
// maxDescriptorSetStorageBuffersDynamic buffers uses a normal set layout with
// STORAGE_BUFFER_DYNAMIC buffers; Pipeline::BindResources keys a cached set by the writes
// without the buffer offsets and binds it with the offsets as dynamic offsets. A miss writes a
// new set; a hit is one vkCmdBindDescriptorSets.
//
// A cached set refers to image views, samplers, buffers and its pipeline layout by handle.
// Destroying one reports the handle (Dead); at the next bind the cache drops the sets that
// name it (a recycled handle would otherwise hit the old set). Their memory stays in the pools
// until a full clear, which frees the pools once the GPU has passed the sets' last use.
//
// Result on Windows (RTX 2060, temple, Pro, 4+4 alternating runs): 99.9% hits, but B's CPU
// per flip didn't move (43.9 vs 43.3 ms on/off): NVIDIA's pushes were cheap per call, so it's
// opt-in. Still to try with RADV, where the pushes were ~21% of B (session 49).

#pragma once

#include "common/types.h"

namespace Vulkan::SotcDescCache {

enum Kind { View, Sampler, Buffer, Layout, NumKinds };

/// Called before an image view, sampler, buffer or pipeline layout handle is destroyed (any
/// thread).
void Dead(Kind kind, u64 handle);

/// SOTC_DESC_CACHE=1 turns the cache on (off by default: no gain on NVIDIA, session 55).
bool Enabled();

} // namespace Vulkan::SotcDescCache
