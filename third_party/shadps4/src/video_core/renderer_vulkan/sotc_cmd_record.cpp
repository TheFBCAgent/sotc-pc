// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 62): the command encoder and decoder of draw-prep shape D, step 2a. See
// sotc_cmd_record.h. SotcRecordBeginRendering is upstream's Scheduler::BeginRendering body.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/sotc_cmd_record.h"
#include "video_core/renderer_vulkan/sotc_wait_timer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

struct BarrierHdr {
    std::uint32_t flags;
    std::uint32_t num_memory;
    std::uint32_t num_buffer;
    std::uint32_t num_image;
};

struct PushConstantsHdr {
    vk::PipelineLayout layout;
    vk::ShaderStageFlags stages;
    std::uint32_t offset;
    std::uint32_t size;
};

struct PushDescHdr {
    vk::PipelineLayout layout;
    vk::PipelineBindPoint point;
    std::uint32_t set;
    std::uint32_t count;
};

enum class InfoKind : std::uint32_t { Image, Buffer, TexelView };

struct WriteHdr {
    std::uint32_t binding;
    std::uint32_t array_element;
    std::uint32_t count;
    vk::DescriptorType type;
    InfoKind kind;
    std::uint32_t pad;
};

struct BindSetHdr {
    vk::PipelineLayout layout;
    vk::DescriptorSet set;
    vk::PipelineBindPoint point;
    std::uint32_t first;
    std::uint32_t num_offsets;
};

struct VertexInputHdr {
    std::uint32_t num_bindings;
    std::uint32_t num_attributes;
};

struct VertexBuffersHdr {
    std::uint32_t first;
    std::uint32_t count;
};

struct ArrayHdr {
    std::uint32_t first;
    std::uint32_t count;
};

/// A transfer's handles and layouts, followed by `count` regions (session 63).
template <typename Src, typename Dst>
struct XferHdr {
    Src src;
    Dst dst;
    vk::ImageLayout src_layout;
    vk::ImageLayout dst_layout;
    std::uint32_t count;
    std::uint32_t pad;
};
using BufBufHdr = XferHdr<vk::Buffer, vk::Buffer>;
using BufImgHdr = XferHdr<vk::Buffer, vk::Image>;
using ImgBufHdr = XferHdr<vk::Image, vk::Buffer>;
using ImgImgHdr = XferHdr<vk::Image, vk::Image>;

struct UpdateBufferHdr {
    vk::Buffer dst;
    vk::DeviceSize offset;
    vk::DeviceSize size;
};

struct ClearColorHdr {
    vk::Image dst;
    vk::ImageLayout layout;
    std::uint32_t count;
    VkClearColorValue color; // the C union: vk::ClearColorValue has no default constructor
};

constexpr std::size_t Al(std::size_t n) {
    return SotcCmdRing::Align(n);
}

/// Copies `n` T's to `out` (8-aligned) and returns the byte after them.
template <typename T>
std::uint8_t* Write(std::uint8_t* out, const T* values, std::size_t n) {
    std::memcpy(out, values, n * sizeof(T));
    return out + Al(n * sizeof(T));
}

template <typename T>
const T* Read(const std::uint8_t*& in, std::size_t n) {
    const T* p = reinterpret_cast<const T*>(in);
    in += Al(n * sizeof(T));
    return p;
}

template <typename T>
T ReadValue(const std::uint8_t*& in) {
    T v;
    std::memcpy(&v, in, sizeof(T));
    in += Al(sizeof(T));
    return v;
}

