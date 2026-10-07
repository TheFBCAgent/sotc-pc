// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/libraries/kernel/process.h. NEO comes from
// sotcgpu_config::neo; the SDK version is SotC's (0x4508101), see gpu/src/shim_core.cpp.
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

s32 sceKernelIsNeoMode();
s32 sceKernelGetCompiledSdkVersion(s32* ver);

} // namespace Libraries::Kernel
