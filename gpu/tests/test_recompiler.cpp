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
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
