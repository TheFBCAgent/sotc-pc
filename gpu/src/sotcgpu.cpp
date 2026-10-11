// SPDX-License-Identifier: GPL-2.0-or-later
// sotcgpu: gpu/include/sotcgpu.h on top of shadPS4's AmdGpu::Liverpool (command
// processor), Vulkan::Presenter (swapchain) and Vulkan::Rasterizer.
//
// The flip path is a port of shadPS4's core/libraries/videoout/driver.cpp (14ada48):
// - sotcgpu_flip(after_gpu_work=false): sent to the GPU thread (Liverpool::SendCommand),
//   like VideoOutDriver::SubmitFlip for non-EOP flips.
// - sotcgpu_flip(after_gpu_work=true): a one-shot GfxFlip IRQ, raised by Liverpool when it
//   reaches the PatchedFlip NOP that GnmDriver writes into the DCB of SubmitAndFlip
//   (sceVideoOutSubmitEopFlip).
// - Either way the GPU thread prepares the frame (Presenter::PrepareFrame) and queues it;
//   the present thread presents one request per vblank (60 Hz) and then calls
//   host->flip_done.
//
// Threads: Liverpool's command processor ("GpuCommandProcessor") runs submissions,
// RELEASE_MEM/EOP interrupts (host->eop_interrupt) and `done` callbacks; the present
// thread ("sotcgpu:Present") presents and calls host->flip_done.

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <stb_image_write.h>

#include "common/assert.h"
#include "common/debug.h" // Tracy zones and FrameMark (session 18; stubs unless SOTC_TRACY)
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/buffer.h"
#include "core/memory.h"
#include "core/libraries/videoout/driver.h"
#include "core/platform.h"
#include "core/signals.h"
#include "emulator.h"
#include "sdl_window.h"
#include "sotcgpu.h"
#include "sotcgpu_internal.h"
#include "sotc_debug.h"
#include "sotc_srt_load.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

#ifdef _WIN32
#include <windows.h>
#undef MemoryBarrier // winnt.h macro; clashes with vk::MemoryBarrier
#else
#include <dlfcn.h>
#include <ucontext.h>
#endif

namespace {

using namespace Libraries::VideoOut;

struct FlipRequest {
    Vulkan::Frame* frame;
    s32 index;
    s64 flip_arg;
};

struct ScreenshotJob {
    std::string path;
    bool done = false;
    bool ok = false;
};

struct Gpu {
    sotcgpu_host host{};

    std::unique_ptr<Frontend::WindowSDL> window;
    std::unique_ptr<AmdGpu::Liverpool> liverpool;
    std::unique_ptr<Vulkan::Presenter> presenter; // windowed
    // Headless (no window): the same pieces Presenter would own, minus the swapchain.
    std::unique_ptr<Vulkan::Instance> instance;
    std::unique_ptr<Vulkan::Scheduler> scheduler;
    std::unique_ptr<Vulkan::Runtime> runtime;
    std::unique_ptr<Vulkan::Rasterizer> headless_rasterizer;
    Vulkan::Rasterizer* rasterizer{};

    VideoOutPort vo_port{};

    // Display buffers (VideoOut slots).
    std::mutex buffers_mutex;
    std::array<VideoOutBuffer, MaxDisplayBuffers> slots{};
    std::array<BufferAttributeGroup, MaxDisplayBufferGroups> groups{};

    // Compute queues known to Liverpool::asc_queues (index = gnm vqid - 1).
    std::mutex asc_mutex;
    u32 num_asc_queues = 0;
    std::array<u32, AmdGpu::Liverpool::NumComputeRings + 1> asc_read_ptrs{};

    // Present thread.
    std::jthread present_thread;
    std::mutex req_mutex;
    std::condition_variable_any req_cv;
    std::queue<FlipRequest> requests;
    bool any_frame_presented = false;

