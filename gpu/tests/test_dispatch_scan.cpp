// SPDX-License-Identifier: GPL-2.0-or-later
// Session 31: the PM4 dispatch scan (video_core/amdgpu/sotc_dispatch_scan.h) on hand-built
// command streams: program addresses from SET_SH_REG, indirect buffers, truncation.
#include <cstdio>
#include <cstdint>
#include <vector>

#include "video_core/amdgpu/sotc_dispatch_scan.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace S = SotcDispatchScan;

static u32 Hdr(u32 opcode, u32 body_dwords) {
    return (3u << 30) | ((body_dwords - 1) << 16) | (opcode << 8);
}

struct Cmds {
    std::vector<u32> d;
    void SetSh(u32 reg, std::initializer_list<u32> vals) {
        d.push_back(Hdr(S::OpSetShReg, u32(vals.size()) + 1));
        d.push_back(reg);
        d.insert(d.end(), vals);
    }
    void SetPgm(u64 addr) {
        SetSh(S::RegComputePgmLo, {u32(addr >> 8), u32(addr >> 40)});
    }
    void Dispatch() {
        d.push_back(Hdr(S::OpDispatchDirect, 4));
        d.insert(d.end(), {8, 1, 1, 1});
    }
    void DispatchIndirect() {
        d.push_back(Hdr(S::OpDispatchIndirect, 2));
        d.insert(d.end(), {0, 1});
    }
    void Nop(u32 n) {
        d.push_back(Hdr(0x10, n));
        d.insert(d.end(), n, 0xdeadbeef);
    }
    void Ib(const std::vector<u32>& ib) {
        const u64 addr = u64(reinterpret_cast<std::uintptr_t>(ib.data()));
        d.push_back(Hdr(S::OpIndirectBuffer, 3));
        d.insert(d.end(), {u32(addr), u32(addr >> 32) & 0xffff, u32(ib.size())});
    }
};

static const u32* ReadIb(u64 addr, u32) {
    return reinterpret_cast<const u32*>(std::uintptr_t(addr));
}

static std::vector<u64> ScanAll(const Cmds& c, S::State& st, int depth = 2) {
    std::vector<u64> out;
    S::Scan(c.d.data(), c.d.size(), st, ReadIb, [&](u64 a, const u32*) {
        out.push_back(a);
        return true;
    }, depth);
    return out;
}

int main() {
    { // program addresses follow the SET_SH_REG writes, padding and other packets are skipped
        Cmds c;
        c.SetPgm(0x10fa000100);
        c.Nop(5);
        c.Dispatch();
        c.d.push_back(0x80000000); // type-2 padding
        c.SetSh(S::RegComputePgmLo, {u32(0x10fb000200ull >> 8)}); // lo only: hi stays
        c.DispatchIndirect();
        c.SetSh(0x240, {1, 2, 3, 4}); // user data, not the program
        c.Dispatch();
        S::State st;
        const auto a = ScanAll(c, st);
        CHECK(a.size() == 3);
        CHECK(a[0] == 0x10fa000100);
        CHECK(a[1] == 0x10fb000200);
        CHECK(a[2] == 0x10fb000200);
        CHECK(!st.truncated);
        CHECK(st.dwords == c.d.size());
    }
    { // a write spanning both registers from an earlier offset (ComputeProgram block writes)
        Cmds c;
        c.SetSh(0x20a, {7, 7, u32(0x1200000000ull >> 8), u32(0x1200000000ull >> 40), 9});
        c.Dispatch();
        S::State st;
        const auto a = ScanAll(c, st);
        CHECK(a.size() == 1 && a[0] == 0x1200000000);
    }
    { // indirect buffers are followed, the state carries across, and depth limits them
        Cmds inner;
        inner.Dispatch(); // uses the outer program
        inner.SetPgm(0x1300000000);
        inner.Dispatch();
        Cmds outer;
        outer.SetPgm(0x1100000000);
        outer.Ib(inner.d);
        outer.Dispatch(); // the inner buffer's last program
        S::State st;
        const auto a = ScanAll(outer, st);
        CHECK(a.size() == 3);
        CHECK(a[0] == 0x1100000000 && a[1] == 0x1300000000 && a[2] == 0x1300000000);
        S::State st0;
        const auto b = ScanAll(outer, st0, 0);
        CHECK(b.size() == 1 && b[0] == 0x1100000000);
    }
    { // a packet running past the end (split across a ring boundary) stops the scan
        Cmds c;
        c.SetPgm(0x1400000000);
        c.Dispatch();
        c.Dispatch();
        S::State st;
        std::vector<u64> out;
        S::Scan(c.d.data(), c.d.size() - 2, st, ReadIb, [&](u64 a, const u32*) {
            out.push_back(a);
            return true;
        });
        CHECK(out.size() == 1);
        CHECK(st.truncated);
    }
    { // on_dispatch can stop early; max_dwords bounds the work
        Cmds c;
        c.SetPgm(0x1500000000);
        for (int i = 0; i < 10; ++i) {
            c.Dispatch();
        }
        S::State st;
        int n = 0;
        CHECK(!S::Scan(c.d.data(), c.d.size(), st, ReadIb, [&](u64, const u32*) { return ++n < 4; }));
        CHECK(n == 4);
        S::State st2;
        st2.max_dwords = 20;
        int m = 0;
        S::Scan(c.d.data(), c.d.size(), st2, ReadIb, [&](u64, const u32*) { return ++m, true; });
        CHECK(st2.truncated && m < 10);
    }
    { // the register copy follows SET_SH_REG (ComputeProgram range only) and DISPATCH_DIRECT
        Cmds c;
        c.SetPgm(0x1600000000);
        c.SetSh(0x240, {0xaaaa, 0xbbbb}); // user data 0, 1 (dword 0x40 of ComputeProgram)
        c.SetSh(0x1000, {0x5555});         // outside: ignored
        c.Dispatch();
        std::vector<u32> regs(0x50, 0);
        S::State st;
        st.cs_regs = regs.data();
        st.cs_regs_dwords = regs.size();
        std::vector<std::vector<u32>> seen;
        S::Scan(c.d.data(), c.d.size(), st, ReadIb, [&](u64, const u32*) {
            seen.emplace_back(regs);
            return true;
        });
        CHECK(seen.size() == 1);
        CHECK(seen[0][0x0c] == u32(0x1600000000ull >> 8) && seen[0][0x0d] == 0x00);
        CHECK(seen[0][0x40] == 0xaaaa && seen[0][0x41] == 0xbbbb);
        CHECK(seen[0][1] == 8 && seen[0][2] == 1 && seen[0][3] == 1 && seen[0][0] == 1);
    }
    if (g_failures) {
        std::printf("%d failures\n", g_failures);
        return 1;
    }
    std::printf("test_dispatch_scan: ok\n");
    return 0;
}
