// SPDX-License-Identifier: GPL-2.0-or-later
// Session 10: gpu/src/sotc_srt_load.h, which keeps SRT walker reads of CPU-written bytes on a
// GPU-tracked page from draining the GPU: ResolveTable (read the table through the unprotected
// backing view) and EmulateLoad (the fallback for a load that still faults).
// The instruction bytes are the four load forms the walker emits, as xbyak encodes them.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "sotc_srt_load.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

enum Gpr { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

struct Machine {
    std::array<uint64_t, 16> gpr{};
    uint64_t rip = 0x1000;
    std::array<uint64_t*, 16> ptrs{};
    // Guest memory: 0x10000..0x12000, GPU-written [gpu_lo, gpu_hi).
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x2000);
    uint64_t gpu_lo = 0x10ff0, gpu_hi = 0x11000;
    int reads = 0, gpu_queries = 0;

    Machine() {
        for (size_t i = 0; i < 16; ++i) {
            ptrs[i] = &gpr[i];
            gpr[i] = 0xdead0000 + i;
        }
        for (size_t i = 0; i < mem.size(); ++i) {
            mem[i] = uint8_t(i * 7 + 3);
        }
    }

    uint64_t Load(uint64_t addr, size_t size) const {
        uint64_t v = 0;
        std::memcpy(&v, &mem[addr - 0x10000], size);
        return v;
    }

    SotcSrt::LoadResult Run(const std::vector<uint8_t>& code, uint64_t fault_addr) {
        std::vector<uint8_t> padded = code;
        padded.resize(15, 0x90); // the decoder looks at up to 15 bytes
        return SotcSrt::EmulateLoad(
            padded.data(), SotcSrt::Regs{ptrs.data(), &rip}, fault_addr,
            [&](uint64_t addr, uint64_t size) {
                ++gpu_queries;
                return addr < gpu_hi && gpu_lo < addr + size;
            },
            [&](uint64_t addr, void* dst, uint64_t size) {
                ++reads;
                std::memcpy(dst, &mem[addr - 0x10000], size);
            });
    }
};

using SotcSrt::LoadResult;

// mov rdi, [rdi + 0x1a60] (48 8B BF disp32): follow a pointer, outside the GPU-written range.
void TestPointerLoadDisp32() {
    Machine m;
    m.gpr[RDI] = 0x10000;
    CHECK(m.Run({0x48, 0x8B, 0xBF, 0x60, 0x1A, 0x00, 0x00}, 0x11a60) == LoadResult::Emulated);
    CHECK(m.gpr[RDI] == m.Load(0x11a60, 8));
    CHECK(m.rip == 0x1000 + 7);
    CHECK(m.gpr[R10] == 0xdead0000 + R10); // nothing else touched
}

// mov r10d, [rdi + 0x10] (44 8B 57 disp8): a dword copy; the upper half of r10 is cleared.
void TestDwordLoadDisp8() {
    Machine m;
    m.gpr[RDI] = 0x11000;
    m.gpr[R10] = 0xffffffffffffffffull;
    CHECK(m.Run({0x44, 0x8B, 0x57, 0x10}, 0x11010) == LoadResult::Emulated);
    CHECK(m.gpr[R10] == m.Load(0x11010, 4));
    CHECK(m.rip == 0x1000 + 4);
}

// mov r10d, [rdi + r10] (46 8B 14 17) and mov rdi, [rdi + r10] (4A 8B 3C 17): computed offsets.
void TestIndexedLoads() {
    Machine m;
    m.gpr[RDI] = 0x10000;
    m.gpr[R10] = 0x1234;
    CHECK(m.Run({0x46, 0x8B, 0x14, 0x17}, 0x11234) == LoadResult::Emulated);
    CHECK(m.gpr[R10] == m.Load(0x11234, 4));
    CHECK(m.rip == 0x1000 + 4);

    Machine p;
    p.gpr[RDI] = 0x10000;
    p.gpr[R10] = 0x1800;
    CHECK(p.Run({0x4A, 0x8B, 0x3C, 0x17}, 0x11800) == LoadResult::Emulated);
    CHECK(p.gpr[RDI] == p.Load(0x11800, 8));
}

// A load overlapping GPU-written bytes is left alone: the fault takes the download path.
void TestGpuWrittenBytesAreNotEmulated() {
    Machine m;
    m.gpr[RDI] = 0x10000;
    // [0x10ffc, 0x11004) overlaps [0x10ff0, 0x11000).
    CHECK(m.Run({0x48, 0x8B, 0xBF, 0xFC, 0x0F, 0x00, 0x00}, 0x10ffc) == LoadResult::GpuWritten);
    CHECK(m.reads == 0);
    CHECK(m.rip == 0x1000);
    CHECK(m.gpr[RDI] == 0x10000);
    // The bytes right after the GPU range are CPU data (SotC's descriptor table case).
    CHECK(m.Run({0x48, 0x8B, 0xBF, 0x00, 0x10, 0x00, 0x00}, 0x11000) == LoadResult::Emulated);
}

