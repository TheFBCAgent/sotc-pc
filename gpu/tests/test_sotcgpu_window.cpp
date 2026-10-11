// SPDX-License-Identifier: GPL-2.0-or-later
// sotcgpu with a real SDL window: register a linear 1280x720 display buffer filled with a
// known colour, flip it (immediate flip), wait for flip_done, then take a screenshot and
// check the PNG's centre pixel. Exercises Presenter (swapchain, the SOTC_NO_IMGUI blit),
// the texture cache's VideoOut path and sotcgpu_screenshot.
// Exit code 77 (skipped): no display, or no Vulkan.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>
#include <stb_image.h>

#include "sotcgpu.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static std::atomic<int> g_flips{0};
static std::string g_last_log;

static bool HostProtect(uint64_t addr, uint64_t size, bool r, bool w) {
    DWORD old = 0;
    const DWORD prot = w ? PAGE_READWRITE : (r ? PAGE_READONLY : PAGE_NOACCESS);
    return VirtualProtect(reinterpret_cast<void*>(addr), size, prot, &old) != 0;
}
static bool HostWriteBacking(uint64_t addr, const void* src, uint64_t size) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(addr), src, size);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), size, old, &tmp);
    return true;
}
static void HostReadSparse(void* dst, uint64_t addr, uint64_t size) {
    std::memcpy(dst, reinterpret_cast<const void*>(addr), size);
}
static uint64_t HostClampRange(uint64_t, uint64_t size) {
    return size;
}
static bool HostIsGpuMapping(uint64_t addr, uint64_t size) {
    return addr + size < (1ull << 40);
}
static void HostEop() {}
static void HostFlipDone(int32_t, int64_t) {
    ++g_flips;
}
static void HostLog(int level, const char* msg) {
    if (level <= 1) {
        std::fprintf(stderr, "[%d] %s\n", level, msg);
    }
    g_last_log = msg;
}

/// SceVideoOutBufferAttribute (40 bytes).
struct BufferAttribute {
    uint32_t pixel_format;
    int32_t tiling_mode;
    int32_t aspect_ratio;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_in_pixel;
    uint32_t option;
    uint32_t reserved0;
    uint64_t reserved1;
};
static_assert(sizeof(BufferAttribute) == 40);

static void Pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("skipped: no display (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Window* window = SDL_CreateWindow("sotcgpu test", 640, 360, SDL_WINDOW_VULKAN);
    if (!window) {
        std::printf("skipped: no window (%s)\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }

    sotcgpu_host host{};
    host.protect = HostProtect;
    host.write_backing = HostWriteBacking;
    host.read_sparse = HostReadSparse;
    host.clamp_range = HostClampRange;
    host.is_gpu_mapping = HostIsGpuMapping;
    host.eop_interrupt = HostEop;
    host.flip_done = HostFlipDone;
    host.log = HostLog;

    sotcgpu_config cfg{};
    cfg.sdl_window = window;
    cfg.width = 640;
    cfg.height = 360;
    if (sotcgpu_init(&host, &cfg) != 0) {
        std::printf("skipped: sotcgpu_init failed (%s)\n", g_last_log.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }

    // A 1280x720 linear A8B8G8R8 (R,G,B,A bytes) buffer below 1 TiB, 64 KiB aligned.
    constexpr uint32_t W = 1280, H = 720;
    constexpr uint64_t size = uint64_t(W) * H * 4;
    uint8_t* buf = nullptr;
    for (uint64_t base = 0x2000000000ull; !buf && base < 0x8000000000ull; base += 0x100000000ull) {
        buf = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(base), size,
                                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!buf) {
        std::fprintf(stderr, "could not allocate the display buffer\n");
        return 1;
    }
    for (uint64_t i = 0; i < size; i += 4) {
        buf[i + 0] = 0x20; // R
        buf[i + 1] = 0xC0; // G
        buf[i + 2] = 0x40; // B
        buf[i + 3] = 0xFF;
    }
    sotcgpu_map_memory(reinterpret_cast<uint64_t>(buf), size);

    const BufferAttribute attr{
        .pixel_format = 0x80002200u, // A8B8G8R8Srgb
        .tiling_mode = 1,            // linear
        .aspect_ratio = 0,
        .width = W,
        .height = H,
        .pitch_in_pixel = W,
    };
    const uint64_t addrs[1] = {reinterpret_cast<uint64_t>(buf)};
    sotcgpu_register_buffers(0, addrs, 1, &attr);

    for (int i = 0; i < 3; ++i) {
        sotcgpu_flip(0, i, false);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_flips.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        Pump(10);
    }
    CHECK(g_flips.load() >= 3);

    // Removed without throwing: where the OS denies the delete, the throwing remove aborted
    // the whole test with no message (the screenshot CHECK below reports it instead).
    const auto png = (std::filesystem::temp_directory_path() / "sotcgpu_test_window.png").string();
    std::error_code ec;
    std::filesystem::remove(png, ec);
    CHECK(sotcgpu_screenshot(png.c_str()));

    std::ifstream in(png, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    CHECK(!data.empty());
    if (!data.empty()) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(data.data(), int(data.size()), &w, &h, &n, 4);
        CHECK(px != nullptr);
        if (px) {
            const unsigned char* c = px + (size_t(h / 2) * w + w / 2) * 4;
            std::printf("screenshot %dx%d, centre pixel %02x %02x %02x\n", w, h, c[0], c[1], c[2]);
            // sRGB in, sRGB out: the colour survives within rounding.
            CHECK(std::abs(int(c[0]) - 0x20) <= 3);
            CHECK(std::abs(int(c[1]) - 0xC0) <= 3);
            CHECK(std::abs(int(c[2]) - 0x40) <= 3);
            stbi_image_free(px);
        }
    }
    in.close();
    std::filesystem::remove(png, ec);

    sotcgpu_shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
