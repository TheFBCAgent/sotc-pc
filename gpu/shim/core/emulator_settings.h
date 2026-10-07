// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/emulator_settings.h: only the getters video_core,
// shader_recompiler and common call, with shadPS4's defaults (14ada48). sotcgpu_init
// fills the few that come from sotcgpu_config (validation, cache directory).
#pragma once

#include <string>
#include "common/types.h"

enum GpuReadbacksMode : int {
    Disabled,
    Relaxed,
    Precise,
};

struct SotcGpuSettings {
    // GPU
    bool null_gpu = false;
    bool copy_gpu_buffers = false;
    u32 readbacks_mode = GpuReadbacksMode::Disabled;
    bool readback_linear_images = false;
    bool direct_memory_access = false;
    bool dump_shaders = false;
    bool patch_shaders = false;
    u32 vblank_frequency = 60;
    std::string present_mode = "Mailbox";
    bool hdr_allowed = false;
    bool fsr_enabled = false;
    bool rcas_enabled = true;
    int rcas_attenuation = 250;
    bool userfaultfd = false;
    bool inline_fetch_shader = false;
    // Vulkan
    s32 gpu_id = -1;
    bool renderdoc_enabled = false;
    bool vkvalidation = false;
    bool vkvalidation_core = true;
    bool vkvalidation_sync = false;
    bool vkvalidation_gpu = false;
    bool vkcrash_diagnostic = false;
    bool vkhost_markers = false;
    bool vkguest_markers = false;
    bool pipeline_cache = false;
    bool pipeline_cache_archived = false;
    // Debug
    bool shader_collect = false;
    // Log (common/logging is replaced by gpu/src/log.cpp; kept for completeness)
    bool log_enable = true;

    bool IsNullGPU() const { return null_gpu; }
    bool IsCopyGpuBuffers() const { return copy_gpu_buffers; }
    u32 GetReadbacksMode() const { return readbacks_mode; }
    bool IsReadbackLinearImagesEnabled() const { return readback_linear_images; }
    bool IsDirectMemoryAccessEnabled() const { return direct_memory_access; }
    bool IsDumpShaders() const { return dump_shaders; }
    bool IsPatchShaders() const { return patch_shaders; }
    u32 GetVblankFrequency() const { return vblank_frequency; }
    std::string GetPresentMode() const { return present_mode; }
    bool IsHdrAllowed() const { return hdr_allowed; }
    bool IsFsrEnabled() const { return fsr_enabled; }
    bool IsRcasEnabled() const { return rcas_enabled; }
    int GetRcasAttenuation() const { return rcas_attenuation; }
    bool IsUserfaultfdTracking() const { return userfaultfd; }
    bool IsInlineFetchShader() const { return inline_fetch_shader; }
    s32 GetGpuId() const { return gpu_id; }
    bool IsRenderdocEnabled() const { return renderdoc_enabled; }
    bool IsVkValidationEnabled() const { return vkvalidation; }
    bool IsVkValidationCoreEnabled() const { return vkvalidation_core; }
    bool IsVkValidationSyncEnabled() const { return vkvalidation_sync; }
    bool IsVkValidationGpuEnabled() const { return vkvalidation_gpu; }
    bool IsVkCrashDiagnosticEnabled() const { return vkcrash_diagnostic; }
    bool IsVkHostMarkersEnabled() const { return vkhost_markers; }
    bool IsVkGuestMarkersEnabled() const { return vkguest_markers; }
    bool IsPipelineCacheEnabled() const { return pipeline_cache; }
    bool IsPipelineCacheArchived() const { return pipeline_cache_archived; }
    bool IsShaderCollect() const { return shader_collect; }
    bool IsLogEnable() const { return log_enable; }
};

SotcGpuSettings& SotcGpuGetSettings();

#define EmulatorSettings (SotcGpuGetSettings())
