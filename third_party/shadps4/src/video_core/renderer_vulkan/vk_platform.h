// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <source_location>
#include <string>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/string_literal.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
enum class WindowSystemType : u8;
class WindowSDL;
} // namespace Frontend

namespace Vulkan {

constexpr u32 TargetVulkanApiVersion = VK_API_VERSION_1_3;

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window);

vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool enable_crash_diagnostic);

vk::UniqueDebugUtilsMessengerEXT CreateDebugCallback(vk::Instance instance);

template <typename T>
concept VulkanHandleType = vk::isVulkanHandleType<T>::value;

template <VulkanHandleType HandleType>
void SetObjectName(vk::Device device, const HandleType& handle, std::string_view debug_name) {
    const vk::DebugUtilsObjectNameInfoEXT name_info = {
        .objectType = HandleType::objectType,
        .objectHandle = reinterpret_cast<u64>(static_cast<typename HandleType::NativeType>(handle)),
        .pObjectName = debug_name.data(),
    };
    auto result = device.setDebugUtilsObjectNameEXT(name_info);
    if (result != vk::Result::eSuccess) {
        LOG_DEBUG(Render_Vulkan, "Could not set object debug name: {}", vk::to_string(result));
    }
}

template <VulkanHandleType HandleType, typename... Args>
void SetObjectName(vk::Device device, const HandleType& handle, const char* format,
                   const Args&... args) {
    const std::string debug_name = fmt::vformat(format, fmt::make_format_args(args...));
    SetObjectName(device, handle, debug_name);
}

// shadow-pc patch (diagnostics, workstream C): VRAM counters and the report hook. The buffer
// cache (EnsureResident, sparse arenas) and the texture cache (UniqueImage) keep the counters;
// sotcgpu installs `report`, which logs a "[vram]" summary. Diagnostic only.
namespace SotcDiag {
// Arena residency (workstream R: suballocated, BufferCache::PublishResidencyStats).
inline std::atomic<u64> arena_resident_bytes{0}; ///< bytes made resident (slices in use)
inline std::atomic<u64> arena_allocations{0};    ///< residency chunk vkAllocateMemory calls, total
inline std::atomic<u64> arena_chunks{0};         ///< live residency chunks
inline std::atomic<u64> arena_chunk_bytes{0};    ///< bytes allocated in them
inline std::atomic<u64> arena_buffers{0};        ///< sparse arena VkBuffers created
inline std::atomic<u64> image_bytes{0};          ///< live VMA images (UniqueImage)
inline std::atomic<u64> image_count{0};
// Image GC (workstream T: TextureCache::GarbageCollectImages, sotc_gc_policy.h). Totals.
inline std::atomic<u64> gc_level{0};       ///< SotcGc::Level of the last tick
inline std::atomic<u64> gc_freed{0};       ///< images freed by the GC
inline std::atomic<u64> gc_freed_stale{0}; ///< ...of which tiled GPU-written, without download
inline std::atomic<u64> gc_downloaded{0};  ///< ...of which linear GPU-written, after a download
inline std::atomic<u64> gc_skipped{0};     ///< old GPU-written images the policy kept
// Emergency image GC (docs/NEXT.md, session 5): a device allocation failed, the texture cache
// freed images (TextureCache::SotcEmergencyCollect) and the caller retried once.
inline std::atomic<u64> gc_emergency_runs{0};
inline std::atomic<u64> gc_emergency_freed{0}; ///< images
inline std::atomic<u64> gc_emergency_bytes{0};
inline std::atomic<u64> gc_emergency_failed{0}; ///< retries that failed as well (then asserted)
// Arena memory that backs guest ranges live images also hold (resident twice). Refreshed with
// gc_breakdown; 0 until the first breakdown.
inline std::atomic<u64> arena_ranges_bytes{0}; ///< resident_ranges total at the breakdown
inline std::atomic<u64> arena_image_bytes{0};  ///< ...of which inside a live image's guest range
inline std::mutex gc_breakdown_mutex;
inline std::string gc_breakdown; ///< live-image breakdown, refreshed by the GC every ~10 s
// Session 23: the staging pool's rings and large buffers (StagingBufferPool::PublishSotcStats),
// refreshed every ~10 s.
inline std::mutex staging_mutex;
inline std::string staging_breakdown;
inline std::atomic<void (*)(const char* why)> report{nullptr};
/// Installed by the texture cache: runs its emergency GC (`request` bytes failed to allocate,
/// `why` names the caller). Returns whether anything was freed, so a retry can help.
inline std::atomic<bool (*)(void* ctx, u64 request, const char* why)> emergency_gc{nullptr};
inline std::atomic<void*> emergency_gc_ctx{nullptr};

inline bool EmergencyGc(u64 request, const char* why) {
    auto* fn = emergency_gc.load(std::memory_order_acquire);
    return fn && fn(emergency_gc_ctx.load(std::memory_order_acquire), request, why);
}

inline void Report(const char* why) {
    if (auto* fn = report.load(std::memory_order_acquire)) {
        fn(why);
    }
}

/// Logs a failed Vulkan call (result name, caller, hint) and the [vram] report. The caller
/// asserts afterwards, as before.
[[gnu::cold, gnu::noinline]] inline void CheckFailed(vk::Result r, std::string_view what,
                                                     const std::source_location& loc) {
    const char* hint = "";
    if (r == vk::Result::eErrorDeviceLost) {
        hint = " -- DEVICE LOST (GPU fault/hang or TDR; see the System event log)";
    } else if (r == vk::Result::eErrorOutOfDeviceMemory) {
        hint = " -- OUT OF DEVICE MEMORY (VRAM)";
    } else if (r == vk::Result::eErrorOutOfHostMemory) {
        hint = " -- OUT OF HOST MEMORY";
    } else if (r == vk::Result::eErrorTooManyObjects) {
        hint = " -- TOO MANY OBJECTS (maxMemoryAllocationCount?)";
    }
    Common::Log::Log(Common::Log::Class::Render_Vulkan, Common::Log::Level::Critical,
                     loc.file_name(), static_cast<int>(loc.line()), loc.function_name(),
                     "[vkcheck] vk::Result={}{}{}{}", vk::to_string(r), what.empty() ? "" : " in ",
                     what, hint);
    Report("vkcheck");
}
} // namespace SotcDiag

template <StringLiteral msg = "">
static void Check(vk::Result r, std::source_location loc = std::source_location::current()) {
    if (r != vk::Result::eSuccess) [[unlikely]] {
        SotcDiag::CheckFailed(r, msg.len <= 1 ? std::string_view{} : std::string_view{msg.value},
                              loc);
    }
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r));
    } else {
        ASSERT_MSG(r == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r));
    }
}

template <StringLiteral msg = "", typename T>
static T Check(vk::ResultValue<T> r, std::source_location loc = std::source_location::current()) {
    if (r.result != vk::Result::eSuccess) [[unlikely]] {
        SotcDiag::CheckFailed(r.result,
                              msg.len <= 1 ? std::string_view{} : std::string_view{msg.value}, loc);
    }
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r.result));
    } else {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r.result));
    }
    return std::move(r.value);
}

} // namespace Vulkan
