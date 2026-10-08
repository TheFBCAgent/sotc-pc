// SPDX-License-Identifier: GPL-2.0-or-later
// Session 6, crash C: VideoCore::SotcGuestHash (third_party/shadps4/src/video_core/texture_cache/
// sotc_guest_hash.h), the hash TextureCache::MarkAsMaybeDirty takes inside the write fault
// handler. It must equal XXH3_64bits over the same bytes and read guest memory only through the
// reader, never in place. No Vulkan device.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "video_core/texture_cache/sotc_guest_hash.h"

using VideoCore::SotcGuestHash::ChunkSize;
using VideoCore::SotcGuestHash::Hash;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static std::vector<std::uint8_t> Pattern(std::uint64_t size) {
    std::vector<std::uint8_t> v(size);
    std::uint32_t x = 0x12345678u;
    for (auto& b : v) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    return v;
}

// Same value as the one-shot XXH3_64bits it replaces, on both sides of the chunk size and of
// XXH3's short/long input split (240 bytes).
static void TestMatchesOneShot() {
    const std::uint64_t sizes[] = {0,    1,    16,   240,  241,  4095,  4096,
                                   4097, 8191, 8192, 8193, 12345, 100000};
    for (std::uint64_t size : sizes) {
        const auto data = Pattern(size);
        const auto base = reinterpret_cast<std::uint64_t>(data.data());
        const std::uint64_t got =
            Hash(base, size, [](std::uint8_t* dst, std::uint64_t src, std::uint64_t n) {
                std::memcpy(dst, reinterpret_cast<const void*>(src), n);
            });
        CHECK(got == XXH3_64bits(data.data(), size));
    }
}

// The reader is called in order, in pieces of at most ChunkSize that cover the range exactly.
static void TestChunks() {
    const std::uint64_t base = 0x1000000000ull, size = 3 * ChunkSize + 100;
    std::uint64_t next = base, calls = 0;
    bool ordered = true, bounded = true;
    Hash(base, size, [&](std::uint8_t* dst, std::uint64_t src, std::uint64_t n) {
        ordered &= src == next;
        bounded &= n > 0 && n <= ChunkSize;
        std::memset(dst, 0, n);
        next = src + n;
        ++calls;
    });
    CHECK(ordered);
    CHECK(bounded);
    CHECK(next == base + size);
    CHECK(calls == 4);
}

// Crash C: the image's guest range is unreadable. A direct read faults (in the game: inside the
// fault handler, a "nested fault"); the hash goes through the reader (CopySparseMemory) instead.
static void TestUnreadableRange() {
    const std::uint64_t size = 2 * 4096;
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
#else
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        p = nullptr;
    }
#endif
    CHECK(p != nullptr);
    if (!p) {
        return;
    }
    const auto backing = Pattern(size); // what the backing view would hold
    const auto base = reinterpret_cast<std::uint64_t>(p);
    const std::uint64_t got = Hash(base + 100, size - 200,
                                   [&](std::uint8_t* dst, std::uint64_t src, std::uint64_t n) {
                                       std::memcpy(dst, backing.data() + (src - base), n);
                                   });
    CHECK(got == XXH3_64bits(backing.data() + 100, size - 200));
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

int main() {
    TestMatchesOneShot();
    TestChunks();
    TestUnreadableRange();
    std::printf("test_guest_hash: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