void Decode(vk::CommandBuffer cb, SotcCmdKind kind, const std::uint8_t* p) {
    using B = SotcCmdBuffer;
    switch (kind) {
    case SotcCmdKind::BindPipeline: {
        const auto c = ReadValue<B::BindPipelineCmd>(p);
        cb.bindPipeline(c.point, c.pipeline);
        break;
    }
    case SotcCmdKind::BeginRendering: {
        SotcRecordBeginRendering(cb, *reinterpret_cast<const RenderState*>(p));
        break;
    }
    case SotcCmdKind::EndRendering:
        cb.endRendering();
        break;
    case SotcCmdKind::PipelineBarrier2: {
        const auto h = ReadValue<BarrierHdr>(p);
        const auto* mem = Read<vk::MemoryBarrier2>(p, h.num_memory);
        const auto* buf = Read<vk::BufferMemoryBarrier2>(p, h.num_buffer);
        const auto* img = Read<vk::ImageMemoryBarrier2>(p, h.num_image);
        cb.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlags(h.flags),
            .memoryBarrierCount = h.num_memory,
            .pMemoryBarriers = h.num_memory ? mem : nullptr,
            .bufferMemoryBarrierCount = h.num_buffer,
            .pBufferMemoryBarriers = h.num_buffer ? buf : nullptr,
            .imageMemoryBarrierCount = h.num_image,
            .pImageMemoryBarriers = h.num_image ? img : nullptr,
        });
        break;
    }
    case SotcCmdKind::PushConstants: {
        const auto h = ReadValue<PushConstantsHdr>(p);
        cb.pushConstants(h.layout, h.stages, h.offset, h.size, p);
        break;
    }
    case SotcCmdKind::PushDescriptorSet: {
        const auto h = ReadValue<PushDescHdr>(p);
        static thread_local std::vector<vk::WriteDescriptorSet> writes;
        writes.resize(h.count);
        for (std::uint32_t i = 0; i < h.count; ++i) {
            const auto w = ReadValue<WriteHdr>(p);
            vk::WriteDescriptorSet& out = writes[i];
            out = vk::WriteDescriptorSet{
                .dstBinding = w.binding,
                .dstArrayElement = w.array_element,
                .descriptorCount = w.count,
                .descriptorType = w.type,
            };
            switch (w.kind) {
            case InfoKind::Image:
                out.pImageInfo = Read<vk::DescriptorImageInfo>(p, w.count);
                break;
            case InfoKind::Buffer:
                out.pBufferInfo = Read<vk::DescriptorBufferInfo>(p, w.count);
                break;
            case InfoKind::TexelView:
                out.pTexelBufferView = Read<vk::BufferView>(p, w.count);
                break;
            }
        }
        cb.pushDescriptorSetKHR(h.point, h.layout, h.set, writes);
        break;
    }
    case SotcCmdKind::BindDescriptorSet: {
        const auto h = ReadValue<BindSetHdr>(p);
        const auto* offsets = Read<std::uint32_t>(p, h.num_offsets);
        cb.bindDescriptorSets(h.point, h.layout, h.first, 1, &h.set, h.num_offsets, offsets);
        break;
    }
    case SotcCmdKind::SetVertexInput: {
        const auto h = ReadValue<VertexInputHdr>(p);
        const auto* bindings = Read<vk::VertexInputBindingDescription2EXT>(p, h.num_bindings);
        const auto* attributes =
            Read<vk::VertexInputAttributeDescription2EXT>(p, h.num_attributes);
        cb.setVertexInputEXT(h.num_bindings, bindings, h.num_attributes, attributes);
        break;
    }
    case SotcCmdKind::BindVertexBuffers: {
        const auto h = ReadValue<VertexBuffersHdr>(p);
        const auto* buffers = Read<vk::Buffer>(p, h.count);
        const auto* offsets = Read<vk::DeviceSize>(p, h.count);
        cb.bindVertexBuffers(h.first, h.count, buffers, offsets);
        break;
    }
    case SotcCmdKind::BindVertexBuffers2: {
        const auto h = ReadValue<VertexBuffersHdr>(p);
        const auto* buffers = Read<vk::Buffer>(p, h.count);
        const auto* offsets = Read<vk::DeviceSize>(p, h.count);
        const auto* sizes = Read<vk::DeviceSize>(p, h.count);
        const auto* strides = Read<vk::DeviceSize>(p, h.count);
        cb.bindVertexBuffers2(h.first, h.count, buffers, offsets, sizes, strides);
        break;
    }
    case SotcCmdKind::BindIndexBuffer: {
        const auto c = ReadValue<B::BindIndexBufferCmd>(p);
        cb.bindIndexBuffer(c.buffer, c.offset, c.type);
        break;
    }
    case SotcCmdKind::Draw: {
        const auto c = ReadValue<B::DrawCmd>(p);
        cb.draw(c.vertices, c.instances, c.first_vertex, c.first_instance);
        break;
    }
    case SotcCmdKind::DrawIndexed: {
        const auto c = ReadValue<B::DrawIndexedCmd>(p);
        cb.drawIndexed(c.indices, c.instances, c.first_index, c.vertex_offset, c.first_instance);
        break;
    }
    case SotcCmdKind::DrawIndirect: {
        const auto c = ReadValue<B::IndirectCmd>(p);
        cb.drawIndirect(c.buffer, c.offset, c.count, c.stride);
        break;
    }
    case SotcCmdKind::DrawIndexedIndirect: {
        const auto c = ReadValue<B::IndirectCmd>(p);
        cb.drawIndexedIndirect(c.buffer, c.offset, c.count, c.stride);
        break;
    }
    case SotcCmdKind::DrawIndirectCount: {
        const auto c = ReadValue<B::IndirectCountCmd>(p);
        cb.drawIndirectCount(c.buffer, c.offset, c.count_buffer, c.count_offset, c.max_count,
                             c.stride);
        break;
    }
    case SotcCmdKind::DrawIndexedIndirectCount: {
        const auto c = ReadValue<B::IndirectCountCmd>(p);
        cb.drawIndexedIndirectCount(c.buffer, c.offset, c.count_buffer, c.count_offset,
                                    c.max_count, c.stride);
        break;
    }
    case SotcCmdKind::Dispatch: {
        const auto c = ReadValue<B::DispatchCmd>(p);
        cb.dispatch(c.x, c.y, c.z);
        break;
    }
    case SotcCmdKind::DispatchIndirect: {
        const auto c = ReadValue<B::DispatchIndirectCmd>(p);
        cb.dispatchIndirect(c.buffer, c.offset);
        break;
    }
    case SotcCmdKind::SetViewports: {
        const auto h = ReadValue<ArrayHdr>(p);
        cb.setViewportWithCount(h.count, Read<vk::Viewport>(p, h.count));
        break;
    }
    case SotcCmdKind::SetScissors: {
        const auto h = ReadValue<ArrayHdr>(p);
        cb.setScissorWithCount(h.count, Read<vk::Rect2D>(p, h.count));
        break;
    }
    case SotcCmdKind::SetBool: {
        const auto c = ReadValue<B::BoolCmd>(p);
        const bool v = c.value != 0;
        switch (c.which) {
        case SotcCmdBool::DepthTest:
            cb.setDepthTestEnable(v);
            break;
        case SotcCmdBool::DepthWrite:
            cb.setDepthWriteEnable(v);
            break;
        case SotcCmdBool::DepthBoundsTest:
            cb.setDepthBoundsTestEnable(v);
            break;
        case SotcCmdBool::DepthBias:
            cb.setDepthBiasEnable(v);
            break;
        case SotcCmdBool::StencilTest:
            cb.setStencilTestEnable(v);
            break;
        case SotcCmdBool::PrimitiveRestart:
            cb.setPrimitiveRestartEnable(v);
            break;
        case SotcCmdBool::RasterizerDiscard:
            cb.setRasterizerDiscardEnable(v);
            break;
        }
        break;
    }
    case SotcCmdKind::SetDepthCompareOp:
        cb.setDepthCompareOp(ReadValue<vk::CompareOp>(p));
        break;
    case SotcCmdKind::SetDepthBounds: {
        const auto v = ReadValue<std::array<float, 2>>(p);
        cb.setDepthBounds(v[0], v[1]);
        break;
    }
    case SotcCmdKind::SetDepthBias: {
        const auto v = ReadValue<std::array<float, 3>>(p);
        cb.setDepthBias(v[0], v[1], v[2]);
        break;
    }
    case SotcCmdKind::SetStencilOp: {
        const auto c = ReadValue<B::StencilOpCmd>(p);
        cb.setStencilOp(c.faces, c.fail, c.pass, c.depth_fail, c.compare);
        break;
    }
    case SotcCmdKind::SetStencilU32: {
        const auto c = ReadValue<B::StencilU32Cmd>(p);
        switch (c.which) {
        case SotcCmdStencilU32::Reference:
            cb.setStencilReference(c.faces, c.value);
            break;
        case SotcCmdStencilU32::WriteMask:
            cb.setStencilWriteMask(c.faces, c.value);
            break;
        case SotcCmdStencilU32::CompareMask:
            cb.setStencilCompareMask(c.faces, c.value);
            break;
        }
        break;
    }
    case SotcCmdKind::SetCullMode:
        cb.setCullMode(ReadValue<vk::CullModeFlags>(p));
        break;
    case SotcCmdKind::SetFrontFace:
        cb.setFrontFace(ReadValue<vk::FrontFace>(p));
        break;
    case SotcCmdKind::SetBlendConstants: {
        const auto v = ReadValue<std::array<float, 4>>(p);
        cb.setBlendConstants(v.data());
        break;
    }
    case SotcCmdKind::SetColorWriteMask: {
        const auto h = ReadValue<ArrayHdr>(p);
        cb.setColorWriteMaskEXT(h.first, h.count, Read<vk::ColorComponentFlags>(p, h.count));
        break;
    }
    case SotcCmdKind::SetLineWidth:
        cb.setLineWidth(ReadValue<float>(p));
        break;
    case SotcCmdKind::SetFeedbackLoop:
        cb.setAttachmentFeedbackLoopEnableEXT(ReadValue<vk::ImageAspectFlags>(p));
        break;
    case SotcCmdKind::CopyBuffer: {
        const auto h = ReadValue<BufBufHdr>(p);
        cb.copyBuffer(h.src, h.dst, h.count, Read<vk::BufferCopy>(p, h.count));
        break;
    }
    case SotcCmdKind::FillBuffer: {
        const auto c = ReadValue<B::FillBufferCmd>(p);
        cb.fillBuffer(c.dst, c.offset, c.size, c.value);
        break;
    }
    case SotcCmdKind::UpdateBuffer: {
        const auto h = ReadValue<UpdateBufferHdr>(p);
        cb.updateBuffer(h.dst, h.offset, h.size, p);
        break;
    }
    case SotcCmdKind::CopyBufferToImage: {
        const auto h = ReadValue<BufImgHdr>(p);
        cb.copyBufferToImage(h.src, h.dst, h.dst_layout, h.count,
                             Read<vk::BufferImageCopy>(p, h.count));
        break;
    }
    case SotcCmdKind::CopyImageToBuffer: {
        const auto h = ReadValue<ImgBufHdr>(p);
        cb.copyImageToBuffer(h.src, h.src_layout, h.dst, h.count,
                             Read<vk::BufferImageCopy>(p, h.count));
        break;
    }
    case SotcCmdKind::CopyImage: {
        const auto h = ReadValue<ImgImgHdr>(p);
        cb.copyImage(h.src, h.src_layout, h.dst, h.dst_layout, h.count,
                     Read<vk::ImageCopy>(p, h.count));
        break;
    }
    case SotcCmdKind::ResolveImage: {
        const auto h = ReadValue<ImgImgHdr>(p);
        cb.resolveImage(h.src, h.src_layout, h.dst, h.dst_layout, h.count,
                        Read<vk::ImageResolve>(p, h.count));
        break;
    }
    case SotcCmdKind::ClearColorImage: {
        const auto h = ReadValue<ClearColorHdr>(p);
        cb.clearColorImage(h.dst, h.layout, reinterpret_cast<const vk::ClearColorValue*>(&h.color),
                           h.count,
                           Read<vk::ImageSubresourceRange>(p, h.count));
        break;
    }
    case SotcCmdKind::Stop:
    case SotcCmdKind::Count:
        UNREACHABLE_MSG("cmd: bad command kind");
    }
}

