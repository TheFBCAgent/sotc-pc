// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc debug tools (workstream X); the interface and the hook list are in sotc_debug.h.
//
// SOTC_DUMP_RT=<dir> (render-target dump, docs/m3/bringup.md "Debug technique that worked"):
// - a screenshot tick (sotcgpu_screenshot) arms it;
// - the next flip starts recording: every draw adds its bound color and depth images
//   (Rasterizer::cb_descs/db_desc after BeginRendering) with a draw count;
// - the flip after that stops recording and, on the GPU thread, downloads each image
//   (Runtime::DownloadImage into a HostCached buffer, Scheduler::Finish), converts it and
//   writes <dir>/dNNN_tMM_<addr>_<format>.png plus a line in <dir>/index.txt.
// SOTC_DUMP_RT_ALL=1 re-arms on every tick; otherwise the dump runs once.
//
// SOTC_GPU_STATS=1: a thread logs and resets the counters once per second, with the blocking
// scheduler waits split by call site (WaitSite). Every 10 s it also logs the top faulting
// guest pages (thread, read/write, GPU-modified, PM4 packet on the GPU thread, GPU drains) and
// the PM4 packets whose GPU-thread faults drained the GPU (session 10 diagnostic).
// SOTC_GPU_DUMP=1: shadPS4's dump_shaders (IR and SPIR-V under <cache_dir>/shader/dumps).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <stb_image_write.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "sotc_debug.h"
#include "sotc_gpu_timer.h"
#include "video_core/amdgpu/pm4_opcodes.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/texture_cache.h"

namespace SotcDebug {

std::atomic<u32> g_flags{0};
std::array<std::atomic<u64>, size_t(Counter::Count)> g_counters{};
std::array<std::atomic<u64>, size_t(WaitSite::Count)> g_wait_sites{};
thread_local WaitSite t_wait_site = WaitSite::Other;
thread_local u32 t_pm4 = NoPm4;
thread_local bool t_gpu_thread = false;

namespace {

// ----------------------------------------------------------------- fault histogram

/// One 4 KiB guest page. A fixed open-addressing table under a mutex: OnFault runs inside the
/// fault handler, so it must not allocate.
struct FaultPage {
    u64 key; ///< page address + 1 (0 = empty slot)
    u32 faults;
    u32 gpu_reads, gpu_writes, guest_reads, guest_writes;
    u32 gpu_modified; ///< faults that found the page GPU-modified
    u32 drains_gpu, drains_queued;
    std::array<u32, 3> pm4;       ///< GPU-thread packets seen on this page (NoPm4 = free)
    std::array<u32, 3> pm4_count; ///< and how often
    u64 gpu_read_rip;             ///< host instruction of the last GPU-thread read fault
    u32 gpu_read_min, gpu_read_max; ///< in-page offsets of the GPU-thread read faults
    std::array<u8, 1024> changed;   ///< per dword of the page: a download found it changed
    u32 compares;                   ///< downloads compared on this page
};

/// A buffer binding the GPU may write (ObtainBuffer is_written), keyed by (addr, size).
struct GpuWrite {
    u64 addr; ///< 0 = empty slot
    u64 size;
    u32 count;
    u32 pm4;
};
constexpr size_t WriteTableSize = 4096; // power of two
std::mutex g_write_mutex;
std::array<GpuWrite, WriteTableSize> g_writes{};
size_t g_writes_used = 0;
constexpr size_t FaultTableSize = 4096; // power of two
std::mutex g_fault_mutex;
std::array<FaultPage, FaultTableSize> g_fault_pages{};
size_t g_fault_used = 0;
u64 g_fault_dropped = 0;              ///< faults on new pages once the table was 3/4 full
std::array<u32, 0x200> g_drain_pm4{}; ///< GPU-thread drains by packet (opcode | 0x100 compute)
u32 g_drain_no_pm4 = 0;               ///< GPU-thread drains outside packet processing

FaultPage* FindPage(u64 page, bool create) {
    const u64 key = page + 1;
    size_t i = size_t(((page >> 12) * 0x9E3779B97F4A7C15ull) >> 52) & (FaultTableSize - 1);
    for (size_t n = 0; n < FaultTableSize; ++n, i = (i + 1) & (FaultTableSize - 1)) {
        FaultPage& e = g_fault_pages[i];
        if (e.key == key) {
            return &e;
        }
        if (e.key == 0) {
            if (!create || g_fault_used >= FaultTableSize * 3 / 4) {
                return nullptr;
            }
            ++g_fault_used;
            e.key = key;
            e.pm4.fill(NoPm4);
            return &e;
        }
    }
    return nullptr;
}

/// "module+0xoffset" for a host instruction address (JIT code has no module).
std::string HostSymbol(u64 rip) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (rip != 0 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rip), &module)) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        return fmt::format("{}+{:#x}", base ? base + 1 : path, rip - u64(module));
    }
#endif
    return fmt::format("{:#x} (no module)", rip);
}

