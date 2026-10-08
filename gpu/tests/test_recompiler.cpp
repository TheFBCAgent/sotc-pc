// SPDX-License-Identifier: GPL-2.0-or-later
// Recompiles two hand-assembled GCN shaders (a vertex shader that exports a position, and
// a pixel shader that exports a colour) to SPIR-V with shadPS4's shader_recompiler, the
// same way Vulkan::PipelineCache::CompileModule does, and checks the SPIR-V module.
// No GPU needed.

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// GCN3 encodings.
static constexpr u32 V_MOV_B32_V0_1_0 = 0x7E0002F2u; // v_mov_b32 v0, 1.0 (inline const 242)
static constexpr u32 V_MOV_B32_V1_0 = 0x7E020280u;   // v_mov_b32 v1, 0   (inline const 128)
static constexpr u32 S_ENDPGM = 0xBF810000u;

// exp pos0, v1, v1, v1, v0 done        (x=0 y=0 z=0 w=1)
static constexpr std::array<u32, 5> kVertexShader = {
    V_MOV_B32_V0_1_0, V_MOV_B32_V1_0,
    0xF80008CFu, // EXP: en=0xf tgt=12(pos0) done=1
    0x00010101u, // vsrc0=v1 vsrc1=v1 vsrc2=v1 vsrc3=v0
    S_ENDPGM,
};

// exp mrt0, v0, v1, v1, v0 done vm     (opaque red)
static constexpr std::array<u32, 5> kPixelShader = {
    V_MOV_B32_V0_1_0, V_MOV_B32_V1_0,
    0xF800180Fu, // EXP: en=0xf tgt=0(mrt0) done=1 vm=1
    0x00010100u, // vsrc0=v0 vsrc1=v1 vsrc2=v1 vsrc3=v0
    S_ENDPGM,
};

// Pixel shader with an SRT: user data s[0:1] is a pointer, and the shader exports the dword it
// points to. The flatten pass turns the load into an SRT walker (x86 code run on every draw).
//   s_load_dword s6, s[0:1], 0    s_waitcnt lgkmcnt(0)    v_mov_b32 v0, s6
//   exp mrt0, v0, v1, v1, v0 done vm
static constexpr std::array<u32, 7> kSrtPixelShader = {
    0xC0030100u, // s_load_dword s6, s[0:1], 0 (SMRD op=0 sdst=6 sbase=0 imm)
    0xBF8C007Fu, // s_waitcnt lgkmcnt(0)
    0x7E000206u, // v_mov_b32 v0, s6
    V_MOV_B32_V1_0,
    0xF800180Fu, // exp mrt0 done vm
    0x00010100u,
    S_ENDPGM,
};

// Pixel shader that reads its SRT with a runtime index, the shape of SotC's depth of field
// shaders (session 8): a scalar loop i = 0..3 loads table[i] (s_load_dword with an SGPR byte
// offset, so the offset is a Phi) and xors it into v0.
//        s_mov_b32 s7, 0          v_mov_b32 v0, 0          v_mov_b32 v1, 0
//   loop: s_lshl_b32 s8, s7, 2    s_load_dword s6, s[0:1], s8    s_waitcnt lgkmcnt(0)
//        v_xor_b32 v0, s6, v0     s_add_u32 s7, s7, 1      s_cmp_lt_u32 s7, 4
//        s_cbranch_scc1 loop      exp mrt0, v0, v1, v1, v0 done vm
static constexpr std::array<u32, 13> kSrtLoopPixelShader = {
    0xBE870380u, // s_mov_b32 s7, 0 (SOP1 op=3)
    0x7E000280u, // v_mov_b32 v0, 0
    V_MOV_B32_V1_0,
    0x8F088207u, // loop: s_lshl_b32 s8, s7, 2 (SOP2 op=30)
    0xC0030008u, // s_load_dword s6, s[0:1], s8 (SMRD imm=0: offset is an SGPR, in bytes)
    0xBF8C007Fu, // s_waitcnt lgkmcnt(0)
    0x3A000006u, // v_xor_b32 v0, s6, v0 (VOP2 op=29)
    0x80078107u, // s_add_u32 s7, s7, 1
    0xBF0A8407u, // s_cmp_lt_u32 s7, 4 (SOPC op=10)
    0xBF85FFF9u, // s_cbranch_scc1 loop (-7 dwords)
    0xF800180Fu, // exp mrt0 done vm
    0x00010100u,
    S_ENDPGM,
};