/// Appends a transfer: its header, then the regions by value.
template <typename Hdr, typename R>
void PutRegions(SotcCmdState& st, SotcCmdKind kind, Hdr h, vk::ArrayProxy<const R> regions) {
    h.count = regions.size();
    auto* out = static_cast<std::uint8_t*>(
        st.Put(kind, Al(sizeof(Hdr)) + Al(regions.size() * sizeof(R))));
    out = Write(out, &h, 1);
    Write(out, regions.data(), regions.size());
}

} // namespace

int SotcCmdState::ParseMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_GPU_CMD");
        int m = 0;
        if (v && std::string_view{v} == "sync") {
            m = 1;
        } else if (v && std::string_view{v} == "1") {
            m = 2;
        }
        LOG_INFO(Render_Vulkan, "command encoding {}",
                 m == 2   ? "on, recorded by stage C (SOTC_GPU_CMD=1, sotc:GpuCmd)"
                 : m == 1 ? "on, decoded after each record (SOTC_GPU_CMD=sync)"
                          : "off (SOTC_GPU_CMD=0)");
        return m;
    }();
    return mode;
}

// Session 64: C spins this long for the next record before it sleeps (B appends a record
// every ~5 us in gameplay), and B this long for C to finish before it sleeps.
constexpr std::chrono::microseconds kCSpin{50};
constexpr std::chrono::microseconds kDrainSpin{200};

