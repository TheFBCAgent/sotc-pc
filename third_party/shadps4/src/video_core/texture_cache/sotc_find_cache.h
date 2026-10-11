// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 36): a cache of TextureCache::FindImage's perfect-match results. FindImage
// walks the page table over the whole guest range (one hash lookup per 256 KiB page) for every
// bound texture and render target, ~8% of the GPU thread in the temple, and nearly every lookup
// finds the same image as last frame. The result of the perfect-match path depends only on the
// request (the Key: every input FindImage compares) and on the set of registered images, whose
// infos never change. So the cache holds (key -> image index) tagged with a generation the
// texture cache bumps on every RegisterImage and UnregisterImage (new, freed, expanded,
// re-created or GC'd images all pass through those). Only a lookup that took the perfect-match
// path and changed nothing (generation the same at its end) is stored, under the generation it
// started with; overlap resolves (which depend on time and touch bindings) are never cached.
// Direct-mapped, GPU thread only. Test: gpu/tests/test_find_cache.cpp.
#pragma once

#include <array>
#include <cstdint>

namespace VideoCore {

class SotcFindCache {
public:
    /// Every input of FindImage's perfect-match path and its downscale check. Zero-initialise
    /// (`Key k{}`) so padding never differs.
    struct Key {
        std::uint64_t guest_address;
        std::uint32_t guest_size;
        std::uint32_t width, height, depth;
        std::uint32_t pixel_format;
        std::uint32_t image_type;
        std::uint32_t num_samples;
        std::uint16_t levels, layers;
        std::uint8_t binding_type;
        std::uint8_t exact_fmt;
        std::uint8_t texel_access;
        std::uint8_t is_depth;
        std::uint8_t is_volume;
        std::uint8_t pad[3];

        bool operator==(const Key&) const = default;
    };
    static_assert(sizeof(Key) == 48);

    static constexpr std::uint32_t Bits = 12; ///< 4096 entries (256 KiB)
    static constexpr std::uint32_t Invalid = 0xffffffffu;

    /// The image index stored for key at the current generation, or Invalid.
    [[nodiscard]] std::uint32_t Lookup(const Key& key) const {
        const Entry& e = entries[Slot(key)];
        return e.gen == gen && e.id != Invalid && e.key == key ? e.id : Invalid;
    }

    /// Stores key -> id for generation `at` (the generation the lookup started with); ignored
    /// when the generation moved since.
    void Store(const Key& key, std::uint32_t id, std::uint64_t at) {
        if (at != gen) {
            return;
        }
        Entry& e = entries[Slot(key)];
        e.key = key;
        e.id = id;
        e.gen = gen;
    }

    /// Any change to the set of registered images.
    void Bump() {
        ++gen;
    }

    [[nodiscard]] std::uint64_t Generation() const {
        return gen;
    }

private:
    struct Entry {
        Key key{};
        std::uint32_t id = Invalid;
        std::uint64_t gen = 0;
    };

    static std::uint32_t Slot(const Key& k) {
        std::uint64_t h = k.guest_address * 0x9e3779b97f4a7c15ull;
        h ^= (std::uint64_t(k.guest_size) << 32 | k.pixel_format) * 0xc2b2ae3d27d4eb4full;
        h ^= (std::uint64_t(k.width) << 32 | k.height) * 0x165667b19e3779f9ull;
        h ^= (std::uint64_t(k.binding_type) << 8 | k.exact_fmt) * 0x27d4eb2f165667c5ull;
        return std::uint32_t(h >> (64 - Bits));
    }

    std::uint64_t gen = 1; ///< entries start at 0: never valid
    std::array<Entry, 1u << Bits> entries{};
};

} // namespace VideoCore
