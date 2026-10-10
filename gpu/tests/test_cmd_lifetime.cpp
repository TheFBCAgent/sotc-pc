// SPDX-License-Identifier: GPL-2.0-or-later
// Session 29, the menu crash: a guest woken by a command buffer's last label may unmap that
// buffer at once. EVENT_WRITE_EOP writes its label and raises the interrupt synchronously on
// the GPU thread, and Liverpool::ProcessGraphics then read the packet header again (NumWords)
// to step past it: a fault on an unmapped page. This test forces the race: the EOP interrupt
// handler (host->eop_interrupt, called on the GPU thread) makes the DCB's page inaccessible
// before returning. With the old code the GPU thread faults and the process dies.
// Needs a Vulkan device (sotcgpu_init headless); exit code 77 when there is none. Exits
// without sotcgpu_shutdown (teardown is what test_sotcgpu_headless's TDR is suspected of).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
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
static std::atomic<int> g_done{0};
static std::atomic<uint64_t> g_revoke_page{0}; // the DCB page the next EOP makes inaccessible
static std::string g_last_log;

static bool SetAccess(uint64_t addr, uint64_t size, bool access) {
#ifdef _WIN32
    DWORD old = 0;
    return VirtualProtect(reinterpret_cast<void*>(addr), size,
                          access ? PAGE_READWRITE : PAGE_NOACCESS, &old) != 0;
#else
    return mprotect(reinterpret_cast<void*>(addr), size,
                    access ? PROT_READ | PROT_WRITE : PROT_NONE) == 0;
#endif
}

static bool HostProtect(uint64_t, uint64_t, bool, bool) {
    return true;
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
// The "guest": sees the EOP and releases the command buffer before the GPU thread moves on.
static void HostEop() {
    const uint64_t page = g_revoke_page.exchange(0);
    if (page) {
        CHECK(SetAccess(page, 4096, false));
    }
    ++g_eops;
}
static void HostFlipDone(int32_t, int64_t) {}
static void HostLog(int level, const char* msg) {
    std::fprintf(stderr, "[%d] %s\n", level, msg);
    g_last_log = msg;
}

static uint32_t Type3(uint32_t opcode, uint32_t body_dwords) {
    return (3u << 30) | ((body_dwords - 1) << 16) | (opcode << 8);
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
            std::_Exit(77);
        }
        std::fprintf(stderr, "sotcgpu_init failed\n");
        std::_Exit(1);
    }

    // "Guest" memory below 1 TiB (PM4 address fields are 40/48-bit): 1 MiB.
    const size_t size = 1 << 20;
#ifdef _WIN32
    uint32_t* mem = nullptr;
    for (uint64_t base = 0x2000000000ull; !mem && base < 0x8000000000ull; base += 0x100000000ull) {
        mem = static_cast<uint32_t*>(VirtualAlloc(reinterpret_cast<void*>(base), size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
#else
    uint32_t* mem = nullptr;
    for (uint64_t base = 0x2000000000ull; !mem && base < 0x8000000000ull; base += 0x100000000ull) {
        void* p = mmap(reinterpret_cast<void*>(base), size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            mem = static_cast<uint32_t*>(p);
        }
    }
#endif
    if (!mem) {
        std::fprintf(stderr, "could not allocate test memory\n");
        std::_Exit(1);
    }
    CHECK(reinterpret_cast<uint64_t>(mem) + size < (1ull << 40));
    sotcgpu_map_memory(reinterpret_cast<uint64_t>(mem), size);

    // Several rounds: each DCB on its own page, ending with EVENT_WRITE_EOP (label + IRQ).
    constexpr int kRounds = 8;
    for (int round = 0; round < kRounds; ++round) {
        volatile uint32_t* label = mem + 0x100 + 2 * round;
        const uint64_t a_label = reinterpret_cast<uint64_t>(label);
        uint32_t* dcb = mem + (1 + round) * 1024; // page 1 + round
        uint32_t n = 0;
        dcb[n++] = Type3(0x10, 2); // NOP, 2 body dwords (something before the EOP)
        dcb[n++] = 0;
        dcb[n++] = 0;
        dcb[n++] = Type3(0x47, 5); // EVENT_WRITE_EOP
        dcb[n++] = 0x14u | (5u << 8);
        dcb[n++] = uint32_t(a_label);
        dcb[n++] = uint32_t((a_label >> 32) & 0xffff) | (2u << 24) | (1u << 29); // irq, data32
        dcb[n++] = 0xD0E00000u + round;
        dcb[n++] = 0;

        g_revoke_page = reinterpret_cast<uint64_t>(dcb);
        const int eops = g_eops.load(), done = g_done.load();
        sotcgpu_submit_gfx(dcb, n, nullptr, 0,
                           [](void* arg) { ++*static_cast<std::atomic<int>*>(arg); }, &g_done);
        sotcgpu_submit_done();
        CHECK(WaitFor(g_eops, eops + 1));
        CHECK(WaitFor(g_done, done + 1));
        CHECK(*label == 0xD0E00000u + round);
    }

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    std::fflush(stdout);
    std::_Exit(g_failures ? 1 : 0);
}