SotcCmdState::~SotcCmdState() {
    if (thread.joinable()) {
        while (!ring.Reserve(static_cast<std::uint32_t>(SotcCmdKind::Stop), 0)) {
            ring.WaitForSpace(kDrainSpin);
        }
        ring.PublishAndWake();
        thread.join();
    }
}

void SotcCmdState::Start() {
    thread = std::thread{[this] { ThreadMain(); }};
}

void SotcCmdState::ThreadMain() {
    Common::SetCurrentThreadName("sotc:GpuCmd");
    std::uint64_t n = 0;
    for (;;) {
        const auto rec = ring.Front();
        if (rec.kind == SotcCmdRing::WrapKind) {
            ring.Sleep(kCSpin); // releases first: a waiting drain ends here
            continue;
        }
        const auto kind = static_cast<SotcCmdKind>(rec.kind);
        if (kind == SotcCmdKind::Stop) {
            ring.Pop();
            ring.Release();
            return;
        }
        // `raw` changes only while C is drained (BeginSession), before the next publish.
        Decode(raw, kind, static_cast<const std::uint8_t*>(rec.payload));
        ring.Pop();
        if ((++n & 63) == 0) {
            ring.Release();
            decoded.store(n, std::memory_order_relaxed);
        }
    }
}

std::string SotcCmdState::StallState() const {
    if (mode != 2) {
        return {};
    }
    const auto r = ring.Snapshot();
    return fmt::format("C: {} commands decoded, ring head {} tail {}, C {}, B {}",
                       decoded.load(std::memory_order_relaxed), r.head, r.tail,
                       r.consumer_sleeping ? "asleep" : "awake",
                       r.producer_waiting ? "waiting on C" : "not waiting on C");
}

