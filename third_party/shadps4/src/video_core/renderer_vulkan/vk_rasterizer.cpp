// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <deque>
#include <mutex>
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/platform.h" // shadow-pc (session 45): IRQs signalled by stage B
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h" // shadow-pc (session 45): fence packets
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/sotc_cpu_copy.h" // shadow-pc (session 21)
#include "video_core/renderer_vulkan/sotc_gpu_pipe.h" // shadow-pc (session 43)
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include "sotc_debug.h" // shadow-pc: gpu/src/debug_dump.cpp
#include <chrono>
#include <unordered_set>

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

// shadow-pc (session 43): the two-stage GPU pipeline (docs/next/gpu-pipe.md). Build step 1:
// stage A (Draw/DrawIndirect/DispatchDirect/DispatchIndirect as Liverpool calls them) tests the
// registers, selects the pipeline and writes a packet (the register blocks written since the
// last packet, the CB/DB extent hints, the compute registers, the pipeline and a snapshot of
// each stage's user data); stage B (SotcPipeRun) replays the delta into its own register copy
// and runs the recording half (Record*, RunDrawFilter) with the snapshots installed.
// SOTC_GPU_PIPE=sync runs B at once on the same thread, after each packet.
// Session 44 (build step 2): SOTC_GPU_PIPE=1 runs B on its own thread (sotc:GpuRecord,
// SotcPipeThread). Every other call from Liverpool's thread into B's state drains first
// (SotcPipeDrain: Liverpool waits until B has run every packet), and so does a fault on
// Liverpool's thread, which is then handled inline as before (SotcFaultOnGpuSide).
// SOTC_GPU_PIPE_VERIFY=N: every Nth packet also carries Liverpool's whole register file; B
// compares its delta-built copy with it and logs the words that differ.
// Session 45 (build step 3): fences (EOP, EOS, ReleaseMem: a copy of the PM4 packet), submit-done,
// idle and the flip IRQ are packets too, so B signals them after recording what precedes them
// and A doesn't drain. A notes each fence's label write (SotcPendingFences) and decides a
// WaitRegMem on such a label from it (SotcPipeWaitMet).
// Session 47 (build step 4): the CPU copy (SOTC_CPU_COPY) is computed on A and committed by B
// in stream order (SotcCpuCopyCompute/Commit); until B has run the commit, A's reads of the
// destination (the walker's tables, windows, emulated loads, the next copy's inputs) see the
// values in A's overlay (SotcPendingRanges). A also notes every packet's written buffer
// bindings there: an A read of such a range waits for B (drain reason "pending"). With
// SOTC_GPU_PIPE_VERIFY, B re-runs the SRT walker for the stages of every Nth packet and
// compares the flattened user data with A's (SotcPipeVerifyWalk).
enum class SotcPipeKind : u32 {
    Draw, DrawIndirect, DispatchDirect, DispatchIndirect, Fence, Event, Stop
};

struct Rasterizer::SotcPipeArgs {
    SotcPipeKind kind;
    bool is_indexed = false;
    u32 index_offset = 0;
    VAddr arg_address = 0;
    u32 offset = 0;
    u32 stride = 0;
    u32 max_count = 0;
    VAddr count_address = 0;
    u16 vertex_sgpr_offset = 0;
    u16 instance_sgpr_offset = 0;
    u32 size = 0;
    bool verify = false; ///< the full register file follows the packet (SOTC_GPU_PIPE_VERIFY)
    SotcDrawFilter filter{};

    bool IsCompute() const {
        return kind == SotcPipeKind::DispatchDirect || kind == SotcPipeKind::DispatchIndirect;
    }
};

struct Rasterizer::SotcPipe {
    static constexpr size_t RingBytes = 16_MB;

    explicit SotcPipe(size_t depth, u32 verify_every_, bool threaded_)
        : ring{RingBytes, depth}, verify_every{verify_every_}, threaded{threaded_} {}

    SotcPacketRing ring;
    const u32 verify_every;
    const bool threaded;               ///< SOTC_GPU_PIPE=1: B is record_thread
    SotcPacketWriter writer;           ///< A
    std::vector<u32> scratch;          ///< A
    u64 submitted = 0;                 ///< A: packets pushed
    AmdGpu::Regs regs{};               ///< B's register copy
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent{};
    AmdGpu::CbDbExtent db_extent{};
    AmdGpu::ComputeProgram cs{};
    u64 packets = 0;                   ///< B
    u64 verify_diffs = 0;              ///< B
    SotcPendingFences pending_fences;  ///< A: label writes of fences B hasn't run

    // Drains (threaded). B stores `retired` (packets run) when it finds the ring empty, before
    // it sleeps; a draining A announces itself in drain_waiting (Dekker pair, seq_cst).
    alignas(64) std::atomic<u64> retired{0};
    std::atomic<bool> drain_waiting{false};
    /// B: packets run (release, after each one: its label write, its CPU-copy commit). Session
    /// 47: every packet (was fence_retired, after fences only).
    std::atomic<u64> progress{0};
    // Session 47, A: pending GPU writes and CPU-copy overlays (step 4), and how far A treats
    // B as done (progress, or `submitted` after a wait).
    SotcPendingRanges pending;
    u64 a_view = 0;
    u64 overlay_copies = 0;  ///< CPU copies computed on A
    u64 overlay_bytes = 0;
    u64 overlay_reads = 0;   ///< A reads served from an overlay
    u64 copy_fallbacks = 0;  ///< copy-kernel dispatches A left to B (then waits)
    std::atomic<u64> verify_walks{0};      ///< B: stages re-walked (SOTC_GPU_PIPE_VERIFY)
    std::atomic<u64> verify_walk_diffs{0}; ///< B: of them, with a flattened dword that differs
    std::atomic<u64> b_idle_ns{0}; ///< B: time asleep on an empty ring

    // Session 46: B's command queue (Liverpool::QueueCommand: guest-fault downloads, A's own
    // faults, flips, video-out registration), run between packets. B holds state_mutex while
    // awake; a drain's owner guard holds it on A (own_depth: A only, nested guards).
    std::mutex state_mutex;
    std::mutex command_mutex;
    std::deque<Common::UniqueFunction<void>> commands; ///< under command_mutex
    std::atomic<u32> num_commands{0};
    u32 own_depth = 0;              ///< A
    std::atomic<u64> commands_run{0}; ///< B
    u64 a_faults = 0;               ///< A: own faults queued to B
    u64 a_fault_ns = 0;             ///< A: time they took
    u64 report_commands = 0;
    // A's counters for the 10 s report (SotcPipeReport).
    static constexpr size_t NumReasons = size_t(SotcDrainReason::Count);
    std::array<u64, NumReasons> drains{};      ///< calls
    std::array<u64, NumReasons> drain_waits{}; ///< calls that found B busy
    std::array<u64, NumReasons> drain_ns{};    ///< time A waited
    u64 fences = 0;    ///< fence packets
    u64 events = 0;    ///< submit-done, idle and flip packets
    u64 waits_met = 0; ///< WaitRegMem met by a pending fence write
    u64 report_packets = 0;
    u64 report_b_idle_ns = 0;
    std::chrono::steady_clock::time_point report_start = std::chrono::steady_clock::now();

    std::thread record_thread; ///< B; joined in ~Rasterizer
};

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    if (const char* mode = std::getenv("SOTC_GPU_PIPE"); mode && *mode && std::strcmp(mode, "0")) {
        // shadow-pc (sessions 43-44): sync (build step 1) or 1 (stage B thread, step 2).
        const char* depth_env = std::getenv("SOTC_GPU_PIPE_DEPTH");
        const char* verify_env = std::getenv("SOTC_GPU_PIPE_VERIFY");
        const size_t depth = depth_env ? std::max(1, std::atoi(depth_env)) : 1024;
        const u32 verify = verify_env ? static_cast<u32>(std::max(0, std::atoi(verify_env))) : 0;
        const bool sync = std::strcmp(mode, "sync") == 0;
        if (sync || std::strcmp(mode, "1") == 0) {
            sotc_pipe = std::make_unique<SotcPipe>(depth, verify, !sync);
            if (!sync) {
                sotc_pipe->record_thread = std::thread{[this] { SotcPipeThread(); }};
            }
            LOG_INFO(Render_Vulkan, "SOTC_GPU_PIPE {} (depth {}, verify every {} packets)",
                     sync ? "sync" : "1 (stage B thread)", depth, verify);
        } else {
            LOG_WARNING(Render_Vulkan, "SOTC_GPU_PIPE={}: only 1, 'sync' and 0 exist; off", mode);
        }
    }
    memory->SetRasterizer(this);

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SotcPrefetchAtBatchEnd(); // shadow-pc (session 13): readback prefetch
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() {
    // shadow-pc (session 44): stop stage B. Liverpool's thread is idle by now (sotcgpu_shutdown
    // checks), so this thread may act as the producer.
    if (sotc_pipe && sotc_pipe->record_thread.joinable()) {
        sotc_pipe->writer.Clear();
        sotc_pipe->writer.Put(SotcPipeArgs{.kind = SotcPipeKind::Stop});
        sotc_pipe->ring.Push(sotc_pipe->writer.Bytes());
        sotc_pipe->record_thread.join();
    }
}

// shadow-pc (session 43): FilterDraw split into the register test (ClassifyDraw, stage A with
// SOTC_GPU_PIPE on) and the special passes it starts (RunDrawFilter, stage B).
Rasterizer::SotcDrawFilter Rasterizer::ClassifyDraw(const AmdGpu::Regs& regs) {
    using Kind = SotcDrawFilter::Kind;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        return {Kind::FastClear};
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        return {Kind::FmaskDecompress};
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        return {Kind::Resolve};
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return {Kind::PrimNone};
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        return {Kind::DsCopy, depth_copy, stencil_copy};
    }
    return {};
}

void Rasterizer::RunDrawFilter(const SotcDrawFilter& filter) {
    using Kind = SotcDrawFilter::Kind;
    switch (filter.kind) {
    case Kind::FastClear:
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        break;
    case Kind::FmaskDecompress:
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        EliminateFastClear();
        break;
    case Kind::Resolve:
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        break;
    case Kind::PrimNone:
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        break;
    case Kind::DsCopy:
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(filter.depth_copy, filter.stencil_copy);
        break;
    case Kind::Draw:
        break;
    }
}

bool Rasterizer::FilterDraw() {
    const auto filter = ClassifyDraw(Regs());
    if (filter.kind == SotcDrawFilter::Kind::Draw) {
        return true;
    }
    RunDrawFilter(filter);
    return false;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    SOTC_ZONE("PrepareRenderState");
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = CbExtent(cb);
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = DbExtent();
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.image_id = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = Regs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, CbExtent(0));
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;
    if (sotc_pipe) {
        SotcPipeSubmit({.kind = SotcPipeKind::Draw, .is_indexed = is_indexed,
                        .index_offset = index_offset});
        return;
    }

    {
        SOTC_ZONE_N(pop_zone, "PopPendingOperations");
        scheduler.PopPendingOperations();
    }

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline || SotcSkipPipeline(pipeline)) { // shadow-pc: SOTC_GPU_SKIP
        return;
    }
    RecordDraw(pipeline, is_indexed, index_offset);
}

void Rasterizer::RecordDraw(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset) {
    const auto& regs = Regs();
    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    SOTC_ZONE_N(ondraw_zone, "SotcDebug::OnDraw + rest");
    SotcDebug::OnDraw(false, texture_cache, cb_descs, state.num_color_attachments,
                      db_desc); // shadow-pc: SOTC_GPU_STATS / SOTC_DUMP_RT

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(index_offset);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    {
        SOTC_ZONE_N(push_zone, "Descriptor push");
        pipeline->BindResources(set_writes, push_data);
    }
    UpdateDynamicState(pipeline, is_indexed);
    {
        SOTC_ZONE_N(begin_zone, "Scheduler::BeginRendering");
        scheduler.BeginRendering(state);
    }

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    SOTC_ZONE_N(cmd_zone, "vkCmd bindPipeline + draw");
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    DebugState.IncDrawCall();

    ResetBindings(false);
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;
    if (sotc_pipe) {
        SotcPipeSubmit({.kind = SotcPipeKind::DrawIndirect,
                        .is_indexed = is_indexed,
                        .arg_address = arg_address,
                        .offset = offset,
                        .stride = stride,
                        .max_count = max_count,
                        .count_address = count_address,
                        .vertex_sgpr_offset = vertex_sgpr_offset,
                        .instance_sgpr_offset = instance_sgpr_offset});
        return;
    }

    {
        SOTC_ZONE_N(pop_zone, "PopPendingOperations");
        scheduler.PopPendingOperations();
    }

    if (!FilterDraw()) {
        return;
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline || SotcSkipPipeline(pipeline)) { // shadow-pc: SOTC_GPU_SKIP
        return;
    }
    RecordDrawIndirect(pipeline, is_indexed, arg_address, offset, stride, max_count,
                       count_address);
}

void Rasterizer::RecordDrawIndirect(const GraphicsPipeline* pipeline, bool is_indexed,
                                   VAddr arg_address, u32 offset, u32 stride, u32 max_count,
                                   VAddr count_address) {
    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    SOTC_ZONE_N(ondraw_zone, "SotcDebug::OnDraw + rest");
    SotcDebug::OnDraw(true, texture_cache, cb_descs, state.num_color_attachments,
                      db_desc); // shadow-pc: SOTC_GPU_STATS / SOTC_DUMP_RT

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }

    const auto size = stride * max_count;
    const auto [buffer, base] = buffer_cache.ObtainBuffer(arg_address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);
    bound_buffers.emplace_back(buffer, base, size, vk::AccessFlagBits2::eIndirectCommandRead);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
        bound_buffers.emplace_back(count_buffer, count_offset, 4,
                                   vk::AccessFlagBits2::eIndirectCommandRead);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    {
        SOTC_ZONE_N(push_zone, "Descriptor push");
        pipeline->BindResources(set_writes, push_data);
    }
    UpdateDynamicState(pipeline, is_indexed);
    {
        SOTC_ZONE_N(begin_zone, "Scheduler::BeginRendering");
        scheduler.BeginRendering(state);
    }

    SOTC_ZONE_N(cmd_zone, "vkCmd bindPipeline + draw");
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_offset, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_offset,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }

    ResetBindings(false);
}

// shadow-pc (session 21): SOTC_CPU_COPY runs SotC's copy kernel on the CPU too (on by default
// since session 24; =0 off). The black boots with a cold pipeline cache (bad SRT pointers at
// ~2 s after cs 0xa402ddac compiles) happen with the copy off as well, see docs/NEXT.md.
// =2 (check) writes the CPU result but still marks the binding GPU-written, and DownloadMemory
// compares downloads with it (SOTC_GPU_STATS "check: ... equal/differ").
static int SotcCpuCopyMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_CPU_COPY");
        const int m = v && *v == '0' ? 0 : v && *v == '2' ? 2 : 1;
        LOG_INFO(Render_Vulkan, "SOTC_CPU_COPY {}", m == 0 ? "off" : m == 2 ? "check" : "on");
        return m;
    }();
    return mode;
}

// shadow-pc (session 22, debug): SOTC_CPU_COPY_LOG=1 logs every copy-kernel dispatch in the
// first 8 s after the first one (=N: from N s after it) (outcome, tick, sharps, control words), to match the walker's
// bad-pointer trails (sotcgpu.cpp, ResolveSrtTable) against CPU-copied destinations.
static bool SotcCpuCopyLogOn() {
    static const int from = [] {
        const char* v = std::getenv("SOTC_CPU_COPY_LOG");
        const int n = v ? std::atoi(v) : 0;
        return n == 1 ? 0 : n > 1 ? n : -1;
    }();
    if (from < 0) {
        return false;
    }
    static const auto start = std::chrono::steady_clock::now();
    const auto t = std::chrono::steady_clock::now() - start;
    return t >= std::chrono::seconds(from) && t < std::chrono::seconds(from + 8);
}

static SotcCpuCopy::Sharp SotcCopySharp(const AmdGpu::Buffer& b) {
    return {
        .base = b.base_address,
        .stride = static_cast<u32>(b.stride),
        .num_records = b.num_records,
        .data_format = b.data_format,
        .num_format = b.num_format,
        .dst_sel_x = b.dst_sel_x,
        .swizzle = b.swizzle_enable != 0,
        .add_tid = b.add_tid_enable != 0,
    };
}

void Rasterizer::SotcCpuCopy(const Shader::Info& cs, const AmdGpu::ComputeProgram& cs_program) {
    if (cs.pgm_hash != Vulkan::SotcCpuCopy::ShaderHash || SotcCpuCopyMode() == 0) {
        return;
    }
    thread_local std::vector<u32> words;
    SotcCopyResult r;
    if (SotcCpuCopyCompute(cs, cs_program, r, words)) {
        SotcCpuCopyCommit(r, words);
    }
}

static void SotcCpuCopyReject(Vulkan::SotcCpuCopy::Reject why, u32 extra = 0) {
    SotcDebug::Count(SotcDebug::Counter::CpuCopyRejected);
    static std::array<std::atomic<bool>, 16> logged{};
    if (!logged[static_cast<u32>(why)].exchange(true)) {
        LOG_INFO(Render_Vulkan, "SOTC_CPU_COPY: dispatch left to the GPU, reason {} ({})",
                 static_cast<u32>(why), extra);
    }
}
// Reasons past SotcCpuCopy::Reject, for the log only.
constexpr auto SotcCopyResources = static_cast<Vulkan::SotcCpuCopy::Reject>(12);
constexpr auto SotcCopyImage = static_cast<Vulkan::SotcCpuCopy::Reject>(13);
constexpr auto SotcCopyWriteFailed = static_cast<Vulkan::SotcCpuCopy::Reject>(14);

bool Rasterizer::SotcCpuCopyCompute(const Shader::Info& cs,
                                    const AmdGpu::ComputeProgram& cs_program, SotcCopyResult& out,
                                    std::vector<u32>& dst_words) {
    namespace C = Vulkan::SotcCpuCopy;
    if (cs.pgm_hash != C::ShaderHash || SotcCpuCopyMode() == 0) {
        return false;
    }
    const bool log_on = SotcCpuCopyLogOn();
    u32 words[2]{};
    AmdGpu::Buffer ctl{}, src{}, dst{};
    const auto trace = [&](const char* what, u32 code = 0) {
        if (log_on) {
            LOG_INFO(Render_Vulkan,
                     "cpu-copy-log: {} {} dim {}x{}x{} ctl {:#x} [{:#x} {:#x}] src {:#x} "
                     "n{} dst {:#x} n{}",
                     what, code, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                     u64{ctl.base_address}, words[0], words[1], u64{src.base_address},
                     u32{src.num_records}, u64{dst.base_address}, u32{dst.num_records});
        }
    };
    const auto reject = [&](C::Reject why, u32 extra = 0) {
        trace("reject", static_cast<u32>(why));
        SotcCpuCopyReject(why, extra);
        return false;
    };
    // Handles 0-2 as in the IR (ctl, src, dst); later ones may only be special read-only
    // buffers (the flattened user data).
    const auto extra_ok = [&] {
        for (size_t i = 3; i < cs.buffers.size(); ++i) {
            if (!cs.buffers[i].IsSpecial() || cs.buffers[i].is_written) {
                return false;
            }
        }
        return true;
    };
    if (cs.buffers.size() < 3 || cs.buffers[0].is_written || cs.buffers[1].is_written ||
        !cs.buffers[2].is_written || cs.buffers[0].IsSpecial() || cs.buffers[1].IsSpecial() ||
        cs.buffers[2].IsSpecial() || !extra_ok()) {
        return reject(SotcCopyResources, static_cast<u32>(cs.buffers.size()));
    }
    ctl = cs.buffers[0].GetSharp(cs);
    src = cs.buffers[1].GetSharp(cs);
    dst = cs.buffers[2].GetSharp(cs);
    if (!ctl.Valid() || !src.Valid() || !dst.Valid() || ctl.base_address == 0 ||
        src.base_address == 0 || dst.base_address == 0) {
        return reject(C::Reject::Sharp);
    }
    // Session 47: on stage A, inputs a pending CPU copy writes are read from its overlay (and a
    // pending GPU write waits for B first); SotcPipeOverlay is null elsewhere.
    // The GPU reads what the arena holds at this point in the batch: guest memory, unless the
    // GPU wrote the bytes (or the texel-buffer path copies an image in).
    const auto read_input = [&](VAddr addr, void* to, u64 bytes) {
        if (const u8* o = SotcPipeOverlay(addr, bytes)) {
            std::memcpy(to, o, bytes);
            return true;
        }
        if (buffer_cache.IsRangeGpuWritten(addr, bytes)) {
            return false;
        }
        memory->CopySparseMemory(addr, static_cast<u8*>(to), bytes);
        return true;
    };
    if (!read_input(ctl.base_address, words, sizeof(words))) {
        trace("gpu-written ctl");
        SotcDebug::Count(SotcDebug::Counter::CpuCopyGpuWritten);
        return false;
    }
    const C::Dispatch d{
        .dim_x = cs_program.dim_x,
        .dim_y = cs_program.dim_y,
        .dim_z = cs_program.dim_z,
        .start_x = cs_program.start_x,
        .start_y = cs_program.start_y,
        .start_z = cs_program.start_z,
        .threads_x = cs_program.num_thread_x.full,
        .threads_y = cs_program.num_thread_y.full,
        .threads_z = cs_program.num_thread_z.full,
        .partial_x = cs_program.num_thread_x.partial,
        .partial_y = cs_program.num_thread_y.partial,
        .partial_z = cs_program.num_thread_z.partial,
    };
    const C::Plan plan =
        C::MakePlan(SotcCopySharp(src), SotcCopySharp(dst), ctl.GetSize(), words[0], words[1], d);
    if (plan.reject != C::Reject::None) {
        return reject(plan.reject);
    }
    const u64 src_bytes = u64{plan.src_dwords} * 4;
    thread_local std::vector<u32> src_words;
    src_words.resize(plan.src_dwords);
    if (!read_input(src.base_address, src_words.data(), src_bytes)) {
        trace("gpu-written src");
        SotcDebug::Count(SotcDebug::Counter::CpuCopyGpuWritten);
        return false;
    }
    dst_words.resize(plan.count);
    C::Run(plan, src_words.data(), dst_words.data());
    out = {
        .src = src.base_address,
        .dst = dst.base_address,
        .src_bytes = src_bytes,
        .dst_vsize = dst.GetSize(),
        .count = plan.count,
    };
    trace("computed");
    return true;
}

void Rasterizer::SotcCpuCopyCommit(const SotcCopyResult& r, std::span<const u32> dst_words) {
    const bool log_on = SotcCpuCopyLogOn();
    const u64 dst_bytes = u64{r.count} * 4;
    const auto trace = [&](const char* what, u64 code = 0) {
        if (log_on) {
            LOG_INFO(Render_Vulkan, "cpu-copy-log: {} {} tick {} src {:#x} dst {:#x} n{}", what,
                     code, scheduler.CurrentTick(), r.src, r.dst, r.count);
        }
    };
    // Session 22: the CPU writes only pages a download (or an earlier CPU copy) already made
    // resident: copying into GPU-only buffers no one reads back cost 1.2 GB (session 21). A
    // table's first frame takes the GPU path; the walker's or a guest read downloads it. The
    // other pages stay GPU-written (gpu_only). =2 (check) copies only fully resident ones.
    // Session 47: with SOTC_GPU_PIPE=1 stage A computed the values (its overlay held them
    // until here); the residency split and the image check are B's state, so they run here.
    const auto cov = buffer_cache.SotcResident().Covered(r.dst, dst_bytes);
    if (cov.marked == 0 || (SotcCpuCopyMode() == 2 && !cov.All())) {
        trace("not resident", cov.marked);
        SotcDebug::Count(SotcDebug::Counter::CpuCopyNotResident);
        return;
    }
    if (texture_cache.IsMeta(r.src) || texture_cache.FindImageFromRange(r.src, r.src_bytes)) {
        if (sotc_copy_commit) {
            // A read guest memory where the GPU reads an image: its overlay may have been wrong.
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render_Vulkan, "pipe: cpu copy source became an image ({:#x} +{:#x})",
                            r.src, r.src_bytes);
            }
        }
        SotcCpuCopyReject(SotcCopyImage);
        return;
    }
    if (log_on && dst_bytes >= 1_MB) {
        // Session 22: how much of the destination is in the working set before a big copy
        // (4 KiB pages).
        LOG_INFO(Render_Vulkan, "cpu-copy-log: working set before: dst {}/{}",
                 SotcDebug::WorkingSetPages(r.dst, dst_bytes), dst_bytes >> 12);
    }
    // Resident runs: written here, and older GPU data in them is never downloaded over the
    // result (a destination byte still GPU-written would be). Other runs: left to the GPU.
    thread_local std::vector<std::pair<VAddr, u64>> gpu_only;
    gpu_only.clear();
    bool written = true;
    buffer_cache.SotcResident().ForEachRun(r.dst, dst_bytes, [&](VAddr a, u64 n, bool resident) {
        if (!resident) {
            gpu_only.emplace_back(a, n);
            return;
        }
        const u32* words = dst_words.data() + (a - r.dst) / 4;
        if (!written || !memory->TryWriteBacking(reinterpret_cast<void*>(a), words, n)) {
            written = false;
            gpu_only.emplace_back(a, n);
            return;
        }
        if (SotcCpuCopyMode() == 1) {
            buffer_cache.SotcForgetGpuWrites(a, n);
        }
    });
    if (!written) {
        // Pieces already written hold the values the GPU writes there anyway.
        SotcCpuCopyReject(SotcCopyWriteFailed);
    }
    // BindBuffers obtains the binding with its clamped V# size; the GPU writes [0, count) of it.
    if (SotcCpuCopyMode() == 1) {
        buffer_cache.SotcSetCpuMirroredWrite(r.dst, r.dst_vsize, gpu_only);
        if (!gpu_only.empty()) {
            u64 gpu_bytes = 0;
            for (const auto& [a, n] : gpu_only) {
                gpu_bytes += n;
            }
            SotcDebug::Count(SotcDebug::Counter::CpuCopyPartial);
            SotcDebug::Count(SotcDebug::Counter::CpuCopyGpuOnlyDwords, gpu_bytes / 4);
        }
    } else {
        buffer_cache.SotcCheckCpuCopy(r.dst, dst_words.data(), r.count);
    }
    trace(gpu_only.empty() ? "copied" : "copied in part", cov.marked);
    SotcDebug::Count(SotcDebug::Counter::CpuCopy);
    SotcDebug::Count(SotcDebug::Counter::CpuCopyDwords, r.count);
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;
    if (sotc_pipe) {
        SotcPipeSubmit({.kind = SotcPipeKind::DispatchDirect});
        return;
    }

    {
        SOTC_ZONE_N(pop_zone, "PopPendingOperations");
        scheduler.PopPendingOperations();
    }

    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline || SotcSkipPipeline(pipeline)) { // shadow-pc: SOTC_GPU_SKIP
        return;
    }
    RecordDispatchDirect(pipeline);
}

void Rasterizer::RecordDispatchDirect(const ComputePipeline* pipeline) {
    const auto& cs_program = CsRegs();
    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if (ExecuteShaderHLE(cs, Regs(), cs_program, *this)) {
        return;
    }

    SotcDebug::SetShader(cs.pgm_hash); // shadow-pc: SOTC_GPU_STATS writer attribution
    if (sotc_copy_commit) {            // shadow-pc (session 47): computed on stage A
        SotcCpuCopyCommit(*sotc_copy_commit, sotc_copy_words);
    } else {
        SotcCpuCopy(cs, cs_program);   // shadow-pc (session 21): SOTC_CPU_COPY
    }
    const bool bound = BindResources(pipeline);
    buffer_cache.SotcSetCpuMirroredWrite(0, 0);
    if (!bound && cs.pgm_hash == SotcCpuCopy::ShaderHash && SotcCpuCopyLogOn()) {
        LOG_INFO(Render_Vulkan, "cpu-copy-log: BindResources failed, dispatch skipped");
    }
    if (!bound) {
        SotcDebug::SetShader(0);
        return;
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    {
        SOTC_ZONE_N(end_zone, "Scheduler::EndRendering");
        scheduler.EndRendering();
    }
    {
        SOTC_ZONE_N(push_zone, "Descriptor push");
        pipeline->BindResources(set_writes, push_data);
    }

    SOTC_ZONE_N(cmd_zone, "vkCmd bindPipeline + dispatch");
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    DebugState.IncDispatch();
    SotcDebug::Count(SotcDebug::Counter::Dispatch); // shadow-pc: SOTC_GPU_STATS
    if (buffer_cache.TakeSotcFlushRequest()) {
        // shadow-pc (session 10): it wrote data the CPU will read soon (BufferCache hot pages):
        // submit now, so the read waits for this batch only, on the side queue.
        SOTC_ZONE_N(hot_zone, "Hot flush submit");
        scheduler.Flush();
        SotcDebug::Count(SotcDebug::Counter::HotFlush);
    }
    SotcDebug::SetShader(0);

    ResetBindings(true);
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;
    if (sotc_pipe) {
        SotcPipeSubmit({.kind = SotcPipeKind::DispatchIndirect,
                        .arg_address = address,
                        .offset = offset,
                        .size = size});
        return;
    }

    {
        SOTC_ZONE_N(pop_zone, "PopPendingOperations");
        scheduler.PopPendingOperations();
    }

    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline || SotcSkipPipeline(pipeline)) { // shadow-pc: SOTC_GPU_SKIP
        return;
    }
    RecordDispatchIndirect(pipeline, address, offset, size);
}

void Rasterizer::RecordDispatchIndirect(const ComputePipeline* pipeline, VAddr address,
                                        u32 offset, u32 size) {

    // shadow-pc: SOTC_GPU_STATS writer attribution
    SotcDebug::SetShader(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);
    if (!BindResources(pipeline)) {
        SotcDebug::SetShader(0);
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);
    bound_buffers.emplace_back(buffer, base, size, vk::AccessFlagBits2::eIndirectCommandRead);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    {
        SOTC_ZONE_N(end_zone, "Scheduler::EndRendering");
        scheduler.EndRendering();
    }
    {
        SOTC_ZONE_N(push_zone, "Descriptor push");
        pipeline->BindResources(set_writes, push_data);
    }

    SOTC_ZONE_N(cmd_zone, "vkCmd bindPipeline + dispatch");
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    DebugState.IncDispatch();
    SotcDebug::Count(SotcDebug::Counter::Dispatch); // shadow-pc: SOTC_GPU_STATS
    if (buffer_cache.TakeSotcFlushRequest()) {
        // shadow-pc (session 10): it wrote data the CPU will read soon (BufferCache hot pages):
        // submit now, so the read waits for this batch only, on the side queue.
        SOTC_ZONE_N(hot_zone, "Hot flush submit");
        scheduler.Flush();
        SotcDebug::Count(SotcDebug::Counter::HotFlush);
    }
    SotcDebug::SetShader(0);

    ResetBindings(true);
}

u64 Rasterizer::Flush() {
    const auto own = SotcPipeDrain(SotcDrainReason::Flush);
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    const auto own = SotcPipeDrain(SotcDrainReason::Finish);
    scheduler.Finish();
}

// shadow-pc patch: the texture GC alone, from Liverpool::Process at a frame boundary.
void Rasterizer::SotcGcTick() {
    const auto own = SotcPipeDrain(SotcDrainReason::Flip);
    texture_cache.RunGarbageCollector();
    // Session 23: the staging pool's frame (its trims) too; from OnSubmit it stopped in gameplay
    // and the ring kept its loading peak (~1.25 GB of host memory). SOTC_STAGING_RING=0: upstream.
    runtime.SotcFlipTick();
    buffer_cache.SotcFlipTick(); // session 33: the hot lines' clock
}

void Rasterizer::OnSubmit() {
    const auto own = SotcPipeDrain(SotcDrainReason::Submit);
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame(); // shadow-pc (session 23): a no-op unless SOTC_STAGING_RING=0
}

void Rasterizer::OnFence() {
    // shadow-pc (session 44): fences are signalled after this, so the guest sees "done" only
    // once stage B has recorded everything before them. Session 45: EOP, EOS and ReleaseMem run
    // on B (no drain there); WriteData still drains here.
    const auto own = SotcPipeDrain(SotcDrainReason::Fence);
    texture_cache.ProcessDownloadImages();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    SOTC_ZONE("Rasterizer::BindResources");
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(Regs());
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline) {
    SOTC_ZONE("BindVertexBuffers");
    const auto& regs = Regs();
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                              regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs.
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.setVertexInputEXT(bindings, attributes);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
        bound_buffers.emplace_back(range.buffer, range.offset, size,
                                   vk::AccessFlagBits2::eVertexAttributeRead);
    }

    // Bind vertex buffers
    VertexInputs<vk::Buffer> host_buffers;
    VertexInputs<vk::DeviceSize> host_offsets;
    VertexInputs<vk::DeviceSize> host_sizes;
    VertexInputs<vk::DeviceSize> host_strides;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    SOTC_ZONE("BindIndexBuffer");
    const auto& regs = Regs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    bound_buffers.emplace_back(buffer, offset, index_buffer_size, vk::AccessFlagBits2::eIndexRead);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindIndexBuffer(buffer->Handle(), offset, index_type);
}

void Rasterizer::ResetBindings(bool is_compute) {
    SOTC_ZONE("ResetBindings");
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, src_access] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        runtime.AccessBuffer(buffer, offset, size, dst_stage, src_access);
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() < 2 || info.buffers.size() > 3 ||
        !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }
    if (info.buffers.size() == 3 && info.buffers[2].buffer_type != Shader::BufferType::Flatbuf) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() < 2 || info.buffers.size() > 3 ||
        !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }
    if (info.buffers.size() == 3 && info.buffers[2].buffer_type != Shader::BufferType::Flatbuf) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

// shadow-pc (session 10): the walker left GPU-written constants as their stale CPU copy in the
// flattened buffer (SotcSrt::PatchableTable); copy the GPU's values over them from the arena, in
// command order, before the draw reads the buffer. Contiguous dwords are copied as one region.
void Rasterizer::SotcPatchFlatBuf(const Shader::Info& stage, VideoCore::Buffer& flat_buffer,
                                  u64 flat_offset) {
    const auto live = stage.FlatPatches(); // shadow-pc (session 43): snapshot-aware
    boost::container::small_vector<std::pair<u16, u64>, 16> patches(live.begin(), live.end());
    std::ranges::sort(patches);
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const VideoCore::Buffer* arena = nullptr;
    const auto flush = [&] {
        if (!copies.empty()) {
            runtime.CopyBuffer(arena, &flat_buffer, copies);
            copies.clear();
        }
    };
    for (const auto& [dst_dw, src] : patches) {
        const auto [buffer, src_offset] = buffer_cache.ObtainBuffer(src, sizeof(u32), false);
        if (buffer != arena) {
            flush();
            arena = buffer;
        }
        const u64 dst_offset = flat_offset + u64{dst_dw} * sizeof(u32);
        if (!copies.empty() && copies.back().srcOffset + copies.back().size == src_offset &&
            copies.back().dstOffset + copies.back().size == dst_offset) {
            copies.back().size += sizeof(u32);
        } else {
            copies.push_back({.srcOffset = src_offset, .dstOffset = dst_offset, .size = 4});
        }
    }
    flush();
    // The draw reads the patched dwords: FlushBarriers before it orders it after the copies.
    needs_barrier = true;
    SotcDebug::Count(SotcDebug::Counter::SrtPatchedDwords, patches.size());
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    SOTC_ZONE("BindBuffers");
    const u64 alignment = instance.StorageMinAlignment();
    for (const auto& desc : stage.buffers) {
        const auto src_access =
            vk::AccessFlagBits2::eShaderRead |
            (desc.is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone);
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |=
                    runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), src_access);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const auto flat = stage.FlatUd(); // shadow-pc (session 43): snapshot-aware
                const u32 ubo_size = flat.size() * sizeof(u32);
                SOTC_ZONE_N(flat_zone, "Flatbuf copy");
                const u64 offset = vk_buffer.Copy(flat.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                if (!stage.FlatPatches().empty()) {
                    SotcPatchFlatBuf(stage, vk_buffer, offset);
                }
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (Regs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = Regs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = CsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp = desc.GetSharp(stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                if (size != vsharp.GetSize()) {
                    // shadow-pc: once per shader. SotC binds "unbounded" buffers (~4 GiB) every
                    // frame; logging each bind wrote ~1,300 lines/s from the GPU thread.
                    static std::unordered_set<u64> clamped_logged;
                    if (clamped_logged.insert(stage.pgm_hash).second) {
                        LOG_WARNING(Render,
                                    "Clamped size from {} to {} for stage {:#x} (logged once "
                                    "per shader)",
                                    vsharp.GetSize(), size, stage.pgm_hash);
                    }
                }
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, src_access);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                }
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, desc.is_written);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

