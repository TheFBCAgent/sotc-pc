/* SPDX-License-Identifier: GPL-2.0-or-later */
/* First version: no GPU page tracking yet. Workstream GI fills this in. */
#include "gpu_bridge.h"

bool gpu_bridge_on_fault(uint64_t addr, bool is_write) {
    (void)addr;
    (void)is_write;
    return false;
}
