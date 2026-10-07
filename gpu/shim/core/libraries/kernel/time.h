// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/libraries/kernel/time.h: the TSC functions
// pm4_cmds.h uses for GPU timestamps. Same source as the runtime's sceKernelReadTsc
// (raw RDTSC), so guest and GPU timestamps agree.
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

u64 sceKernelGetTscFrequency();
u64 sceKernelReadTsc();
u64 sceKernelGetProcessTime();

} // namespace Libraries::Kernel