// shadow-pc patch (session 40): see SotcTexMemo in vk_rasterizer.h.
static bool SotcTexMemoOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_TEX_MEMO");
        const bool enabled = !(v && *v == '0');
        LOG_INFO(Render_Vulkan, "texture-set memo {}", enabled ? "on" : "off (SOTC_TEX_MEMO=0)");
        return enabled;
    }();
    return on;
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    SOTC_ZONE("BindTextures");
    const u32 first_image_idx = image_infos.size();
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    u32 num_images{};

    // shadow-pc patch (session 40): the texture-set memo (SotcTexMemo in vk_rasterizer.h).
    const bool memo_on = SotcTexMemoOn();
    SotcTexMemo* memo = nullptr;
    boost::container::static_vector<AmdGpu::Image, Shader::NUM_IMAGES> sharps;
    if (memo_on) {
        const auto slot = (reinterpret_cast<uintptr_t>(&stage) >> 4) * 0x9E3779B97F4A7C15ull >>
                          (64 - SotcTexMemoBits);
        memo = &sotc_tex_memo[slot];
        for (const auto& image_desc : stage.images) {
            sharps.push_back(image_desc.GetSharp(stage));
        }
        if (((sotc_tex_hits + sotc_tex_misses) & 0xFFFFF) == 0xFFFFF) {
            LOG_INFO(Render_Vulkan, "texture-set memo: {} hits, {} misses", sotc_tex_hits,
                     sotc_tex_misses);
            sotc_tex_hits = sotc_tex_misses = 0;
        }
        if (memo->stage == &stage && memo->pgm_hash == stage.pgm_hash &&
            memo->generation == texture_cache.SotcFindState().first &&
            memo->sharps.size() == sharps.size() &&
            std::memcmp(memo->sharps.data(), sharps.data(),
                        sharps.size() * sizeof(AmdGpu::Image)) == 0) {
            ++sotc_tex_hits;
            num_images = static_cast<u32>(memo->bindings.size());
            for (u32 i = 0; i < num_images; ++i) {
                auto& [image_id, desc] = image_bindings[i];
                image_id = memo->bindings[i].image_id;
                desc = memo->bindings[i].desc;
                if (!image_id) {
                    continue;
                }
                texture_cache.SotcTouchFound(image_id);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                if (image->binding.is_bound) {
                    image->binding.force_general |= memo->written[i] != 0;
                }
                image->binding.is_bound = 1u;
            }
            image_descriptor_array_sizes = memo->array_sizes;
            goto sotc_second_pass;
        }
        ++sotc_tex_misses;
    }
    { // the full first pass (scoped: the memo hit jumps past it)
        const auto find_state = texture_cache.SotcFindState();
        bool memo_store = memo_on;
        std::array<VideoCore::ImageId, Shader::NUM_IMAGES> found_ids{};
        for (const auto& image_desc : stage.images) {
            const auto tsharp = image_desc.GetSharp(stage);
            const auto data_fmt = tsharp.GetDataFmt();
            const auto num_fmt = tsharp.GetNumberFmt();
            if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
                image_bindings[num_images++].image_id = {};
                image_descriptor_array_sizes.push_back(1);
                continue;
            }

            if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
                !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
                LOG_WARNING(Render_Vulkan,
                            "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                            "data_format={}, num_format={}",
                            tsharp.Address(), tsharp.pitch, tsharp.width,
                            static_cast<u32>(data_fmt), static_cast<u32>(num_fmt));
                memo_store = false; // a later mapping can make the address valid
                image_bindings[num_images++].image_id = {};
                image_descriptor_array_sizes.push_back(1);
                continue;
            }

            const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
            const u32 num_bindings = image_desc.NumBindings(stage);

            for (auto i = 0; i < num_bindings; i++) {
                auto& [image_id, desc] = image_bindings[num_images++];
                std::construct_at(&desc, tsharp, image_desc);

                if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                    ASSERT(num_bindings == 1);
                    desc.view_info.range.base.level += image_desc.constant_mip_index;
                    desc.view_info.range.extent.levels = 1;
                } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                    desc.view_info.range.base.level += i;
                    desc.view_info.range.extent.levels = 1;
                }

                image_id = texture_cache.FindImage(desc);
                found_ids[num_images - 1] = image_id;
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    // If this image has an associated depth image, it's a stencil attachment.
                    // Redirect the access to the actual depth-stencil buffer.
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                if (image->binding.is_bound) {
                    // The image is already bound. In case if it is about to be used as storage we
                    // need to force general layout on it.
                    image->binding.force_general |= image_desc.is_written;
                }
                image->binding.is_bound = 1u;
            }

            image_descriptor_array_sizes.push_back(num_bindings);
        }

        if (memo_store && texture_cache.SotcFindState() == find_state) {
            memo->stage = &stage;
            memo->pgm_hash = stage.pgm_hash;
            memo->generation = find_state.first;
            memo->sharps.assign(sharps.begin(), sharps.end());
            memo->bindings.resize(num_images);
            memo->written.resize(num_images);
            u32 b = 0;
            for (u32 d = 0; d < image_descriptor_array_sizes.size(); ++d) {
                for (u32 k = 0; k < image_descriptor_array_sizes[d]; ++k, ++b) {
                    memo->bindings[b].image_id = found_ids[b];
                    memo->bindings[b].desc = image_bindings[b].desc;
                    memo->written[b] = stage.images[d].is_written;
                }
            }
            memo->array_sizes = image_descriptor_array_sizes;
        } else if (memo_on) {
            memo->stage = nullptr;
        }
    }

sotc_second_pass:
    // Second pass to re-bind images that were updated after binding
    for (u32 i = 0; i < num_images; ++i) {
        auto& [image_id, desc] = image_bindings[i];
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            // shadow-pc patch (session 16): guest view levels to host levels for a downscaled
            // texture (SOTC_TEX_DOWNSCALE); used by the view and the transition below.
            texture_cache.SotcMapView(image, desc.view_info);
            auto& image_view = texture_cache.FindTexture(image_id, desc);
            const auto binding = image.binding;

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                }
            } else {
                if (is_storage) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    needs_barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                Regs().ta_bc_base.Address() == 0)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                        "border_color_type={}, border_color_base={:#x}",
                        static_cast<u32>(ssharp.max_aniso.Value()),
                        static_cast<u32>(ssharp.filter_mode.Value()),
                        static_cast<u32>(ssharp.mip_filter.Value()),
                        static_cast<u32>(ssharp.border_color_type.Value()),
                        Regs().ta_bc_base.Address());
            ssharp = AmdGpu::Sampler{};
        }
        SOTC_ZONE_N(sampler_zone, "GetSampler");
        const auto vk_sampler =
            texture_cache.GetSampler(ssharp, Regs().ta_bc_base, sampler.is_depth);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

// shadow-pc patch (session 38): the render-state memo, see SotcBeginMemo in vk_rasterizer.h.
// After bbport's RenderStateMemo (bloodborne_pc, docs/parallel_gpu.md).
Rasterizer::SotcBeginSignature Rasterizer::SotcMakeBeginSignature(
    const GraphicsPipeline* pipeline) const {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    SotcBeginSignature sig;
    const u32 num_color = std::bit_width(key.mrt_mask);
    for (u32 cb = 0; cb < num_color; ++cb) {
        const auto& [image_id, desc] = cb_descs[cb];
        sig.ids[cb] = image_id;
        if (image_id) {
            sig.views[cb] = desc.view_info;
            sig.meta_addresses[cb] = regs.color_buffers[cb].CmaskAddress();
        }
        sig.color_samples[cb] = key.color_samples[cb];
    }
    constexpr u32 db = AmdGpu::NUM_COLOR_BUFFERS;
    sig.ids[db] = db_desc.image_id;
    if (db_desc.image_id) {
        sig.views[db] = db_desc.desc.view_info;
        sig.meta_addresses[db] = regs.depth_htile_data_base.GetAddress();
    }
    sig.mrt_mask = key.mrt_mask;
    std::memcpy(&sig.depth_control, &regs.depth_control, sizeof(sig.depth_control));
    sig.depth_valid = regs.depth_buffer.DepthValid();
    sig.stencil_valid = regs.depth_buffer.StencilValid();
    sig.epoch = texture_cache.SotcRenderStateEpoch();
    return sig;
}

static bool SotcRenderStateMemoOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_RS_MEMO");
        const bool enabled = !(v && *v == '0');
        LOG_INFO(Render_Vulkan, "render-state memo {}", enabled ? "on" : "off (SOTC_RS_MEMO=0)");
        return enabled;
    }();
    return on;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    SOTC_ZONE("Rasterizer::BeginRendering");
    if (!SotcRenderStateMemoOn()) {
        return BeginRenderingFull(pipeline);
    }
    if (((sotc_begin_hits + sotc_begin_misses) & 0xFFFFF) == 0xFFFFF) {
        LOG_INFO(Render_Vulkan, "render-state memo: {} hits, {} misses", sotc_begin_hits,
                 sotc_begin_misses);
        sotc_begin_hits = sotc_begin_misses = 0;
    }
    auto& memo = sotc_begin_memo;
    const auto& regs = Regs();
    if (memo.valid && scheduler.IsRenderingWith(memo.state) &&
        !regs.depth_render_control.depth_clear_enable &&
        !regs.depth_render_control.stencil_clear_enable &&
        SotcMakeBeginSignature(pipeline) == memo.signature) {
        // The targets must still be the images remembered: not sampled by this draw (that needs
        // the feedback-loop layout), not re-created, the same backing, and not CPU-modified
        // (the full path would upload them, which ends the pass).
        bool usable = true;
        for (u32 i = 0; i < memo.signature.ids.size() && usable; ++i) {
            const auto id = memo.signature.ids[i];
            if (!id) {
                continue;
            }
            const auto& image = texture_cache.GetImage(id);
            usable = !image.binding.is_bound && !image.binding.needs_rebind &&
                     image.backing == memo.backings[i] &&
                     False(image.flags & VideoCore::ImageFlagBits::Dirty);
        }
        if (usable) {
            ++sotc_begin_hits;
            attachment_feedback_loop = false;
            for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
                if (const auto id = memo.signature.ids[cb]; id) {
                    texture_cache.UpdateImage(id); // LRU touch and tracking; clean, so no upload
                }
            }
            return memo.state;
        }
    }
    ++sotc_begin_misses;
    memo.valid = false;
    const RenderState state = BeginRenderingFull(pipeline);
    if (attachment_feedback_loop) {
        return state;
    }
    // States with clears are not remembered: the next draw's state differs (no clear).
    bool clears =
        state.depth_stencil_attachment.depth_clear || state.depth_stencil_attachment.stencil_clear;
    for (u32 i = 0; i < state.num_color_attachments; ++i) {
        clears |= state.color_attachments[i].is_clear != 0;
    }
    if (!clears) {
        // After the full path: it may have re-found a target (needs_rebind) and registered images.
        memo.signature = SotcMakeBeginSignature(pipeline);
        memo.state = state;
        for (u32 i = 0; i < memo.signature.ids.size(); ++i) {
            const auto id = memo.signature.ids[i];
            memo.backings[i] = id ? texture_cache.GetImage(id).backing : nullptr;
        }
        memo.valid = true;
    }
    return state;
}