    // Screenshot.
    std::mutex shot_mutex;
    std::condition_variable shot_cv;
    ScreenshotJob* shot_job = nullptr;
};

Gpu* g_gpu = nullptr;
u64 ResolveSrtTable(u64 ptr, u64 bytes, u64 table); // the SRT walker's table resolver (session 10)

void Log(int level, const std::string& msg) {
    SotcGpu::HostLog(level, msg.c_str());
}

// ----------------------------------------------------------------- Vulkan probe

/// True when a Vulkan loader exists and reports at least one physical device. Run
/// before shadPS4's Instance, which asserts instead of failing softly.
bool ProbeVulkan() {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) {
        Log(1, "gpu: vulkan-1.dll not found");
        return false;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib) {
        Log(1, "gpu: libvulkan not found");
        return false;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
    if (!gipa) {
        return false;
    }
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) {
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "sotcgpu-probe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst{};
    if (create(&ci, nullptr, &inst) != VK_SUCCESS) {
        Log(1, "gpu: vkCreateInstance failed");
        return false;
    }
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        gipa(inst, "vkEnumeratePhysicalDevices"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    uint32_t count = 0;
    const bool ok = enumerate && enumerate(inst, &count, nullptr) == VK_SUCCESS && count > 0;
    if (destroy) {
        destroy(inst, nullptr);
    }
    if (!ok) {
        Log(1, "gpu: no Vulkan physical device");
    }
    return ok;
}

// ----------------------------------------------------------------- VRAM report
// Workstream C (docs/next/c.md): a "[vram]" line every 10 s, and one right before any Vulkan
// failure asserts (Vulkan::SotcDiag::report, called by Check and the allocation hooks in
// buffer.cpp, buffer_cache.cpp and image.cpp). Diagnostic only.

std::atomic<const Vulkan::Instance*> g_vram_instance{nullptr};
std::jthread g_vram_thread;
u32 g_vram_max_allocs = 0;

void VramReport(const char* why) {
    namespace D = Vulkan::SotcDiag;
    constexpr u64 MiB = 1ull << 20;
    const bool periodic = std::strcmp(why, "periodic") == 0;
    std::string heaps;
    if (const Vulkan::Instance* inst = g_vram_instance.load(std::memory_order_acquire)) {
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        vk::PhysicalDeviceMemoryProperties2 props{
            .pNext = inst->CanReportMemoryUsage() ? &budget : nullptr};
        inst->GetPhysicalDevice().getMemoryProperties2(&props);
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> vma{};
        vmaGetHeapBudgets(inst->GetAllocator(), vma.data());
        const auto& mp = props.memoryProperties;
        for (u32 i = 0; i < mp.memoryHeapCount; ++i) {
            const bool local = bool(mp.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal);
            heaps += fmt::format(" | heap{}{} usage={} budget={} size={} vma={} MiB", i,
                                 local ? "(local)" : "", budget.heapUsage[i] / MiB,
                                 budget.heapBudget[i] / MiB, mp.memoryHeaps[i].size / MiB,
                                 vma[i].statistics.blockBytes / MiB);
        }
        if (!inst->CanReportMemoryUsage()) {
            heaps += " | (no VK_EXT_memory_budget: usage/budget are 0)";
        }
    }
    // Workstream T: image GC totals (TextureCache::GarbageCollectImages) and its breakdown of
    // the live images, refreshed by the GPU thread every ~10 s.
    static constexpr const char* kGcLevel[] = {"idle", "normal", "pressured", "critical",
                                               "emergency"};
    // Session 5: arena memory inside live images' guest ranges (resident twice), refreshed
    // with the breakdown.
    const u64 arena_ranges = D::arena_ranges_bytes.load();
    const u64 arena_images = D::arena_image_bytes.load();
    std::string breakdown;
    {
        std::scoped_lock lk{D::gc_breakdown_mutex};
        breakdown = D::gc_breakdown;
    }
    Log(periodic ? 2 : 0,
        fmt::format("[vram] {}: arena resident={} MiB in {} chunks / {} MiB ({} allocations, "
                    "device limit {}), {} arena buffers; images {} live, {} MiB; image gc {}: "
                    "freed {} (stale {}, downloaded {}), skipped {}; emergency gc {} runs, freed "
                    "{} / {} MiB, {} failed retries; arena ranges {} MiB, {} MiB ({}%) inside "
                    "live images{}",
                    why, D::arena_resident_bytes.load() / MiB, D::arena_chunks.load(),
                    D::arena_chunk_bytes.load() / MiB, D::arena_allocations.load(),
                    g_vram_max_allocs, D::arena_buffers.load(), D::image_count.load(),
                    D::image_bytes.load() / MiB, kGcLevel[std::min<u64>(D::gc_level.load(), 4)],
                    D::gc_freed.load(), D::gc_freed_stale.load(), D::gc_downloaded.load(),
                    D::gc_skipped.load(), D::gc_emergency_runs.load(),
                    D::gc_emergency_freed.load(), D::gc_emergency_bytes.load() / MiB,
                    D::gc_emergency_failed.load(), arena_ranges / MiB, arena_images / MiB,
                    arena_ranges ? arena_images * 100 / arena_ranges : 0, heaps));
    if (!breakdown.empty()) {
        Log(periodic ? 2 : 0, "[vram] images: " + breakdown);
    }
    // Session 23: the staging pool (StagingBufferPool::PublishSotcStats).
    std::string staging;
    {
        std::scoped_lock lk{D::staging_mutex};
        staging = D::staging_breakdown;
    }
    if (!staging.empty()) {
        Log(periodic ? 2 : 0, "[vram] staging: " + staging);
    }
}

void VramStart(const Vulkan::Instance& inst) {
    g_vram_max_allocs = inst.GetPhysicalDevice().getProperties().limits.maxMemoryAllocationCount;
    g_vram_instance.store(&inst, std::memory_order_release);
    VramReport("start");
    g_vram_thread = std::jthread{[](std::stop_token st) {
        Common::SetCurrentThreadName("sotcgpu:Vram");
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lk{m};
        while (!cv.wait_for(lk, st, std::chrono::seconds(10), [] { return false; }) &&
               !st.stop_requested()) {
            VramReport("periodic");
        }
    }};
}

void VramStop() {
    g_vram_thread = {}; // requests stop and joins
    g_vram_instance.store(nullptr, std::memory_order_release);
}

// ----------------------------------------------------------------- screenshot

/// Reads `frame` (in VK_IMAGE_LAYOUT_GENERAL, as before Presenter::Present) back through
/// an RGBA8 blit and writes it as a PNG. Runs on the present thread.
bool CaptureFrame(Vulkan::Frame* frame, const std::string& path) {
    Gpu& g = *g_gpu;
    const Vulkan::Instance& inst = g.rasterizer->GetRuntime().GetInstance();
    const vk::Device device = inst.GetDevice();
    const u32 width = frame->width, height = frame->height;
    if (width == 0 || height == 0) {
        LOG_WARNING(Render, "screenshot: frame has no size");
        return false;
    }

    // Wait until the frame's own rendering is done.
    const vk::SemaphoreWaitInfo wait_info{
        .semaphoreCount = 1,
        .pSemaphores = &frame->ready_semaphore,
        .pValues = &frame->ready_tick,
    };
    if (device.waitSemaphores(wait_info, std::numeric_limits<u64>::max()) !=
        vk::Result::eSuccess) {
        LOG_WARNING(Render, "screenshot: waiting for the frame failed");
        return false;
    }

    // Intermediate RGBA8 image (the blit converts whatever the swapchain format is).
    const vk::ImageCreateInfo image_ci{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
    };
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage raw_image{};
    VmaAllocation image_alloc{};
    const VkImageCreateInfo raw_ci = static_cast<VkImageCreateInfo>(image_ci);
    if (vmaCreateImage(inst.GetAllocator(), &raw_ci, &alloc_ci, &raw_image, &image_alloc,
                       nullptr) != VK_SUCCESS) {
        LOG_WARNING(Render, "screenshot: no {}x{} RGBA8 image", width, height);
        return false;
    }
    const vk::Image rgba{raw_image};
    VideoCore::Buffer readback{inst, 0, u64(width) * height * 4,
                               VideoCore::MemoryType::HostCached};

    auto pool = device.createCommandPool({
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = inst.GetGraphicsQueueFamilyIndex(),
    });
    bool ok = false;
    if (pool.result == vk::Result::eSuccess) {
        auto cmdbufs = device.allocateCommandBuffers({
            .commandPool = pool.value,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        if (cmdbufs.result == vk::Result::eSuccess) {
            const vk::CommandBuffer cmd = cmdbufs.value[0];
            (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            const vk::ImageMemoryBarrier to_dst{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = rgba,
                .subresourceRange = range,
            };
            const vk::MemoryBarrier frame_ready{
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            };
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                vk::PipelineStageFlagBits::eTransfer, {}, frame_ready, {},
                                to_dst);
            const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            const vk::ImageBlit blit{
                .srcSubresource = layers,
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(width), s32(height), 1}},
                .dstSubresource = layers,
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(width), s32(height), 1}},
            };
            cmd.blitImage(frame->image, vk::ImageLayout::eGeneral, rgba,
                          vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
            const vk::ImageMemoryBarrier to_src{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = rgba,
                .subresourceRange = range,
            };
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_src);
            const vk::BufferImageCopy copy{
                .imageSubresource = layers,
                .imageExtent = {width, height, 1},
            };
            cmd.copyImageToBuffer(rgba, vk::ImageLayout::eTransferSrcOptimal, readback.Handle(),
                                  copy);
            const vk::MemoryBarrier to_host{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eHostRead,
            };
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {}, to_host, {}, {});
            (void)cmd.end();

            auto fence = device.createFence({});
            if (fence.result == vk::Result::eSuccess) {
                const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
                vk::Result res;
                {
                    std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
                    res = inst.GetGraphicsQueue().submit(submit, fence.value);
                }
                if (res == vk::Result::eSuccess &&
                    device.waitForFences(fence.value, true, std::numeric_limits<u64>::max()) ==
                        vk::Result::eSuccess) {
                    std::vector<u8> pixels(readback.mapped_data.begin(),
                                           readback.mapped_data.begin() + u64(width) * height * 4);
                    for (size_t i = 3; i < pixels.size(); i += 4) {
                        pixels[i] = 255;
                    }
                    FILE* f = std::fopen(path.c_str(), "wb");
                    if (f) {
                        const auto write = [](void* ctx, void* data, int size) {
                            std::fwrite(data, 1, size, static_cast<FILE*>(ctx));
                        };
                        ok = stbi_write_png_to_func(write, f, int(width), int(height), 4,
                                                    pixels.data(), int(width) * 4) != 0;
                        ok = (std::fclose(f) == 0) && ok;
                        if (!ok) {
                            LOG_WARNING(Render, "screenshot: writing {} failed", path);
                        }
                    } else {
#ifdef _WIN32
                        const unsigned long win_error = GetLastError();
#else
                        const unsigned long win_error = 0;
#endif
                        LOG_WARNING(Render, "screenshot: cannot open {} ({}, os error {})", path,
                                    std::strerror(errno), win_error);
                    }
                } else {
                    LOG_WARNING(Render, "screenshot: submit or fence wait failed ({})",
                             vk::to_string(res));
                }
                device.destroyFence(fence.value);
            }
        }
        device.destroyCommandPool(pool.value);
    }
    vmaDestroyImage(inst.GetAllocator(), raw_image, image_alloc);
    return ok;
}

