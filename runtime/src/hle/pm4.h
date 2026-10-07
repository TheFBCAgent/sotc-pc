/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Null PM4 executor (workstream G2) and the host-side hooks GnmDriver (G1)
 * calls. Owned by G2; GnmDriver uses only this interface. */
#ifndef SOTC_PM4_H
#define SOTC_PM4_H

#include <stdbool.h>
#include <stdint.h>

#include "equeue.h"

/* Run a submission: walk the PM4 packets of the draw command buffer and the
 * optional constant command buffer (either may be NULL with 0 dwords), apply
 * their memory side effects (EOP/RELEASE_MEM label writes, WRITE_DATA,
 * DMA_DATA, ...) and trigger EOP interrupts on the registered equeues. Draws
 * and state are ignored. Work completes before this returns. Thread-safe.
 * Compute queues (DingDong) call it with the new ring range as `dcb`. */
void pm4_execute(const uint32_t *dcb, uint32_t dcb_dwords,
                 const uint32_t *ccb, uint32_t ccb_dwords);

/* sceGnmAddEqEvent / sceGnmDeleteEqEvent: register or unregister `eq` for
 * EVFILT_GRAPHICS_CORE event `ident` (calls equeue_add_event/remove_event).
 * EOP interrupts from pm4_execute trigger every registered (eq, ident). */
bool pm4_add_eop_equeue(equeue *eq, uint64_t ident, void *udata);
bool pm4_remove_eop_equeue(equeue *eq, uint64_t ident);

/* Queue a flip once the preceding submission has completed; used by
 * sceGnmSubmitAndFlipCommandBuffers. Implemented in videoout.c. Returns
 * SCE_OK or a VideoOut error code. */
int32_t videoout_submit_flip(int32_t handle, int32_t buf_index,
                             uint32_t flip_mode, int64_t flip_arg);

/* ---- asynchronous queues (added at M2 integration) ----------------------
 * The GPU runs submissions asynchronously, and games rely on it: a command
 * buffer may WAIT_REG_MEM on a label the CPU writes only after submitting.
 * pm4_submit queues the buffers on a worker thread per queue (graphics, or a
 * compute ring by vqid), which runs them in order and blocks in WAIT_REG_MEM /
 * MEM_SEMAPHORE until satisfied. `done` (may be NULL) runs on the worker after
 * the buffers, e.g. to flip or to advance a compute ring's read pointer. The
 * buffers must stay valid until then, as on the real GPU. */
enum { PM4_QUEUE_GFX = 0, PM4_MAX_QUEUES = 72 }; /* compute rings: vqid 1..64 */
typedef void (*pm4_done_fn)(void *arg);
void pm4_submit(uint32_t queue, const uint32_t *dcb, uint32_t dcb_dwords,
                const uint32_t *ccb, uint32_t ccb_dwords, pm4_done_fn done, void *arg);
/* Block until every queue has drained (sceGnmSubmitDone-style idling, tests). */
void pm4_wait_idle(void);

/* ---- M3: the GPU bridge (gpu_bridge.c) ------------------------------------
 * While sotcgpu is active (gpu_bridge_active), pm4_submit forwards to it
 * instead of the null queues. These are what its callbacks end up calling. */
/* An EOP / RELEASE_MEM interrupt: trigger every registered EOP equeue. */
void pm4_raise_eop_interrupt(void);
/* sotcgpu presented buffer `buf_index`: update the flip status and fire flip
 * events (videoout.c). Flips complete in submission order. */
void videoout_flip_done(int32_t buf_index, int64_t flip_arg);
/* Rate-limited "flip queue full" warning (videoout.c). */
void videoout_log_queue_full(const char *what);

#endif