// Compute shader (8x8 workgroup) in the shape of SotC's tiled lighting, cs 0xfb974c0e (session 9):
// it reads lane 32, branches on the value, and reads lane 32 again inside the branch.
//        v_readlane_b32 s0, v0, 32    s_cmp_eq_u32 s0, 0    s_cbranch_scc1 end
//        v_readlane_b32 s1, v0, 32    v_mov_b32 v2, s1      ds_write_b32 v1, v2
//   end: s_endpgm
static constexpr std::array<u32, 10> kWave64BranchComputeShader = {
    0xD2020000u, 0x00014100u, // v_readlane_b32 s0, v0, 32 (VOP3 op=257, src1=inline 32)
    0xBF068000u,              // s_cmp_eq_u32 s0, 0 (SOPC op=6)
    0xBF850004u,              // s_cbranch_scc1 end (+4 dwords)
    0xD2020001u, 0x00014100u, // v_readlane_b32 s1, v0, 32
    0x7E040201u,              // v_mov_b32 v2, s1
    0xD8340000u, 0x00000201u, // ds_write_b32 v1, v2 (DS op=13)
    S_ENDPGM,
};

static Shader::Profile MakeProfile() {
    Shader::Profile profile{};
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    profile.max_shared_memory_size = 48 * 1024;
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = 32;
    profile.sparse_page_shift = 16;
    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_int64 = true;
    profile.support_float16 = true;
    profile.support_float64 = true;
    return profile;
}

/// SPIR-V module sanity: magic, version, and an OpEntryPoint for `model`.
static bool HasEntryPoint(const std::vector<u32>& spv, u32 model) {
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        return false;
    }
    for (size_t i = 5; i < spv.size();) {
        const u32 words = spv[i] >> 16;
        const u32 op = spv[i] & 0xffff;
        if (words == 0) {
            return false;
        }
        if (op == 15 /* OpEntryPoint */ && i + 1 < spv.size() && spv[i + 1] == model) {
            return true;
        }
        i += words;
    }
    return false;
}

static std::vector<u32> Recompile(Shader::HwStage hw, Shader::SwStage sw, std::span<const u32> code,
                                  Shader::RuntimeInfo& runtime_info) {
    static std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{user_data},
        .code = code,
        .hash = 0x1234,
    };
    Shader::Info info{hw, sw, params};
    Shader::Pools pools;
    const Shader::Profile profile = MakeProfile();
    Shader::Backend::Bindings binding{};
    const auto program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
}

static void SetupFragment(Shader::RuntimeInfo& runtime_info) {
    runtime_info.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    runtime_info.props.num_allocated_vgprs = 4;
    auto& cb = runtime_info.hw.fs.color_buffers[0];
    cb.data_format = AmdGpu::DataFormat::Format8_8_8_8;
    cb.num_format = AmdGpu::NumberFormat::Unorm;
    cb.num_conversion = AmdGpu::NumberConversion::None;
    cb.export_format = AmdGpu::ShaderExportFormat::ABGR_32;
    cb.swizzle.r = AmdGpu::CompSwizzle::Red;
    cb.swizzle.g = AmdGpu::CompSwizzle::Green;
    cb.swizzle.b = AmdGpu::CompSwizzle::Blue;
    cb.swizzle.a = AmdGpu::CompSwizzle::Alpha;
}

