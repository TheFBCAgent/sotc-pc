// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <queue>
#include <unordered_set>
#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsDivergentInst(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::LaneId:
        return true;
    case IR::Opcode::GetAttributeU32:
        return inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId;
    default:
        return false;
    }
}

// shadow-pc: the search stops at uniform_insts, whose result is the same in the whole workgroup.
// Ported from AjncSystem 162261147b (after Pink-shadPS4, luizgustavs, wave64_uniform_branches).
static bool IsDivergentCondition(const IR::U1& condition,
                                 const std::unordered_set<const IR::Inst*>& uniform_insts) {
    if (condition.IsImmediate()) {
        return false;
    }
    std::unordered_set<const IR::Inst*> visited{condition.Inst()};
    std::queue<const IR::Inst*> queue;
    queue.push(condition.Inst());
    while (!queue.empty()) {
        const IR::Inst* inst = queue.front();
        queue.pop();
        if (IsDivergentInst(inst)) {
            return true;
        }
        if (uniform_insts.contains(inst)) {
            continue;
        }
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            const IR::Inst* arg_inst = inst->Arg(arg).TryInst();
            if (arg_inst && visited.insert(arg_inst).second) {
                queue.push(arg_inst);
            }
        }
    }
    return false;
}

static std::vector<IR::Block*> FindUniformBlocks(
    const IR::Program& program, const std::unordered_set<const IR::Inst*>& uniform_insts) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<const IR::Block*> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond, uniform_insts);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop:
            loops.push_back(node.data.loop.merge);
            break;
        case Type::Repeat:
            if (loops.empty() || loops.back() != node.data.repeat.merge) {
                return {};
            }
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0 && loops.empty()) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