std::string Pm4Name(u32 op) {
    using AmdGpu::PM4ItOpcode;
    if (op == NoPm4) {
        return "-";
    }
    const char* queue = (op & 0x100) ? "c:" : "g:";
    const char* name = nullptr;
    switch (PM4ItOpcode(op & 0xff)) {
    case PM4ItOpcode::Nop: name = "NOP"; break;
    case PM4ItOpcode::DispatchDirect: name = "DISPATCH_DIRECT"; break;
    case PM4ItOpcode::DispatchIndirect: name = "DISPATCH_INDIRECT"; break;
    case PM4ItOpcode::CondExec: name = "COND_EXEC"; break;
    case PM4ItOpcode::DrawIndexIndirect: name = "DRAW_INDEX_INDIRECT"; break;
    case PM4ItOpcode::DrawIndex2: name = "DRAW_INDEX_2"; break;
    case PM4ItOpcode::DrawIndirectMulti: name = "DRAW_INDIRECT_MULTI"; break;
    case PM4ItOpcode::DrawIndexAuto: name = "DRAW_INDEX_AUTO"; break;
    case PM4ItOpcode::WriteData: name = "WRITE_DATA"; break;
    case PM4ItOpcode::DrawIndexIndirectMulti: name = "DRAW_INDEX_INDIRECT_MULTI"; break;
    case PM4ItOpcode::MemSemaphore: name = "MEM_SEMAPHORE"; break;
    case PM4ItOpcode::WaitRegMem: name = "WAIT_REG_MEM"; break;
    case PM4ItOpcode::IndirectBuffer: name = "INDIRECT_BUFFER"; break;
    case PM4ItOpcode::CopyData: name = "COPY_DATA"; break;
    case PM4ItOpcode::EventWrite: name = "EVENT_WRITE"; break;
    case PM4ItOpcode::EventWriteEop: name = "EVENT_WRITE_EOP"; break;
    case PM4ItOpcode::EventWriteEos: name = "EVENT_WRITE_EOS"; break;
    case PM4ItOpcode::ReleaseMem: name = "RELEASE_MEM"; break;
    case PM4ItOpcode::DmaData: name = "DMA_DATA"; break;
    case PM4ItOpcode::AcquireMem: name = "ACQUIRE_MEM"; break;
    default: break;
    }
    return name ? fmt::format("{}{}", queue, name) : fmt::format("{}{:#x}", queue, op & 0xff);
}

const char* WaitSiteName(size_t i) {
    static constexpr std::array<const char*, size_t(WaitSite::Count)> names{
        "other", "bufdl-gpu", "bufdl-queued", "imgdl", "gc", "gds", "stream", "faultbuf",
        "present"};
    return names[i];
}

