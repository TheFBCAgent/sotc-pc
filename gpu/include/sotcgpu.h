/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sotcgpu: shadPS4's video_core + shader_recompiler (Vulkan) behind a C
 * interface. The runtime (C) calls in; the library calls back through
 * sotcgpu_host. Guest virtual addresses are GPU addresses (below 1 TiB).
 * Owned by workstream GL (implementation) and GI (runtime side); the
 * signatures below change only at integration. */
#ifndef SOTCGPU_H
#define SOTCGPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sotcgpu_done_fn)(void *arg);

typedef struct sotcgpu_host {
    /* Page tracking (shadPS4 PageManager): change guest page protection. */
    bool (*protect)(uint64_t addr, uint64_t size, bool allow_read, bool allow_write);
    /* Write guest memory regardless of its protection (Memory::TryWriteBacking). */
    bool (*write_backing)(uint64_t addr, const void *src, uint64_t size);
    /* Copy guest memory; unmapped holes read as zero (Memory::CopySparseMemory). */
    void (*read_sparse)(void *dst, uint64_t addr, uint64_t size);
    /* Bytes from addr that are mapped contiguously, at most size (ClampRangeSize). */
    uint64_t (*clamp_range)(uint64_t addr, uint64_t size);
    /* Whole range mapped and GPU-visible (Memory::IsValidGpuMapping). */
    bool (*is_gpu_mapping)(uint64_t addr, uint64_t size);
    /* An EOP / RELEASE_MEM interrupt (GnmDriver EOP equeue events). */
    void (*eop_interrupt)(void);
    /* A flip was presented: VideoOut status and flip events. */
    void (*flip_done)(int32_t buf_index, int64_t flip_arg);
    /* Log a line; level as in log.h (0 error .. 4 trace). */
    void (*log)(int level, const char *msg);
} sotcgpu_host;

typedef struct sotcgpu_config {
    void *sdl_window;      /* SDL_Window*, created by the runtime's main thread */
    uint32_t width, height; /* initial swapchain size */
    bool neo;              /* SOTC_NEO=1: PS4 Pro GPU */
    bool validation;       /* SOTC_VK_VALIDATION=1 */
    const char *cache_dir; /* pipeline/shader cache directory, or NULL */
} sotcgpu_config;

/* 0 on success; anything else means "no GPU": the runtime uses the null executor. */
int sotcgpu_init(const sotcgpu_host *host, const sotcgpu_config *cfg);
void sotcgpu_shutdown(void);

/* Submissions (Liverpool::SubmitGfx / SubmitAsc). Return at once; `done` (may
 * be NULL) runs once the command processor has consumed the buffers, e.g. to
 * advance a compute ring's read pointer. */
void sotcgpu_submit_gfx(const uint32_t *dcb, uint32_t dcb_dwords, const uint32_t *ccb,
                        uint32_t ccb_dwords, sotcgpu_done_fn done, void *arg);
void sotcgpu_submit_compute(uint32_t vqid, const uint32_t *acb, uint32_t dwords,
                            sotcgpu_done_fn done, void *arg);
void sotcgpu_submit_done(void);

/* Display buffers: `attribute` is the guest SceVideoOutBufferAttribute (40 bytes). */
void sotcgpu_register_buffers(int32_t group, const uint64_t *addrs, int32_t count,
                              const void *attribute);
/* Present buffer buf_index (-1: blank). after_gpu_work: ordered after all GPU
 * work submitted so far (SubmitAndFlip); else immediate (sceVideoOutSubmitFlip).
 * Completion is reported through host->flip_done. */
void sotcgpu_flip(int32_t buf_index, int64_t flip_arg, bool after_gpu_work);

/* Guest memory events (Rasterizer::MapMemory / UnmapMemory). */
void sotcgpu_map_memory(uint64_t addr, uint64_t size);
void sotcgpu_unmap_memory(uint64_t addr, uint64_t size);
/* From the fault handler: true if the GPU's page tracking caused this fault
 * and has handled it (the access should be retried). */
bool sotcgpu_handle_fault(uint64_t addr, bool is_write);

/* Debug: write the most recently presented frame as a PNG. */
bool sotcgpu_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif
