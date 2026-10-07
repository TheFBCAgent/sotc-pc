// SPDX-License-Identifier: GPL-2.0-or-later
// Implementations behind the gpu/shim/ headers: what shadPS4's core/ provides to
// video_core, rebuilt on top of sotcgpu_host (the runtime) and sotcgpu_config.

#include <chrono>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/time.h"
#include "core/memory.h"
#include "emulator.h"
#include "sdl_window.h"
#include "sotc_debug.h"
#include "sotcgpu_internal.h"

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace SotcGpu {

const sotcgpu_host* g_host = nullptr;
bool g_neo = false;
std::filesystem::path g_user_dir = "sotcgpu";

void HostLog(int level, const char* msg) {
    const sotcgpu_host* host = g_host;
    if (host && host->log) {
        host->log(level, msg);
    } else {
        std::fprintf(stderr, "%s\n", msg);
    }
}

} // namespace SotcGpu

using SotcGpu::g_host;

// ---------------------------------------------------------------- settings

SotcGpuSettings& SotcGpuGetSettings() {
    static SotcGpuSettings settings;
    return settings;
}

// ---------------------------------------------------------------- memory

namespace Core {

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    const bool allow_read = True(perms & MemoryPermission::Read);
    const bool allow_write = True(perms & MemoryPermission::Write);
    if (!g_host || !g_host->protect) {
        return;
    }
    if (!g_host->protect(virtual_addr, size, allow_read, allow_write)) {
        LOG_ERROR(Render, "protect({:#x}, {:#x}, r={}, w={}) failed", virtual_addr, size,
                  allow_read, allow_write);
    }
}

bool MemoryManager::IsValidGpuMapping(VAddr virtual_addr, u64 size) {
    if (g_host && g_host->is_gpu_mapping) {
        return g_host->is_gpu_mapping(virtual_addr, size);
    }
    // shadPS4: the PS4's GPU can only handle 40 bit addresses.
    return virtual_addr + size < 0x10000000000ULL;
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    if (g_host && g_host->clamp_range) {
        return g_host->clamp_range(virtual_addr, size);
    }
    return size;
}

void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    SotcDebug::Count(SotcDebug::Counter::CopyBytes, size); // SOTC_GPU_STATS: guest->GPU copies
    if (g_host && g_host->read_sparse) {
        g_host->read_sparse(dest, source, size);
        return;
    }
    std::memcpy(dest, reinterpret_cast<const void*>(source), size);
}

bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    if (g_host && g_host->write_backing) {
        return g_host->write_backing(reinterpret_cast<u64>(address), data, size);
    }
    std::memcpy(address, data, size);
    return true;
}

// ---------------------------------------------------------------- emulator

void Emulator::Shutdown() {
    Common::Log::Flush();
}

void Emulator::FillElfInfo() {
    auto& info = Common::ElfInfo::Instance();
    info.initialized = true;
    info.game_serial = "CUSA08809";
    info.title = "Shadow of the Colossus";
    info.app_ver = "01.00";
    info.sdk_ver = 0x4508101;
    info.firmware_ver = 0x4508101;
    info.raw_firmware_ver = 0x4508101;
}

} // namespace Core

// ---------------------------------------------------------------- kernel

namespace Libraries::Kernel {

s32 sceKernelIsNeoMode() {
    return SotcGpu::g_neo ? 1 : 0;
}

s32 sceKernelGetCompiledSdkVersion(s32* ver) {
    if (ver) {
        *ver = 0x4508101;
    }
    return 0;
}

u64 sceKernelGetTscFrequency() {
    static const u64 freq = Common::EstimateRDTSCFrequency();
    return freq;
}

u64 sceKernelReadTsc() {
    return __rdtsc();
}

u64 sceKernelGetProcessTime() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace Libraries::Kernel

// ---------------------------------------------------------------- window

namespace Frontend {

WindowSDL::WindowSDL(SDL_Window* window_, s32 width_, s32 height_)
    : window{window_}, width{width_}, height{height_} {
    if (!window) {
        return;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
#if defined(_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
#else
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    }
#endif
    Refresh();
}

void WindowSDL::Refresh() const {
    if (!window) {
        return;
    }
    int w = 0, h = 0;
    if (SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0) {
        width = w;
        height = h;
    }
}

s32 WindowSDL::GetWidth() const {
    Refresh();
    return width;
}

s32 WindowSDL::GetHeight() const {
    Refresh();
    return height;
}

} // namespace Frontend