RenderState Rasterizer::BeginRenderingFull(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = Regs();
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.image_id; image_id) {
        auto& desc = db_desc.desc;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = CbExtent(0);
    const auto& mrt1_hint = CbExtent(1);
    VideoCore::TextureCache::ImageDesc mrt0_desc{Regs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{Regs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 Regs().color_buffers[0].Address(),
                                 Regs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = Regs();

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = Regs().depth_view.slice_start;
    sub_range.extent.layers = Regs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    const auto own = SotcPipeDrain(SotcDrainReason::Dma);
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    const auto own = SotcPipeDrain(SotcDrainReason::Dma);
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    const auto own = SotcPipeDrain(SotcDrainReason::Gds);
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size, false, assume_locks);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    SOTC_ZONE("UpdateDynamicState");
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = Regs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto own = SotcPipeDrain(SotcDrainReason::Marker);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto own = SotcPipeDrain(SotcDrainReason::Marker);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto own = SotcPipeDrain(SotcDrainReason::Marker);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto own = SotcPipeDrain(SotcDrainReason::Marker);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#ifdef __linux__
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

// shadow-pc (session 43): the two-stage GPU pipeline, see SotcPipeArgs above.
const AmdGpu::Regs& Rasterizer::Regs() const {
    return sotc_pipe ? sotc_pipe->regs : liverpool->regs;
}

const AmdGpu::CbDbExtent& Rasterizer::CbExtent(u32 cb) const {
    return sotc_pipe ? sotc_pipe->cb_extent[cb] : liverpool->last_cb_extent[cb];
}

const AmdGpu::CbDbExtent& Rasterizer::DbExtent() const {
    return sotc_pipe ? sotc_pipe->db_extent : liverpool->last_db_extent;
}

const AmdGpu::ComputeProgram& Rasterizer::CsRegs() const {
    return sotc_pipe ? sotc_pipe->cs : liverpool->GetCsRegs();
}

// Stage A: what Draw/Dispatch did before recording, then the packet.
void Rasterizer::SotcPipeSubmit(SotcPipeArgs args) {
    SOTC_ZONE("SotcPipeSubmit");
    auto& pipe = *sotc_pipe;
    if (pipe.threaded) {
        // Session 47: forget what B has run. Only here, between walks: a walker may still
        // read an overlay SotcPipeOverlay handed out earlier in this packet's selection.
        pipe.a_view = std::max(pipe.a_view, pipe.progress.load(std::memory_order_acquire));
        if ((pipe.submitted & 15) == 0 && !pipe.pending.Empty()) {
            pipe.pending.Retire(pipe.a_view);
        }
    }
    const Pipeline* pipeline = nullptr;
    if (args.IsCompute()) {
        const ComputePipeline* cp = pipeline_cache.GetComputePipeline();
        if (!cp || SotcSkipPipeline(cp)) { // shadow-pc: SOTC_GPU_SKIP
            return;
        }
        pipeline = cp;
    } else {
        args.filter = ClassifyDraw(liverpool->regs);
        if (args.filter.kind == SotcDrawFilter::Kind::Draw) {
            const GraphicsPipeline* gp =
                args.kind == SotcPipeKind::DrawIndirect
                    ? pipeline_cache.GetGraphicsPipeline({
                          .vertex_sgpr_offset = args.vertex_sgpr_offset,
                          .instance_sgpr_offset = args.instance_sgpr_offset,
                      })
                    : pipeline_cache.GetGraphicsPipeline();
            if (!gp || SotcSkipPipeline(gp)) { // shadow-pc: SOTC_GPU_SKIP
                return;
            }
            pipeline = gp;
        }
    }

    // Session 44: the CPU copy's dispatch writes guest memory on B that the SRT walker here
    // reads within a few packets (gpu-pipe.md, rule 2). Session 47: A computes it (B commits)
    // and reads the destination from its overlay meanwhile.
    const bool cpu_copy =
        args.IsCompute() &&
        static_cast<const ComputePipeline*>(pipeline)->GetStage(Shader::SwStage::Compute).pgm_hash ==
            SotcCpuCopy::ShaderHash;
    const u64 seq = pipe.submitted + 1;
    SotcCopyResult copy{};
    thread_local std::vector<u32> copy_words;
    bool copied = false;
    if (cpu_copy && pipe.threaded && args.kind == SotcPipeKind::DispatchDirect) {
        const auto& cs =
            static_cast<const ComputePipeline*>(pipeline)->GetStage(Shader::SwStage::Compute);
        copied = SotcCpuCopyCompute(cs, liverpool->GetCsRegs(), copy, copy_words) &&
                 copy_words.size() * sizeof(u32) <= 4_MB;
    }
    if (pipe.threaded && pipeline) {
        SotcPipeNoteWrites(pipeline, seq, copied ? copy.dst : 0);
    }
    args.verify = pipe.verify_every && (pipe.submitted + 1) % pipe.verify_every == 0;

    auto& w = pipe.writer;
    w.Clear();
    w.Put(args);
    SotcEncodeRegDelta(liverpool->sotc_dirty, liverpool->regs.reg_array.data(), w, pipe.scratch);
    w.Put(liverpool->last_cb_extent);
    w.Put(liverpool->last_db_extent);
    if (args.IsCompute()) {
        w.Put(liverpool->GetCsRegs());
    }
    w.Put(pipeline);
    if (pipeline) {
        u64 num_stages = 0;
        for (const auto* stage : pipeline->GetStages()) {
            num_stages += stage != nullptr;
        }
        w.Put(num_stages);
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            const auto snap = stage->MakeSnapshot();
            w.Put(snap.info);
            w.Put(snap.pgm_base);
            w.PutSpan(snap.user_data);
            w.PutSpan(snap.flat);
            // std::pair is not trivially copyable (its assignment), but its bytes are.
            w.Put<u64>(snap.patches.size());
            w.PutRaw(snap.patches.data(), snap.patches.size_bytes());
        }
    }
    if (args.IsCompute()) {
        // Session 47: the CPU copy A computed (count 0: none, B runs SotcCpuCopy itself).
        w.Put<u64>(copied ? copy.count : 0);
        if (copied) {
            w.Put(copy);
            w.PutRaw(copy_words.data(), copy_words.size() * sizeof(u32));
        }
    }
    if (args.verify) {
        w.PutRaw(liverpool->regs.reg_array.data(), sizeof(liverpool->regs.reg_array));
    }
    ASSERT_MSG(w.Bytes().size() <= pipe.ring.MaxPayload(), "pipe packet too large: {} bytes",
               w.Bytes().size());
    if (!pipe.threaded && w.Bytes().size() > SotcPipe::RingBytes / 4) {
        // Sync: B's unpublished pops (SotcPacketRing::PublishEvery) must not leave too little
        // room for a huge packet (a ClearState delta is ~220 KiB, so this never runs in practice).
        pipe.ring.Publish();
    }
    pipe.ring.Push(w.Bytes());
    ++pipe.submitted;
    if (!pipe.threaded) {
        SotcPipeRun(); // sync: B runs at once
        return;
    }
    if (copied) {
        ++pipe.overlay_copies;
        pipe.overlay_bytes += copy_words.size() * sizeof(u32);
        pipe.pending.AddOverlay(seq, copy.dst, std::exchange(copy_words, {}));
    } else if (cpu_copy) {
        // A copy left to the GPU (a GPU-written source, ~half of them): its destination is a
        // pending GPU write (SotcPipeNoteWrites), so an A read of it waits for B ("pending")
        // and then downloads, as the walker did before. Sync mode: B already ran it.
        ++pipe.copy_fallbacks;
    }
    if ((pipe.submitted & 4095) == 0) {
        SotcPipeReport();
    }
}

// Stage B, sync mode: runs the queued packets in order.
void Rasterizer::SotcPipeRun() {
    SOTC_ZONE("SotcPipeRun");
    auto& pipe = *sotc_pipe;
    for (auto bytes = pipe.ring.Front(); !bytes.empty(); bytes = pipe.ring.Front()) {
        SotcPipeRunPacket(bytes);
        pipe.ring.Pop();
    }
}

// Stage B, SOTC_GPU_PIPE=1: the sotc:GpuRecord thread.
void Rasterizer::SotcPipeThread() {
    Common::SetCurrentThreadName("sotc:GpuRecord");
    SotcDebug::OnGpuThreadStart(); // its scheduler waits count as GPU-thread waits
    auto& pipe = *sotc_pipe;
    u64 run = 0;
    // Session 46: B owns its state while awake; A's drain guard takes it while B sleeps.
    std::unique_lock own{pipe.state_mutex};
    for (;;) {
        SotcPipeRunCommands(); // between packets
        auto bytes = pipe.ring.Front();
        if (bytes.empty()) {
            // Idle: hand A the state, tell a draining A, then sleep until a packet or command.
            pipe.ring.Publish();
            own.unlock();
            pipe.retired.store(run, std::memory_order_seq_cst);
            if (pipe.drain_waiting.load(std::memory_order_seq_cst)) {
                pipe.retired.notify_all();
            }
            const auto t0 = std::chrono::steady_clock::now();
            bytes = pipe.ring.WaitFront(
                [&] { return pipe.num_commands.load(std::memory_order_seq_cst) != 0; });
            pipe.b_idle_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - t0)
                                             .count()),
                                     std::memory_order_relaxed);
            own.lock(); // waits while A holds a drain guard
            if (bytes.empty()) {
                continue; // woken for a command
            }
        }
        const auto kind = SotcPacketReader{bytes}.Get<SotcPipeArgs>().kind;
        if (kind == SotcPipeKind::Stop) {
            SotcPipeRunCommands();
            pipe.ring.Pop();
            pipe.ring.Publish();
            return;
        }
        SotcPipeRunPacket(bytes);
        pipe.ring.Pop();
        ++run;
        pipe.progress.store(run, std::memory_order_release); // after its label write or commit
    }
}

void Rasterizer::SotcPipeWait(SotcDrainReason reason) {
    if (!sotc_pipe || !sotc_pipe->threaded ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return;
    }
    auto& pipe = *sotc_pipe;
    const size_t i = size_t(reason);
    ++pipe.drains[i];
    if (pipe.retired.load(std::memory_order_acquire) == pipe.submitted) {
        return;
    }
    SOTC_ZONE("SotcPipeDrain wait");
    ++pipe.drain_waits[i];
    const auto t0 = std::chrono::steady_clock::now();
    // A pushed packets while it owned B's state: B is blocked on state_mutex, so let it run.
    const bool owned = pipe.own_depth > 0;
    if (owned) {
        pipe.state_mutex.unlock();
    }
    pipe.drain_waiting.store(true, std::memory_order_seq_cst);
    for (u64 r = pipe.retired.load(std::memory_order_seq_cst); r != pipe.submitted;
         r = pipe.retired.load(std::memory_order_seq_cst)) {
        pipe.retired.wait(r, std::memory_order_acquire);
    }
    pipe.drain_waiting.store(false, std::memory_order_relaxed);
    if (owned) {
        pipe.state_mutex.lock();
    }
    pipe.drain_ns[i] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
}

// Session 46: wait for B, then own its state until the guard goes (B may run a command when
// it is idle; it can't while A holds state_mutex).
Rasterizer::SotcPipeOwner Rasterizer::SotcPipeDrain(SotcDrainReason reason) {
    if (!sotc_pipe || !sotc_pipe->threaded ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return {};
    }
    SotcPipeWait(reason);
    auto& pipe = *sotc_pipe;
    if (pipe.own_depth++ == 0) {
        if (!pipe.state_mutex.try_lock()) {
            SOTC_ZONE("SotcPipeDrain command wait"); // B is running a command
            const auto t0 = std::chrono::steady_clock::now();
            pipe.state_mutex.lock();
            pipe.drain_ns[size_t(reason)] += u64(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
        }
    }
    return SotcPipeOwner{this};
}

void Rasterizer::SotcPipeRelease() {
    auto& pipe = *sotc_pipe;
    if (--pipe.own_depth == 0) {
        pipe.state_mutex.unlock();
    }
}

bool Rasterizer::SotcPipeCommand(Common::UniqueFunction<void>&& func) {
    if (!sotc_pipe || !sotc_pipe->threaded) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    {
        std::scoped_lock lk{pipe.command_mutex};
        pipe.commands.push_back(std::move(func));
    }
    pipe.num_commands.fetch_add(1, std::memory_order_seq_cst);
    pipe.ring.Kick();
    return true;
}

// B, between packets and on waking: the queued commands, oldest first.
void Rasterizer::SotcPipeRunCommands() {
    auto& pipe = *sotc_pipe;
    while (pipe.num_commands.load(std::memory_order_acquire) != 0) {
        Common::UniqueFunction<void> func;
        {
            std::scoped_lock lk{pipe.command_mutex};
            func = std::move(pipe.commands.front());
            pipe.commands.pop_front();
        }
        pipe.num_commands.fetch_sub(1, std::memory_order_relaxed);
        SOTC_ZONE("SotcPipe command");
        func();
        pipe.commands_run.fetch_add(1, std::memory_order_relaxed);
    }
}

// Session 45: A hands B a submit-done, idle or flip.
bool Rasterizer::SotcPipeDefer(SotcPipeEvent event) {
    if (!sotc_pipe || !sotc_pipe->threaded) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    pipe.writer.Clear();
    pipe.writer.Put(SotcPipeArgs{.kind = SotcPipeKind::Event, .size = u32(event)});
    pipe.ring.Push(pipe.writer.Bytes());
    ++pipe.submitted;
    ++pipe.events;
    return true;
}

// Session 45: A hands B a fence packet (a copy: B never reads PM4 memory, which the guest may
// free once the fence signals) and notes the label write it will make.
bool Rasterizer::SotcPipeFence(const AmdGpu::PM4Header* header, u32 pipe_id) {
    if (!sotc_pipe || !sotc_pipe->threaded) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    const u64 seq = pipe.submitted + 1;
    auto& pending = pipe.pending_fences;
    pending.Retire(std::max(pipe.retired.load(std::memory_order_acquire),
                            pipe.progress.load(std::memory_order_acquire)));
    const auto note = [&](u64 addr, u32 bytes, AmdGpu::DataSelect sel, u64 value) {
        const bool known =
            sel == AmdGpu::DataSelect::Data32Low || sel == AmdGpu::DataSelect::Data64;
        pending.Add(seq, addr, bytes, known ? value : 0, known);
    };
    switch (header->type3.opcode.Value()) {
    case AmdGpu::PM4ItOpcode::EventWriteEop: {
        const auto* eop = reinterpret_cast<const AmdGpu::PM4CmdEventWriteEop*>(header);
        const auto sel = eop->data_sel.Value();
        if (sel != AmdGpu::DataSelect::None) {
            note(reinterpret_cast<u64>(eop->Address<u32>()),
                 sel == AmdGpu::DataSelect::Data32Low ? 4 : 8, sel, eop->DataQWord());
        }
        break;
    }
    case AmdGpu::PM4ItOpcode::EventWriteEos: {
        const auto* eos = reinterpret_cast<const AmdGpu::PM4CmdEventWriteEos*>(header);
        const bool gds = eos->command == AmdGpu::PM4CmdEventWriteEos::Command::GdsStore;
        note(eos->Address<u64>(), 4,
             gds ? AmdGpu::DataSelect::GdsMemStore : AmdGpu::DataSelect::Data32Low,
             eos->DataDWord());
        break;
    }
    case AmdGpu::PM4ItOpcode::ReleaseMem: {
        const auto* rel = reinterpret_cast<const AmdGpu::PM4CmdReleaseMem*>(header);
        const auto sel = rel->data_sel.Value();
        note(rel->Address<u64>(),
             sel == AmdGpu::DataSelect::Data32Low     ? 4
             : sel == AmdGpu::DataSelect::GdsMemStore ? u32(rel->num_dw) * 4
                                                      : 8,
             sel, rel->DataQWord());
        break;
    }
    default:
        return false;
    }
    const u32 dwords = header->type3.NumWords() + 1;
    pipe.writer.Clear();
    pipe.writer.Put(SotcPipeArgs{.kind = SotcPipeKind::Fence, .offset = pipe_id, .size = dwords});
    pipe.writer.PutRaw(header, dwords * sizeof(u32));
    pipe.ring.Push(pipe.writer.Bytes());
    ++pipe.submitted;
    ++pipe.fences;
    return true;
}

bool Rasterizer::SotcPipeWaitMet(const AmdGpu::PM4CmdWaitRegMem& wait) {
    if (!sotc_pipe || !sotc_pipe->threaded || sotc_pipe->pending_fences.Empty() ||
        wait.mem_space.Value() != AmdGpu::PM4CmdWaitRegMem::MemSpace::Memory) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    auto& pending = pipe.pending_fences;
    pending.Retire(std::max(pipe.retired.load(std::memory_order_acquire),
                            pipe.progress.load(std::memory_order_acquire)));
    u32 value = 0;
    switch (pending.Find(reinterpret_cast<u64>(wait.Address()), &value)) {
    case SotcPendingFences::Lookup::None:
        return false; // poll memory: the CPU writes it, or B already did
    case SotcPendingFences::Lookup::Value:
        if (wait.TestValue(value)) {
            ++pipe.waits_met;
            return true;
        }
        break;
    case SotcPendingFences::Lookup::Unknown:
        break;
    }
    SotcPipeWait(SotcDrainReason::WaitFence); // then memory holds what B wrote
    pending.Retire(pipe.submitted);
    return false;
}

bool Rasterizer::SotcFaultOnGpuSide() {
    const auto id = std::this_thread::get_id();
    const bool threaded = sotc_pipe && sotc_pipe->threaded;
    if (id == liverpool->GetGpuCommandProcessorThread()) {
        // Session 46: inline only while A owns B's state (B is blocked then, so a queued fault
        // would never run); otherwise queued to B, handled between packets.
        return !threaded || sotc_pipe->own_depth > 0;
    }
    return threaded && id == sotc_pipe->record_thread.get_id();
}

void Rasterizer::SotcNoteQueuedFault(std::chrono::steady_clock::time_point start) {
    if (!sotc_pipe || std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return;
    }
    ++sotc_pipe->a_faults;
    sotc_pipe->a_fault_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
}

// Session 47: A's view of pending writes (see vk_rasterizer.h).
const u8* Rasterizer::SotcPipeOverlay(VAddr addr, u64 size) {
    if (!sotc_pipe || !sotc_pipe->threaded || sotc_pipe->pending.Empty() || size == 0 ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return nullptr;
    }
    auto& pipe = *sotc_pipe;
    const u8* at = nullptr;
    switch (pipe.pending.Find(addr, size, pipe.a_view, &at)) {
    case SotcPendingRanges::Read::None:
        return nullptr;
    case SotcPendingRanges::Read::Overlay:
        ++pipe.overlay_reads;
        return at;
    case SotcPendingRanges::Read::Wait:
        break;
    }
    SotcPipeWait(SotcDrainReason::Pending); // then guest memory and B's ranges are current
    pipe.a_view = pipe.submitted;
    return nullptr;
}

void Rasterizer::SotcPipeMergeRead(VAddr addr, u8* dst, u64 size) {
    if (!sotc_pipe || !sotc_pipe->threaded || sotc_pipe->pending.Empty() || size == 0 ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return;
    }
    auto& pipe = *sotc_pipe;
    if (pipe.pending.Merge(addr, dst, size, pipe.a_view)) {
        return;
    }
    SotcPipeWait(SotcDrainReason::Pending);
    pipe.a_view = pipe.submitted;
    memory->CopySparseMemory(addr, dst, size);
}

// Session 47, A: the buffer ranges packet `seq` may write (its written bindings, clamped as
// BindBuffers clamps them), except the CPU copy's destination at `skip` (its overlay).
void Rasterizer::SotcPipeNoteWrites(const Pipeline* pipeline, u64 seq, VAddr skip) {
    auto& pipe = *sotc_pipe;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& desc : stage->buffers) {
            if (!desc.is_written || desc.IsSpecial()) {
                continue;
            }
            const auto vsharp = desc.GetSharp(*stage);
            if (vsharp.base_address == 0 || vsharp.base_address == skip ||
                vsharp.GetSize() == 0) {
                continue;
            }
            pipe.pending.AddWrite(seq, vsharp.base_address,
                                  memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize()));
        }
    }
}

// Session 47, B (SOTC_GPU_PIPE_VERIFY): re-runs each stage's SRT walker now, in stream order
// (B's faults download GPU data, B's commits are in guest memory), and compares the flattened
// user data with what A's walk produced. A difference means A read memory a packet before it
// changed (a pending write the overlay or the pending set missed). Dwords the GPU patches
// (FlatPatches: A read the CPU copy on purpose) are skipped.
void Rasterizer::SotcPipeVerifyWalk(std::span<const Shader::UdSnapshot> snaps) {
    auto& pipe = *sotc_pipe;
    thread_local std::vector<u32> flat;
    for (const auto& snap : snaps) {
        const auto* info = snap.info;
        if (!info || !info->srt_info.walker_func || snap.flat.empty()) {
            continue;
        }
        flat.assign(snap.flat.size(), 0);
        std::memcpy(flat.data(), snap.user_data.data(),
                    std::min(snap.user_data.size_bytes(), flat.size() * sizeof(u32)));
        Shader::SrtSetWalkInfo(info);
        info->srt_info.walker_func(snap.user_data.data(), flat.data(), &Shader::g_srt_helpers);
        Shader::SrtSetWalkInfo(nullptr);
        pipe.verify_walks.fetch_add(1, std::memory_order_relaxed);
        u32 diffs = 0, first = 0;
        for (u32 i = 0; i < flat.size(); ++i) {
            if (flat[i] == snap.flat[i]) {
                continue;
            }
            const bool patched = std::ranges::any_of(
                snap.patches, [i](const auto& p) { return p.first == i; });
            if (!patched && diffs++ == 0) {
                first = i;
            }
        }
        if (diffs == 0) {
            continue;
        }
        if (pipe.verify_walk_diffs.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_ERROR(Render_Vulkan,
                      "pipe verify: packet {}: shader {:#x} walker: {} flattened dwords differ "
                      "(first {}: B {:#x}, A {:#x})",
                      pipe.packets, info->pgm_hash, diffs, first, flat[first], snap.flat[first]);
        }
    }
}

// A, every 4,096 packets: every 10 s, one line with the packets, the drains by reason (calls /
// calls that waited / ms waited), and how long B slept on an empty ring.
void Rasterizer::SotcPipeReport() {
    auto& pipe = *sotc_pipe;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - pipe.report_start).count();
    if (secs < 10.0) {
        return;
    }
    static constexpr const char* Names[] = {"fence", "submit", "flush",    "finish",
                                            "gds",   "dma",    "marker",   "command",
                                            "flip",  "idle",   "cpu-copy", "fault",
                                            "wait-fence", "pending"};
    static_assert(std::size(Names) == SotcPipe::NumReasons);
    std::string by_reason;
    u64 total = 0, waits = 0, ns = 0;
    for (size_t i = 0; i < SotcPipe::NumReasons; ++i) {
        total += pipe.drains[i];
        waits += pipe.drain_waits[i];
        ns += pipe.drain_ns[i];
        if (pipe.drains[i]) {
            by_reason += fmt::format(" {} {}/{}/{:.1f}", Names[i], pipe.drains[i],
                                     pipe.drain_waits[i], pipe.drain_ns[i] / 1e6);
        }
    }
    const u64 b_idle = pipe.b_idle_ns.load(std::memory_order_relaxed);
    const u64 commands = pipe.commands_run.load(std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan,
             "pipe: {:.1f} s: {} packets ({} fences, {} events), {} drains ({} waited, {:.1f} "
             "ms), B idle {:.1f} ms, {} waits met by pending fences, {} commands on B, {} A "
             "faults queued ({:.1f} ms);{}",
             secs, pipe.submitted - pipe.report_packets, pipe.fences, pipe.events, total, waits,
             ns / 1e6, (b_idle - pipe.report_b_idle_ns) / 1e6, pipe.waits_met,
             commands - pipe.report_commands, pipe.a_faults, pipe.a_fault_ns / 1e6, by_reason);
    LOG_INFO(Render_Vulkan,
             "pipe: A copies {} ({:.1f} MB), {} left to B, {} overlay reads, {} pending ranges "
             "({} big); verify: {} walks, {} differ",
             pipe.overlay_copies, pipe.overlay_bytes / 1e6, pipe.copy_fallbacks,
             pipe.overlay_reads, pipe.pending.Size(), pipe.pending.Big(),
             pipe.verify_walks.load(std::memory_order_relaxed),
             pipe.verify_walk_diffs.load(std::memory_order_relaxed));
    pipe.overlay_copies = 0;
    pipe.overlay_bytes = 0;
    pipe.copy_fallbacks = 0;
    pipe.overlay_reads = 0;
    pipe.report_commands = commands;
    pipe.a_faults = 0;
    pipe.a_fault_ns = 0;
    pipe.fences = 0;
    pipe.events = 0;
    pipe.waits_met = 0;
    pipe.drains = {};
    pipe.drain_waits = {};
    pipe.drain_ns = {};
    pipe.report_packets = pipe.submitted;
    pipe.report_b_idle_ns = b_idle;
    pipe.report_start = now;
}

void Rasterizer::SotcPipeRunPacket(std::span<const u8> bytes) {
    auto& pipe = *sotc_pipe;
    {
        SotcPacketReader r{bytes};
        const auto args = r.Get<SotcPipeArgs>();
        if (args.kind == SotcPipeKind::Fence) {
            // Session 45: A's copy of the PM4 packet (offset = the compute queue's pipe id).
            liverpool->SotcRunFence(
                reinterpret_cast<const AmdGpu::PM4Header*>(r.Take(args.size * sizeof(u32))),
                args.offset);
            return;
        }
        if (args.kind == SotcPipeKind::Event) {
            switch (static_cast<SotcPipeEvent>(args.size)) {
            case SotcPipeEvent::SubmitDone: {
                ZoneScopedNC("SubmitDone (OnSubmit + Flush)", 0x2a9d8f);
                OnSubmit();
                Flush();
                break;
            }
            case SotcPipeEvent::Idle:
                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
                break;
            case SotcPipeEvent::Flip:
                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                break;
            }
            return;
        }
        SotcApplyRegDelta<AmdGpu::Regs::NumRegs>(r, pipe.regs.reg_array.data());
        pipe.cb_extent = r.Get<decltype(pipe.cb_extent)>();
        pipe.db_extent = r.Get<AmdGpu::CbDbExtent>();
        if (args.IsCompute()) {
            pipe.cs = r.Get<AmdGpu::ComputeProgram>();
        }
        const auto* pipeline = r.Get<const Pipeline*>();
        boost::container::static_vector<Shader::UdSnapshot, Shader::MaxStageTypes> snaps;
        if (pipeline) {
            const auto num_stages = r.Get<u64>();
            for (u64 i = 0; i < num_stages; ++i) {
                auto& snap = snaps.emplace_back();
                snap.info = r.Get<const Shader::Info*>();
                snap.pgm_base = r.Get<VAddr>();
                snap.user_data = r.GetSpan<u32>();
                snap.flat = r.GetSpan<u32>();
                const auto num_patches = r.Get<u64>();
                snap.patches = {reinterpret_cast<const std::pair<u16, u64>*>(
                                    r.Take(num_patches * sizeof(std::pair<u16, u64>))),
                                num_patches};
            }
        }
        SotcCopyResult copy{};
        std::span<const u32> copy_words;
        if (args.IsCompute()) {
            if (const auto count = r.Get<u64>()) {
                copy = r.Get<SotcCopyResult>();
                copy_words = {reinterpret_cast<const u32*>(r.Take(count * sizeof(u32))),
                              size_t(count)};
            }
        }
        ++pipe.packets;
        if (args.verify) {
            // Liverpool's register file as A sent this packet.
            const auto* live = reinterpret_cast<const u32*>(r.Take(sizeof(pipe.regs.reg_array)));
            u32 diffs = 0, first = 0;
            for (u32 i = 0; i < AmdGpu::Regs::NumRegs; ++i) {
                if (pipe.regs.reg_array[i] != live[i] && diffs++ == 0) {
                    first = i;
                }
            }
            if (diffs && pipe.verify_diffs++ < 32) {
                LOG_ERROR(Render_Vulkan,
                          "pipe verify: packet {}: {} register words differ (first {:#x}: "
                          "B {:#x}, A {:#x})",
                          pipe.packets, diffs, first, pipe.regs.reg_array[first], live[first]);
            }
        }
        if (args.verify && pipe.threaded) {
            SotcPipeVerifyWalk(snaps);
        }
        {
            Shader::SotcUdScope ud_scope{snaps};
            {
                SOTC_ZONE_N(pop_zone, "PopPendingOperations");
                scheduler.PopPendingOperations();
            }
            switch (args.kind) {
            case SotcPipeKind::Draw:
                if (args.filter.kind != SotcDrawFilter::Kind::Draw) {
                    RunDrawFilter(args.filter);
                } else {
                    RecordDraw(static_cast<const GraphicsPipeline*>(pipeline), args.is_indexed,
                               args.index_offset);
                }
                break;
            case SotcPipeKind::DrawIndirect:
                if (args.filter.kind != SotcDrawFilter::Kind::Draw) {
                    RunDrawFilter(args.filter);
                } else {
                    RecordDrawIndirect(static_cast<const GraphicsPipeline*>(pipeline),
                                       args.is_indexed, args.arg_address, args.offset,
                                       args.stride, args.max_count, args.count_address);
                }
                break;
            case SotcPipeKind::DispatchDirect:
                if (!copy_words.empty()) {
                    sotc_copy_commit = &copy;
                    sotc_copy_words = copy_words;
                }
                RecordDispatchDirect(static_cast<const ComputePipeline*>(pipeline));
                sotc_copy_commit = nullptr;
                sotc_copy_words = {};
                break;
            case SotcPipeKind::DispatchIndirect:
                RecordDispatchIndirect(static_cast<const ComputePipeline*>(pipeline),
                                       args.arg_address, args.offset, args.size);
                break;
            case SotcPipeKind::Stop:
                break;
            }
        }
    }
}

} // namespace Vulkan