void SotcCmdState::Drain() {
    if (mode != 2) {
        DecodeAll();
        return;
    }
    ring.PublishAndWake();
    if (ring.Drained()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    SotcWaitTimer::Where w{SotcWaitTimer::b_where, "C drain"};
    ring.WaitDrained(kDrainSpin);
    ++drain_waits;
    drain_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
}

void SotcCmdState::DecodeAll() {
    ring.Publish();
    for (;;) {
        const auto rec = ring.Front();
        if (rec.kind == SotcCmdRing::WrapKind) {
            break;
        }
        Decode(raw, static_cast<SotcCmdKind>(rec.kind),
               static_cast<const std::uint8_t*>(rec.payload));
        ring.Pop();
    }
    ring.Release();
}

void SotcCmdState::Break(const std::source_location& loc) {
    Drain();
    open = false;
    ++breaks;
    Publish();
    std::scoped_lock lk{break_mutex};
    for (std::size_t i = 0; i < num_break_sites; ++i) {
        BreakSite& s = break_sites[i];
        if (s.line == loc.line() && s.file == loc.file_name()) {
            ++s.count;
            return;
        }
    }
    if (num_break_sites < break_sites.size()) {
        break_sites[num_break_sites++] = {loc.file_name(), loc.line(), 1};
    }
}

void SotcCmdState::FlushSubmit() {
    Drain();
    open = false;
    ++flush_submits;
}

void SotcCmdState::Close() {
    if (mode == 2) {
        // Session 64: C records it; the scope stays open until a drain shuts it.
        ring.PublishAndWake();
    } else {
        Drain();
        open = false;
    }
    if ((scopes & 255) == 0) {
        Publish();
    }
}

void SotcCmdState::OnFull() {
    ++full_drains;
    if (mode == 2) {
        ring.WaitForSpace(kDrainSpin);
        return;
    }
    Drain();
}

std::string SotcCmdState::TakeReport() {
    if (mode == 0) {
        return {};
    }
    const std::array<std::uint64_t, 8> now{
        pub_scopes.load(std::memory_order_relaxed), pub_commands.load(std::memory_order_relaxed),
        pub_bytes.load(std::memory_order_relaxed), pub_breaks.load(std::memory_order_relaxed),
        pub_full_drains.load(std::memory_order_relaxed),
        pub_flush_submits.load(std::memory_order_relaxed),
        pub_drain_waits.load(std::memory_order_relaxed),
        pub_drain_ns.load(std::memory_order_relaxed)};
    std::array<std::uint64_t, 8> d;
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = now[i] - reported[i];
    }
    reported = now;
    std::string sites;
    {
        std::scoped_lock lk{break_mutex};
        auto sorted = break_sites;
        std::sort(sorted.begin(), sorted.begin() + num_break_sites,
                  [](const BreakSite& a, const BreakSite& b) { return a.count > b.count; });
        for (std::size_t i = 0; i < num_break_sites; ++i) {
            const BreakSite& s = sorted[i];
            if (s.count == 0) {
                continue;
            }
            std::string_view file = s.file;
            if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
                file.remove_prefix(slash + 1);
            }
            sites += fmt::format(" {}:{} {}x,", file, s.line, s.count);
        }
        for (std::size_t i = 0; i < num_break_sites; ++i) {
            break_sites[i].count = 0;
        }
    }
    // Session 64: with stage C, breaks and in-scope submits are drains; "waited" counts the
    // ones that found C still recording, and B's time in them.
    return fmt::format("cmd: {} scopes, {} commands ({:.1f} each, {:.0f} bytes each), {} broken "
                       "({:.1f}%), {} full-ring drains, {} in-scope submits, {} drains waited "
                       "{:.1f} ms; breaks by site:{}",
                       d[0], d[1], d[0] ? double(d[1]) / d[0] : 0.0,
                       d[1] ? double(d[2]) / d[1] : 0.0, d[3], d[0] ? 100.0 * d[3] / d[0] : 0.0,
                       d[4], d[5], d[6], d[7] / 1e6, sites);
}

void SotcRecordBeginRendering(vk::CommandBuffer cmdbuf, const RenderState& render_state) {
    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    cmdbuf.beginRendering(rendering_info);
}

// The encoders with variable-size payloads.