// shadow-pc patch (flatten_extended_userdata_pass.cpp, PushPtr): a null SRT pointer reads zeros
// for that walk only. shadPS4 faulted and patched the load to `xor` for good, so a pointer the
// game filled in later still read 0 (SotC's intermittent black screen at boot). Without the
// patch the null walks crash this test (no fault handler here); the first one already runs inside
// TranslateProgram (Info::RefreshFlatBuf with zeroed user data).
static void TestSrtWalkerNullPointer() {
    static std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{user_data},
        .code = kSrtPixelShader,
        .hash = 0x5678,
    };
    Shader::Info info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, params};
    Shader::RuntimeInfo runtime_info{};
    SetupFragment(runtime_info);
    runtime_info.props.num_user_data = 2; // s[0:1] hold the pointer
    Shader::Pools pools;
    const auto program =
        Shader::TranslateProgram(kSrtPixelShader, pools, info, runtime_info, MakeProfile());
    (void)program;
    const auto walker = info.srt_info.walker_func;
    const u32 size = info.srt_info.flattened_bufsize_dw;
    std::printf("srt walker: %p, flat buffer %u dwords\n", reinterpret_cast<void*>(walker), size);
    CHECK(walker != nullptr);
    CHECK(size > Shader::NUM_USER_DATA_REGS);
    if (!walker || size <= Shader::NUM_USER_DATA_REGS) {
        return;
    }
    auto walk = [&](u64 root) {
        std::array<u32, Shader::NUM_USER_DATA_REGS> ud{};
        std::memcpy(ud.data(), &root, sizeof(root));
        std::vector<u32> flat(size, 0xCDCDCDCDu);
        walker(ud.data(), flat.data(), &Shader::SrtCopyWindow);
        return flat;
    };
    alignas(8) static u32 table[4] = {0xDEADBEEFu, 0, 0, 0};
    const auto has = [&](const std::vector<u32>& flat, u32 v) {
        for (u32 i = Shader::NUM_USER_DATA_REGS; i < flat.size(); ++i) {
            if (flat[i] == v) {
                return true;
            }
        }
        return false;
    };
    // A null pointer (and a tiny garbage one) read zeros, without a fault.
    for (const u64 bad : {u64{0}, u64{0x860}}) {
        const auto flat = walk(bad);
        CHECK(!has(flat, 0xDEADBEEFu) && !has(flat, 0xCDCDCDCDu));
    }
    // The same walker, once the pointer is valid, reads the real data (shadPS4's fault patch made
    // it read 0 from then on).
    CHECK(has(walk(reinterpret_cast<u64>(table)), 0xDEADBEEFu));
    CHECK(!has(walk(0), 0xDEADBEEFu));
    CHECK(has(walk(reinterpret_cast<u64>(table)), 0xDEADBEEFu));
}

// shadow-pc patch (flatten_extended_userdata_pass.cpp, srt.h): a ReadConst indexed by a loop
// counter can't be flattened to one slot. shadPS4 left its flags 0, so the shader read flatbuf[0]
// (user data s0) for every index: SotC's blocky depth of field. The walker now copies a window
// of the table into the flattened buffer and the ReadConst indexes it.
static void TestSrtDynamicIndex() {
    static std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{user_data},
        .code = kSrtLoopPixelShader,
        .hash = 0x9abc,
    };
    Shader::Info info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, params};
    Shader::RuntimeInfo runtime_info{};
    SetupFragment(runtime_info);
    runtime_info.props.num_user_data = 2; // s[0:1] hold the pointer
    Shader::Pools pools;
    const Shader::Profile profile = MakeProfile();
    const auto program =
        Shader::TranslateProgram(kSrtLoopPixelShader, pools, info, runtime_info, profile);

    // Every runtime-indexed ReadConst reads from the same window; none is left at slot 0.
    u32 windowed = 0, unflattened = 0, window = 0;
    for (const Shader::IR::Block* block : program.blocks) {
        for (const Shader::IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != Shader::IR::Opcode::ReadConst || inst.Arg(1).IsImmediate()) {
                continue;
            }
            const u32 flags = inst.Flags<u32>();
            if (flags & Shader::ReadConstDynamicWindowFlag) {
                window = flags & ~Shader::ReadConstDynamicWindowFlag;
                ++windowed;
            } else if (flags == 0) {
                ++unflattened;
            }
        }
    }
    const u32 size = info.srt_info.flattened_bufsize_dw;
    std::printf("dynamic srt: %u windowed read(s) at dword %u, %u unflattened, flat buffer %u\n",
                windowed, window, unflattened, size);
    CHECK(windowed >= 1 && unflattened == 0);
    CHECK(window >= Shader::NUM_USER_DATA_REGS);
    CHECK(window + Shader::ReadConstDynamicWindowDwords <= size);
    const auto walker = info.srt_info.walker_func;
    CHECK(walker != nullptr);
    if (windowed == 0 || !walker || window + Shader::ReadConstDynamicWindowDwords > size) {
        return;
    }

    // The walker copies the table: the window holds table[0..4095] in order (no host Memory here,
    // so CopySparseMemory is a plain memcpy).
    static std::vector<u32> table(Shader::ReadConstDynamicWindowDwords);
    for (u32 i = 0; i < table.size(); ++i) {
        table[i] = 0xA0000000u + i;
    }
    auto walk = [&](u64 root) {
        std::array<u32, Shader::NUM_USER_DATA_REGS> ud{};
        std::memcpy(ud.data(), &root, sizeof(root));
        std::vector<u32> flat(size, 0xCDCDCDCDu);
        walker(ud.data(), flat.data(), &Shader::SrtCopyWindow);
        return flat;
    };
    auto flat = walk(reinterpret_cast<u64>(table.data()));
    for (const u32 i : {0u, 1u, 2u, 3u, Shader::ReadConstDynamicWindowDwords - 1}) {
        CHECK(flat[window + i] == table[i]);
    }
    // A null table pointer reads zeros (PushPtr's zero block), without a fault.
    flat = walk(0);
    CHECK(flat[window] == 0 && flat[window + 3] == 0);

    // And the shader still compiles to SPIR-V.
    Shader::Backend::Bindings binding{};
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
    CHECK(HasEntryPoint(spv, 4 /* Fragment */));
}

