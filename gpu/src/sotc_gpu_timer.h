// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 12): GPU time per second from Vulkan timestamps (SOTC_GPU_STATS only).
// A timestamp at the start and the end of each submission, on the graphics queue
// (Scheduler::SubmitExecution, every scheduler) and on the side-copy queue
// (BufferCache::SotcSideCopy). The stats thread reads finished slots without waiting (it
// checks the submission's timeline semaphore or a fence the submitter already waited on).
// Implemented in gpu_timer.cpp; the interval math is sotc_gpu_busy.h.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace SotcDebug::GpuTimer {

enum class Queue : u8 { Main, Side };

/// Records a query reset and the start timestamp into `cmd` (outside a render pass). Returns
/// the slot, or -1 when stats are off, timestamps are unsupported or every slot is in flight.
int Begin(const Vulkan::Instance& instance, vk::CommandBuffer cmd, Queue queue);
/// Records the end timestamp of `slot` into `cmd`.
void End(vk::CommandBuffer cmd, int slot);
/// The submission holding `slot` signals `semaphore` at `tick` when it is done.
void Submitted(int slot, vk::Semaphore semaphore, u64 tick);
/// The submission holding `slot` is done (its fence was waited on).
void Completed(int slot);

struct Report {
    double busy_ms;  ///< union of all intervals, overlap counted once
    double main_ms;  ///< union of the graphics-queue intervals
    double side_ms;  ///< union of the side-copy intervals
    u32 timed;       ///< intervals collected
    u32 untimed;     ///< submissions without a slot (ring full) since the last report
    u32 in_flight;   ///< slots not yet finished
};
/// Stats thread: collects every finished slot (no waits) and frees it.
Report Collect();
/// SotcDebug::Shutdown, with the GPU idle: destroys the query pool.
void Destroy();

} // namespace SotcDebug::GpuTimer