void SotcCmdBuffer::beginRendering(const RenderState& state) const {
    if (!st->open) {
        SotcRecordBeginRendering(st->raw, state);
        return;
    }
    st->PutValue(SotcCmdKind::BeginRendering, state);
}

void SotcCmdBuffer::pipelineBarrier2(const vk::DependencyInfo& info) const {
    if (!st->open) {
        st->raw.pipelineBarrier2(info);
        return;
    }
    ASSERT(!info.pNext);
    const BarrierHdr h{u32(VkDependencyFlags(info.dependencyFlags)), info.memoryBarrierCount,
                       info.bufferMemoryBarrierCount, info.imageMemoryBarrierCount};
    const std::size_t size = Al(sizeof(h)) + Al(h.num_memory * sizeof(vk::MemoryBarrier2)) +
                             Al(h.num_buffer * sizeof(vk::BufferMemoryBarrier2)) +
                             Al(h.num_image * sizeof(vk::ImageMemoryBarrier2));
    auto* out = static_cast<std::uint8_t*>(st->Put(SotcCmdKind::PipelineBarrier2, size));
    out = Write(out, &h, 1);
    out = Write(out, info.pMemoryBarriers, h.num_memory);
    out = Write(out, info.pBufferMemoryBarriers, h.num_buffer);
    Write(out, info.pImageMemoryBarriers, h.num_image);
}

void SotcCmdBuffer::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                  std::uint32_t offset, std::uint32_t size,
                                  const void* values) const {
    if (!st->open) {
        st->raw.pushConstants(layout, stages, offset, size, values);
        return;
    }
    const PushConstantsHdr h{layout, stages, offset, size};
    auto* out = static_cast<std::uint8_t*>(
        st->Put(SotcCmdKind::PushConstants, Al(sizeof(h)) + Al(size)));
    out = Write(out, &h, 1);
    std::memcpy(out, values, size);
}

void SotcCmdBuffer::pushDescriptorSetKHR(
    vk::PipelineBindPoint point, vk::PipelineLayout layout, std::uint32_t set,
    vk::ArrayProxy<const vk::WriteDescriptorSet> writes) const {
    if (!st->open) {
        st->raw.pushDescriptorSetKHR(point, layout, set, writes);
        return;
    }
    std::size_t size = Al(sizeof(PushDescHdr));
    for (const auto& w : writes) {
        ASSERT(!w.pNext);
        size += Al(sizeof(WriteHdr));
        if (w.pImageInfo) {
            size += Al(w.descriptorCount * sizeof(vk::DescriptorImageInfo));
        } else if (w.pBufferInfo) {
            size += Al(w.descriptorCount * sizeof(vk::DescriptorBufferInfo));
        } else {
            size += Al(w.descriptorCount * sizeof(vk::BufferView));
        }
    }
    auto* out = static_cast<std::uint8_t*>(st->Put(SotcCmdKind::PushDescriptorSet, size));
    const PushDescHdr h{layout, point, set, writes.size()};
    out = Write(out, &h, 1);
    for (const auto& w : writes) {
        WriteHdr wh{w.dstBinding, w.dstArrayElement, w.descriptorCount, w.descriptorType,
                    InfoKind::TexelView, 0};
        if (w.pImageInfo) {
            wh.kind = InfoKind::Image;
            out = Write(out, &wh, 1);
            out = Write(out, w.pImageInfo, w.descriptorCount);
        } else if (w.pBufferInfo) {
            wh.kind = InfoKind::Buffer;
            out = Write(out, &wh, 1);
            out = Write(out, w.pBufferInfo, w.descriptorCount);
        } else {
            out = Write(out, &wh, 1);
            out = Write(out, w.pTexelBufferView, w.descriptorCount);
        }
    }
}

void SotcCmdBuffer::bindDescriptorSets(vk::PipelineBindPoint point, vk::PipelineLayout layout,
                                       std::uint32_t first, vk::DescriptorSet set,
                                       std::span<const std::uint32_t> dynamic_offsets) const {
    if (!st->open) {
        st->raw.bindDescriptorSets(point, layout, first, 1, &set,
                                   static_cast<u32>(dynamic_offsets.size()),
                                   dynamic_offsets.data());
        return;
    }
    const BindSetHdr h{layout, set, point, first, static_cast<u32>(dynamic_offsets.size())};
    auto* out = static_cast<std::uint8_t*>(
        st->Put(SotcCmdKind::BindDescriptorSet, Al(sizeof(h)) + Al(dynamic_offsets.size_bytes())));
    out = Write(out, &h, 1);
    Write(out, dynamic_offsets.data(), dynamic_offsets.size());
}

