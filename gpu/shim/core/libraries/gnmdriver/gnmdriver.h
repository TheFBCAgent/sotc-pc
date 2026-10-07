// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/libraries/gnmdriver/gnmdriver.h: the GPU core
// clock pm4_cmds.h converts TSC ticks with (shadPS4: 911 MHz on NEO, else 800 MHz).
#pragma once

#include "common/types.h"
#include "core/libraries/kernel/process.h"

namespace Libraries::GnmDriver {

inline u32 sceGnmGetGpuCoreClockFrequency() {
    return Libraries::Kernel::sceKernelIsNeoMode() ? 911'000'000u : 800'000'000u;
}

} // namespace Libraries::GnmDriver