/// Present thread side of a screenshot: capture `frame` if a job is waiting.
void ServeScreenshot(Vulkan::Frame* frame, s32 index = -2) {
    Gpu& g = *g_gpu;
    std::unique_lock lk{g.shot_mutex};
    if (!g.shot_job) {
        return;
    }
    ScreenshotJob* job = g.shot_job;
    g.shot_job = nullptr;
    lk.unlock();
    const bool ok = frame && CaptureFrame(frame, job->path);
    LOG_INFO(Render, "screenshot {} of buffer {}", job->path, index);
    lk.lock();
    job->ok = ok;
    job->done = true;
    g.shot_cv.notify_all();
}

// ----------------------------------------------------------------- flips

/// GPU thread: prepare the frame for a flip and queue it for the present thread
/// (VideoOutDriver::SubmitFlipInternal).
void SubmitFlipInternal(s32 index, s64 flip_arg) {
    FrameMark; // Tracy: one guest frame per flip, on the GPU thread
    ZoneScopedNC("SubmitFlip", 0x2a9d8f);
    Gpu& g = *g_gpu;
    SotcDebug::OnFlip(*g.rasterizer); // shadow-pc debug: SOTC_DUMP_RT frame boundary
    Vulkan::Frame* frame = nullptr;
    if (g.presenter) {
        if (index < 0) {
            frame = g.presenter->PrepareBlankFrame(false);
        } else {
            BufferAttributeGroup group{};
            VAddr address = 0;
            {
                std::scoped_lock lk{g.buffers_mutex};
                const auto& slot = g.slots[index];
                if (slot.group_index >= 0) {
                    group = g.groups[slot.group_index];
                    address = slot.address_left;
                }
            }
            if (address == 0) {
                LOG_ERROR(Lib_VideoOut, "flip of unregistered buffer {}", index);
                frame = g.presenter->PrepareBlankFrame(false);
            } else {
                frame = g.presenter->PrepareFrame(group, address);
            }
        }
    }
    {
        std::scoped_lock lk{g.req_mutex};
        g.requests.push({frame, index, flip_arg});
        g.req_cv.notify_one();
    }
    // One texture-GC tick per flip, on the GPU thread at the frame boundary. shadPS4 runs the GC
    // from OnSubmit, which needs every queue drained; during SotC's cinematics and gameplay the
    // next frame is always queued, so the GC (nearly) stopped and VRAM ran out at the gameplay
    // load. Image destruction is deferred to GPU completion (DeleteImage), so this is safe.
    g.rasterizer->SotcGcTick();
}

