// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim: the Tracy profiler is compiled out. These are the definitions
// Tracy's own public/tracy/Tracy.hpp provides when TRACY_ENABLE is not defined,
// limited to the names shadPS4's common/video_core use.
#pragma once

#include <cstdint>

#define ZoneNamed(x, y)
#define ZoneScoped
#define ZoneScopedN(x)
#define ZoneScopedC(x)
#define ZoneScopedNC(x, y)
#define ZoneText(x, y)
#define ZoneName(x, y)
#define FrameMark
#define FrameMarkNamed(x)
#define TracyMessage(x, y)
#define TracyMessageL(x)
#define TracyMessageC(x, y, z)
#define TracyMessageLC(x, y)
#define TracyAlloc(x, y)
#define TracyFree(x)
#define TracyAllocN(x, y, z)
#define TracyFreeN(x, y)
#define TracyFiberEnter(x)
#define TracyFiberLeave
#define TracyLockable(type, varname) type varname
#define TracyFile __FILE__
#define TracyLine __LINE__
#define TracyFunction __func__

namespace tracy {
struct SourceLocationData {
    const char* name;
    const char* function;
    const char* file;
    uint32_t line;
    uint32_t color;
};
struct Color {
    enum ColorType : uint32_t {
        DarkOrange = 0xFF8C00,
        Red = 0xFF0000,
        HotPink = 0xFF69B4,
    };
};
} // namespace tracy
