// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <utility>
#include <boost/container/small_vector.hpp>
#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "common/unique_function.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
union PM4Header;
struct PM4CmdWaitRegMem;
union Regs;
union CbDbExtent;
struct ComputeProgram;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

class GraphicsPipeline;
class Runtime;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    void Draw(bool is_indexed, u32 index_offset = 0);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// shadow-pc (session 10): see vk_rasterizer.cpp.
    void SotcPatchFlatBuf(const Shader::Info& stage, VideoCore::Buffer& flat_buffer,
                          u64 flat_offset);
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false);
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();
    void SotcGcTick(); // shadow-pc patch
    void OnFence();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId();
#endif

    /// shadow-pc (session 43, two-stage GPU pipeline, docs/next/gpu-pipe.md): the registers the
    /// recording half (stage B) reads. With SOTC_GPU_PIPE off they are Liverpool's; with it on,
    /// B's copy, built from the packets' register deltas.
    const AmdGpu::Regs& Regs() const;
    const AmdGpu::CbDbExtent& CbExtent(u32 cb) const;
    const AmdGpu::CbDbExtent& DbExtent() const;
    const AmdGpu::ComputeProgram& CsRegs() const;

    /// shadow-pc (session 44): with SOTC_GPU_PIPE=1, stage A (Liverpool's thread) waits here
    /// until stage B (sotc:GpuRecord) has run every packet handed over, before it touches B's
    /// state (caches, scheduler, presenter). A no-op on any other thread or mode.
    /// Session 46: B also runs commands between packets, so SotcPipeDrain returns an owner
    /// guard: until it is destroyed B runs nothing (it holds SotcPipe::state_mutex). Hold it
    /// for as long as A touches B's state. SotcPipeWait only waits (A then reads guest memory
    /// B wrote, not B's state).
    enum class SotcDrainReason : u32 {
        Fence, Submit, Flush, Finish, Gds, Dma, Marker, Command, Flip, Idle, CpuCopy, Fault,
        WaitFence, Pending, Count
    };
    class [[nodiscard]] SotcPipeOwner {
    public:
        SotcPipeOwner() = default;
        explicit SotcPipeOwner(Rasterizer* r) : rasterizer{r} {}
        SotcPipeOwner(const SotcPipeOwner&) = delete;
        SotcPipeOwner& operator=(const SotcPipeOwner&) = delete;
        SotcPipeOwner(SotcPipeOwner&& o) noexcept : rasterizer{std::exchange(o.rasterizer, nullptr)} {}
        ~SotcPipeOwner() {
            if (rasterizer) {
                rasterizer->SotcPipeRelease();
            }
        }

    private:
        Rasterizer* rasterizer = nullptr;
    };
    SotcPipeOwner SotcPipeDrain(SotcDrainReason reason);
    void SotcPipeWait(SotcDrainReason reason);
    /// shadow-pc (session 46): with SOTC_GPU_PIPE=1, queues a Liverpool command to stage B
    /// (run between packets) and returns true; otherwise false and leaves `func` alone.
    bool SotcPipeCommand(Common::UniqueFunction<void>&& func);

    /// shadow-pc (session 45): with SOTC_GPU_PIPE=1, stage B runs these in stream order and the
    /// calls return true; otherwise false and the caller runs them as before. SotcPipeFence
    /// copies the fence packet (EOP, EOS, ReleaseMem; B runs Liverpool::SotcRunFence on the
    /// copy) and notes its label write for SotcPipeWaitMet.
    enum class SotcPipeEvent : u32 { SubmitDone, Idle, Flip };
    bool SotcPipeDefer(SotcPipeEvent event);
    bool SotcPipeFence(const AmdGpu::PM4Header* header, u32 pipe_id);
    /// A WaitRegMem on memory a pending fence writes: true when the newest pending write
    /// passes the test. When it fails (or A can't predict it), drains so the poll reads memory.
    bool SotcPipeWaitMet(const AmdGpu::PM4CmdWaitRegMem& wait);
    /// shadow-pc (session 44): the fault handler's "GPU-side thread" test (assume_locks: handle
    /// the fault inline instead of queuing it to Liverpool's thread). Liverpool's thread (after
    /// a drain, with SOTC_GPU_PIPE=1) and stage B's.
    /// Session 46: Liverpool's thread is GPU-side only while it owns B's state (inside a
    /// drain's guard); otherwise its faults are queued to B like a guest thread's.
    bool SotcFaultOnGpuSide();
    /// Session 46: the fault handler reports how long a fault it queued to B took (A only).
    void SotcNoteQueuedFault(std::chrono::steady_clock::time_point start);
    /// shadow-pc (session 47): A's reads of guest memory that packets B hasn't run will write
    /// (docs/next/gpu-pipe.md, step 4). SotcPipeOverlay: the values of [addr, addr + size)
    /// when a pending CPU copy writes all of it (its overlay, valid until A's next packet);
    /// null when guest memory is current, after waiting for B if a pending GPU write (or a
    /// partial overlay) touches the range. SotcPipeMergeRead: patches dst, which holds guest
    /// memory read at addr, with the pending overlays (waits and re-reads under a GPU write).
    /// No-ops (null) on other threads and with the pipe off or in sync mode.
    const u8* SotcPipeOverlay(VAddr addr, u64 size);
    void SotcPipeMergeRead(VAddr addr, u8* dst, u64 size);