// Anything else is not ours: stores, the wrong fault address, rip-relative and segment loads.
void TestOtherInstructionsAreRejected() {
    Machine m;
    m.gpr[RDI] = 0x10000;
    m.gpr[RSI] = 0x10000;
    // mov [rsi + 0x40], r10d (44 89 56 40): a store.
    CHECK(m.Run({0x44, 0x89, 0x56, 0x40}, 0x10040) == LoadResult::NotALoad);
    // mov r10d, [rdi + 0x10] but the fault is elsewhere.
    CHECK(m.Run({0x44, 0x8B, 0x57, 0x10}, 0x10800) == LoadResult::NotALoad);
    // mov rax, [rip + 0x100] (48 8B 05 disp32).
    CHECK(m.Run({0x48, 0x8B, 0x05, 0x00, 0x01, 0x00, 0x00}, 0x10000) == LoadResult::NotALoad);
    // mov rax, fs:[0x28] (64 48 8B 04 25 28 00 00 00).
    CHECK(m.Run({0x64, 0x48, 0x8B, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00}, 0x28) ==
          LoadResult::NotALoad);
    // movzx eax, byte [rdi] (0F B6 07): not a mov.
    CHECK(m.Run({0x0F, 0xB6, 0x07}, 0x10000) == LoadResult::NotALoad);
    CHECK(m.reads == 0);
    CHECK(m.rip == 0x1000);
}

// The fault-free path: a table is read through the backing alias only when a page of it is
// blocked and none of its bytes are GPU-written.
void TestResolveTable() {
    const uint64_t table = 0x104276fa60, alias = 0x7fff0000a60;
    // GPU-written bytes end 16 bytes before the table (SotC's descriptor table).
    const auto gpu_written = [](uint64_t a, uint64_t n) {
        return a < 0x104276fa50 && 0x104273ed10 < a + n;
    };
    int backing_calls = 0;
    const auto blocked = [&](uint64_t, uint64_t) {
        ++backing_calls;
        return alias;
    };
    const auto not_blocked = [](uint64_t, uint64_t) { return uint64_t{0}; };
    CHECK(SotcSrt::ResolveTable(table, 0x580, blocked, gpu_written) == alias);
    CHECK(SotcSrt::ResolveTable(table, 0x580, not_blocked, gpu_written) == table);
    // A table that starts inside the GPU-written buffer keeps the raw pointer (fault, download).
    CHECK(SotcSrt::ResolveTable(0x104276fa40, 0x40, blocked, gpu_written) == 0x104276fa40);
    // Unknown extent (a computed offset): raw pointer, the host isn't asked.
    backing_calls = 0;
    CHECK(SotcSrt::ResolveTable(table, 0, blocked, gpu_written) == table);
    CHECK(backing_calls == 0);
}

struct Leaf {
    uint16_t table, src_dw, dst_dw;
    bool pointer;
};

// GPU-written constants are patched on the GPU; GPU-written pointers or descriptors download.
void TestPatchableTable() {
    const uint64_t table = 0x1042710110;
    // GPU-written: dwords 32..39 of the table (SotC's per-view constants).
    const auto gpu_written = [&](uint64_t a, uint64_t n) {
        return a < table + 40 * 4 && table + 32 * 4 < a + n;
    };
    const auto no_sharps = [](uint32_t) { return false; };
    std::vector<std::pair<uint32_t, uint64_t>> patches;
    const auto patch = [&](uint32_t dst, uint64_t src) { patches.emplace_back(dst, src); };
    const std::vector<Leaf> constants{{2, 32, 26, false}, {2, 33, 27, false}, {2, 2, 28, false},
                                      {3, 32, 40, false}};
    CHECK(SotcSrt::PatchableTable(table, 2, constants, gpu_written, no_sharps, patch));
    CHECK(patches.size() == 2); // dword 2 is CPU data, table 3 is another table
    CHECK(patches.size() == 2 && patches[0] == std::make_pair(26u, table + 32 * 4));
    // A GPU-written descriptor: download, no patches.
    patches.clear();
    CHECK(!SotcSrt::PatchableTable(table, 2, constants, gpu_written,
                                   [](uint32_t dst) { return dst == 27; }, patch));
    CHECK(patches.empty());
    // A GPU-written pointer the walker follows: download.
    const std::vector<Leaf> pointer{{2, 34, 30, true}};
    CHECK(!SotcSrt::PatchableTable(table, 2, pointer, gpu_written, no_sharps, patch));
    // A pointer outside the GPU-written bytes is fine.
    const std::vector<Leaf> cpu_pointer{{2, 0, 30, true}, {2, 33, 31, false}};
    CHECK(SotcSrt::PatchableTable(table, 2, cpu_pointer, gpu_written, no_sharps, patch));
    // No leaf map for the table (walker from the pipeline cache): download.
    CHECK(!SotcSrt::PatchableTable(table, 5, constants, gpu_written, no_sharps, patch));
}

} // namespace

int main() {
    TestPointerLoadDisp32();
    TestDwordLoadDisp8();
    TestIndexedLoads();
    TestGpuWrittenBytesAreNotEmulated();
    TestOtherInstructionsAreRejected();
    TestResolveTable();
    TestPatchableTable();
    if (g_failures) {
        std::printf("test_srt_load: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_srt_load: ok\n");
    return 0;
}
