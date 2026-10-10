// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 31): find the compute dispatches ahead of the GPU thread in a PM4 stream,
// with the compute program address each one would run (COMPUTE_PGM_LO/HI as set by the
// SET_SH_REG packets before it). Vulkan-free; test: gpu/tests/test_dispatch_scan.cpp.
//
// Option B (compute pipelines on a cache miss): with both pipeline caches cold, the gameplay load
// builds ~200 compute pipelines back to back on the GPU thread (~110 ms each, no flip for ~20 s).
// The dispatches are already in the command stream; this finds them so their pipelines can be
// started early.
#pragma once

#include <cstddef>
#include <cstdint>

namespace SotcDispatchScan {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 OpSetShReg = 0x76;
constexpr u32 OpDispatchDirect = 0x15;
constexpr u32 OpDispatchIndirect = 0x16;
constexpr u32 OpIndirectBuffer = 0x3f;
constexpr u32 RegComputePgmLo = 0x20c; // SH register offsets (ComputeProgram at 0x200)
constexpr u32 RegComputePgmHi = 0x20d;

constexpr u32 RegComputeBase = 0x200; // ComputeProgram's first register (dispatch_initiator)

struct State {
    u32 pgm_lo = 0;
    u32 pgm_hi = 0;
    /// Optional: a copy of the ComputeProgram registers (from RegComputeBase), kept up to date
    /// by SET_SH_REG and DISPATCH_DIRECT (dims, initiator) as the scan goes.
    u32* cs_regs = nullptr;
    size_t cs_regs_dwords = 0;
    size_t dwords = 0;   ///< dwords scanned (across indirect buffers)
    size_t max_dwords = 1u << 20;
    bool truncated = false; ///< stopped at max_dwords or a malformed packet

    /// The program address as ComputeProgram::Address() returns it (the registers hold addr >> 8).
    u64 ProgramAddress() const {
        return ((u64(pgm_hi & 0xff) << 32) | pgm_lo) << 8;
    }
};

/// Walks [cmds, cmds + size) packet by packet, following INDIRECT_BUFFER packets up to `depth`
/// levels (read_ib(addr, dwords) returns the buffer's dwords or nullptr). Calls
/// on_dispatch(program_address, packet) for each dispatch; it returns false to stop the scan.
/// Returns false when stopped early (by on_dispatch or truncation).
template <typename ReadIb, typename OnDispatch>
bool Scan(const u32* cmds, size_t size, State& st, ReadIb&& read_ib, OnDispatch&& on_dispatch,
          int depth = 2) {
    size_t pos = 0;
    while (pos < size) {
        const u32 header = cmds[pos];
        const u32 type = header >> 30;
        if (type == 2) {
            ++pos;
            ++st.dwords;
            continue;
        }
        if (type != 3) {
            st.truncated = true;
            return false;
        }
        const u32 words = (((header >> 16) & 0x3fff) + 1) & 0x3fff; // body dwords
        const u32 opcode = (header >> 8) & 0xff;
        if (pos + 1 + words > size) {
            st.truncated = true; // split across a ring boundary: the rest comes in a later submit
            return false;
        }
        st.dwords += 1 + words;
        if (st.dwords > st.max_dwords) {
            st.truncated = true;
            return false;
        }
        const u32* body = cmds + pos + 1;
        switch (opcode) {
        case OpSetShReg:
            if (words >= 2) {
                const u32 reg = body[0] & 0xffff;
                for (u32 i = 0; i + 1 < words; ++i) {
                    if (st.cs_regs && reg + i >= RegComputeBase &&
                        reg + i < RegComputeBase + st.cs_regs_dwords) {
                        st.cs_regs[reg + i - RegComputeBase] = body[1 + i];
                    }
                    if (reg + i == RegComputePgmLo) {
                        st.pgm_lo = body[1 + i];
                    } else if (reg + i == RegComputePgmHi) {
                        st.pgm_hi = body[1 + i];
                    }
                }
            }
            break;
        case OpDispatchDirect:
            if (st.cs_regs && st.cs_regs_dwords >= 4 && words >= 4) {
                st.cs_regs[1] = body[0]; // dim_x, dim_y, dim_z
                st.cs_regs[2] = body[1];
                st.cs_regs[3] = body[2];
                st.cs_regs[0] = body[3]; // dispatch_initiator
            }
            [[fallthrough]];
        case OpDispatchIndirect:
            if (!on_dispatch(st.ProgramAddress(), cmds + pos)) {
                return false;
            }
            break;
        case OpIndirectBuffer:
            if (depth > 0 && words >= 3) {
                const u64 addr = (u64(body[1] & 0xffff) << 32) | body[0];
                const u32 ib_dwords = body[2] & 0xfffff;
                if (const u32* ib = read_ib(addr, ib_dwords)) {
                    if (!Scan(ib, ib_dwords, st, read_ib, on_dispatch, depth - 1)) {
                        return false;
                    }
                }
            }
            break;
        default:
            break;
        }
        pos += 1 + words;
    }
    return true;
}

} // namespace SotcDispatchScan