private:
    /// shadow-pc (session 43): what FilterDraw found in the registers (stage A decides, B runs it).
    struct SotcDrawFilter {
        enum class Kind : u32 { Draw, FastClear, FmaskDecompress, Resolve, PrimNone, DsCopy };
        Kind kind = Kind::Draw;
        bool depth_copy = false;
        bool stencil_copy = false;
    };
    static SotcDrawFilter ClassifyDraw(const AmdGpu::Regs& regs);
    void RunDrawFilter(const SotcDrawFilter& filter);

    /// The recording halves of Draw, DrawIndirect, DispatchDirect and DispatchIndirect: what
    /// follows pipeline selection (stage B).
    void RecordDraw(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset);
    void RecordDrawIndirect(const GraphicsPipeline* pipeline, bool is_indexed,
                            VAddr arg_address, u32 offset, u32 stride, u32 max_count,
                            VAddr count_address);
    void RecordDispatchDirect(const ComputePipeline* pipeline);
    void RecordDispatchIndirect(const ComputePipeline* pipeline, VAddr address, u32 offset,
                                u32 size);

    struct SotcPipe;
    struct SotcPipeArgs;
    void SotcPipeSubmit(SotcPipeArgs args);
    void SotcPipeRun();
    void SotcPipeRunPacket(std::span<const u8> bytes);
    void SotcPipeThread();
    void SotcPipeReport();
    void SotcPipeRelease();
    void SotcPipeRunCommands();
    void SotcPipeNoteWrites(const Pipeline* pipeline, u64 seq, VAddr skip);
    void SotcPipeVerifyWalk(std::span<const Shader::UdSnapshot> snaps);
    std::unique_ptr<SotcPipe> sotc_pipe; ///< null: SOTC_GPU_PIPE off

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    RenderState BeginRenderingFull(const GraphicsPipeline* pipeline);
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();

    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    bool BindResources(const Pipeline* pipeline);
    /// shadow-pc (session 21, SOTC_CPU_COPY): see sotc_cpu_copy.h.
    void SotcCpuCopy(const Shader::Info& cs, const AmdGpu::ComputeProgram& cs_program);
    /// Session 47: SotcCpuCopy in two halves, so stage A computes and stage B commits (in
    /// stream order). Compute reads the control and source words (through A's overlays) and
    /// fills `words` with the destination's values; false if the copy is left to the GPU.
    /// Commit writes them into guest memory where the pages are resident and tells the buffer
    /// cache, before the dispatch's BindResources.
    struct SotcCopyResult {
        VAddr src = 0;
        VAddr dst = 0;
        u64 src_bytes = 0;
        u64 dst_vsize = 0; ///< the destination V#'s size (the binding BindBuffers obtains)
        u32 count = 0;     ///< destination dwords
    };
    bool SotcCpuCopyCompute(const Shader::Info& cs, const AmdGpu::ComputeProgram& cs_program,
                            SotcCopyResult& out, std::vector<u32>& words);
    void SotcCpuCopyCommit(const SotcCopyResult& r, std::span<const u32> words);
    /// Session 47: B's precomputed copy for the dispatch being recorded (null: compute here).
    const SotcCopyResult* sotc_copy_commit = nullptr;
    std::span<const u32> sotc_copy_words;

    void BindVertexBuffers(const GraphicsPipeline* pipeline);
    void BindIndexBuffer(u32 index_offset = 0);

    void ResetBindings(bool is_compute);

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    struct ImageBinding {
        VideoCore::ImageId image_id;
        VideoCore::TextureCache::ImageDesc desc;
    };
    std::array<ImageBinding, Shader::NUM_IMAGES> image_bindings;
    std::array<ImageBinding, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    ImageBinding db_desc;

    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;

    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        vk::AccessFlags2 src_access;
    };
    std::vector<BoundBuffer> bound_buffers;
    std::vector<VideoCore::ImageId> bound_images;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;

    bool attachment_feedback_loop{};
    bool needs_barrier{};

    /// shadow-pc patch (session 38, after bbport's RenderStateMemo): ~93% of draws continue the
    /// render pass the previous draw opened with the same targets. A draw whose inputs match the
    /// last remembered state, while the scheduler still has that pass open, reuses the state
    /// instead of redoing the target lookups, uploads checks, transitions and meta reads.
    /// Anything recorded outside the pass (copies, dispatches, uploads, barriers) ends it, so the
    /// open pass proves that no layout changed. SOTC_RS_MEMO=0 turns it off.
    struct SotcBeginSignature {
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> ids{};
        std::array<VideoCore::ImageViewInfo, AmdGpu::NUM_COLOR_BUFFERS + 1> views{};
        std::array<VAddr, AmdGpu::NUM_COLOR_BUFFERS + 1> meta_addresses{}; ///< CMASK, HTILE
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
        u32 mrt_mask = 0;
        u32 depth_control = 0;
        bool depth_valid = false;
        bool stencil_valid = false;
        std::pair<u64, u64> epoch{}; ///< TextureCache::SotcRenderStateEpoch
        bool operator==(const SotcBeginSignature&) const = default;
    };
    SotcBeginSignature SotcMakeBeginSignature(const GraphicsPipeline* pipeline) const;
    struct SotcBeginMemo {
        bool valid = false;
        SotcBeginSignature signature;
        RenderState state;
        /// The images' backings when remembered (SetBackingSamples swaps them).
        std::array<const void*, AmdGpu::NUM_COLOR_BUFFERS + 1> backings{};
    } sotc_begin_memo;
    u64 sotc_begin_hits = 0, sotc_begin_misses = 0;

    /// shadow-pc patch (session 40, after bbport's texture-set memo): ~82% of graphics stages
    /// and ~92% of compute stages bind the same T# words as the last time they were bound, and
    /// nothing registered or unregistered an image in between. BindTextures' first pass (the
    /// ImageDesc construction and the FindImage lookups) then gives the same result, so a stage
    /// remembers it. A hit only redoes what the lookups did to the images (access tick, LRU), the
    /// depth redirect and the binding flags; the second pass (rebinds, views, transitions,
    /// descriptors) always runs. Kept only when every lookup was a cacheable perfect match.
    /// Direct-mapped by stage, GPU thread only. SOTC_TEX_MEMO=0 turns it off.
    struct SotcTexMemo {
        const Shader::Info* stage = nullptr;
        u64 pgm_hash = 0;
        u64 generation = 0;
        std::vector<AmdGpu::Image> sharps;        ///< stage.images' T# words, in order
        std::vector<ImageBinding> bindings;       ///< after the first pass, before the redirect
        std::vector<u8> written;                  ///< per binding: image_desc.is_written
        boost::container::small_vector<u32, 8> array_sizes;
    };
    static constexpr u32 SotcTexMemoBits = 10;
    std::vector<SotcTexMemo> sotc_tex_memo{1u << SotcTexMemoBits};
    u64 sotc_tex_hits = 0, sotc_tex_misses = 0;
};

} // namespace Vulkan
