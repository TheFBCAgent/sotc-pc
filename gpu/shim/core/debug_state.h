// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/debug_state.h: the devtools state video_core
// touches, as no-ops (there are no devtools in shadow-pc).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"



namespace DebugStateType {

class DebugStateImpl {
public:
    std::pair<u32, u32> game_resolution{};
    std::pair<u32, u32> output_resolution{};
    bool is_using_fsr{};

    bool IsGuestThreadsPaused() const { return false; }
    void IncFlipFrameNum() {}
    void IncDrawCall() noexcept {}
    void IncDispatch() noexcept {}
    void IncGnmFrameNum() {}
    bool DumpingCurrentFrame() const { return false; }
    bool DumpingCurrentReg() { return false; }
    bool ShouldPauseInSubmit() const { return false; }
    template <typename R>
    void PushRegsDump(uintptr_t, uintptr_t, const R&) {}
    template <typename CsState>
    void PushRegsDumpCompute(uintptr_t, uintptr_t, const CsState&) {}
    template <typename Stage>
    void CollectShader(const std::string&, Stage, vk::ShaderModule, std::span<const u32>,
                       std::span<const u32>, std::span<const u32>, bool) {}
};

} // namespace DebugStateType

inline DebugStateType::DebugStateImpl& SotcGpuDebugState() {
    static DebugStateType::DebugStateImpl state;
    return state;
}

#define DebugState (SotcGpuDebugState())