/// sceVideoOutSetFlipRate; may be set before sotcgpu_init.
std::atomic<s32> g_flip_rate{0};

void PresentThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("sotcgpu:Present");
    Gpu& g = *g_gpu;
    const bool windowed = g.presenter != nullptr;
    const auto vblank =
        std::chrono::nanoseconds(1'000'000'000 / EmulatorSettings.GetVblankFrequency());
    auto next = std::chrono::steady_clock::now();
    u64 vblank_count = 0;

    while (!stoken.stop_requested()) {
        std::optional<FlipRequest> req;
        {
            std::unique_lock lk{g.req_mutex};
            if (windowed) {
                // One flip per vblank, as shadPS4's VideoOutDriver::PresentThread.
                next += vblank;
                const auto now = std::chrono::steady_clock::now();
                if (next < now) {
                    next = now;
                }
                g.req_cv.wait_until(lk, stoken, next, [] { return false; });
            } else {
                g.req_cv.wait(lk, stoken, [&] { return !g.requests.empty(); });
            }
            if (stoken.stop_requested()) {
                break;
            }
            // At flip rate r a flip waits for a vblank where count % (r + 1) == 0, as
            // shadPS4's VideoOutDriver::PresentThread.
            const u64 period = u64(g_flip_rate.load(std::memory_order_relaxed)) + 1;
            const bool flip_vblank = !windowed || vblank_count++ % period == 0;
            if (flip_vblank && !g.requests.empty()) {
                req = g.requests.front();
                g.requests.pop();
            }
        }

        if (req) {
            if (req->frame) {
                ServeScreenshot(req->frame, req->index);
                g.presenter->Present(req->frame);
                g.any_frame_presented = true;
            }
            if (g.host.flip_done) {
                g.host.flip_done(req->index, req->flip_arg);
            }
        } else if (windowed && g.any_frame_presented) {
            // Nothing to flip: only redraw when a screenshot is waiting for a frame.
            bool want_shot;
            {
                std::scoped_lock lk{g.shot_mutex};
                want_shot = g.shot_job != nullptr;
            }
            if (want_shot) {
                if (Vulkan::Frame* frame = g.presenter->PrepareLastFrame()) {
                    ServeScreenshot(frame);
                    g.presenter->Present(frame, true);
                }
            }
        }
    }
}

// ----------------------------------------------------------------- faults

bool DispatchFault(u64 addr, bool is_write, u64 rip) {
#ifdef _WIN32
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = is_write ? 1 : 0;
    record.ExceptionInformation[1] = static_cast<ULONG_PTR>(addr);
    CONTEXT context{};
    context.Rip = rip; // the SRT walker handler patches the instruction at Rip
    EXCEPTION_POINTERS pointers{&record, &context};
    void* ctx = &pointers;
#else
    ucontext_t uc{};
    uc.uc_mcontext.gregs[REG_ERR] = is_write ? 2 : 0;
    uc.uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(rip);
    void* ctx = &uc;
#endif
    return Core::Signals::Instance()->DispatchAccessViolation(ctx, reinterpret_cast<void*>(addr));
}

} // namespace

// =================================================================== contract

