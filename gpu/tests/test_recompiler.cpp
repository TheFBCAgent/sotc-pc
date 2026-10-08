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
        walker(ud.data(), flat.data());
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
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
