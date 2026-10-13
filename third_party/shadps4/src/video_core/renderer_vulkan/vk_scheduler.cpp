// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/sotc_wait_timer.h" // shadow-pc (session 49)
#include "sotc_debug.h" // shadow-pc: gpu/src/debug_dump.cpp
#include "sotc_gpu_timer.h" // shadow-pc: gpu/src/gpu_timer.cpp

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    sotc_cmd.mode = SotcCmdState::ParseMode(); // shadow-pc (session 62)
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    BeginSession();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    // shadow-pc (session 62): the body is SotcRecordBeginRendering (sotc_cmd_record.cpp).
    Cmd().beginRendering(render_state);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Cmd().endRendering(); // shadow-pc (session 62)
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    return upload_cmdbuf;
}

void Scheduler::Flush(SubmitInfo& info, std::source_location loc) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info, loc);
}

void Scheduler::Flush(std::source_location loc) {
    SubmitInfo info{};
    Flush(info, loc);
}

void Scheduler::Finish(std::source_location loc) {
    ZoneScopedNC("Scheduler::Finish", 0xd62828); // shadow-pc: Tracy (session 18)
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info, loc);
    Wait(presubmit_tick, loc);
}

void Scheduler::Wait(u64 tick, std::source_location loc) {
    ZoneScopedNC("Scheduler::Wait", 0xf77f00); // shadow-pc: Tracy (session 18)
    // shadow-pc: SOTC_GPU_STATS counts blocking waits and their time (session 12).
    const bool counted = SotcDebug::StatsEnabled() && !IsFree(tick);
    std::chrono::steady_clock::time_point wait_start;
    if (counted) [[unlikely]] {
        SotcDebug::CountWait();
        wait_start = std::chrono::steady_clock::now();
    }
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info, loc);
    }
    work_semaphore.Wait(tick);
    if (counted) [[unlikely]] {
        SotcDebug::AddWaitNs(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - wait_start)
                                 .count());
    }
}

void Scheduler::PopPendingOperations() {
    std::unique_lock lk(pending_ops_mutex);
    // shadow-pc: this runs before every draw and dispatch, and Refresh is a kernel call on RADV
    // (vkGetSemaphoreCounterValue -> DRM_IOCTL_SYNCOBJ_QUERY, ~15% of the GPU thread in
    // gameplay). Query only when the oldest op belongs to a submitted batch the known tick
    // hasn't reached, and at most every 100 us; Wait refreshes the tick itself.
    if (pending_ops.empty()) {
        return;
    }
    const u64 front_tick = pending_ops.front().gpu_tick;
    if (!work_semaphore.IsFree(front_tick) && front_tick < work_semaphore.CurrentTick()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - sotc_last_refresh >= std::chrono::microseconds(100)) {
            sotc_last_refresh = now;
            work_semaphore.Refresh();
        }
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

void Scheduler::BeginSession() {
    EndSession();

    auto& session = sessions.emplace_back();

    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    session.primary = command_pool.Commit();
    Check(session.primary.begin(begin_info));
    sotc_cmd.raw = session.primary; // shadow-pc (session 62): the decoder's target

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();
    sotc_bound_graphics = sotc_bound_compute = vk::Pipeline{};

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::BindPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) {
    static const bool memo_on = [] {
        const char* v = std::getenv("SOTC_BIND_MEMO");
        const bool on = !(v && *v == '0');
        LOG_INFO(Render_Vulkan, "pipeline-bind memo {}", on ? "on" : "off (SOTC_BIND_MEMO=0)");
        return on;
    }();
    vk::Pipeline& bound =
        point == vk::PipelineBindPoint::eCompute ? sotc_bound_compute : sotc_bound_graphics;
    if ((++sotc_bind_calls & 0x3FFFFF) == 0) {
        LOG_INFO(Render_Vulkan, "pipeline-bind memo: {} of {} binds skipped", sotc_bind_skips,
                 0x400000);
        sotc_bind_skips = 0;
    }
    if (memo_on && bound == pipeline) {
        ++sotc_bind_skips;
        return;
    }
    bound = pipeline;
    Cmd().bindPipeline(point, pipeline); // shadow-pc (session 62)
}

void Scheduler::EndSession() {
    if (sessions.empty()) {
        return;
    }

    if (on_session) {
        on_session();
    }

    const auto& session = sessions.back();
    if (session.upload) {
        Check(session.upload.end());
    }

    EndRendering();
    Check(session.primary.end());
}

void Scheduler::SubmitExecution(SubmitInfo& info, const std::source_location& loc) {
    SotcWaitTimer::CountSubmit(loc); // shadow-pc (session 58)
    // shadow-pc (session 63): a submit inside a record's scope (a hot flush) decodes what the
    // record appended so far, submits with the scope shut, and reopens it on the next command
    // buffer: the rest of the record stays encoded. Step 3 makes this a Submit command.
    const bool sotc_reopen = sotc_cmd.open;
    if (sotc_reopen) {
        sotc_cmd.FlushSubmit();
    }
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndSession();

    std::vector<vk::CommandBuffer> cmd_buffers;
    cmd_buffers.reserve(sessions.size() * 2);

    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
    }
    sessions.clear();

    // shadow-pc (session 12, SOTC_GPU_STATS): timestamps around the whole submission, in two
    // small command buffers of their own (gpu/src/gpu_timer.cpp).
    int timer_slot = -1;
    if (SotcDebug::StatsEnabled()) [[unlikely]] {
        SotcDebug::Count(SotcDebug::Counter::Submit);
        const vk::CommandBufferBeginInfo timer_begin = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        const vk::CommandBuffer start = command_pool.Commit();
        Check(start.begin(timer_begin));
        timer_slot = SotcDebug::GpuTimer::Begin(instance, start, SotcDebug::GpuTimer::Queue::Main);
        Check(start.end());
        if (timer_slot >= 0) {
            const vk::CommandBuffer stop = command_pool.Commit();
            Check(stop.begin(timer_begin));
            SotcDebug::GpuTimer::End(stop, timer_slot);
            Check(stop.end());
            cmd_buffers.insert(cmd_buffers.begin(), start);
            cmd_buffers.push_back(stop);
        }
    }

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    std::array<vk::PipelineStageFlags, std::tuple_size_v<decltype(info.wait_semas)>>
        wait_stage_masks;
    wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
        .pCommandBuffers = cmd_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    vk::Result submit_result;
    {
        SotcWaitTimer::Scope sotc_t{SotcWaitTimer::Submit};
        submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    SotcDebug::GpuTimer::Submitted(timer_slot, timeline, signal_value); // shadow-pc

    work_semaphore.Refresh();
    BeginSession();

    // Apply pending operations
    PopPendingOperations();
    sotc_cmd.open = sotc_reopen; // shadow-pc (session 63)
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const SotcCmdBuffer& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