void SotcCmdBuffer::setVertexInputEXT(
    vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> bindings,
    vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> attributes) const {
    if (!st->open) {
        st->raw.setVertexInputEXT(bindings, attributes);
        return;
    }
    const VertexInputHdr h{bindings.size(), attributes.size()};
    auto* out = static_cast<std::uint8_t*>(st->Put(
        SotcCmdKind::SetVertexInput,
        Al(sizeof(h)) + Al(h.num_bindings * sizeof(vk::VertexInputBindingDescription2EXT)) +
            Al(h.num_attributes * sizeof(vk::VertexInputAttributeDescription2EXT))));
    out = Write(out, &h, 1);
    out = Write(out, bindings.data(), h.num_bindings);
    Write(out, attributes.data(), h.num_attributes);
}

void SotcCmdBuffer::bindVertexBuffers(std::uint32_t first, std::uint32_t count,
                                      const vk::Buffer* buffers,
                                      const vk::DeviceSize* offsets) const {
    if (!st->open) {
        st->raw.bindVertexBuffers(first, count, buffers, offsets);
        return;
    }
    const VertexBuffersHdr h{first, count};
    auto* out = static_cast<std::uint8_t*>(st->Put(
        SotcCmdKind::BindVertexBuffers,
        Al(sizeof(h)) + Al(count * sizeof(vk::Buffer)) + Al(count * sizeof(vk::DeviceSize))));
    out = Write(out, &h, 1);
    out = Write(out, buffers, count);
    Write(out, offsets, count);
}

void SotcCmdBuffer::bindVertexBuffers2(std::uint32_t first, std::uint32_t count,
                                       const vk::Buffer* buffers, const vk::DeviceSize* offsets,
                                       const vk::DeviceSize* sizes,
                                       const vk::DeviceSize* strides) const {
    if (!st->open) {
        st->raw.bindVertexBuffers2(first, count, buffers, offsets, sizes, strides);
        return;
    }
    ASSERT(sizes && strides);
    const VertexBuffersHdr h{first, count};
    auto* out = static_cast<std::uint8_t*>(
        st->Put(SotcCmdKind::BindVertexBuffers2,
                Al(sizeof(h)) + Al(count * sizeof(vk::Buffer)) +
                    3 * Al(count * sizeof(vk::DeviceSize))));
    out = Write(out, &h, 1);
    out = Write(out, buffers, count);
    out = Write(out, offsets, count);
    out = Write(out, sizes, count);
    Write(out, strides, count);
}

void SotcCmdBuffer::setViewportWithCount(vk::ArrayProxy<const vk::Viewport> viewports) const {
    if (!st->open) {
        st->raw.setViewportWithCount(viewports);
        return;
    }
    const ArrayHdr h{0, viewports.size()};
    auto* out = static_cast<std::uint8_t*>(st->Put(
        SotcCmdKind::SetViewports, Al(sizeof(h)) + Al(h.count * sizeof(vk::Viewport))));
    out = Write(out, &h, 1);
    Write(out, viewports.data(), h.count);
}

void SotcCmdBuffer::setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> scissors) const {
    if (!st->open) {
        st->raw.setScissorWithCount(scissors);
        return;
    }
    const ArrayHdr h{0, scissors.size()};
    auto* out = static_cast<std::uint8_t*>(
        st->Put(SotcCmdKind::SetScissors, Al(sizeof(h)) + Al(h.count * sizeof(vk::Rect2D))));
    out = Write(out, &h, 1);
    Write(out, scissors.data(), h.count);
}

void SotcCmdBuffer::setColorWriteMaskEXT(
    std::uint32_t first, vk::ArrayProxy<const vk::ColorComponentFlags> masks) const {
    if (!st->open) {
        st->raw.setColorWriteMaskEXT(first, masks);
        return;
    }
    const ArrayHdr h{first, masks.size()};
    auto* out = static_cast<std::uint8_t*>(st->Put(
        SotcCmdKind::SetColorWriteMask,
        Al(sizeof(h)) + Al(h.count * sizeof(vk::ColorComponentFlags))));
    out = Write(out, &h, 1);
    Write(out, masks.data(), h.count);
}

void SotcCmdBuffer::SetBool(SotcCmdBool which, bool v) const {
    if (!st->open) {
        switch (which) {
        case SotcCmdBool::DepthTest:
            st->raw.setDepthTestEnable(v);
            break;
        case SotcCmdBool::DepthWrite:
            st->raw.setDepthWriteEnable(v);
            break;
        case SotcCmdBool::DepthBoundsTest:
            st->raw.setDepthBoundsTestEnable(v);
            break;
        case SotcCmdBool::DepthBias:
            st->raw.setDepthBiasEnable(v);
            break;
        case SotcCmdBool::StencilTest:
            st->raw.setStencilTestEnable(v);
            break;
        case SotcCmdBool::PrimitiveRestart:
            st->raw.setPrimitiveRestartEnable(v);
            break;
        case SotcCmdBool::RasterizerDiscard:
            st->raw.setRasterizerDiscardEnable(v);
            break;
        }
        return;
    }
    st->PutValue(SotcCmdKind::SetBool, BoolCmd{which, v ? 1u : 0u});
}