/// Every 10 s: the top faulting pages and the GPU-thread drains by packet; resets them.
void LogFaultReport(double secs) {
    std::vector<FaultPage> pages;
    std::array<u32, 0x200> drain_pm4;
    u32 drain_no_pm4;
    u64 dropped;
    std::vector<GpuWrite> writes;
    {
        std::scoped_lock lk{g_write_mutex};
        for (const GpuWrite& w : g_writes) {
            if (w.addr != 0) {
                writes.push_back(w);
            }
        }
        g_writes.fill({});
        g_writes_used = 0;
    }
    std::sort(writes.begin(), writes.end(),
              [](const GpuWrite& a, const GpuWrite& b) { return a.count > b.count; });
    {
        std::scoped_lock lk{g_fault_mutex};
        for (const FaultPage& e : g_fault_pages) {
            if (e.key != 0) {
                pages.push_back(e);
            }
        }
        drain_pm4 = g_drain_pm4;
        drain_no_pm4 = g_drain_no_pm4;
        dropped = g_fault_dropped;
        g_fault_pages.fill({});
        g_fault_used = 0;
        g_fault_dropped = 0;
        g_drain_pm4.fill(0);
        g_drain_no_pm4 = 0;
    }
    u64 total = 0, drains_gpu = 0, drains_queued = 0;
    for (const FaultPage& e : pages) {
        total += e.faults;
        drains_gpu += e.drains_gpu;
        drains_queued += e.drains_queued;
    }
    LOG_INFO(Render,
             "fault pages ({:.0f}s): {} faults on {} pages (+{} on pages past the table), GPU "
             "drains from faults: {} on the GPU thread, {} queued by guest faults",
             secs, total, pages.size(), dropped, drains_gpu, drains_queued);
    const auto print = [&](const char* title, auto key) {
        std::sort(pages.begin(), pages.end(),
                  [&](const FaultPage& a, const FaultPage& b) { return key(a) > key(b); });
        for (size_t i = 0; i < pages.size() && i < 10 && key(pages[i]) > 0; ++i) {
            const FaultPage& e = pages[i];
            std::string pm4;
            for (size_t k = 0; k < e.pm4.size(); ++k) {
                if (e.pm4[k] != NoPm4) {
                    pm4 += fmt::format(" {}x{}", Pm4Name(e.pm4[k]), e.pm4_count[k]);
                }
            }
            LOG_INFO(Render,
                     "  {} #{} page {:#x}: faults {} gpu r/w {}/{} guest r/w {}/{} "
                     "gpu-modified {} drains gpu/queued {}/{} pm4{}",
                     title, i + 1, e.key - 1, e.faults, e.gpu_reads, e.gpu_writes, e.guest_reads,
                     e.guest_writes, e.gpu_modified, e.drains_gpu, e.drains_queued,
                     pm4.empty() ? std::string{" -"} : pm4);
        }
    };
    print("top faults", [](const FaultPage& e) { return u64(e.faults); });
    print("top drains", [](const FaultPage& e) { return u64(e.drains_gpu) + e.drains_queued; });
    // pages is now sorted by drains: who reads them, and which GPU-written bindings cover them.
    for (size_t i = 0; i < pages.size() && i < 5 && pages[i].drains_gpu + pages[i].drains_queued;
         ++i) {
        const FaultPage& e = pages[i];
        const u64 page = e.key - 1;
        std::string covers;
        size_t n = 0;
        for (const GpuWrite& w : writes) {
            if (w.addr < page + 0x1000 && page < w.addr + w.size) {
                if (n++ < 4) {
                    covers += fmt::format(" [{:#x} +{:#x} x{} {}]", w.addr, w.size, w.count,
                                          Pm4Name(w.pm4));
                }
            }
        }
        std::string changed;
        for (u32 dw = 0; dw < 1024;) {
            if (!e.changed[dw]) {
                ++dw;
                continue;
            }
            u32 end = dw;
            while (end < 1024 && e.changed[end]) {
                ++end;
            }
            changed += fmt::format(" +{:#x}..+{:#x}", dw * 4, end * 4);
            dw = end;
        }
        LOG_INFO(Render,
                 "  drain page {:#x}: gpu-thread reads at +{:#x}..+{:#x} from {}; written by {} "
                 "binding(s):{}; {} downloads compared, changed:{}",
                 page, e.gpu_read_min, e.gpu_read_max, HostSymbol(e.gpu_read_rip), n,
                 covers.empty() ? std::string{" none seen"} : covers, e.compares,
                 changed.empty() ? std::string{" nothing"} : changed);
    }
    std::vector<std::pair<u32, u32>> ops;
    for (u32 op = 0; op < drain_pm4.size(); ++op) {
        if (drain_pm4[op] != 0) {
            ops.emplace_back(drain_pm4[op], op);
        }
    }
    std::sort(ops.rbegin(), ops.rend());
    std::string line;
    for (size_t i = 0; i < ops.size() && i < 8; ++i) {
        line += fmt::format(" {} {}", Pm4Name(ops[i].second), ops[i].first);
    }
    if (drain_no_pm4 != 0) {
        line += fmt::format(" (no packet) {}", drain_no_pm4);
    }
    LOG_INFO(Render, "  GPU-thread fault drains by packet:{}", line.empty() ? " none" : line);
}

// ----------------------------------------------------------------- stats

std::jthread g_stats_thread;

void StatsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("sotcgpu:Stats");
    std::mutex m;
    std::condition_variable_any cv;
    auto take = [](Counter c) {
        return g_counters[size_t(c)].exchange(0, std::memory_order_relaxed);
    };
    auto last = std::chrono::steady_clock::now();
    auto last_report = last;
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{m};
            cv.wait_for(lk, stoken, std::chrono::seconds(1), [] { return false; });
        }
        if (stoken.stop_requested()) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - last).count();
        last = now;
        const u64 draws = take(Counter::Draw), indirect = take(Counter::DrawIndirect),
                  dispatches = take(Counter::Dispatch), waits = take(Counter::SchedulerWait),
                  bytes = take(Counter::CopyBytes), rf = take(Counter::ReadFault),
                  wf = take(Counter::WriteFault), emulated = take(Counter::SrtEmulated),
                  aliased = take(Counter::SrtAliased), submits = take(Counter::Submit),
                  flips = take(Counter::Flip), gpu_wait_ns = take(Counter::GpuThreadWaitNs),
                  other_wait_ns = take(Counter::OtherWaitNs),
                  side_wait_ns = take(Counter::SideCopyWaitNs);
        // Session 12: GPU time from timestamps (sotc_gpu_timer.h), collected without waiting,
        // so a second's numbers trail by up to a second.
        const GpuTimer::Report gpu = GpuTimer::Collect();
        const double per_s = secs > 0.0 ? 1.0 / secs : 0.0;
        LOG_INFO(Render,
                 "gpu stats ({:.2f}s): draws {} indirect {} dispatches {} sched-waits {} "
                 "guest->gpu {:.1f} MiB faults r/w {}/{} walker aliased/emulated {}/{} | "
                 "gpu busy {:.0f} ms/s (main {:.0f}, side {:.0f}; {} timed, {} untimed, {} in "
                 "flight), submits {}, flips {}, gpu-thread wait {:.0f} ms/s (other threads "
                 "{:.0f}; side-copy fences {:.0f})",
                 secs, draws, indirect, dispatches, waits, double(bytes) / (1024.0 * 1024.0), rf,
                 wf, aliased, emulated, gpu.busy_ms * per_s, gpu.main_ms * per_s,
                 gpu.side_ms * per_s, gpu.timed, gpu.untimed, gpu.in_flight, submits, flips,
                 double(gpu_wait_ns) / 1e6 * per_s, double(other_wait_ns) / 1e6 * per_s,
                 double(side_wait_ns) / 1e6 * per_s);
        std::string sites;
        for (size_t i = 0; i < g_wait_sites.size(); ++i) {
            if (const u64 n = g_wait_sites[i].exchange(0, std::memory_order_relaxed)) {
                sites += fmt::format(" {} {}", WaitSiteName(i), n);
            }
        }
        const u64 dl_batch = take(Counter::DownloadInBatch),
                  dl_sub = take(Counter::DownloadSubmitted), dl_done = take(Counter::DownloadDone),
                  dl_unknown = take(Counter::DownloadUnknownTick),
                  dl_side = take(Counter::SideReadback),
                  patched_tables = take(Counter::SrtPatchedTables),
                  patched_dwords = take(Counter::SrtPatchedDwords),
                  hot_flushes = take(Counter::HotFlush);
        if (!sites.empty() || dl_batch + dl_sub + dl_done + dl_unknown + patched_tables != 0) {
            LOG_INFO(Render,
                     "  sched-waits by site:{}; buffer downloads written in batch/submitted/"
                     "done/unknown {}/{}/{}/{}, side copies {}; walker tables patched on "
                     "the GPU {} ({} dwords); hot flushes {}",
                     sites, dl_batch, dl_sub, dl_done, dl_unknown, dl_side, patched_tables,
                     patched_dwords, hot_flushes);
        }
        if (now - last_report >= std::chrono::seconds(10)) {
            LogFaultReport(std::chrono::duration<double>(now - last_report).count());
            last_report = now;
        }
    }
}