extern "C" {

int sotcgpu_init(const sotcgpu_host* host, const sotcgpu_config* cfg) {
    if (g_gpu || !host || !cfg) {
        return -1;
    }
    auto gpu = std::make_unique<Gpu>();
    gpu->host = *host;
    SotcGpu::g_host = &gpu->host;
    SotcGpu::g_neo = cfg->neo;
    auto& settings = SotcGpuGetSettings();
    if (cfg->cache_dir && *cfg->cache_dir) {
        SotcGpu::g_user_dir = cfg->cache_dir;
        // shadPS4's on-disk pipeline cache (P1): <cache_dir>/pipelines-vN/CUSA08809/, one file per
        // shader binary, shader meta and pipeline key, written by its IO thread as each pipeline
        // is first compiled (so a killed run keeps them); WarmUp recompiles them all at start.
        // Bump the version when a local shader_recompiler patch changes the generated SPIR-V.
        // On by default since session 21 (SOTC_GPU_PIPELINE_CACHE=0 turns it off): the warm-up
        // crash of sessions 3-4 is gone; 1219 pipelines preload in ~0.9 s and the 5 s stall going
        // into gameplay shrank to ~1 s.
        constexpr int kPipelineCacheVersion = 8; // 8: ReadLane past the workgroup on wave64 (session 28)
        const char* pc = std::getenv("SOTC_GPU_PIPELINE_CACHE");
        settings.pipeline_cache = !(pc && *pc == '0');
        Common::FS::SetUserPath(Common::FS::PathType::CacheDir,
                                SotcGpu::g_user_dir /
                                    fmt::format("pipelines-v{}", kPipelineCacheVersion));
    }
    settings.vkvalidation = cfg->validation;
    // SotC computes its per-view constants (camera matrices) on the GPU, and the SRT walker
    // reads them on the CPU at draw time: without readbacks every scene draw got a stale
    // matrix and only effects rendered. Precise read-protects GPU-written pages so the
    // CPU sees the GPU's data. SOTC_GPU_READBACKS=0|1|2 (off, relaxed, precise).
    settings.readbacks_mode = GpuReadbacksMode::Precise;
    if (const char* rb = std::getenv("SOTC_GPU_READBACKS"); rb && *rb >= '0' && *rb <= '2') {
        settings.readbacks_mode = u32(*rb - '0');
    }
    settings.skip_shaders.clear();
    if (const char* skip = std::getenv("SOTC_GPU_SKIP")) {
        for (const char* p = skip; *p;) {
            char* end = nullptr;
            const u64 hash = std::strtoull(p, &end, 16);
            if (end == p) {
                break;
            }
            settings.skip_shaders.push_back(hash);
            LOG_WARNING(Render, "SOTC_GPU_SKIP: skipping draws with shader {:#x}", hash);
            p = *end == ',' ? end + 1 : end;
        }
    }
    SotcDebug::Init(); // SOTC_GPU_DUMP, SOTC_GPU_STATS, SOTC_DUMP_RT
    Core::Emulator::FillElfInfo();

    if (!ProbeVulkan()) {
        SotcGpu::g_host = nullptr;
        return -1;
    }

    g_gpu = gpu.get();
    Vulkan::SotcDiag::report = VramReport;
    Shader::SetSrtTableResolver(&ResolveSrtTable); // session 10: fault-free walker table reads
    try {
        for (auto& slot : gpu->slots) {
            slot.group_index = -1;
        }
        gpu->liverpool = std::make_unique<AmdGpu::Liverpool>();
        gpu->liverpool->SetVoPort(&gpu->vo_port);

        if (cfg->sdl_window) {
            gpu->window = std::make_unique<Frontend::WindowSDL>(
                static_cast<SDL_Window*>(cfg->sdl_window), s32(cfg->width), s32(cfg->height));
            gpu->presenter =
                std::make_unique<Vulkan::Presenter>(*gpu->window, gpu->liverpool.get());
            gpu->rasterizer = &gpu->presenter->GetRasterizer();
        } else {
            // shadPS4's (bool, bool) Instance constructor only enumerates devices; the
            // window one with a headless WindowSDL creates the device as well.
            gpu->window = std::make_unique<Frontend::WindowSDL>(nullptr, s32(cfg->width),
                                                                s32(cfg->height));
            gpu->instance = std::make_unique<Vulkan::Instance>(
                *gpu->window, settings.GetGpuId(), cfg->validation, false);
            gpu->scheduler = std::make_unique<Vulkan::Scheduler>(*gpu->instance);
            gpu->runtime = std::make_unique<Vulkan::Runtime>(*gpu->instance, *gpu->scheduler);
            gpu->headless_rasterizer = std::make_unique<Vulkan::Rasterizer>(
                *gpu->instance, *gpu->scheduler, *gpu->runtime, gpu->liverpool.get());
            gpu->rasterizer = gpu->headless_rasterizer.get();
        }
        gpu->liverpool->ReserveCopyBufferSpace();
        VramStart(gpu->rasterizer->GetRuntime().GetInstance());
    } catch (const std::exception& e) {
        Log(0, std::string("gpu: init failed: ") + e.what());
        g_gpu = nullptr;
        SotcGpu::g_host = nullptr;
        return -1;
    }

    Platform::IrqC::Instance()->Register(
        Platform::InterruptId::GfxEop,
        [](Platform::InterruptId) {
            if (g_gpu && g_gpu->host.eop_interrupt) {
                g_gpu->host.eop_interrupt();
            }
        },
        gpu.get());

    gpu->present_thread = std::jthread{PresentThread};
    Log(2, cfg->sdl_window ? "gpu: Vulkan presenter ready" : "gpu: Vulkan ready (headless)");
    gpu.release();
    return 0;
}

void sotcgpu_shutdown(void) {
    Gpu* g = g_gpu;
    if (!g) {
        return;
    }
    Platform::IrqC::Instance()->Unregister(Platform::InterruptId::GfxEop, g);
    g->present_thread.request_stop();
    g->present_thread.join();
    VramStop();
    {
        // Fail any waiting screenshot.
        std::scoped_lock lk{g->shot_mutex};
        if (g->shot_job) {
            g->shot_job->done = true;
            g->shot_job = nullptr;
            g->shot_cv.notify_all();
        }
    }
    Storage::DataBase::Instance().Close(); // finishes the queued pipeline cache writes
    if (!g->liverpool->IsGpuIdle()) {
        // The command processor may be stuck in a WAIT_REG_MEM; joining it would hang.
        Log(1, "gpu: shutdown with GPU work pending; leaking the GPU state");
        SotcGpu::g_host = nullptr;
        return;
    }
    SotcDebug::Shutdown();
    g->liverpool->BindRasterizer(nullptr);
    g->presenter.reset();
    g->headless_rasterizer.reset();
    g->runtime.reset();
    g->scheduler.reset();
    g->instance.reset();
    g->liverpool.reset();
    Core::Signals::Instance()->RemoveHandlers();
    g_gpu = nullptr;
    SotcGpu::g_host = nullptr;
    delete g;
}

void sotcgpu_submit_gfx(const uint32_t* dcb, uint32_t dcb_dwords, const uint32_t* ccb,
                        uint32_t ccb_dwords, sotcgpu_done_fn done, void* arg) {
    Gpu* g = g_gpu;
    if (!g) {
        if (done) {
            done(arg);
        }
        return;
    }
    Common::UniqueFunction<void> on_done{};
    if (done) {
        on_done = [done, arg] { done(arg); };
    }
    g->liverpool->SubmitGfx(std::span<const u32>{dcb, dcb ? dcb_dwords : 0u},
                            std::span<const u32>{ccb, ccb ? ccb_dwords : 0u},
                            std::move(on_done));
}

void sotcgpu_submit_compute(uint32_t vqid, const uint32_t* acb, uint32_t dwords,
                            sotcgpu_done_fn done, void* arg) {
    Gpu* g = g_gpu;
    if (!g || vqid == 0 || vqid >= AmdGpu::Liverpool::NumTotalQueues) {
        if (g) {
            LOG_ERROR(Render, "submit_compute: invalid vqid {}", vqid);
        }
        if (done) {
            done(arg);
        }
        return;
    }
    {
        // Liverpool::ProcessCompute indexes asc_queues by vqid - 1 and keeps packets split
        // at the ring end there. The runtime owns the real ring and read pointer (it
        // advances it from `done`), so each queue gets a scratch read pointer.
        std::scoped_lock lk{g->asc_mutex};
        while (g->num_asc_queues < vqid) {
            const u32 i = g->num_asc_queues++;
            const auto id = g->liverpool->asc_queues.Insert(
                VAddr{0}, &g->asc_read_ptrs[i], 0x40000000u, i / AmdGpu::Liverpool::NumQueuesPerPipe);
            ASSERT(id.index == i);
        }
    }
    Common::UniqueFunction<void> on_done{};
    if (done) {
        on_done = [done, arg] { done(arg); };
    }
    g->liverpool->SubmitAsc(vqid, std::span<const u32>{acb, dwords}, std::move(on_done));
}

void sotcgpu_submit_done(void) {
    if (g_gpu) {
        g_gpu->liverpool->SubmitDone();
    }
}

void sotcgpu_register_buffers(int32_t group, const uint64_t* addrs, int32_t count,
                              const void* attribute) {
    Gpu* g = g_gpu;
    if (!g || !addrs || !attribute || group < 0 || group >= s32(MaxDisplayBufferGroups) ||
        count < 0 || count > s32(MaxDisplayBuffers)) {
        return;
    }
    BufferAttributeGroup grp{};
    grp.is_occupied = true;
    std::memcpy(&grp.attrib, attribute, sizeof(BufferAttribute));
    {
        std::scoped_lock lk{g->buffers_mutex};
        g->groups[group] = grp;
        for (s32 i = 0; i < count; ++i) {
            if (addrs[i] == 0) {
                continue; // slot not part of this call
            }
            g->slots[i] = VideoOutBuffer{
                .group_index = group,
                .address_left = static_cast<uintptr_t>(addrs[i]),
                .address_right = 0,
            };
        }
    }
    if (g->presenter) {
        // Like VideoOutDriver::RegisterBuffers: create the texture cache images. This
        // touches the texture cache, so it runs on the GPU thread.
        std::vector<u64> list(addrs, addrs + count);
        g->liverpool->SendCommand<true>([g, grp, list] {
            for (const u64 addr : list) {
                if (addr != 0) {
                    g->presenter->RegisterVideoOutSurface(grp, addr);
                }
            }
        });
    }
}

void sotcgpu_flip(int32_t buf_index, int64_t flip_arg, bool after_gpu_work) {
    Gpu* g = g_gpu;
    if (!g) {
        return;
    }
    if (buf_index >= s32(MaxDisplayBuffers) || buf_index < -1) {
        buf_index = -1;
    }
    if (after_gpu_work) {
        Platform::IrqC::Instance()->RegisterOnce(
            Platform::InterruptId::GfxFlip,
            [buf_index, flip_arg](Platform::InterruptId) { SubmitFlipInternal(buf_index, flip_arg); });
    } else {
        g->liverpool->SendCommand([buf_index, flip_arg] { SubmitFlipInternal(buf_index, flip_arg); });
    }
}

void sotcgpu_set_flip_rate(int32_t rate) {
    g_flip_rate.store(rate < 0 ? 0 : rate, std::memory_order_relaxed);
}

void sotcgpu_map_memory(uint64_t addr, uint64_t size) {
    if (g_gpu && g_gpu->rasterizer) {
        // As shadPS4's MemoryManager: RegisterMemory gives the pages page-manager state.
        // Without it the page manager never write-protects and CPU writes go unseen.
        g_gpu->rasterizer->RegisterMemory(addr, size);
        g_gpu->rasterizer->MapMemory(addr, size);
    }
}

void sotcgpu_unmap_memory(uint64_t addr, uint64_t size) {
    if (g_gpu && g_gpu->rasterizer) {
        g_gpu->rasterizer->UnmapMemory(addr, size);
    }
}

namespace {
/// SOTC_SRT_GPU_PATCH=1 turns on the GPU-side patching of walker constants. Off by default:
/// in session 10 it replaced each download by 3-4 patches (a download unprotects its window,
/// a patch doesn't) and many hot tables also hold GPU-written descriptors (no gain, 10 -> 9 fps).
bool SrtGpuPatchEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SOTC_SRT_GPU_PATCH");
        return env && *env == '1';
    }();
    return enabled;
}

