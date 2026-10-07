/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sotcgpu without a GPU: init fails, so the runtime keeps its null PM4
 * executor, and nothing else is ever called. Built when SOTC_VULKAN is OFF.
 *
 * With SOTCGPU_NULL_RECORDING defined (tests only, see sotcgpu_null.h), init
 * succeeds and every call is recorded; submissions complete at once and
 * flips are reported through host->flip_done like a real presenter. */
#include "sotcgpu.h"

#ifndef SOTCGPU_NULL_RECORDING

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

bool sotcgpu_handle_fault(uint64_t addr, bool is_write, uint64_t rip) {
    (void)addr, (void)is_write, (void)rip;
    return false;
}

bool sotcgpu_screenshot(const char *path) {
    (void)path;
    return false;
}

#else /* SOTCGPU_NULL_RECORDING */

#include "sotcgpu_null.h"

sotcgpu_recorder sotcgpu_rec_state;

static void rec(sotcgpu_rec_kind kind, uint64_t a, uint64_t b, uint64_t c, bool flag) {
    int i = __atomic_fetch_add(&sotcgpu_rec_state.count, 1, __ATOMIC_SEQ_CST);
    if (i < SOTCGPU_REC_MAX) sotcgpu_rec_state.recs[i] = (sotcgpu_rec){kind, a, b, c, flag};
}

int sotcgpu_init(const sotcgpu_host *host, const sotcgpu_config *cfg) {
    sotcgpu_rec_state.host = host;
    sotcgpu_rec_state.cfg = *cfg;
    rec(REC_INIT, cfg->width, cfg->height, (uint64_t)(uintptr_t)cfg->sdl_window, cfg->neo);
    return sotcgpu_rec_state.init_result;
}

void sotcgpu_shutdown(void) { rec(REC_SHUTDOWN, 0, 0, 0, false); }

void sotcgpu_submit_gfx(const uint32_t *dcb, uint32_t dcb_dwords, const uint32_t *ccb,
                        uint32_t ccb_dwords, sotcgpu_done_fn done, void *arg) {
    rec(REC_SUBMIT_GFX, (uint64_t)(uintptr_t)dcb, dcb_dwords, (uint64_t)(uintptr_t)ccb,
        ccb_dwords != 0);
    if (done) done(arg);
}

void sotcgpu_submit_compute(uint32_t vqid, const uint32_t *acb, uint32_t dwords,
                            sotcgpu_done_fn done, void *arg) {
    rec(REC_SUBMIT_COMPUTE, vqid, (uint64_t)(uintptr_t)acb, dwords, false);
    if (done) done(arg);
}

void sotcgpu_submit_done(void) { rec(REC_SUBMIT_DONE, 0, 0, 0, false); }

void sotcgpu_register_buffers(int32_t group, const uint64_t *addrs, int32_t count,
                              const void *attribute) {
    rec(REC_REGISTER_BUFFERS, (uint64_t)group, (uint64_t)count, (uint64_t)(uintptr_t)attribute,
        false);
    sotcgpu_rec_state.buffer_count = count;
    for (int32_t i = 0; i < count && i < 16; i++) sotcgpu_rec_state.buffers[i] = addrs[i];
}

void sotcgpu_flip(int32_t buf_index, int64_t flip_arg, bool after_gpu_work) {
    rec(REC_FLIP, (uint64_t)(int64_t)buf_index, (uint64_t)flip_arg, 0, after_gpu_work);
    /* Present at once, from this thread (the bridge defers it anyway). */
    if (sotcgpu_rec_state.host) sotcgpu_rec_state.host->flip_done(buf_index, flip_arg);
}

void sotcgpu_map_memory(uint64_t addr, uint64_t size) { rec(REC_MAP, addr, size, 0, false); }
void sotcgpu_unmap_memory(uint64_t addr, uint64_t size) { rec(REC_UNMAP, addr, size, 0, false); }

bool sotcgpu_handle_fault(uint64_t addr, bool is_write, uint64_t rip) {
    (void)rip;
    rec(REC_FAULT, addr, 0, 0, is_write);
    if (!sotcgpu_rec_state.fault_unprotects || !sotcgpu_rec_state.host) return false;
    /* Like the page manager: the watched page becomes writable again. */
    sotcgpu_rec_state.host->protect(addr & ~0xfffull, 0x1000, true, true);
    return true;
}

bool sotcgpu_screenshot(const char *path) {
    rec(REC_SCREENSHOT, 0, 0, 0, path != NULL);
    return false;
}

#endif
