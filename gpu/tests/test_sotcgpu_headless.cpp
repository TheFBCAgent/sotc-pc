// SPDX-License-Identifier: GPL-2.0-or-later
// sotcgpu end to end without a window, through the C contract only:
// - init with sdl_window = NULL (Vulkan instance + device, no swapchain);
// - a DCB with WRITE_DATA and EVENT_WRITE_EOP (data + interrupt): both labels are written
//   and host->eop_interrupt fires; the submission's `done` callback runs;
// - an ACB on compute queue 1 with RELEASE_MEM: its label is written, `done` runs
//   (Liverpool handles RELEASE_MEM on compute queues only; the graphics ring's
//   end-of-pipe packet is EVENT_WRITE_EOP);
// - an immediate flip is reported through host->flip_done;
// - faults are claimed inside GPU-mapped memory only.
// Exit code 77 (ctest SKIP_RETURN_CODE): no Vulkan loader or no device.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "sotcgpu.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static std::atomic<int> g_eops{0};
static std::atomic<int> g_flips{0};
static std::atomic<int64_t> g_flip_arg{0};
static std::atomic<int> g_flip_index{-2};
static std::atomic<int> g_done_gfx{0};
static std::atomic<int> g_done_compute{0};
static std::string g_last_log;

// ------------------------------------------------------------- host callbacks

