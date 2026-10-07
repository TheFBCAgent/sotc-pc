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
#include <vector>

#include <stb_image_write.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/buffer.h"
#include "core/libraries/videoout/driver.h"
#include "core/platform.h"
#include "core/signals.h"
#include "emulator.h"
#include "sdl_window.h"
#include "sotcgpu.h"
#include "sotcgpu_internal.h"
#include "video_core/amdgpu/liverpool.h"
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

// ----------------------------------------------------------------- screenshot

/// Reads `frame` (in VK_IMAGE_LAYOUT_GENERAL, as before Presenter::Present) back through
/// an RGBA8 blit and writes it as a PNG. Runs on the present thread.
bool CaptureFrame(Vulkan::Frame* frame, const std::string& path) {
    Gpu& g = *g_gpu;
    const Vulkan::Instance& inst = g.rasterizer->GetRuntime().GetInstance();
    const vk::Device device = inst.GetDevice();
    const u32 width = frame->width, height = frame->height;
    if (width == 0 || height == 0) {
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
                    }
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
    Gpu& g = *g_gpu;
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
    std::scoped_lock lk{g.req_mutex};
    g.requests.push({frame, index, flip_arg});
    g.req_cv.notify_one();
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
    if (cfg->cache_dir && *cfg->cache_dir) {
        SotcGpu::g_user_dir = cfg->cache_dir;
    }
    auto& settings = SotcGpuGetSettings();
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
    Core::Emulator::FillElfInfo();

    if (!ProbeVulkan()) {
        SotcGpu::g_host = nullptr;
        return -1;
    }

    g_gpu = gpu.get();
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
    {
        // Fail any waiting screenshot.
        std::scoped_lock lk{g->shot_mutex};
        if (g->shot_job) {
            g->shot_job->done = true;
            g->shot_job = nullptr;
            g->shot_cv.notify_all();
        }
    }
    if (!g->liverpool->IsGpuIdle()) {
        // The command processor may be stuck in a WAIT_REG_MEM; joining it would hang.
        Log(1, "gpu: shutdown with GPU work pending; leaking the GPU state");
        SotcGpu::g_host = nullptr;
        return;
    }
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

bool sotcgpu_handle_fault(uint64_t addr, bool is_write, uint64_t rip) {
    if (!g_gpu) {
        return false;
    }
    return DispatchFault(addr, is_write, rip);
}

bool sotcgpu_screenshot(const char* path) {
    Gpu* g = g_gpu;
    if (!g || !path || !g->presenter) {
        return false;
    }
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
