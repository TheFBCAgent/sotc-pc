// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim: Tracy's Vulkan zones compiled out, as Tracy's own
// public/tracy/TracyVulkan.hpp does without TRACY_ENABLE.
#pragma once

#include "tracy/Tracy.hpp"

#define TracyVkContext(x, y, z, w) nullptr
#define TracyVkContextCalibrated(x, y, z, w, a, b) nullptr
#define TracyVkContextHostCalibrated(x, y, z, w, a) nullptr
#define TracyVkDestroy(x)
#define TracyVkContextName(c, x, y)
#define TracyVkNamedZone(c, x, y, z, w)
#define TracyVkNamedZoneC(c, x, y, z, w, a)
#define TracyVkZone(c, x, y)
#define TracyVkZoneC(c, x, y, z)
#define TracyVkCollect(c, x)

namespace tracy {
class VkCtxScope {};
} // namespace tracy

using TracyVkCtx = void*;
