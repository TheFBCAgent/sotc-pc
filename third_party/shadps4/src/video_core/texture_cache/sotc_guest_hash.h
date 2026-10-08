// SPDX-FileCopyrightText: Copyright 2026 shadow-pc
// SPDX-License-Identifier: GPL-2.0-or-later
//
// shadow-pc patch (session 6, crash C): hash guest memory without dereferencing it, kept free of
// Vulkan so gpu/tests/test_guest_hash.cpp can test it.
//
// TextureCache::MarkAsMaybeDirty hashed an image's guest bytes in place. It runs from
// InvalidateMemory, i.e. inside the guest write fault handler, and part of the range can be
// unreadable there (read-protected by the buffer cache's GPU-modified tracking, or briefly
// unmapped). The read faulted inside the handler: "nested fault" in XXH3_hashLong, process gone.
// Hash() instead pulls the bytes through `read` (Memory::CopySparseMemory: physical backing or a
// protection-lifting copy, holes read as zero) in fixed chunks, so it never touches guest pages
// itself and never allocates. The value equals XXH3_64bits over the same bytes.

#pragma once

#include <cstdint>

#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY // XXH3_state_t on the stack
#endif
#include <xxhash.h>

namespace VideoCore::SotcGuestHash {

inline constexpr std::uint64_t ChunkSize = 4096;

// read(std::uint8_t* dst, std::uint64_t guest_addr, std::uint64_t size)
template <typename Read>
std::uint64_t Hash(std::uint64_t addr, std::uint64_t size, Read&& read) {
    alignas(64) std::uint8_t buf[ChunkSize];
    if (size <= ChunkSize) {
        read(buf, addr, size);
        return XXH3_64bits(buf, size);
    }
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (std::uint64_t off = 0; off < size; off += ChunkSize) {
        const std::uint64_t n = size - off < ChunkSize ? size - off : ChunkSize;
        read(buf, addr + off, n);
        XXH3_64bits_update(&state, buf, n);
    }
    return XXH3_64bits_digest(&state);
}

} // namespace VideoCore::SotcGuestHash
