// SPDX-License-Identifier: GPL-2.0-or-later
// Replaces shadPS4's common/logging/log.cpp (which needs spdlog): every LOG_* line from
// common, shader_recompiler and video_core goes to sotcgpu_host::log.
// Levels: Critical/Error -> 0, Warning -> 1, Info -> 2, Debug -> 3, Trace -> 4.
// The default filter is Info; SOTC_GPU_LOG=0..4 changes it (4 = Trace).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <fmt/format.h>

#include "common/logging/classes.h"
#include "common/logging/log.h"
#include "sotcgpu_internal.h"

namespace Common::Log {

static std::array<Level, NUM_LOG_CLASSES> InitialLevels() {
    Level level = Level::Info;
    if (const char* env = std::getenv("SOTC_GPU_LOG"); env && *env) {
        switch (std::atoi(env)) {
        case 0:
            level = Level::Error;
            break;
        case 1:
            level = Level::Warning;
            break;
        case 2:
            level = Level::Info;
            break;
        case 3:
            level = Level::Debug;
            break;
        default:
            level = Level::Trace;
            break;
        }
    }
    std::array<Level, NUM_LOG_CLASSES> levels;
    levels.fill(level);
    return levels;
}

std::array<Level, NUM_LOG_CLASSES> g_class_levels = InitialLevels();

void Setup(std::string_view) {}
void Switch(std::string_view, bool) {}
void Shutdown() {}
void Flush() {
    std::fflush(stderr);
}
void Terminate() {}
void UpdateSinks() {}
void UpdateLogLevels(std::string_view) {}
void UpdateLogFlushLevel(std::string_view) {}

static int HostLevel(Level level) {
    switch (level) {
    case Level::Trace:
        return 4;
    case Level::Debug:
        return 3;
    case Level::Info:
        return 2;
    case Level::Warning:
        return 1;
    default:
        return 0;
    }
}

void VLog(Class log_class, Level level, const char* file, int line, const char* func,
          fmt::string_view format, fmt::format_args args) {
    std::string msg;
    try {
        msg = fmt::vformat(format, args);
    } catch (...) {
        msg = std::string(format.data(), format.size()) + " (format error)";
    }
    std::string text;
    if (level >= Level::Warning) {
        const char* base = file;
        for (const char* p = file; p && *p; ++p) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        text = fmt::format("gpu: [{}] {}:{} {}: {}", NameOf(log_class), base ? base : "", line,
                           func ? func : "", msg);
    } else {
        text = fmt::format("gpu: [{}] {}", NameOf(log_class), msg);
    }
    SotcGpu::HostLog(HostLevel(level), text.c_str());
}

} // namespace Common::Log
