/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Runtime side of the sotcgpu contract (gpu/include/sotcgpu.h): backend
 * choice, the sotcgpu_host callbacks, GPU page tracking and the glue that
 * VideoOut, GnmDriver, the PM4 queues and memory.c call.
 * Owned by workstream GI (M3). */
#ifndef SOTC_GPU_BRIDGE_H
#define SOTC_GPU_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called first by the fault handler for access faults: true if the GPU's
 * page tracking caused the fault and handled it, or the fault was in the GPU
 * library's own generated code and was fixed up (retry the access). */
bool gpu_bridge_on_fault(uint64_t addr, bool is_write, uint64_t rip);
/* For fault reports: the page's GPU tracking state and host protection. */
void gpu_bridge_describe(uint64_t addr, char *buf, size_t len);

/* ---- backend ------------------------------------------------------------
 * SOTC_GPU=vulkan|null picks the backend. Unset, it is vulkan when a game is
 * loaded and null otherwise (unit tests). Vulkan falls back to the null PM4
 * executor when sotcgpu_init fails (the null sotcgpu always fails).
 * gpu_bridge_init runs once; it can block on the window (window_sdl_handle),
 * so call it from a guest thread holding no runtime lock. Later calls return
 * at once. gpu_bridge_active never initializes. */
void gpu_bridge_init(void);
bool gpu_bridge_active(void);
/* Test hook: replace window_sdl_handle (call before gpu_bridge_init). */
void gpu_bridge_set_window_provider(void *(*fn)(void));

/* Submissions while the GPU is active (pm4_submit forwards here). `done` runs
 * on the bridge's event thread once sotcgpu has consumed the buffers. */
typedef void (*gpu_bridge_done_fn)(void *arg);
void gpu_bridge_submit(uint32_t queue, const uint32_t *dcb, uint32_t dcb_dwords,
                       const uint32_t *ccb, uint32_t ccb_dwords, gpu_bridge_done_fn done,
                       void *arg);

/* ---- memory.c hooks ------------------------------------------------------
 * The bridge keeps one byte per 4 KiB guest page: the protection the guest
 * asked for (host PLAT_PROT_* bits) and the GPU's read/write restrictions.
 * The host protection is always guest & GPU. While the GPU is inactive these
 * just call plat_protect / do nothing, exactly as before. */
/* Serializes protection changes; memory.c holds it around splits so the GPU
 * can't re-protect pages it is copying. Lock order: memory lock, then this. */
void gpu_bridge_track_lock(void);
void gpu_bridge_track_unlock(void);
/* A fresh mapping with host protection `prot` (already applied). */
void gpu_bridge_guest_map(uint64_t addr, uint64_t size, int prot);
/* A piece of a split mapping, re-mapped with `prot`: re-apply GPU restrictions. */
void gpu_bridge_guest_remap(uint64_t addr, uint64_t size, int prot);
/* The range is no longer mapped. */
void gpu_bridge_guest_unmap(uint64_t addr, uint64_t size);
/* sceKernelMprotect: set the guest protection and apply guest & GPU. */
bool gpu_bridge_guest_protect(uint64_t addr, uint64_t size, int prot);
/* Copy to or from guest memory regardless of protection (temporarily lifting
 * it under the tracking lock). `guest` is the guest-side address. */
bool gpu_bridge_force_copy(uint64_t guest, void *host, uint64_t size, bool to_guest);
/* Tell the GPU about GPU-visible maps and unmaps. Call without memory locks
 * held: sotcgpu may call back into memory.c (protect, read_sparse). */
void gpu_bridge_notify_map(uint64_t addr, uint64_t size);
void gpu_bridge_notify_unmap(uint64_t addr, uint64_t size);

/* ---- memory.c queries the bridge forwards to sotcgpu ---------------------- */
bool hle_memory_write_backing(uint64_t addr, const void *src, uint64_t size);
void hle_memory_read_sparse(void *dst, uint64_t addr, uint64_t size);
uint64_t hle_memory_clamp_range(uint64_t addr, uint64_t size);
bool hle_memory_is_gpu_mapping(uint64_t addr, uint64_t size);
/* Visit every mapped (non-reserved) region, outside the memory lock (a
 * snapshot): replays the existing mappings to sotcgpu at init. */
void hle_memory_for_each_mapping(void (*fn)(uint64_t addr, uint64_t size, void *ctx), void *ctx);

#endif