static IR::Inst* FindBallotForMaskedBitCount(const IR::Inst& mbcnt) {
    IR::Value value = mbcnt.Arg(0);
    if (value.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* inst = value.Inst();
    if (inst->GetOpcode() != IR::Opcode::CompositeExtractU32x2 || inst->Arg(0).IsImmediate()) {
        return nullptr;
    }
    inst = inst->Arg(0).Inst();
    if (inst->GetOpcode() != IR::Opcode::UnpackUint2x32 || inst->Arg(0).IsImmediate()) {
        return nullptr;
    }
    inst = inst->Arg(0).Inst();
    return inst->GetOpcode() == IR::Opcode::Ballot ? inst : nullptr;
}

// shadow-pc: the identity of the reduction that produced `value` (looking through bitcasts), so
// combining it with a lane that holds the identity leaves the result unchanged.
static std::optional<u32> ReductionIdentity(const IR::Value& value) {
    IR::Value v = value;
    while (!v.IsImmediate()) {
        const IR::Inst* inst = v.Inst();
        switch (inst->GetOpcode()) {
        case IR::Opcode::BitCastF32U32:
        case IR::Opcode::BitCastU32F32:
            v = inst->Arg(0);
            continue;
        case IR::Opcode::FPAdd32:
            return 0u; // +0.0f
        case IR::Opcode::FPMul32:
            return 0x3f800000u; // 1.0f
        case IR::Opcode::FPMin32:
            return 0x7f800000u; // +inf
        case IR::Opcode::FPMax32:
            return 0xff800000u; // -inf
        case IR::Opcode::IAdd32:
        case IR::Opcode::BitwiseOr32:
        case IR::Opcode::BitwiseXor32:
        case IR::Opcode::UMax32:
            return 0u;
        case IR::Opcode::BitwiseAnd32:
        case IR::Opcode::UMin32:
            return 0xffffffffu;
        case IR::Opcode::SMin32:
            return 0x7fffffffu;
        case IR::Opcode::SMax32:
            return 0x80000000u;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// shadow-pc: a workgroup of at most 32 threads runs in one GCN wave64 with the lanes from
// num_threads up inactive. Wave64 reductions still read them ("ReadLane 32"), which on a 32-wide
// host subgroup is past the subgroup (OpGroupNonUniformBroadcast, undefined). Read the identity
// of the reduction instead, or fold the lane into the active ones when the op is unknown.
// SotC's exposure shader (cs 0x87cf9b8a, 32 threads) has 40 such FPAdd and 2 FPMin reductions;
// the sums came out wrong and the sky was blown out (session 11, bug e). After AjncSystem
// 162261147b (ReductionIdentity).
static void LowerReadLanesPastWorkgroup(IR::Program& program, u32 num_threads) {
    u32 folded = 0;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ReadLane || !inst.Arg(1).IsImmediate()) {
                continue;
            }
            const u32 lane = inst.Arg(1).U32();
            if (lane < num_threads) {
                continue;
            }
            if (const auto identity = ReductionIdentity(inst.Arg(0))) {
                inst.ReplaceUsesWithAndRemove(IR::Value{*identity});
            } else {
                inst.SetArg(1, IR::Value{lane % num_threads});
                ++folded;
            }
        }
    }
    if (folded != 0) {
        LOG_WARNING(Render_Recompiler,
                    "{} ReadLane(s) past the {}-thread workgroup folded into it in shader {:#x}",
                    folded, num_threads, program.info.pgm_hash);
    }
}

// shadow-pc: names shaders with 33..63 threads that read a lane past the workgroup (session 28).
static void WarnReadLanesPastWorkgroup(const IR::Program& program, u32 num_threads) {
    u32 count = 0;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            count += inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate() &&
                     inst.Arg(1).U32() >= num_threads;
        }
    }
    if (count != 0) {
        LOG_WARNING(Render_Recompiler,
                    "{} ReadLane(s) past the {}-thread workgroup left as is in shader {:#x}",
                    count, num_threads, program.info.pgm_hash);
    }
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    // shadow-pc: lanes past the workgroup are inactive on a wave64 host subgroup (AMD) as well,
    // so this runs before the subgroup_size == 64 return (session 28). With <= 32 threads the
    // whole upper half of the GCN wave is inactive and holds the reduction identity, whatever
    // lane is read. With 33..63 threads a lane past the workgroup may hold a partial result of
    // the active upper lanes, so it is only reported.
    if (num_threads <= 32) {
        LowerReadLanesPastWorkgroup(program, num_threads);
        return;
    }
    if (num_threads < 64) {
        WarnReadLanesPastWorkgroup(program, num_threads);
    }
    if (profile.subgroup_size == 64) {
        return;
    }

    const auto is_unpack = [](const IR::Use& use) {
        return use.user->GetOpcode() == IR::Opcode::UnpackUint2x32;
    };
    // ReadLane and Ballot below are exchanged through LDS, so they are the same in the whole
    // 64-lane wave.
    const auto is_lowered = [&](const IR::Inst& inst) {
        return (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate()) ||
               (inst.GetOpcode() == IR::Opcode::Ballot &&
                std::ranges::any_of(inst.Uses(), is_unpack));
    };

    std::vector<IR::Inst*> worklist;
    // shadow-pc: in a one-wave workgroup, a branch on a lowered value is uniform too, so the
    // ReadLane and Ballot in it are lowered as well. Repeat until no more blocks become uniform.
    // SotC's tiled lighting (cs 0xfb974c0e, 8x8 tiles) reduces over the tile, branches on the
    // result and reduces again inside the branch: that inner "ReadLane 32" read past the 32-wide
    // host subgroup, so the two halves of a tile (rows 0-3 and 4-7) disagreed and lit surfaces
    // got horizontal stripes (session 9, bug b). From AjncSystem 162261147b.
    std::unordered_set<const IR::Inst*> uniform_insts;
    auto uniform_blocks = FindUniformBlocks(program, uniform_insts);
    while (num_threads <= 64) {
        const size_t known = uniform_insts.size();
        for (const IR::Block* block : uniform_blocks) {
            for (const IR::Inst& inst : block->Instructions()) {
                if (is_lowered(inst)) {
                    uniform_insts.insert(&inst);
                }
            }
        }
        if (uniform_insts.size() == known) {
            break;
        }
        uniform_blocks = FindUniformBlocks(program, uniform_insts);
    }
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        const auto push_worklist = [&](IR::Inst& inst) {
            if (is_uniform) {
                worklist.push_back(&inst);
            } else {
                // shadow-pc: name the shader so it can go into SOTC_GPU_SKIP.
                LOG_WARNING(Render_Recompiler,
                            "{} instruction in non uniform control flow in shader {:#x}",
                            inst.GetOpcode(), program.info.pgm_hash);
            }
        };
        for (IR::Inst& inst : block->Instructions()) {
            if (is_lowered(inst)) {
                push_worklist(inst);
            } else if (inst.GetOpcode() == IR::Opcode::MaskedBitCount32) {
                IR::Inst* const ballot = FindBallotForMaskedBitCount(inst);
                if (ballot == nullptr ||
                    std::ranges::contains(uniform_blocks, ballot->GetParent())) {
                    worklist.push_back(&inst);
                }
            }
        }
    }
    if (worklist.empty()) {
        return;
    }

    const u32 scratch_base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u64));
    const u32 scratch_size =
        (Common::AlignUp(num_threads, 64) / profile.subgroup_size) * sizeof(u32);
    program.info.shared_memory_scratch_size =
        scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;

    for (IR::Inst* inst : worklist) {
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64", inst->GetOpcode());
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 subgroup_id = ir.ShiftRightLogical(invocation_index, ir.Imm32(5));
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, mask_low, offset);
            ir.Barrier();
            const IR::U64 mask = IR::U64{ir.LoadShared(64, false, offset)};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            const IR::U32 lane32 = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(31));
            const IR::U32 half = ir.ShiftRightLogical(IR::U32{inst->Arg(1)}, ir.Imm32(5u));
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, lane32), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false,
                                      ir.IAdd(ir.BitwiseAnd(offset, ir.Imm32(~7u)),
                                              ir.ShiftLeftLogical(half, ir.Imm32(2u))))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::MaskedBitCount32) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            const IR::U32 thread_mask{
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(2).U1() ? 1u : 0u)};
            const IR::U32 masked_value{
                ir.BitCount(ir.BitwiseAnd(IR::U32{inst->Arg(0)}, thread_mask))};
            inst->ReplaceUsesWithAndRemove(ir.IAdd(masked_value, IR::U32{inst->Arg(1)}));
        }
    }
}

} // namespace Shader::Optimization
