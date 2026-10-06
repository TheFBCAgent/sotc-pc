/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Runtime side of the sotcgpu contract (gpu/include/sotcgpu.h).
 * Owned by workstream GI (M3). */
#ifndef SOTC_GPU_BRIDGE_H
#define SOTC_GPU_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* Called first by the fault handler for access faults: true if the GPU's
 * page tracking caused the fault and handled it (retry the access). */
bool gpu_bridge_on_fault(uint64_t addr, bool is_write);

#endif
