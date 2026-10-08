// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 10): emulate an SRT walker load that faulted on a GPU-tracked page.
//
// The SRT walker (shader_recompiler flatten_extended_userdata_pass.cpp) is x86 code generated
// per shader. It runs on the GPU thread before every draw and dispatch and reads the shader's
// tables from guest memory with plain loads: `mov rdi, [rdi + disp|r10]` (follow a pointer) and
// `mov r10d, [rdi + disp|r10]` (copy a dword into the flattened buffer). Precise readbacks
// read-protect a whole 4 KiB page as soon as the GPU may write any byte of it, so a walker read
// of CPU-written bytes next to a GPU-written buffer drained the GPU: in SotC's gameplay ~45% of
// all drains came from one descriptor table that starts 16 bytes past a 200 KB buffer that a
// compute shader writes ~1,200 times a second.
//
// EmulateLoad decodes the faulting instruction and, when the bytes it reads aren't GPU-written
// (the buffer cache's byte-precise gpu_modified_ranges), performs the load from the CPU copy,
// which is current for those bytes, and steps over the instruction. GPU-written bytes still take
// the download path. Header-only and Vulkan-free: gpu/tests/test_srt_load.cpp.
#pragma once

#include <Zydis/Zydis.h>
#include <cstdint>

namespace SotcSrt {

/// Pointers into the faulting thread's saved context: general-purpose registers in x86
/// encoding order (rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15), and rip.
struct Regs {
    uint64_t* const* gpr;
    uint64_t* rip;
};

enum class LoadResult {
    NotALoad,   ///< not a plain `mov reg, [mem]` this emulator handles (or not the faulting one)
    GpuWritten, ///< the bytes it reads may be GPU-written: take the download path
    Emulated,   ///< done: the register holds the CPU copy and rip is past the instruction
};

/// `is_gpu_written(addr, size)` and `read(addr, dst, size)` access the guest memory model.
template <typename IsGpuWritten, typename Read>
LoadResult EmulateLoad(const uint8_t* code, const Regs& regs, uint64_t fault_addr,
                       IsGpuWritten&& is_gpu_written, Read&& read) {
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(
            ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return LoadResult::NotALoad;
    }
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, 15, &inst, ops)) ||
        inst.mnemonic != ZYDIS_MNEMONIC_MOV || inst.operand_count_visible != 2 ||
        ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
        return LoadResult::NotALoad;
    }
    const ZydisRegister dst = ops[0].reg.value;
    const ZydisRegisterClass dst_class = ZydisRegisterGetClass(dst);
    const uint32_t size = dst_class == ZYDIS_REGCLASS_GPR64   ? 8
                          : dst_class == ZYDIS_REGCLASS_GPR32 ? 4
                                                              : 0;
    const auto& mem = ops[1].mem;
    if (size == 0 || mem.type != ZYDIS_MEMOP_TYPE_MEM || mem.segment == ZYDIS_REGISTER_FS ||
        mem.segment == ZYDIS_REGISTER_GS) {
        return LoadResult::NotALoad;
    }
    const auto gpr_value = [&](ZydisRegister reg, uint64_t& out) {
        if (reg == ZYDIS_REGISTER_NONE) {
            out = 0;
            return true;
        }
        if (ZydisRegisterGetClass(reg) != ZYDIS_REGCLASS_GPR64) {
            return false; // rip-relative or 32-bit addressing: not walker code
        }
        out = *regs.gpr[ZydisRegisterGetId(reg)];
        return true;
    };
    uint64_t base, index;
    if (!gpr_value(mem.base, base) || !gpr_value(mem.index, index)) {
        return LoadResult::NotALoad;
    }
    const uint64_t addr = base + index * (mem.scale ? mem.scale : 1) +
                          static_cast<uint64_t>(mem.disp.value);
    if (fault_addr < addr || fault_addr >= addr + size) {
        return LoadResult::NotALoad; // the decode disagrees with the fault: leave it alone
    }
    if (is_gpu_written(addr, uint64_t{size})) {
        return LoadResult::GpuWritten;
    }
    uint64_t value = 0; // a 32-bit destination zero-extends, as the real mov does
    read(addr, &value, uint64_t{size});
    *regs.gpr[ZydisRegisterGetId(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
                                                                  dst))] = value;
    *regs.rip += inst.length;
    return LoadResult::Emulated;
}

/// Session 10, the fault-free path: where the walker should read a table of `bytes` bytes at
/// `ptr` (Shader::SrtResolveTable). `backing_if_blocked(ptr, bytes)` gives an unprotected alias
/// when GPU tracking blocks a page of the table (else 0). The alias is only used when none of the
/// table's bytes are GPU-written; otherwise the walker reads `ptr`, faults and downloads, so it
/// sees the GPU's data. bytes == 0 (extent unknown): `ptr`.
template <typename BackingIfBlocked, typename IsGpuWritten>
uint64_t ResolveTable(uint64_t ptr, uint64_t bytes, BackingIfBlocked&& backing_if_blocked,
                      IsGpuWritten&& is_gpu_written) {
    if (bytes == 0) {
        return ptr;
    }
    const uint64_t alias = backing_if_blocked(ptr, bytes);
    if (alias == 0 || is_gpu_written(ptr, bytes)) {
        return ptr;
    }
    return alias;
}

/// Session 10, GPU-written constants: a table whose GPU-written bytes the walker only copies into
/// the flattened buffer as shader constants doesn't need a download. The walker reads the CPU
/// copy and the GPU overwrites those dwords before the draw (Rasterizer::BindBuffers), in
/// command order, as the PS4 shader would read them. A GPU-written leaf the walker follows
/// (a pointer) or the host reads (a resource descriptor, `is_sharp(dst_dw)`) keeps the
/// download. `leaves` are the walker's leaves (Shader::SrtLeaf); `patch(dst_dw, src_addr)`
/// receives the dwords to copy. Returns false (and patches nothing) if a download is needed.
template <typename Leaves, typename IsGpuWritten, typename IsSharp, typename Patch>
bool PatchableTable(uint64_t ptr, uint32_t table, const Leaves& leaves,
                    IsGpuWritten&& is_gpu_written, IsSharp&& is_sharp, Patch&& patch) {
    bool any = false;
    for (const auto& leaf : leaves) {
        if (leaf.table != table) {
            continue;
        }
        any = true;
        const uint64_t src = ptr + uint64_t{leaf.src_dw} * 4;
        if (is_gpu_written(src, uint64_t{leaf.pointer ? 8u : 4u}) &&
            (leaf.pointer || is_sharp(leaf.dst_dw))) {
            return false;
        }
    }
    if (!any) {
        return false; // no leaf map (e.g. a walker from the pipeline cache): download
    }
    for (const auto& leaf : leaves) {
        const uint64_t src = ptr + uint64_t{leaf.src_dw} * 4;
        if (leaf.table == table && is_gpu_written(src, 4)) {
            patch(leaf.dst_dw, src);
        }
    }
    return true;
}

} // namespace SotcSrt
