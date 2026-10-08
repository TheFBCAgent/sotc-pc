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
    /* Session 10 (SRT walker reads): if a page of [addr, addr+size) is read-blocked by GPU
     * page tracking and the range lies in one direct-memory view, the host address of the
     * range in the unprotected physical backing view; else 0. Optional (NULL). */
    uint64_t (*backing_if_blocked)(uint64_t addr, uint64_t size);
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
/* sceVideoOutSetFlipRate: present at most one flip every rate + 1 vblanks
 * (0: 60 Hz, 1: 30 Hz, 2: 20 Hz). (Added at M3 integration.) */
void sotcgpu_set_flip_rate(int32_t rate);

/* Guest memory events (Rasterizer::MapMemory / UnmapMemory). */
void sotcgpu_map_memory(uint64_t addr, uint64_t size);
void sotcgpu_unmap_memory(uint64_t addr, uint64_t size);
/* From the fault handler: true if the GPU's page tracking caused this fault
 * and has handled it (the access should be retried). `rip` is the faulting
 * instruction: a fault inside the library's own generated code (the SRT
 * walker reading through a bad pointer) is fixed up there and retried.
 * (Changed at M3 integration: `rip` added.) */
bool sotcgpu_handle_fault(uint64_t addr, bool is_write, uint64_t rip);
/* From the fault handler, before sotcgpu_handle_fault, for a read fault on a
 * page the GPU blocked: if `rip` is an SRT walker load whose bytes the GPU
 * hasn't written (the page is blocked for other bytes on it), performs the load
 * from the CPU copy through `gpr` (x86 encoding order) and advances `*rip_ptr`;
 * returns true. Otherwise false and nothing changes. (Session 10.) */
bool sotcgpu_emulate_load(uint64_t addr, uint64_t rip, uint64_t *const *gpr, uint64_t *rip_ptr);

/* Debug: write the most recently presented frame as a PNG. */
bool sotcgpu_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif
