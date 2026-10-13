// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 62): draw-prep shape D, step 2a (docs/next/draw-prep.md, section 11):
// stage B's record path encodes its Vulkan commands into a ring (sotc_cmd_ring.h) instead of
// recording them, and a decoder records them later. SOTC_GPU_CMD=0 (default): today's path.
// SOTC_GPU_CMD=sync: the decoder runs on B right after each record (no thread yet, step 3).
// - SotcCmdBuffer has vk::CommandBuffer's method names for the calls the record path makes, so
//   a site changes only where it gets its command buffer (Scheduler::Cmd()). While a scope is
//   open (Scheduler::SotcCmdOpen, one per Record*), a call appends a command holding values
//   only (handles, offsets, the descriptor writes, push constants, barrier arrays: rule 2);
//   otherwise it records directly.
// - Ordering: everything else still records directly through Scheduler::CommandBuffer(). Such
//   an access inside an open scope (an upload, an overlap copy, a hot flush's submit) "breaks"
//   it: the pending commands are decoded first, and the rest of that record records directly.
//   So a site that holds a raw command buffer across a call to an encoded function still
//   records in order. Breaks are counted by call site (the cmd: line): they are the sites
//   step 2b converts.
// - Session 64 (step 3a): SOTC_GPU_CMD=1 decodes on a thread of its own, stage C
//   (sotc:GpuCmd), started at the first scope. B still owns the command pool, the session's
//   begin/end and every submit: before those, and before any direct access to the command
//   buffer, B waits until C has recorded everything appended (a "drain", Drain). So in thread
//   mode the scope stays open between records (commands outside a record are appended too),
//   and only a drain shuts it, until the next record opens it. C only ever touches the command
//   buffer while B is not, and the pool only while B allocates nothing from it.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <thread>
#include <type_traits>

#include "video_core/renderer_vulkan/sotc_cmd_ring.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

struct RenderState;

enum class SotcCmdKind : std::uint32_t {
    BindPipeline,
    BeginRendering,
    EndRendering,
    PipelineBarrier2,
    PushConstants,
    PushDescriptorSet,
    BindDescriptorSet,
    SetVertexInput,
    BindVertexBuffers,
    BindVertexBuffers2,
    BindIndexBuffer,
    Draw,
    DrawIndexed,
    DrawIndirect,
    DrawIndexedIndirect,
    DrawIndirectCount,
    DrawIndexedIndirectCount,
    Dispatch,
    DispatchIndirect,
    SetViewports,
    SetScissors,
    SetBool,        // the dynamic-state enables (SotcCmdBool)
    SetDepthCompareOp,
    SetDepthBounds,
    SetDepthBias,
    SetStencilOp,
    SetStencilU32,  // reference, write and compare masks (SotcCmdStencilU32)
    SetCullMode,
    SetFrontFace,
    SetBlendConstants,
    SetColorWriteMask,
    SetLineWidth,
    SetFeedbackLoop,
    // Session 63 (step 2b): Runtime's copies and clears.
    CopyBuffer,
    FillBuffer,
    UpdateBuffer,
    CopyBufferToImage,
    CopyImageToBuffer,
    CopyImage,
    ResolveImage,
    ClearColorImage,
    Stop, // session 64: ends stage C's thread (~Scheduler)
    Count,
};

enum class SotcCmdBool : std::uint32_t {
    DepthTest,
    DepthWrite,
    DepthBoundsTest,
    DepthBias,
    StencilTest,
    PrimitiveRestart,
    RasterizerDiscard,
};

enum class SotcCmdStencilU32 : std::uint32_t {
    Reference,
    WriteMask,
    CompareMask,
};

/// The encoder's state, a member of the Scheduler (only B uses it).
struct SotcCmdState {
    SotcCmdRing ring{std::size_t{4} << 20};
    vk::CommandBuffer raw{}; // the scheduler's current primary command buffer
    bool open = false;
    int mode = 0; // 0 off, 1 sync, 2 thread (session 64)
    std::thread thread; // stage C (mode 2), started at the first scope
    std::atomic<std::uint64_t> decoded{0}; // C: commands recorded (every 64), for StallState
    // Session 65: C's spin before it sleeps (SOTC_GPU_CMD_SPIN, us), and how many scopes B
    // closes per wake-up of a sleeping C (SOTC_GPU_CMD_WAKE; drains always wake it).
    std::chrono::nanoseconds c_spin{std::chrono::microseconds{50}};
    std::uint32_t wake_every = 1;
    std::uint32_t unwoken = 0;             // B: scopes closed since the last wake call
    std::atomic<std::uint64_t> c_sleeps{0}; // C: times it slept

    // Counters for the cmd: line. B counts in plain fields and publishes them at the end of
    // each scope (relaxed stores); the report (on A) reads only the published ones.
    std::uint64_t scopes = 0;
    std::uint64_t commands = 0;
    std::uint64_t breaks = 0;
    std::uint64_t full_drains = 0;
    std::uint64_t flush_submits = 0; // submits inside a scope (session 63)
    std::uint64_t drain_waits = 0;   // drains that found C busy (session 64)
    std::uint64_t drain_ns = 0;      // B's time waiting for them
    std::uint64_t wakes = 0;         // scope ends that woke C (session 65)
    std::atomic<std::uint64_t> pub_scopes{0};
    std::atomic<std::uint64_t> pub_commands{0};
    std::atomic<std::uint64_t> pub_bytes{0};
    std::atomic<std::uint64_t> pub_breaks{0};
    std::atomic<std::uint64_t> pub_full_drains{0};
    std::atomic<std::uint64_t> pub_flush_submits{0};
    std::atomic<std::uint64_t> pub_drain_waits{0};
    std::atomic<std::uint64_t> pub_drain_ns{0};
    std::atomic<std::uint64_t> pub_wakes{0};
    std::array<std::uint64_t, 10> reported{}; // the report's last values (A)
    struct BreakSite {
        const char* file;
        std::uint32_t line;
        std::uint64_t count;
    };
    std::mutex break_mutex; // break_sites: B adds, the report takes
    std::array<BreakSite, 32> break_sites{};
    std::size_t num_break_sites = 0;

    SotcCmdState() = default;
    ~SotcCmdState();
    SotcCmdState(const SotcCmdState&) = delete;
    SotcCmdState& operator=(const SotcCmdState&) = delete;

    /// SOTC_GPU_CMD, read once.
    static int ParseMode();

    /// Starts stage C (mode 2) unless it runs.
    void StartThread() {
        if (!thread.joinable()) [[unlikely]] {
            Start();
        }
    }

    /// Every command appended so far is recorded into `raw` when this returns: sync mode
    /// decodes them here, thread mode waits for C.
    void Drain();
    /// A direct access to the command buffer inside an open scope: decode, then stay direct.
    void Break(const std::source_location& loc);
    /// A submit inside an open scope (session 63): decode, then shut the scope for the
    /// submit; Scheduler::SubmitExecution reopens it on the next command buffer.
    void FlushSubmit();
    /// The end of a record's scope: decode what it appended.
    void Close();
    /// The ring was full: decode what is there (sync mode) so Reserve can go on.
    void OnFull();

    /// Reserves a record, draining when the ring is full.
    void* Put(SotcCmdKind kind, std::size_t payload) {
        ++commands;
        for (;;) {
            if (void* p = ring.Reserve(static_cast<std::uint32_t>(kind), payload)) {
                return p;
            }
            OnFull();
        }
    }
    template <typename T>
    void PutValue(SotcCmdKind kind, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::memcpy(Put(kind, sizeof(T)), &value, sizeof(T));
    }

    /// Stores the counters for the report.
    void Publish() {
        pub_scopes.store(scopes, std::memory_order_relaxed);
        pub_commands.store(commands, std::memory_order_relaxed);
        pub_bytes.store(ring.ReservedBytes(), std::memory_order_relaxed);
        pub_breaks.store(breaks, std::memory_order_relaxed);
        pub_full_drains.store(full_drains, std::memory_order_relaxed);
        pub_flush_submits.store(flush_submits, std::memory_order_relaxed);
        pub_drain_waits.store(drain_waits, std::memory_order_relaxed);
        pub_drain_ns.store(drain_ns, std::memory_order_relaxed);
        pub_wakes.store(wakes, std::memory_order_relaxed);
    }