void SotcCmdBuffer::SetStencilU32(SotcCmdStencilU32 which, vk::StencilFaceFlags faces,
                                  std::uint32_t v) const {
    if (!st->open) {
        switch (which) {
        case SotcCmdStencilU32::Reference:
            st->raw.setStencilReference(faces, v);
            break;
        case SotcCmdStencilU32::WriteMask:
            st->raw.setStencilWriteMask(faces, v);
            break;
        case SotcCmdStencilU32::CompareMask:
            st->raw.setStencilCompareMask(faces, v);
            break;
        }
        return;
    }
    st->PutValue(SotcCmdKind::SetStencilU32, StencilU32Cmd{which, faces, v});
}

// Transfers (session 63, step 2b).

void SotcCmdBuffer::copyBuffer(vk::Buffer src, vk::Buffer dst,
                               vk::ArrayProxy<const vk::BufferCopy> regions) const {
    if (!st->open) {
        st->raw.copyBuffer(src, dst, regions);
        return;
    }
    PutRegions(*st, SotcCmdKind::CopyBuffer, BufBufHdr{src, dst, {}, {}, 0, 0}, regions);
}

void SotcCmdBuffer::fillBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size,
                               std::uint32_t value) const {
    if (!st->open) {
        st->raw.fillBuffer(dst, offset, size, value);
        return;
    }
    st->PutValue(SotcCmdKind::FillBuffer, FillBufferCmd{dst, offset, size, value});
}

void SotcCmdBuffer::updateBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size,
                                 const void* data) const {
    if (!st->open) {
        st->raw.updateBuffer(dst, offset, size, data);
        return;
    }
    ASSERT(size <= 64);
    const UpdateBufferHdr h{dst, offset, size};
    auto* out = static_cast<std::uint8_t*>(
        st->Put(SotcCmdKind::UpdateBuffer, Al(sizeof(h)) + Al(size)));
    out = Write(out, &h, 1);
    std::memcpy(out, data, size);
}

void SotcCmdBuffer::copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout layout,
                                      vk::ArrayProxy<const vk::BufferImageCopy> regions) const {
    if (!st->open) {
        st->raw.copyBufferToImage(src, dst, layout, regions);
        return;
    }
    PutRegions(*st, SotcCmdKind::CopyBufferToImage, BufImgHdr{src, dst, {}, layout, 0, 0},
               regions);
}

void SotcCmdBuffer::copyImageToBuffer(vk::Image src, vk::ImageLayout layout, vk::Buffer dst,
                                      vk::ArrayProxy<const vk::BufferImageCopy> regions) const {
    if (!st->open) {
        st->raw.copyImageToBuffer(src, layout, dst, regions);
        return;
    }
    PutRegions(*st, SotcCmdKind::CopyImageToBuffer, ImgBufHdr{src, dst, layout, {}, 0, 0},
               regions);
}

void SotcCmdBuffer::copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                              vk::ImageLayout dst_layout,
                              vk::ArrayProxy<const vk::ImageCopy> regions) const {
    if (!st->open) {
        st->raw.copyImage(src, src_layout, dst, dst_layout, regions);
        return;
    }
    PutRegions(*st, SotcCmdKind::CopyImage, ImgImgHdr{src, dst, src_layout, dst_layout, 0, 0},
               regions);
}

void SotcCmdBuffer::resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                                 vk::ImageLayout dst_layout,
                                 vk::ArrayProxy<const vk::ImageResolve> regions) const {
    if (!st->open) {
        st->raw.resolveImage(src, src_layout, dst, dst_layout, regions);
        return;
    }
    PutRegions(*st, SotcCmdKind::ResolveImage,
               ImgImgHdr{src, dst, src_layout, dst_layout, 0, 0}, regions);
}

void SotcCmdBuffer::clearColorImage(vk::Image dst, vk::ImageLayout layout,
                                    const vk::ClearColorValue& color,
                                    vk::ArrayProxy<const vk::ImageSubresourceRange> ranges) const {
    if (!st->open) {
        st->raw.clearColorImage(dst, layout, color, ranges);
        return;
    }
    PutRegions(*st, SotcCmdKind::ClearColorImage, ClearColorHdr{dst, layout, 0, static_cast<const VkClearColorValue&>(color)}, ranges);
}

} // namespace Vulkan
