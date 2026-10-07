// SPDX-License-Identifier: GPL-2.0-or-later
// sotcgpu internals shared by sotcgpu.cpp and the shim implementations.
#pragma once

#include <filesystem>
#include "sotcgpu.h"

namespace SotcGpu {

/// The runtime's callbacks (null before sotcgpu_init and after shutdown).
extern const sotcgpu_host* g_host;
/// sotcgpu_config::neo.
extern bool g_neo;
/// Root for shadPS4's user paths (shader dumps, pipeline cache).
extern std::filesystem::path g_user_dir;

/// Sends a line to host->log, or stderr when there is no host.
void HostLog(int level, const char* msg);

} // namespace SotcGpu
