// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/devtools/layer.h: no devtools overlay.
#pragma once

#include "imgui/imgui_layer.h"

namespace Core::Devtools {

class Layer final : public ImGui::Layer {
public:
    static void DrawNullGpuNotice() {}
};

} // namespace Core::Devtools