static bool HostProtect(uint64_t addr, uint64_t size, bool r, bool w) {
#ifdef _WIN32
    DWORD old = 0;
    const DWORD prot = w ? PAGE_READWRITE : (r ? PAGE_READONLY : PAGE_NOACCESS);
    return VirtualProtect(reinterpret_cast<void*>(addr), size, prot, &old) != 0;
#else
    return true;
#endif
}
static bool HostWriteBacking(uint64_t addr, const void* src, uint64_t size) {
    std::memcpy(reinterpret_cast<void*>(addr), src, size);
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
static void HostEop() {
    ++g_eops;
}
static void HostFlipDone(int32_t index, int64_t arg) {
    g_flip_index = index;
    g_flip_arg = arg;
    ++g_flips;
}
static void HostLog(int level, const char* msg) {
    std::fprintf(stderr, "[%d] %s\n", level, msg);
    g_last_log = msg;
}

// ------------------------------------------------------------- PM4

static uint32_t Type3(uint32_t opcode, uint32_t body_dwords, bool compute = false) {
    return (3u << 30) | ((body_dwords - 1) << 16) | (opcode << 8) | (compute ? 2u : 0u);
}

static bool WaitFor(const std::atomic<int>& value, int at_least) {
    for (int i = 0; i < 500; ++i) {
        if (value.load() >= at_least) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool WaitLabel(volatile uint32_t* label, uint32_t value) {
    for (int i = 0; i < 500; ++i) {
        if (*label == value) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int main() {
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
    cfg.sdl_window = nullptr;
    cfg.width = 1280;
    cfg.height = 720;

    if (sotcgpu_init(&host, &cfg) != 0) {
        if (g_last_log.find("not found") != std::string::npos ||
            g_last_log.find("no Vulkan") != std::string::npos ||
            g_last_log.find("vkCreateInstance") != std::string::npos) {
            std::printf("skipped: no Vulkan (%s)\n", g_last_log.c_str());
            return 77;
        }
        std::fprintf(stderr, "sotcgpu_init failed\n");
        return 1;
    }

    // "Guest" memory: page-aligned, below 1 TiB on any sane allocator; 1 MiB.
#ifdef _WIN32
    // Fixed placement: PM4 address fields are 40/48-bit, and the OS may hand out
    // addresses above 1 TiB.
    uint32_t* mem = nullptr;
    for (uint64_t base = 0x2000000000ull; !mem && base < 0x8000000000ull; base += 0x100000000ull) {
        mem = static_cast<uint32_t*>(VirtualAlloc(reinterpret_cast<void*>(base), 1 << 20,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!mem) {
        std::fprintf(stderr, "could not allocate test memory below 1 TiB\n");
        return 1;
    }
#else
    auto* mem = static_cast<uint32_t*>(std::aligned_alloc(4096, 1 << 20));
    std::memset(mem, 0, 1 << 20);
#endif
    CHECK(reinterpret_cast<uint64_t>(mem) + (1 << 20) < (1ull << 40));
    sotcgpu_map_memory(reinterpret_cast<uint64_t>(mem), 1 << 20);

    volatile uint32_t* label_write = mem + 0x100;
    volatile uint32_t* label_eop = mem + 0x102;
    volatile uint32_t* label_rel = mem + 0x104;
    const uint64_t a_write = reinterpret_cast<uint64_t>(label_write);
    const uint64_t a_eop = reinterpret_cast<uint64_t>(label_eop);
    const uint64_t a_rel = reinterpret_cast<uint64_t>(label_rel);

    // ---- DCB: WRITE_DATA + EVENT_WRITE_EOP (Data32Low, IRQ when write confirmed)
    uint32_t* dcb = mem + 0x1000;
    uint32_t n = 0;
    dcb[n++] = Type3(0x37, 4);                   // WRITE_DATA
    dcb[n++] = (5u << 8) | (1u << 20);           // dst_sel=memory, wr_confirm
    dcb[n++] = uint32_t(a_write);
    dcb[n++] = uint32_t(a_write >> 32);
    dcb[n++] = 0xCAFEBABEu;
    dcb[n++] = Type3(0x47, 5);                   // EVENT_WRITE_EOP
    dcb[n++] = 0x14u | (5u << 8);                // CACHE_FLUSH_AND_INV_TS_EVENT, index 5
    dcb[n++] = uint32_t(a_eop);
    dcb[n++] = uint32_t((a_eop >> 32) & 0xffff) | (2u << 24) | (1u << 29); // int_sel=2 data_sel=1
    dcb[n++] = 0x600DF00Du;
    dcb[n++] = 0;

    const int eops_before = g_eops.load();
    sotcgpu_submit_gfx(dcb, n, nullptr, 0, [](void* arg) { ++*static_cast<std::atomic<int>*>(arg); },
                       &g_done_gfx);
    sotcgpu_submit_done();

    CHECK(WaitLabel(label_write, 0xCAFEBABEu));
    CHECK(WaitLabel(label_eop, 0x600DF00Du));
    CHECK(WaitFor(g_eops, eops_before + 1));
    CHECK(WaitFor(g_done_gfx, 1));

    // ---- ACB on compute queue 1: RELEASE_MEM (Data32Low, no interrupt)
    uint32_t* acb = mem + 0x2000;
    n = 0;
    acb[n++] = Type3(0x49, 6, true);             // RELEASE_MEM
    acb[n++] = 0x14u | (5u << 8);
    acb[n++] = (1u << 29);                       // data_sel=1, int_sel=0, dst_sel=0
    acb[n++] = uint32_t(a_rel);
    acb[n++] = uint32_t(a_rel >> 32);
    acb[n++] = 0x5EED1234u;
    acb[n++] = 0;
    sotcgpu_submit_compute(1, acb, n,
                           [](void* arg) { ++*static_cast<std::atomic<int>*>(arg); },
                           &g_done_compute);
    CHECK(WaitLabel(label_rel, 0x5EED1234u));
    CHECK(WaitFor(g_done_compute, 1));

    // ---- Immediate blank flip: reported through flip_done even without a presenter.
    sotcgpu_flip(-1, 42, false);
    CHECK(WaitFor(g_flips, 1));
    CHECK(g_flip_arg.load() == 42);
    CHECK(g_flip_index.load() == -1);

    // ---- Faults: outside GPU-mapped memory they are not the GPU's; inside, shadPS4's page
    // manager claims every one (it invalidates the caches and asks for a retry).
    CHECK(!sotcgpu_handle_fault(reinterpret_cast<uint64_t>(mem) + (2 << 20), true, 0));
    CHECK(sotcgpu_handle_fault(reinterpret_cast<uint64_t>(mem + 0x3000), true, 0));

    // ---- No presenter: screenshots are unavailable.
    CHECK(!sotcgpu_screenshot("unused.png"));

    sotcgpu_unmap_memory(reinterpret_cast<uint64_t>(mem), 1 << 20);
    sotcgpu_shutdown();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