// ----------------------------------------------------------------- render-target dump

enum class RtState : int { Off, Idle, Armed, Recording, Done };
std::atomic<RtState> g_rt_state{RtState::Off};
std::string g_rt_dir;
bool g_rt_all = false;
u32 g_rt_seq = 0;
std::atomic<u32> g_rt_skip_ticks{0}; ///< SOTC_DUMP_RT_SKIP: screenshot ticks to ignore first

struct RtRecord {
    VideoCore::ImageId id;
    u64 uid;
    VAddr address;
    vk::Format format;
    u32 width, height;
    bool as_color, as_depth;
    u32 draws, indirect_draws;
};
std::mutex g_rt_mutex;
std::vector<RtRecord> g_rt_records;
u64 g_rt_total_draws = 0;

/// Readback buffer, reused across dumps (Runtime tracks buffer barriers by pointer). Freed
/// in Shutdown; leaked if the GPU state is leaked.
VideoCore::Buffer* g_readback = nullptr;

// --- pixel decoding

float HalfToFloat(u16 h) {
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    float v;
    if (exp == 0) {
        v = std::ldexp(float(mant), -24);
    } else if (exp == 31) {
        v = mant ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    } else {
        v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    }
    return sign ? -v : v;
}

/// Unsigned small float (R11G11B10F): 5-bit exponent, `mbits` mantissa bits.
float UFloat(u32 bits, u32 mbits) {
    const u32 exp = bits >> mbits, mant = bits & ((1u << mbits) - 1);
    if (exp == 0) {
        return std::ldexp(float(mant), -14 - int(mbits));
    }
    if (exp == 31) {
        return mant ? std::numeric_limits<float>::quiet_NaN()
                    : std::numeric_limits<float>::infinity();
    }
    return std::ldexp(float(mant | (1u << mbits)), int(exp) - 15 - int(mbits));
}

