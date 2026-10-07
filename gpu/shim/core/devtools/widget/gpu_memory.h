// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/devtools/widget/gpu_memory.h: the viewer is
// never enabled, so the staging pool never publishes.
#pragma once

#include <string>
#include <vector>
#include "common/types.h"

namespace Core::Devtools::Widget {

struct GpuMemoryRow {
    std::string name;
    u64 bytes{};
};

struct GpuMemoryGroup {
    std::string name;
    std::vector<GpuMemoryRow> rows;
};

class GpuMemoryViewer {
public:
    static bool IsEnabled() {
        return false;
    }
    static void Publish(GpuMemoryGroup) {}
};

} // namespace Core::Devtools::Widget
