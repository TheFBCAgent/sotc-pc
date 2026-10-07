// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's sdl_window.h. The runtime (workstream GW) creates and
// owns the SDL_Window on the process main thread; this wrapper only reads the native
// handle and the drawable size from it. A null window means headless (no presenter).
#pragma once

#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 {
    Headless,
    Windows,
    X11,
    Wayland,
    Metal,
};

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    /// Wraps an existing window; width/height are the initial swapchain size.
    WindowSDL(SDL_Window* window, s32 width, s32 height);

    /// Current drawable size in pixels (queried from SDL, falls back to the last value).
    s32 GetWidth() const;
    s32 GetHeight() const;

    bool IsOpen() const {
        return window != nullptr;
    }

    [[nodiscard]] SDL_Window* GetSDLWindow() const {
        return window;
    }

    WindowSystemInfo GetWindowInfo() const {
        return window_info;
    }

private:
    void Refresh() const;

    SDL_Window* window{};
    mutable s32 width{};
    mutable s32 height{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
