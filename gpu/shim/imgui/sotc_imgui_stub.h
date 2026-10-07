// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc: Dear ImGui and shadPS4's src/imgui are compiled out. This header gives
// every ImGui name video_core still references outside the presenter's draw block a
// no-op definition. The draw block itself (the game frame drawn as an ImGui image)
// is replaced by a plain blit under SOTC_NO_IMGUI; see third_party/shadps4/README.md.
// Each imgui/... shim header includes this one.
#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

using ImGuiID = unsigned int;
using ImTextureID = u64;

namespace Frontend {
class WindowSDL;
}
namespace Vulkan {
class Instance;
}

namespace ImGui {

class Layer {
public:
    virtual ~Layer() = default;
    static void AddLayer(Layer*) {}
    static void RemoveLayer(Layer*) {}
    virtual void Draw() {}
};

class RefCountedTexture {
public:
    struct Image {
        ImTextureID im_id;
        u32 width;
        u32 height;
    };
    static RefCountedTexture DecodePngTexture(std::vector<u8>) {
        return {};
    }
    static RefCountedTexture DecodePngFile(std::filesystem::path) {
        return {};
    }
    [[nodiscard]] Image GetTexture() const {
        return {};
    }
    explicit(false) operator bool() const {
        return false;
    }
};

namespace Core {
inline void Initialize(const ::Vulkan::Instance&, const ::Frontend::WindowSDL&, u32, vk::Format,
                       const vk::AllocationCallbacks* = nullptr) {}
inline void OnSurfaceFormatChange(vk::Format) {}
inline void Shutdown(const vk::Device&) {}
inline ImGuiID NewFrame(bool = false) {
    return 0;
}
inline void Render(const vk::CommandBuffer&, const vk::ImageView&, const vk::Extent2D&) {}
inline bool MustKeepDrawing() {
    return false;
}
namespace TextureManager {
inline void Submit() {}
} // namespace TextureManager
} // namespace Core

namespace Vulkan {
inline ImTextureID AddTexture(vk::ImageView, vk::ImageLayout,
                              const vk::AllocationCallbacks* = nullptr) {
    return 0;
}
inline void RemoveTexture(ImTextureID) {}
} // namespace Vulkan

namespace Friends {
inline void Register() {}
inline void Unregister() {}
} // namespace Friends
namespace InvitationPrompt {
inline void Register() {}
inline void Unregister() {}
} // namespace InvitationPrompt
namespace ShadNetNotify {
inline void Register() {}
inline void Unregister() {}
} // namespace ShadNetNotify

} // namespace ImGui

namespace shadNotifications {
enum class position {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};
inline void QueueNotification(std::string, float, position, std::vector<u8> = {}, bool = false) {}
} // namespace shadNotifications