    /// The cmd: line since the last call (any thread; empty when SOTC_GPU_CMD=0).
    std::string TakeReport();
    /// Session 64: C's and the ring's state for the stall watchdog (any thread).
    std::string StallState() const;

private:
    void Start();
    void DecodeAll();
    void ThreadMain();
};

/// Records the RenderingInfo for `state` (Scheduler::BeginRendering's body, upstream).
void SotcRecordBeginRendering(vk::CommandBuffer cmdbuf, const RenderState& state);

/// vk::CommandBuffer's interface for the calls the record path makes: appends while the
/// scope is open, records directly otherwise.
class SotcCmdBuffer {
public:
    explicit SotcCmdBuffer(SotcCmdState& st_) : st{&st_} {}

    void bindPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) const {
        if (!st->open) {
            st->raw.bindPipeline(point, pipeline);
            return;
        }
        st->PutValue(SotcCmdKind::BindPipeline, BindPipelineCmd{point, pipeline});
    }
    void beginRendering(const RenderState& state) const;
    void endRendering() const {
        if (!st->open) {
            st->raw.endRendering();
            return;
        }
        st->Put(SotcCmdKind::EndRendering, 0);
    }
    void pipelineBarrier2(const vk::DependencyInfo& info) const;
    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, std::uint32_t offset,
                       std::uint32_t size, const void* values) const;
    void pushDescriptorSetKHR(vk::PipelineBindPoint point, vk::PipelineLayout layout,
                              std::uint32_t set,
                              vk::ArrayProxy<const vk::WriteDescriptorSet> writes) const;
    void bindDescriptorSets(vk::PipelineBindPoint point, vk::PipelineLayout layout,
                            std::uint32_t first, vk::DescriptorSet set,
                            std::span<const std::uint32_t> dynamic_offsets) const;
    void setVertexInputEXT(
        vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> bindings,
        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> attributes) const;
    void bindVertexBuffers(std::uint32_t first, std::uint32_t count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const;
    void bindVertexBuffers2(std::uint32_t first, std::uint32_t count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const;
    void bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) const {
        if (!st->open) {
            st->raw.bindIndexBuffer(buffer, offset, type);
            return;
        }
        st->PutValue(SotcCmdKind::BindIndexBuffer, BindIndexBufferCmd{buffer, offset, type});
    }
    void draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t first_vertex,
              std::uint32_t first_instance) const {
        if (!st->open) {
            st->raw.draw(vertices, instances, first_vertex, first_instance);
            return;
        }
        st->PutValue(SotcCmdKind::Draw,
                     DrawCmd{vertices, instances, first_vertex, first_instance});
    }
    void drawIndexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first_index,
                     std::int32_t vertex_offset, std::uint32_t first_instance) const {
        if (!st->open) {
            st->raw.drawIndexed(indices, instances, first_index, vertex_offset, first_instance);
            return;
        }
        st->PutValue(SotcCmdKind::DrawIndexed,
                     DrawIndexedCmd{indices, instances, first_index, vertex_offset,
                                    first_instance});
    }
    void drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, std::uint32_t count,
                      std::uint32_t stride) const {
        if (!st->open) {
            st->raw.drawIndirect(buffer, offset, count, stride);
            return;
        }
        st->PutValue(SotcCmdKind::DrawIndirect, IndirectCmd{buffer, offset, count, stride});
    }
    void drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, std::uint32_t count,
                             std::uint32_t stride) const {
        if (!st->open) {
            st->raw.drawIndexedIndirect(buffer, offset, count, stride);
            return;
        }
        st->PutValue(SotcCmdKind::DrawIndexedIndirect,
                     IndirectCmd{buffer, offset, count, stride});
    }
    void drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                           vk::DeviceSize count_offset, std::uint32_t max_count,
                           std::uint32_t stride) const {
        if (!st->open) {
            st->raw.drawIndirectCount(buffer, offset, count_buffer, count_offset, max_count,
                                      stride);
            return;
        }
        st->PutValue(SotcCmdKind::DrawIndirectCount,
                     IndirectCountCmd{buffer, offset, count_buffer, count_offset, max_count,
                                      stride});
    }
    void drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                  vk::Buffer count_buffer, vk::DeviceSize count_offset,
                                  std::uint32_t max_count, std::uint32_t stride) const {
        if (!st->open) {
            st->raw.drawIndexedIndirectCount(buffer, offset, count_buffer, count_offset,
                                             max_count, stride);
            return;
        }
        st->PutValue(SotcCmdKind::DrawIndexedIndirectCount,
                     IndirectCountCmd{buffer, offset, count_buffer, count_offset, max_count,
                                      stride});
    }
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) const {
        if (!st->open) {
            st->raw.dispatch(x, y, z);
            return;
        }
        st->PutValue(SotcCmdKind::Dispatch, DispatchCmd{x, y, z});
    }
    void dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const {
        if (!st->open) {
            st->raw.dispatchIndirect(buffer, offset);
            return;
        }
        st->PutValue(SotcCmdKind::DispatchIndirect, DispatchIndirectCmd{buffer, offset});
    }

    // Dynamic state (DynamicState::Commit).
    void setViewportWithCount(vk::ArrayProxy<const vk::Viewport> viewports) const;
    void setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> scissors) const;
    void setDepthTestEnable(bool v) const {
        SetBool(SotcCmdBool::DepthTest, v);
    }
    void setDepthWriteEnable(bool v) const {
        SetBool(SotcCmdBool::DepthWrite, v);
    }
    void setDepthBoundsTestEnable(bool v) const {
        SetBool(SotcCmdBool::DepthBoundsTest, v);
    }
    void setDepthBiasEnable(bool v) const {
        SetBool(SotcCmdBool::DepthBias, v);
    }
    void setStencilTestEnable(bool v) const {
        SetBool(SotcCmdBool::StencilTest, v);
    }
    void setPrimitiveRestartEnable(bool v) const {
        SetBool(SotcCmdBool::PrimitiveRestart, v);
    }
    void setRasterizerDiscardEnable(bool v) const {
        SetBool(SotcCmdBool::RasterizerDiscard, v);
    }
    void setDepthCompareOp(vk::CompareOp op) const {
        if (!st->open) {
            st->raw.setDepthCompareOp(op);
            return;
        }
        st->PutValue(SotcCmdKind::SetDepthCompareOp, op);
    }
    void setDepthBounds(float min, float max) const {
        if (!st->open) {
            st->raw.setDepthBounds(min, max);
            return;
        }
        st->PutValue(SotcCmdKind::SetDepthBounds, std::array<float, 2>{min, max});
    }
    void setDepthBias(float constant, float clamp, float slope) const {
        if (!st->open) {
            st->raw.setDepthBias(constant, clamp, slope);
            return;
        }
        st->PutValue(SotcCmdKind::SetDepthBias, std::array<float, 3>{constant, clamp, slope});
    }
    void setStencilOp(vk::StencilFaceFlags faces, vk::StencilOp fail, vk::StencilOp pass,
                      vk::StencilOp depth_fail, vk::CompareOp compare) const {
        if (!st->open) {
            st->raw.setStencilOp(faces, fail, pass, depth_fail, compare);
            return;
        }
        st->PutValue(SotcCmdKind::SetStencilOp,
                     StencilOpCmd{faces, fail, pass, depth_fail, compare});
    }
    void setStencilReference(vk::StencilFaceFlags faces, std::uint32_t v) const {
        SetStencilU32(SotcCmdStencilU32::Reference, faces, v);
    }
    void setStencilWriteMask(vk::StencilFaceFlags faces, std::uint32_t v) const {
        SetStencilU32(SotcCmdStencilU32::WriteMask, faces, v);
    }
    void setStencilCompareMask(vk::StencilFaceFlags faces, std::uint32_t v) const {
        SetStencilU32(SotcCmdStencilU32::CompareMask, faces, v);
    }
    void setCullMode(vk::CullModeFlags mode) const {
        if (!st->open) {
            st->raw.setCullMode(mode);
            return;
        }
        st->PutValue(SotcCmdKind::SetCullMode, mode);
    }
    void setFrontFace(vk::FrontFace face) const {
        if (!st->open) {
            st->raw.setFrontFace(face);
            return;
        }
        st->PutValue(SotcCmdKind::SetFrontFace, face);
    }
    void setBlendConstants(const float constants[4]) const {
        if (!st->open) {
            st->raw.setBlendConstants(constants);
            return;
        }
        std::array<float, 4> v;
        std::memcpy(v.data(), constants, sizeof(v));
        st->PutValue(SotcCmdKind::SetBlendConstants, v);
    }
    void setColorWriteMaskEXT(std::uint32_t first,
                              vk::ArrayProxy<const vk::ColorComponentFlags> masks) const;
    void setLineWidth(float width) const {
        if (!st->open) {
            st->raw.setLineWidth(width);
            return;
        }
        st->PutValue(SotcCmdKind::SetLineWidth, width);
    }
    void setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspects) const {
        if (!st->open) {
            st->raw.setAttachmentFeedbackLoopEnableEXT(aspects);
            return;
        }
        st->PutValue(SotcCmdKind::SetFeedbackLoop, aspects);
    }

    // Transfers (Runtime's copies and clears, session 63). The regions are copied (rule 2).
    void copyBuffer(vk::Buffer src, vk::Buffer dst,
                    vk::ArrayProxy<const vk::BufferCopy> regions) const;
    void fillBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size,
                    std::uint32_t value) const;
    /// Up to 64 bytes (Runtime::InlineData writes 4).
    void updateBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size,
                      const void* data) const;
    void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout layout,
                           vk::ArrayProxy<const vk::BufferImageCopy> regions) const;
    void copyImageToBuffer(vk::Image src, vk::ImageLayout layout, vk::Buffer dst,
                           vk::ArrayProxy<const vk::BufferImageCopy> regions) const;
    void copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                   vk::ImageLayout dst_layout, vk::ArrayProxy<const vk::ImageCopy> regions) const;
    void resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                      vk::ImageLayout dst_layout,
                      vk::ArrayProxy<const vk::ImageResolve> regions) const;
    void clearColorImage(vk::Image dst, vk::ImageLayout layout, const vk::ClearColorValue& color,
                         vk::ArrayProxy<const vk::ImageSubresourceRange> ranges) const;

    // Payloads (decoded in sotc_cmd_record.cpp).
    struct BindPipelineCmd {
        vk::PipelineBindPoint point;
        vk::Pipeline pipeline;
    };
    struct BindIndexBufferCmd {
        vk::Buffer buffer;
        vk::DeviceSize offset;
        vk::IndexType type;
    };
    struct DrawCmd {
        std::uint32_t vertices, instances, first_vertex, first_instance;
    };
    struct DrawIndexedCmd {
        std::uint32_t indices, instances, first_index;
        std::int32_t vertex_offset;
        std::uint32_t first_instance;
    };
    struct IndirectCmd {
        vk::Buffer buffer;
        vk::DeviceSize offset;
        std::uint32_t count, stride;
    };
    struct IndirectCountCmd {
        vk::Buffer buffer;
        vk::DeviceSize offset;
        vk::Buffer count_buffer;
        vk::DeviceSize count_offset;
        std::uint32_t max_count, stride;
    };
    struct DispatchCmd {
        std::uint32_t x, y, z;
    };
    struct DispatchIndirectCmd {
        vk::Buffer buffer;
        vk::DeviceSize offset;
    };
    struct FillBufferCmd {
        vk::Buffer dst;
        vk::DeviceSize offset, size;
        std::uint32_t value;
    };
    struct BoolCmd {
        SotcCmdBool which;
        std::uint32_t value;
    };
    struct StencilOpCmd {
        vk::StencilFaceFlags faces;
        vk::StencilOp fail, pass, depth_fail;
        vk::CompareOp compare;
    };
    struct StencilU32Cmd {
        SotcCmdStencilU32 which;
        vk::StencilFaceFlags faces;
        std::uint32_t value;
    };

private:
    void SetBool(SotcCmdBool which, bool v) const;
    void SetStencilU32(SotcCmdStencilU32 which, vk::StencilFaceFlags faces,
                       std::uint32_t v) const;

    SotcCmdState* st;
};

} // namespace Vulkan
