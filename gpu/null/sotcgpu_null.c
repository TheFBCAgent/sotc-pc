/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sotcgpu without a GPU: init fails, so the runtime keeps its null PM4
 * executor, and nothing else is ever called. Built when SOTC_VULKAN is OFF. */
#include "sotcgpu.h"

int sotcgpu_init(const sotcgpu_host *host, const sotcgpu_config *cfg) {
    (void)host;
    (void)cfg;
    return -1;
}

void sotcgpu_shutdown(void) {}

void sotcgpu_submit_gfx(const uint32_t *dcb, uint32_t dcb_dwords, const uint32_t *ccb,
                        uint32_t ccb_dwords, sotcgpu_done_fn done, void *arg) {
    (void)dcb, (void)dcb_dwords, (void)ccb, (void)ccb_dwords;
    if (done) done(arg);
}

void sotcgpu_submit_compute(uint32_t vqid, const uint32_t *acb, uint32_t dwords,
                            sotcgpu_done_fn done, void *arg) {
    (void)vqid, (void)acb, (void)dwords;
    if (done) done(arg);
}

void sotcgpu_submit_done(void) {}

void sotcgpu_register_buffers(int32_t group, const uint64_t *addrs, int32_t count,
                              const void *attribute) {
    (void)group, (void)addrs, (void)count, (void)attribute;
}

void sotcgpu_flip(int32_t buf_index, int64_t flip_arg, bool after_gpu_work) {
    (void)buf_index, (void)flip_arg, (void)after_gpu_work;
}

void sotcgpu_map_memory(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
void sotcgpu_unmap_memory(uint64_t addr, uint64_t size) { (void)addr, (void)size; }

bool sotcgpu_handle_fault(uint64_t addr, bool is_write) {
    (void)addr, (void)is_write;
    return false;
}

bool sotcgpu_screenshot(const char *path) {
    (void)path;
    return false;
}