// shadow-pc patch (lower_wave64_pass.cpp): on a 32-wide host subgroup, a wave64 ReadLane of lane
// 32 must go through LDS. shadPS4 did that only outside branches that depend on the invocation, so
// a ReadLane inside a branch on an earlier (already lowered) ReadLane read past the subgroup:
// SotC's striped lighting. A branch on a lowered value is uniform, so both get lowered.
static void TestWave64UniformBranch() {
    static std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{user_data},
        .code = kWave64BranchComputeShader,
        .hash = 0xdef0,
    };
    Shader::Info info{Shader::HwStage::Compute, Shader::SwStage::Compute, params};
    Shader::RuntimeInfo runtime_info{};
    runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    runtime_info.props.num_allocated_vgprs = 4;
    runtime_info.hw.cs.workgroup_size = {8, 8, 1};
    runtime_info.hw.cs.shared_memory_size = 256;
    Shader::Pools pools;
    const Shader::Profile profile = MakeProfile();
    const auto program =
        Shader::TranslateProgram(kWave64BranchComputeShader, pools, info, runtime_info, profile);

    // No ReadLane is left with a lane past the host subgroup, and both went through LDS.
    u32 past_subgroup = 0, read_lanes = 0, shared_loads = 0;
    for (const Shader::IR::Block* block : program.blocks) {
        for (const Shader::IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == Shader::IR::Opcode::ReadLane) {
                ++read_lanes;
                past_subgroup += !inst.Arg(1).IsImmediate() || inst.Arg(1).U32() >= 32;
            } else if (inst.GetOpcode() == Shader::IR::Opcode::LoadSharedU32) {
                ++shared_loads;
            }
        }
    }
    std::printf("wave64 branch: %u ReadLane(s), %u past the subgroup, %u LDS load(s)\n",
                read_lanes, past_subgroup, shared_loads);
    CHECK(read_lanes == 2 && past_subgroup == 0);
    CHECK(shared_loads == 2);

    Shader::Backend::Bindings binding{};
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
    CHECK(HasEntryPoint(spv, 5 /* GLCompute */));
}

int main() {
    {
        Shader::RuntimeInfo runtime_info{};
        runtime_info.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
        runtime_info.props.num_allocated_vgprs = 4;
        runtime_info.props.num_input_vgprs = 0;
        const auto spv = Recompile(Shader::HwStage::Vertex, Shader::SwStage::Vertex,
                                   kVertexShader, runtime_info);
        std::printf("vertex shader: %zu SPIR-V words\n", spv.size());
        CHECK(HasEntryPoint(spv, 0 /* Vertex */));
    }
    {
        Shader::RuntimeInfo runtime_info{};
        runtime_info.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        runtime_info.props.num_allocated_vgprs = 4;
        auto& cb = runtime_info.hw.fs.color_buffers[0];
        cb.data_format = AmdGpu::DataFormat::Format8_8_8_8;
        cb.num_format = AmdGpu::NumberFormat::Unorm;
        cb.num_conversion = AmdGpu::NumberConversion::None;
        cb.export_format = AmdGpu::ShaderExportFormat::ABGR_32;
        cb.swizzle.r = AmdGpu::CompSwizzle::Red;
        cb.swizzle.g = AmdGpu::CompSwizzle::Green;
        cb.swizzle.b = AmdGpu::CompSwizzle::Blue;
        cb.swizzle.a = AmdGpu::CompSwizzle::Alpha;
        const auto spv = Recompile(Shader::HwStage::Fragment, Shader::SwStage::Fragment,
                                   kPixelShader, runtime_info);
        std::printf("pixel shader: %zu SPIR-V words\n", spv.size());
        CHECK(HasEntryPoint(spv, 4 /* Fragment */));
    }
    TestSrtWalkerNullPointer();
    TestSrtDynamicIndex();
    TestWave64UniformBranch();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