template <typename T>
T Load(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

struct FormatInfo {
    const char* name;
    u32 bytes;      ///< per texel in the download
    u32 channels;   ///< decoded channels (1..4)
    bool normalize; ///< map [min, max] to [0, 255] instead of clamping [0, 1]
    vk::ImageAspectFlagBits aspect;
    void (*decode)(const u8* p, float* out);
};

const FormatInfo* GetFormat(vk::Format f) {
    using F = vk::Format;
    constexpr auto C = vk::ImageAspectFlagBits::eColor;
    constexpr auto D = vk::ImageAspectFlagBits::eDepth;
    constexpr auto S = vk::ImageAspectFlagBits::eStencil;
    static const FormatInfo rgba8{"RGBA8", 4, 4, false, C, [](const u8* p, float* o) {
                                      for (int i = 0; i < 4; ++i) o[i] = p[i] / 255.f;
                                  }};
    static const FormatInfo bgra8{"BGRA8", 4, 4, false, C, [](const u8* p, float* o) {
                                      o[0] = p[2] / 255.f, o[1] = p[1] / 255.f;
                                      o[2] = p[0] / 255.f, o[3] = p[3] / 255.f;
                                  }};
    static const FormatInfo a2b10g10r10{"RGB10A2", 4, 4, false, C, [](const u8* p, float* o) {
                                            const u32 v = Load<u32>(p);
                                            o[0] = (v & 0x3ff) / 1023.f;
                                            o[1] = ((v >> 10) & 0x3ff) / 1023.f;
                                            o[2] = ((v >> 20) & 0x3ff) / 1023.f;
                                            o[3] = (v >> 30) / 3.f;
                                        }};
    static const FormatInfo a2r10g10b10{"BGR10A2", 4, 4, false, C, [](const u8* p, float* o) {
                                            const u32 v = Load<u32>(p);
                                            o[2] = (v & 0x3ff) / 1023.f;
                                            o[1] = ((v >> 10) & 0x3ff) / 1023.f;
                                            o[0] = ((v >> 20) & 0x3ff) / 1023.f;
                                            o[3] = (v >> 30) / 3.f;
                                        }};
    static const FormatInfo r11g11b10{"R11G11B10F", 4, 3, false, C, [](const u8* p, float* o) {
                                          const u32 v = Load<u32>(p);
                                          o[0] = UFloat(v & 0x7ff, 6);
                                          o[1] = UFloat((v >> 11) & 0x7ff, 6);
                                          o[2] = UFloat(v >> 22, 5);
                                      }};
    static const FormatInfo rgba16f{"RGBA16F", 8, 4, false, C, [](const u8* p, float* o) {
                                        for (int i = 0; i < 4; ++i)
                                            o[i] = HalfToFloat(Load<u16>(p + 2 * i));
                                    }};
    static const FormatInfo rgba16{"RGBA16", 8, 4, false, C, [](const u8* p, float* o) {
                                       for (int i = 0; i < 4; ++i)
                                           o[i] = Load<u16>(p + 2 * i) / 65535.f;
                                   }};
    static const FormatInfo rg16f{"RG16F", 4, 2, false, C, [](const u8* p, float* o) {
                                      o[0] = HalfToFloat(Load<u16>(p));
                                      o[1] = HalfToFloat(Load<u16>(p + 2));
                                  }};
    static const FormatInfo rg16{"RG16", 4, 2, false, C, [](const u8* p, float* o) {
                                     o[0] = Load<u16>(p) / 65535.f;
                                     o[1] = Load<u16>(p + 2) / 65535.f;
                                 }};
    static const FormatInfo rgba32f{"RGBA32F", 16, 4, false, C, [](const u8* p, float* o) {
                                        std::memcpy(o, p, 16);
                                    }};
    static const FormatInfo rg32f{"RG32F", 8, 2, true, C,
                                  [](const u8* p, float* o) { std::memcpy(o, p, 8); }};
    static const FormatInfo r32f{"R32F", 4, 1, true, C,
                                 [](const u8* p, float* o) { o[0] = Load<float>(p); }};
    static const FormatInfo r32u{"R32U", 4, 1, true, C,
                                 [](const u8* p, float* o) { o[0] = float(Load<u32>(p)); }};
    static const FormatInfo r16f{"R16F", 2, 1, true, C,
                                 [](const u8* p, float* o) { o[0] = HalfToFloat(Load<u16>(p)); }};
    static const FormatInfo r16{"R16", 2, 1, false, C,
                                [](const u8* p, float* o) { o[0] = Load<u16>(p) / 65535.f; }};
    static const FormatInfo rg8{"RG8", 2, 2, false, C, [](const u8* p, float* o) {
                                    o[0] = p[0] / 255.f, o[1] = p[1] / 255.f;
                                }};
    static const FormatInfo r8{"R8", 1, 1, false, C, [](const u8* p, float* o) { o[0] = p[0] / 255.f; }};
    static const FormatInfo d32{"D32", 4, 1, true, D,
                                [](const u8* p, float* o) { o[0] = Load<float>(p); }};
    static const FormatInfo d24{"D24", 4, 1, true, D, [](const u8* p, float* o) {
                                    o[0] = (Load<u32>(p) & 0xffffff) / 16777215.f;
                                }};
    static const FormatInfo d16{"D16", 2, 1, true, D,
                                [](const u8* p, float* o) { o[0] = Load<u16>(p) / 65535.f; }};
    static const FormatInfo s8{"S8", 1, 1, true, S, [](const u8* p, float* o) { o[0] = p[0]; }};
    switch (f) {
    case F::eR8G8B8A8Unorm:
    case F::eR8G8B8A8Srgb:
    case F::eR8G8B8A8Snorm:
    case F::eR8G8B8A8Uint:
        return &rgba8;
    case F::eB8G8R8A8Unorm:
    case F::eB8G8R8A8Srgb:
        return &bgra8;
    case F::eA2B10G10R10UnormPack32:
    case F::eA2B10G10R10UintPack32:
        return &a2b10g10r10;
    case F::eA2R10G10B10UnormPack32:
        return &a2r10g10b10;
    case F::eB10G11R11UfloatPack32:
        return &r11g11b10;
    case F::eR16G16B16A16Sfloat:
        return &rgba16f;
    case F::eR16G16B16A16Unorm:
    case F::eR16G16B16A16Uint:
        return &rgba16;
    case F::eR16G16Sfloat:
        return &rg16f;
    case F::eR16G16Unorm:
    case F::eR16G16Uint:
        return &rg16;
    case F::eR32G32B32A32Sfloat:
        return &rgba32f;
    case F::eR32G32Sfloat:
        return &rg32f;
    case F::eR32Sfloat:
        return &r32f;
    case F::eR32Uint:
    case F::eR32Sint:
        return &r32u;
    case F::eR16Sfloat:
        return &r16f;
    case F::eR16Unorm:
    case F::eR16Uint:
        return &r16;
    case F::eR8G8Unorm:
    case F::eR8G8Uint:
        return &rg8;
    case F::eR8Unorm:
    case F::eR8Uint:
        return &r8;
    case F::eD32Sfloat:
    case F::eD32SfloatS8Uint:
        return &d32;
    case F::eX8D24UnormPack32:
    case F::eD24UnormS8Uint:
        return &d24;
    case F::eD16Unorm:
    case F::eD16UnormS8Uint:
        return &d16;
    case F::eS8Uint:
        return &s8;
    default:
        return nullptr;
    }
}

struct Converted {
    std::vector<u8> pixels; ///< gray (1 channel) or RGB
    int channels;
    float min, max; ///< over every decoded channel (finite values)
    bool uniform;   ///< every texel has the same raw bytes
};

Converted Convert(const FormatInfo& fi, const u8* data, u32 w, u32 h) {
    const u64 n = u64(w) * h;
    Converted out{};
    out.channels = fi.channels == 1 ? 1 : 3;
    out.pixels.resize(n * out.channels);
    out.uniform = true;
    std::vector<float> values(n * fi.channels);
    float mn = std::numeric_limits<float>::infinity(), mx = -mn;
    for (u64 i = 0; i < n; ++i) {
        const u8* p = data + i * fi.bytes;
        if (out.uniform && i > 0 && std::memcmp(p, data, fi.bytes) != 0) {
            out.uniform = false;
        }
        float px[4]{};
        fi.decode(p, px);
        for (u32 c = 0; c < fi.channels; ++c) {
            const float v = px[c];
            values[i * fi.channels + c] = v;
            if (std::isfinite(v)) {
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
        }
    }
    if (!(mn <= mx)) {
        mn = mx = 0.f;
    }
    out.min = mn;
    out.max = mx;
    const float lo = fi.normalize ? mn : 0.f;
    const float scale = fi.normalize ? (mx > mn ? 1.f / (mx - mn) : 0.f) : 1.f;
    for (u64 i = 0; i < n; ++i) {
        for (int c = 0; c < out.channels; ++c) {
            const float v = u32(c) < fi.channels ? values[i * fi.channels + c] : 0.f;
            const float t = std::isfinite(v) ? std::clamp((v - lo) * scale, 0.f, 1.f) : 0.f;
            out.pixels[i * out.channels + c] = u8(t * 255.f + 0.5f);
        }
    }
    return out;
}

bool WritePng(const std::string& path, const Converted& c, u32 w, u32 h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const auto write = [](void* ctx, void* data, int size) {
        std::fwrite(data, 1, size, static_cast<FILE*>(ctx));
    };
    bool ok = stbi_write_png_to_func(write, f, int(w), int(h), c.channels, c.pixels.data(),
                                     int(w) * c.channels) != 0;
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

/// Downloads, converts and writes every recorded target (GPU thread, recording stopped).
void DumpTargets(Vulkan::Rasterizer& rasterizer) {
    std::vector<RtRecord> records;
    u64 total_draws;
    {
        std::scoped_lock lk{g_rt_mutex};
        records.swap(g_rt_records);
        total_draws = g_rt_total_draws;
    }
    const u32 seq = g_rt_seq++;
    auto& tc = rasterizer.GetTextureCache();
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = rasterizer.GetScheduler();
    const auto& instance = runtime.GetInstance();

    std::error_code ec;
    std::filesystem::create_directories(g_rt_dir, ec);
    FILE* index = std::fopen((std::filesystem::path(g_rt_dir) / "index.txt").string().c_str(), "a");
    if (index) {
        std::fprintf(index, "# dump %u: %zu targets, %llu draws\n", seq, records.size(),
                     (unsigned long long)total_draws);
        std::fprintf(index, "# file address format size role draws(direct/indirect) min max "
                            "uniform\n");
    }
    LOG_INFO(Render, "dump_rt {}: {} targets, {} draws -> {}", seq, records.size(), total_draws,
             g_rt_dir);

    for (size_t t = 0; t < records.size(); ++t) {
        const RtRecord& r = records[t];
        const char* role = r.as_color && r.as_depth ? "color+depth" : r.as_depth ? "depth" : "color";
        const FormatInfo* fi = GetFormat(r.format);
        const std::string fmt_name = fi ? fi->name : vk::to_string(r.format);
        std::string status;

        // The image may have been destroyed since the draw: look it up by address and uid.
        VideoCore::Image* image = nullptr;
        tc.ForEachImageWithAddress(r.address, [&](VideoCore::ImageId id, VideoCore::Image& img) {
            if (id == r.id && img.image_uid == r.uid) {
                image = &img;
            }
        });
        const std::string file = fmt::format("d{:03}_t{:02}_{:#x}_{}.png", seq, t, r.address,
                                             fmt_name);
        Converted conv{};
        bool written = false;
        if (!image) {
            status = "gone";
        } else if (!fi) {
            status = "unsupported format";
        } else if (image->info.num_samples > 1) {
            status = fmt::format("msaa x{} skipped", image->info.num_samples);
        } else {
            const u32 w = image->info.size.width, h = image->info.size.height;
            const u64 size = u64(w) * h * fi->bytes;
            if (!g_readback || g_readback->SizeBytes() < size) {
                delete g_readback;
                g_readback = new VideoCore::Buffer(instance, 0, std::max<u64>(size, 16_MB),
                                                   VideoCore::MemoryType::HostCached,
                                                   "sotc dump_rt readback");
            }
            const vk::BufferImageCopy copy{
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource{
                    .aspectMask = fi->aspect,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {0, 0, 0},
                .imageExtent = {w, h, 1},
            };
            runtime.DownloadImage(image, g_readback, std::span{&copy, 1});
            scheduler.Finish();
            g_readback->Invalidate(0, size);
            conv = Convert(*fi, g_readback->mapped_data.data(), w, h);
            written = WritePng((std::filesystem::path(g_rt_dir) / file).string(), conv, w, h);
            status = written ? "ok" : "write failed";
        }
        LOG_INFO(Render,
                 "dump_rt {} t{:02}: {:#x} {} {}x{} {} draws {}/{} min {:g} max {:g}{} -> {}",
                 seq, t, r.address, fmt_name, r.width, r.height, role, r.draws, r.indirect_draws,
                 conv.min, conv.max, written && conv.uniform ? " UNIFORM" : "",
                 written ? file : status);
        if (index) {
            std::fprintf(index, "%s %#llx %s %ux%u %s %u/%u %g %g %s\n",
                         written ? file.c_str() : ("-(" + status + ")").c_str(),
                         (unsigned long long)r.address, fmt_name.c_str(), r.width, r.height, role,
                         r.draws, r.indirect_draws, conv.min, conv.max,
                         written ? (conv.uniform ? "yes" : "no") : "-");
        }
    }
    if (index) {
        std::fclose(index);
    }
}

} // namespace

void OnFault(VAddr addr, bool is_write, bool gpu_thread, bool gpu_modified, u64 rip) {
    const u32 pm4 = gpu_thread ? t_pm4 : NoPm4;
    std::scoped_lock lk{g_fault_mutex};
    FaultPage* e = FindPage(addr & ~VAddr{0xfff}, true);
    if (!e) {
        ++g_fault_dropped;
        return;
    }
    ++e->faults;
    ++(gpu_thread ? (is_write ? e->gpu_writes : e->gpu_reads)
                  : (is_write ? e->guest_writes : e->guest_reads));
    e->gpu_modified += gpu_modified ? 1 : 0;
    if (gpu_thread && !is_write) {
        const u32 off = u32(addr & 0xfff);
        if (e->gpu_reads == 1) {
            e->gpu_read_min = e->gpu_read_max = off;
        }
        e->gpu_read_min = std::min(e->gpu_read_min, off);
        e->gpu_read_max = std::max(e->gpu_read_max, off);
        e->gpu_read_rip = rip;
    }
    if (pm4 != NoPm4) {
        for (size_t k = 0; k < e->pm4.size(); ++k) {
            if (e->pm4[k] == pm4 || e->pm4[k] == NoPm4) {
                e->pm4[k] = pm4;
                ++e->pm4_count[k];
                break;
            }
        }
    }
}

void OnDownloadCompare(VAddr addr, const u8* cpu, const u8* gpu, u64 size) {
    std::scoped_lock lk{g_fault_mutex};
    for (u64 page = addr & ~VAddr{0xfff}; page < addr + size; page += 0x1000) {
        FaultPage* e = FindPage(page, false);
        if (!e) {
            continue;
        }
        ++e->compares;
        const u64 lo = std::max<u64>(page, addr), hi = std::min<u64>(page + 0x1000, addr + size);
        for (u64 a = lo & ~u64{3}; a + 4 <= hi; a += 4) {
            if (a < addr) {
                continue;
            }
            if (std::memcmp(cpu + (a - addr), gpu + (a - addr), 4) != 0) {
                e->changed[(a - page) / 4] = 1;
            }
        }
    }
}

void OnGpuWrite(VAddr addr, u64 size) {
    if (addr == 0) {
        return;
    }
    const u32 pm4 = t_pm4;
    std::scoped_lock lk{g_write_mutex};
    size_t i = size_t(((addr ^ (size << 20)) * 0x9E3779B97F4A7C15ull) >> 52) & (WriteTableSize - 1);
    for (size_t n = 0; n < WriteTableSize; ++n, i = (i + 1) & (WriteTableSize - 1)) {
        GpuWrite& w = g_writes[i];
        if (w.addr == addr && w.size == size) {
            ++w.count;
            w.pm4 = pm4;
            return;
        }
        if (w.addr == 0) {
            if (g_writes_used >= WriteTableSize * 3 / 4) {
                return;
            }
            ++g_writes_used;
            w = {addr, size, 1, pm4};
            return;
        }
    }
}

void OnFaultDrain(VAddr addr, bool gpu_thread) {
    const u32 pm4 = gpu_thread ? t_pm4 : NoPm4;
    std::scoped_lock lk{g_fault_mutex};
    if (FaultPage* e = FindPage(addr & ~VAddr{0xfff}, false)) {
        ++(gpu_thread ? e->drains_gpu : e->drains_queued);
    }
    if (gpu_thread) {
        ++(pm4 < g_drain_pm4.size() ? g_drain_pm4[pm4] : g_drain_no_pm4);
    }
}

void RecordTargets(bool indirect, VideoCore::TextureCache& tc, const VideoCore::ImageId* colors,
                   u32 num_colors, VideoCore::ImageId depth) {
    std::scoped_lock lk{g_rt_mutex};
    ++g_rt_total_draws;
    const auto add = [&](VideoCore::ImageId id, bool is_depth) {
        if (!id) {
            return;
        }
        const VideoCore::Image& image = tc.GetImage(id);
        auto it = std::ranges::find_if(g_rt_records, [&](const RtRecord& r) {
            return r.id == id && r.uid == image.image_uid;
        });
        if (it == g_rt_records.end()) {
            it = g_rt_records.insert(g_rt_records.end(),
                                     RtRecord{
                                         .id = id,
                                         .uid = image.image_uid,
                                         .address = image.info.guest_address,
                                         .format = image.info.pixel_format,
                                         .width = image.info.size.width,
                                         .height = image.info.size.height,
                                     });
        }
        (is_depth ? it->as_depth : it->as_color) = true;
        ++(indirect ? it->indirect_draws : it->draws);
    };
    for (u32 i = 0; i < num_colors; ++i) {
        add(colors[i], false);
    }
    add(depth, true);
}

void Init() {
    auto& settings = SotcGpuGetSettings();
    if (const char* v = std::getenv("SOTC_GPU_DUMP"); v && *v && *v != '0') {
        settings.dump_shaders = true;
        LOG_WARNING(Render, "SOTC_GPU_DUMP: dumping shaders to <cache_dir>/shader/dumps");
    }
    u32 flags = 0;
    if (const char* v = std::getenv("SOTC_GPU_STATS"); v && *v && *v != '0') {
        flags |= StatsOn;
        for (auto& c : g_counters) {
            c.store(0, std::memory_order_relaxed);
        }
        g_stats_thread = std::jthread{StatsThread};
    }
    if (const char* v = std::getenv("SOTC_DUMP_RT"); v && *v) {
        g_rt_dir = v;
        const char* all = std::getenv("SOTC_DUMP_RT_ALL");
        g_rt_all = all && *all && *all != '0';
        const char* skip = std::getenv("SOTC_DUMP_RT_SKIP");
        g_rt_skip_ticks.store(skip ? u32(std::strtoul(skip, nullptr, 10)) : 0);
        g_rt_state.store(RtState::Idle);
        LOG_WARNING(Render, "SOTC_DUMP_RT: render targets of one frame per {} into {}",
                    g_rt_all ? "screenshot tick" : "run (first screenshot tick)", g_rt_dir);
    }
    g_flags.store(flags, std::memory_order_relaxed);
}

void Shutdown() {
    g_flags.store(0, std::memory_order_relaxed);
    if (g_stats_thread.joinable()) {
        g_stats_thread.request_stop();
        g_stats_thread.join();
    }
    GpuTimer::Destroy();
    delete g_readback;
    g_readback = nullptr;
    g_rt_state.store(RtState::Off);
    std::scoped_lock lk{g_rt_mutex};
    g_rt_records.clear();
}

void OnScreenshotTick() {
    // SOTC_DUMP_RT_SKIP=N: the first N ticks don't arm (reach gameplay before a one-shot dump).
    for (u32 n = g_rt_skip_ticks.load(); n != 0;) {
        if (g_rt_skip_ticks.compare_exchange_weak(n, n - 1)) {
            return;
        }
    }
    RtState expected = RtState::Idle;
    g_rt_state.compare_exchange_strong(expected, RtState::Armed);
}

void OnFlip(Vulkan::Rasterizer& rasterizer) {
    Count(Counter::Flip);
    const RtState state = g_rt_state.load(std::memory_order_relaxed);
    if (state == RtState::Armed) {
        {
            std::scoped_lock lk{g_rt_mutex};
            g_rt_records.clear();
            g_rt_total_draws = 0;
        }
        g_rt_state.store(RtState::Recording);
        g_flags.fetch_or(RecordRt, std::memory_order_relaxed);
    } else if (state == RtState::Recording) {
        g_flags.fetch_and(~u32(RecordRt), std::memory_order_relaxed);
        DumpTargets(rasterizer);
        g_rt_state.store(g_rt_all ? RtState::Idle : RtState::Done);
    }
}

} // namespace SotcDebug