/// The flattened-buffer dwords the host reads as resource descriptors (sharps).
std::vector<bool> SharpDwords(const Shader::Info& info) {
    std::vector<bool> sharp(info.flattened_ud_buf.size() + 8);
    const auto mark_dw = [&](u32 dw) {
        if (dw < sharp.size()) {
            sharp[dw] = true;
        }
    };
    const auto mark = [&](const auto& fetch) {
        using Fetch = std::remove_cvref_t<decltype(fetch)>;
        if (fetch.summary == Fetch::Summary::SingleLoad) {
            for (u32 i = 0; i < Fetch::N; ++i) {
                mark_dw(fetch.offsets[0] + i);
            }
        } else if (fetch.summary == Fetch::Summary::MultiLoad) {
            for (u32 i = 0; i < Fetch::N; ++i) {
                if ((fetch.load_mask >> i) & 1) {
                    mark_dw(fetch.offsets[i]);
                }
            }
        }
    };
    for (const auto& b : info.buffers) {
        mark(b.sharp_fetch);
    }
    for (const auto& i : info.images) {
        mark(i.sharp_fetch);
    }
    for (const auto& s : info.samplers) {
        mark(s.sharp_fetch);
        if (s.post_op_tsharp_dw3_off != Shader::UNKNOWN_LOCATION) {
            mark_dw(s.post_op_tsharp_dw3_off);
        }
    }
    for (const auto& f : info.fmasks) {
        for (u32 i = 0; i < 8; ++i) {
            mark_dw(f.sharp_idx + i);
        }
    }
    return sharp;
}

/// SOTC_GPU_STATS, once per shader and table: a walker table with GPU-written bytes (read with
/// a download). Which of its leaves are GPU-written, and whether each feeds a pointer the
/// walker follows, a resource descriptor the host binds (sharp), or only shader constants.
void LogGpuWrittenTable(u64 ptr, u64 bytes, u64 table, VideoCore::BufferCache& buffer_cache) {
    const auto* info = static_cast<const Shader::Info*>(Shader::SrtWalkInfo());
    static std::unordered_set<u64> seen; // GPU thread only
    if (!info || seen.size() >= 256 || !seen.insert(info->pgm_hash ^ (table << 48)).second) {
        return;
    }
    const auto sharp = SharpDwords(*info); // the same test as PatchableTable
    u32 leaves = 0, written = 0, pointers = 0, sharps = 0, constants = 0;
    std::string list;
    for (const auto& leaf : info->srt_leaves) {
        if (leaf.table != table) {
            continue;
        }
        ++leaves;
        const u64 src = ptr + u64{leaf.src_dw} * 4;
        if (!buffer_cache.IsRangeGpuWritten(src, leaf.pointer ? 8 : 4)) {
            continue;
        }
        ++written;
        const char* kind = leaf.pointer                                         ? "ptr"
                           : leaf.dst_dw < sharp.size() && sharp[leaf.dst_dw] ? "sharp"
                                                                               : "const";
        (leaf.pointer ? pointers : kind[0] == 's' ? sharps : constants)++;
        if (written <= 12) {
            list += fmt::format(" {}:{}->{}", kind, leaf.src_dw, leaf.dst_dw);
        }
    }
    LOG_INFO(Render,
             "srt gpu-written table: shader {:#x} stage {} table {} at {:#x} +{:#x}: {} leaves, "
             "{} GPU-written ({} pointers, {} sharps, {} constants):{}",
             info->pgm_hash, u32(info->sw_stage), table, ptr, bytes, leaves, written, pointers,
             sharps, constants, list);
}

/// Session 22 (debug, SOTC_CPU_COPY_LOG=1): the walker's last tables; when one is about to be
/// read at an address that isn't guest GPU memory, logs the shader and the trail with the first
/// dwords of each earlier table (one of them holds the bad pointer). First 8 s only (=N: 8 s
/// from N s after the first table).
bool SrtTrailEnabled() {
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

void SrtTrail(u64 ptr, u64 bytes, u64 table, VideoCore::BufferCache& buffer_cache) {
    struct Entry {
        const void* walk;
        u64 ptr, bytes, table;
    };
    static std::array<Entry, 8> trail{};
    static u32 next = 0;
    static u32 logged = 0;
    const void* walk = Shader::SrtWalkInfo();
    trail[next++ % trail.size()] = {walk, ptr, bytes, table};
    if (Core::Memory::Instance()->IsValidGpuMapping(ptr, std::max<u64>(bytes, 4)) ||
        logged >= 40) {
        return;
    }
    ++logged;
    const auto* info = static_cast<const Shader::Info*>(walk);
    LOG_WARNING(Render, "srt-trail: shader {:#x} table {} bad ptr {:#x} +{:#x}",
                info ? info->pgm_hash : 0, table, ptr, bytes);
    for (u32 k = 1; k < trail.size(); ++k) {
        const Entry& e = trail[(next - 1 - k) % trail.size()];
        if (e.walk != walk || e.ptr == 0) {
            break;
        }
        std::array<u32, 16> dw{};
        const u64 n = std::min<u64>(e.bytes ? e.bytes : 64, sizeof(dw));
        if (Core::Memory::Instance()->IsValidGpuMapping(e.ptr, n)) {
            Core::Memory::Instance()->CopySparseMemory(e.ptr, reinterpret_cast<u8*>(dw.data()), n);
        }
        std::string words;
        for (u64 i = 0; i < n / 4; ++i) {
            words += fmt::format(" {:08x}", dw[i]);
        }
        LOG_WARNING(Render, "srt-trail:   -{} table {} at {:#x} +{:#x} gpu-written {}:{}", k,
                    e.table, e.ptr, e.bytes,
                    e.bytes && buffer_cache.IsRangeGpuWritten(e.ptr, e.bytes), words);
    }
}

/// Shader::SrtResolveTable's resolver (session 10): see SotcSrt::ResolveTable.
u64 ResolveSrtTable(u64 ptr, u64 bytes, u64 table) {
    Gpu* g = g_gpu;
    if (!g || !g->rasterizer || !g->host.backing_if_blocked) {
        return ptr;
    }
    auto& rasterizer = *g->rasterizer;
    // gpu_modified_ranges belongs to the GPU thread, where the walker runs.
    if (std::this_thread::get_id() != rasterizer.GetGpuCommandProcessorThread()) {
        return ptr;
    }
    auto& buffer_cache = rasterizer.GetBufferCache();
    if (SotcDebug::StatsEnabled()) {
        const auto* walk = static_cast<const Shader::Info*>(Shader::SrtWalkInfo());
        SotcDebug::OnSrtTable(walk ? walk->pgm_hash : 0, table, ptr, bytes);
    }
    if (SrtTrailEnabled()) {
        SrtTrail(ptr, bytes, table, buffer_cache);
    }
    const auto is_gpu_written = [&](u64 a, u64 n) { return buffer_cache.IsRangeGpuWritten(a, n); };
    const u64 resolved = SotcSrt::ResolveTable(
        ptr, bytes, [g](u64 a, u64 n) { return g->host.backing_if_blocked(a, n); },
        is_gpu_written);
    using SotcDebug::SrtOutcome;
    if (resolved != ptr) {
        SotcDebug::Count(SotcDebug::Counter::SrtAliased);
        SotcDebug::SetSrtOutcome(SrtOutcome::Aliased);
        return resolved;
    }
    if (bytes == 0 || !is_gpu_written(ptr, bytes)) {
        SotcDebug::SetSrtOutcome(bytes == 0 ? SrtOutcome::NoExtent : SrtOutcome::NotWritten);
        return ptr;
    }
    // GPU-written constants: read the CPU copy, the GPU patches them before the draw.
    const auto* info = static_cast<const Shader::Info*>(Shader::SrtWalkInfo());
    if (info && SrtGpuPatchEnabled()) {
        if (const u64 alias = g->host.backing_if_blocked(ptr, bytes)) {
            const auto sharp = SharpDwords(*info);
            if (SotcSrt::PatchableTable(
                    ptr, u32(table), info->srt_leaves, is_gpu_written,
                    [&](u32 dw) { return dw < sharp.size() && sharp[dw]; },
                    [&](u32 dst, u64 src) { info->sotc_flat_patches.emplace_back(u16(dst), src); })) {
                SotcDebug::Count(SotcDebug::Counter::SrtPatchedTables);
                SotcDebug::SetSrtOutcome(SrtOutcome::Patched);
                return alias;
            }
            SotcDebug::SetSrtOutcome(SrtOutcome::NotPatchable);
        } else {
            SotcDebug::SetSrtOutcome(SrtOutcome::NotBlocked);
        }
    } else {
        SotcDebug::SetSrtOutcome(info ? SrtOutcome::PatchOff : SrtOutcome::NoInfo);
    }
    if (SotcDebug::StatsEnabled()) {
        LogGpuWrittenTable(ptr, bytes, table, buffer_cache);
    }
    return ptr;
}
} // namespace

bool sotcgpu_emulate_load(uint64_t addr, uint64_t rip, uint64_t* const* gpr, uint64_t* rip_ptr) {
    Gpu* g = g_gpu;
    if (!g || !g->rasterizer || !gpr || !rip_ptr ||
        !Shader::SrtWalkerContains(reinterpret_cast<const void*>(rip))) {
        return false;
    }
    auto& rasterizer = *g->rasterizer;
    // The walker runs on the GPU thread, which also owns the buffer cache's written ranges.
    if (std::this_thread::get_id() != rasterizer.GetGpuCommandProcessorThread()) {
        return false;
    }
    auto& buffer_cache = rasterizer.GetBufferCache();
    const auto result = SotcSrt::EmulateLoad(
        reinterpret_cast<const uint8_t*>(rip), SotcSrt::Regs{gpr, rip_ptr}, addr,
        [&](u64 a, u64 n) { return buffer_cache.IsRangeGpuWritten(a, n); },
        [](u64 a, void* dst, u64 n) {
            // Never faults: protected pages are read from the backing.
            Core::Memory::Instance()->CopySparseMemory(a, static_cast<u8*>(dst), n);
        });
    if (result != SotcSrt::LoadResult::Emulated) {
        return false;
    }
    SotcDebug::Count(SotcDebug::Counter::SrtEmulated);
    return true;
}

bool sotcgpu_handle_fault(uint64_t addr, bool is_write, uint64_t rip) {
    if (!g_gpu) {
        return false;
    }
    // Tracy: a GPU-tracked page fault, from whichever thread touched the page (guest threads:
    // the time they wait for the GPU's data); value = the address.
    ZoneScopedNC("GPU page fault", 0xe63946);
    ZoneValue(addr);
    if (SotcDebug::StatsEnabled() && g_gpu->rasterizer) [[unlikely]] {
        SotcDebug::Count(is_write ? SotcDebug::Counter::WriteFault
                                  : SotcDebug::Counter::ReadFault);
        // Racy read of the tracker bits: good enough for the histogram.
        auto& rasterizer = *g_gpu->rasterizer;
        SotcDebug::OnFault(addr, is_write,
                           std::this_thread::get_id() == rasterizer.GetGpuCommandProcessorThread(),
                           rasterizer.GetBufferCache().IsRegionGpuModified(addr & ~u64{0xfff},
                                                                           0x1000),
                           rip);
        if (std::this_thread::get_id() == rasterizer.GetGpuCommandProcessorThread()) {
            const auto start = std::chrono::steady_clock::now();
            const bool handled = DispatchFault(addr, is_write, rip);
            SotcDebug::OnGpuFaultTime(
                addr, u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count()));
            return handled;
        }
    }
    return DispatchFault(addr, is_write, rip);
}

bool sotcgpu_screenshot(const char* path) {
    Gpu* g = g_gpu;
    if (!g || !path || !g->presenter) {
        return false;
    }
    SotcDebug::OnScreenshotTick(); // arms SOTC_DUMP_RT
    ScreenshotJob job{path};
    std::unique_lock lk{g->shot_mutex};
    if (g->shot_job) {
        return false; // one at a time
    }
    g->shot_job = &job;
    if (!g->shot_cv.wait_for(lk, std::chrono::seconds(2), [&] { return job.done; })) {
        if (g->shot_job == &job) {
            g->shot_job = nullptr;
            return false;
        }
        // The present thread took it: wait for it to finish (it never blocks for long).
        g->shot_cv.wait(lk, [&] { return job.done; });
    }
    return job.ok;
}

} // extern "C"
