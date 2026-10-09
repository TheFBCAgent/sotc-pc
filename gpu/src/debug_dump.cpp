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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
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
thread_local u64 t_shader = 0;
thread_local u64 t_dispatch_seq = 0;
thread_local bool t_gpu_thread = false;

namespace {

// ----------------------------------------------------------------- fault histogram

const char* SrtOutcomeName(SrtOutcome o) {
    constexpr const char* names[] = {"pending",   "aliased",     "not GPU-written",
                                     "extent unknown", "no walker info", "patch off",
                                     "page not blocked", "not patchable", "patched"};
    return names[u32(o)];
}


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
    u64 guest_rip;                ///< guest instruction of the last guest-thread fault
    u32 gpu_read_min, gpu_read_max; ///< in-page offsets of the GPU-thread read faults
    std::array<u8, 1024> changed;   ///< per dword of the page: a download found it changed
    u32 compares;                   ///< downloads compared on this page
    u64 gpu_fault_ns;               ///< time the GPU thread spent in faults on this page
    u64 srt_shader, srt_table, srt_ptr, srt_bytes; ///< the walker table of the last such fault
    SrtOutcome srt_outcome;                        ///< and what ResolveSrtTable did with it
};

/// A buffer binding the GPU may write (ObtainBuffer is_written), keyed by (addr, size).
struct GpuWrite {
    u64 addr; ///< 0 = empty slot
    u64 size;
    u32 count;
    u32 pm4;
    u64 seq;    ///< g_write_seq at the last write (the newest covering binding wrote last)
    u64 shader; ///< compute program hash of the last write (0 = not a dispatch)
    u64 tick;   ///< scheduler tick of the last write
};
constexpr size_t WriteTableSize = 4096; // power of two
u64 g_writes_dropped = 0; ///< new bindings once the table was 3/4 full
/// Session 17: the last bindings written on each 64 KiB page (g_writes overflows in 10 s), so a
/// drain finds the binding covering its line and the one that put its 64 KiB in the batch.
/// Under g_write_mutex; cleared when it grows past 1M pages.
std::unordered_map<u64, std::array<GpuWrite, 8>> g_page_writes;
std::mutex g_write_mutex;
std::array<GpuWrite, WriteTableSize> g_writes{};
size_t g_writes_used = 0;
u64 g_write_seq = 0;

/// Session 15: drains that needed the batch being recorded (a Scheduler::Finish each), by page,
/// reader (GPU-thread packet and host instruction, or the guest instruction) and the newest
/// GPU-written binding covering the address. Under g_fault_mutex.
struct InBatchDrain {
    u64 page;
    bool gpu_thread;
    u32 reader_pm4;
    u64 reader_rip;
    u64 writer_addr, writer_size;
    u32 writer_pm4;
    u32 count, was_hot, request_in_batch;
};
std::vector<InBatchDrain> g_in_batch;
u32 g_in_batch_total = 0, g_in_batch_hot = 0, g_in_batch_hot_full = 0, g_in_batch_dropped = 0,
    g_in_batch_request = 0;

/// Session 17 step 1: how the in-batch drains' lines and pages come back, per reader, and the
/// writers that fed them (compute shader hash + binding size). The last-seen maps persist across
/// reports (bounded), the per-reader tables reset with the 10 s report. Under g_fault_mutex.
struct ReuseSeen {
    u64 frame, tick;
};
std::unordered_map<u64, ReuseSeen> g_reuse_lines, g_reuse_pages; ///< 256 B line / 64 KiB page
constexpr size_t ReuseBuckets = 6; ///< new, same frame, 1 frame, 2-3, 4-15, 16+ frames ago
struct ReuseWriter {
    u64 shader, size;
    u32 count;
};
struct ReuseReader {
    bool gpu_thread;
    u64 rip;
    u32 drains;
    std::array<u32, ReuseBuckets> line_gap, page_gap;
    u32 line_near, page_near; ///< the line/page needed an in-batch drain <= 32 submits ago
    u32 line_hot;             ///< the line was hot (the hot flush missed it anyway)
    u32 own_in_batch;         ///< the binding covering the read address was written in the batch
    u32 no_writer;            ///< no binding written in the batch found on the 64 KiB
    std::unordered_set<u64> lines, pages;
    std::vector<ReuseWriter> writers;
};
std::vector<ReuseReader> g_reuse;
/// Writer keys (shader, binding size) that fed an in-batch drain: reads fed and bindings written
/// per report (the extra submits a writer-keyed flush would cost). Under g_write_mutex.
struct FeederStats {
    u32 reads;         ///< in-batch drains it was the newest covering writer of
    u32 dispatches;    ///< dispatches that wrote such a binding (a flush each would add a submit)
    u64 last_dispatch; ///< t_dispatch_seq of the last one
};
struct FeederKeyHash {
    size_t operator()(const std::pair<u64, u64>& k) const {
        return std::hash<u64>{}(k.first * 31 + k.second);
    }
};
std::unordered_map<std::pair<u64, u64>, FeederStats, FeederKeyHash> g_feeders;

size_t ReuseBucket(const ReuseSeen* seen, u64 frame) {
    if (!seen) {
        return 0;
    }
    const u64 gap = frame - seen->frame;
    return gap == 0 ? 1 : gap == 1 ? 2 : gap <= 3 ? 3 : gap <= 15 ? 4 : 5;
}

constexpr size_t FaultTableSize = 4096; // power of two
std::mutex g_fault_mutex;
std::array<FaultPage, FaultTableSize> g_fault_pages{};
size_t g_fault_used = 0;
u64 g_fault_dropped = 0;              ///< faults on new pages once the table was 3/4 full
                                      ///< (the last quarter is kept for GPU-thread pages)
std::array<u32, 0x200> g_drain_pm4{}; ///< GPU-thread drains by packet (opcode | 0x100 compute)
u32 g_drain_no_pm4 = 0;               ///< GPU-thread drains outside packet processing

FaultPage* FindPage(u64 page, bool create, bool gpu_thread = false) {
    const u64 key = page + 1;
    size_t i = size_t(((page >> 12) * 0x9E3779B97F4A7C15ull) >> 52) & (FaultTableSize - 1);
    for (size_t n = 0; n < FaultTableSize; ++n, i = (i + 1) & (FaultTableSize - 1)) {
        FaultPage& e = g_fault_pages[i];
        if (e.key == key) {
            return &e;
        }
        if (e.key == 0) {
            // Guest faults fill the table (~100k/10 s on thousands of pages); session 19: the
            // GPU thread's few pages keep the last quarter, so a once-per-frame one isn't lost.
            if (!create || g_fault_used >= (gpu_thread ? FaultTableSize - 1 : FaultTableSize * 3 / 4)) {
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
    std::vector<InBatchDrain> in_batch;
    u32 ib_total, ib_hot, ib_full, ib_dropped, ib_request;
    std::vector<ReuseReader> reuse;
    std::vector<std::pair<std::pair<u64, u64>, FeederStats>> feeders;
    u64 writes_dropped;
    {
        std::scoped_lock lk{g_write_mutex};
        for (auto& [key, f] : g_feeders) {
            feeders.emplace_back(key, f);
            f.reads = 0;
            f.dispatches = 0;
        }
        for (const GpuWrite& w : g_writes) {
            if (w.addr != 0) {
                writes.push_back(w);
            }
        }
        g_writes.fill({});
        g_writes_used = 0;
        writes_dropped = std::exchange(g_writes_dropped, 0);
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
        in_batch.swap(g_in_batch);
        ib_total = std::exchange(g_in_batch_total, 0);
        ib_hot = std::exchange(g_in_batch_hot, 0);
        ib_full = std::exchange(g_in_batch_hot_full, 0);
        ib_dropped = std::exchange(g_in_batch_dropped, 0);
        ib_request = std::exchange(g_in_batch_request, 0);
        reuse.swap(g_reuse);
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
    // Who reads a drained page, and which GPU-written bindings cover it.
    const auto detail = [&](const char* title, const FaultPage& e) {
        const u64 page = e.key - 1;
        std::string covers;
        size_t n = 0;
        for (const GpuWrite& w : writes) {
            if (w.addr < page + 0x1000 && page < w.addr + w.size) {
                if (n++ < 4) {
                    covers += fmt::format(" [{:#x} +{:#x} x{} {}{}]", w.addr, w.size, w.count,
                                          Pm4Name(w.pm4),
                                          w.shader ? fmt::format(" cs {:#x}", w.shader)
                                                   : std::string{});
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
                 "  {} page {:#x}: gpu-thread reads at +{:#x}..+{:#x} from {}; written by {} "
                 "binding(s):{}; {} downloads compared, changed:{}",
                 title, page, e.gpu_read_min, e.gpu_read_max,
                 HostSymbol(e.gpu_read_rip) +
                     (e.srt_ptr ? fmt::format(" (walker: shader {:#x} table {} at {:#x} +{:#x}, {})",
                                              e.srt_shader, e.srt_table, e.srt_ptr, e.srt_bytes,
                                              SrtOutcomeName(e.srt_outcome))
                                : std::string{}),
                 n,
                 covers.empty() ? std::string{" none seen"} : covers, e.compares,
                 changed.empty() ? std::string{" nothing"} : changed);
    };
    // pages is now sorted by drains.
    for (size_t i = 0; i < pages.size() && i < 5 && pages[i].drains_gpu + pages[i].drains_queued;
         ++i) {
        detail("drain", pages[i]);
    }
    // Session 19: the GPU thread's own faults by time spent, which the guest pages above crowd
    // out.
    std::sort(pages.begin(), pages.end(), [](const FaultPage& a, const FaultPage& b) {
        return a.gpu_fault_ns > b.gpu_fault_ns;
    });
    for (size_t i = 0; i < pages.size() && i < 8 && pages[i].gpu_fault_ns; ++i) {
        const FaultPage& e = pages[i];
        detail(fmt::format("gpu-thread fault #{} ({:.1f} ms/s, {} faults, {} drains, {})", i + 1,
                           double(e.gpu_fault_ns) / 1e6 / secs, e.gpu_reads + e.gpu_writes,
                           e.drains_gpu, Pm4Name(e.pm4[0]))
                   .c_str(),
               e);
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
    // Session 15: the drains that took a Scheduler::Finish, by page, reader and writer.
    LOG_INFO(Render,
             "  in-batch drains: {} ({} on a line already hot, {} with the hot list full, {} "
             "past the table, {} with the faulting 64 KiB itself written in the batch)",
             ib_total, ib_hot, ib_full, ib_dropped, ib_request);
    std::sort(in_batch.begin(), in_batch.end(),
              [](const InBatchDrain& a, const InBatchDrain& b) { return a.count > b.count; });
    for (size_t i = 0; i < in_batch.size() && i < 12; ++i) {
        const InBatchDrain& d = in_batch[i];
        const std::string reader =
            d.gpu_thread ? fmt::format("gpu {} {}", Pm4Name(d.reader_pm4), HostSymbol(d.reader_rip))
                         : fmt::format("guest {:#x}", d.reader_rip);
        const std::string writer =
            d.writer_addr ? fmt::format("[{:#x} +{:#x} {}]", d.writer_addr, d.writer_size,
                                        Pm4Name(d.writer_pm4))
                          : std::string{"none seen"};
        LOG_INFO(Render, "  in-batch #{} page {:#x}: {}x (hot {}, own 64K in batch {}) reader {} "
                 "writer {}",
                 i + 1, d.page, d.count, d.was_hot, d.request_in_batch, reader, writer);
    }
    // Session 17: reuse per reader. Gaps: frames since the same line/page last needed an
    // in-batch drain (new, 0, 1, 2-3, 4-15, 16+); near = within 32 submits.
    std::sort(reuse.begin(), reuse.end(),
              [](const ReuseReader& a, const ReuseReader& b) { return a.drains > b.drains; });
    const auto gaps = [](const std::array<u32, ReuseBuckets>& g) {
        return fmt::format("{}/{}/{}/{}/{}/{}", g[0], g[1], g[2], g[3], g[4], g[5]);
    };
    for (size_t i = 0; i < reuse.size() && i < 8; ++i) {
        ReuseReader& r = reuse[i];
        std::sort(r.writers.begin(), r.writers.end(),
                  [](const ReuseWriter& a, const ReuseWriter& b) { return a.count > b.count; });
        std::string writers;
        for (size_t k = 0; k < r.writers.size() && k < 4; ++k) {
            writers += fmt::format(" [{:#010x} +{:#x} {}x]", r.writers[k].shader,
                                   r.writers[k].size, r.writers[k].count);
        }
        LOG_INFO(Render,
                 "  reuse {} {:#x}: {} drains, {} lines, {} pages, hot {}, own line in batch {}, "
                 "no writer {} | line gaps {} near {} | page gaps {} near {} | batch writers on "
                 "the 64K ({}):{}",
                 r.gpu_thread ? "gpu" : "guest", r.rip, r.drains, r.lines.size(), r.pages.size(),
                 r.line_hot, r.own_in_batch, r.no_writer, gaps(r.line_gap), r.line_near,
                 gaps(r.page_gap), r.page_near, r.writers.size(), writers);
    }
    std::sort(feeders.begin(), feeders.end(),
              [](const auto& a, const auto& b) { return a.second.reads > b.second.reads; });
    std::string feeder_line;
    for (size_t i = 0; i < feeders.size() && i < 10; ++i) {
        const auto& [key, f] = feeders[i];
        feeder_line += fmt::format(" [{:#010x} +{:#x} reads {} dispatches {}]", key.first,
                                   key.second, f.reads, f.dispatches);
    }
    LOG_INFO(Render, "  reuse feeders ({} keys, {} written bindings past the table):{}",
             feeders.size(), writes_dropped, feeder_line);
}

// ----------------------------------------------------------------- stats

std::jthread g_stats_thread;

// Session 13 step 1: download recency per 64 KiB page. GPU thread only (OnDownloadPages and
// OnFlip both run there); the stats thread only bumps g_stats_second.
std::atomic<u64> g_frame{0};
std::atomic<u64> g_stats_second{0};
struct PageRecency {
    u64 frame;
    u64 second;
};
std::unordered_map<u64, PageRecency> g_download_pages;

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
        g_stats_second.fetch_add(1, std::memory_order_relaxed);
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
                  hot_flushes = take(Counter::HotFlush),
                  trimmed = take(Counter::WindowTrimmed),
                  recent_batch = take(Counter::DownloadRecentInBatch),
                  recent_sub = take(Counter::DownloadRecentSubmitted),
                  recent_done = take(Counter::DownloadRecentDone),
                  dl_pages = take(Counter::DownloadPages), dl_bytes = take(Counter::DownloadBytes),
                  pf_hit = take(Counter::PrefetchHit), pf_waited = take(Counter::PrefetchHitWaited),
                  pf_nothot = take(Counter::PrefetchMissNotHot),
                  pf_noentry = take(Counter::PrefetchMissNoEntry),
                  pf_partial = take(Counter::PrefetchMissPartial),
                  pf_overwritten = take(Counter::PrefetchMissOverwritten),
                  pf_stale = take(Counter::PrefetchMissStale),
                  pf_pages = take(Counter::PrefetchPages), pf_bytes = take(Counter::PrefetchBytes);
        if (!sites.empty() || dl_batch + dl_sub + dl_done + dl_unknown + patched_tables != 0) {
            LOG_INFO(Render,
                     "  sched-waits by site:{}; buffer downloads written in batch/submitted/"
                     "done/unknown {}/{}/{}/{}, side copies {}; walker tables patched on "
                     "the GPU {} ({} dwords); hot flushes {}; windows trimmed {}",
                     sites, dl_batch, dl_sub, dl_done, dl_unknown, dl_side, patched_tables,
                     patched_dwords, hot_flushes, trimmed);
            // Session 13: a download whose pages were all downloaded in the last 2 frames
            // would have been covered by a prefetch of the hot pages.
            LOG_INFO(Render,
                     "  download recency: pages downloaded in the last 2 frames too, in batch "
                     "{}/{}, submitted {}/{}, done {}/{}; distinct 64 KiB pages {}, {:.2f} MiB",
                     recent_batch, dl_batch, recent_sub, dl_sub, recent_done, dl_done, dl_pages,
                     double(dl_bytes) / (1024.0 * 1024.0));
        }
        if (pf_hit + pf_nothot + pf_noentry + pf_partial + pf_overwritten + pf_stale +
                pf_pages !=
            0) {
            LOG_INFO(Render,
                     "  readback prefetch: hits {} (after a wait {}), misses not hot {} no entry "
                     "{} partial {} overwritten {} stale {}; copied {} pages, {:.2f} MiB",
                     pf_hit, pf_waited, pf_nothot, pf_noentry, pf_partial, pf_overwritten,
                     pf_stale, pf_pages, double(pf_bytes) / (1024.0 * 1024.0));
        }
        const u64 cc = take(Counter::CpuCopy), cc_dwords = take(Counter::CpuCopyDwords),
                  cc_bindings = take(Counter::CpuCopyBindings),
                  cc_gpu = take(Counter::CpuCopyGpuWritten),
                  cc_rejected = take(Counter::CpuCopyRejected),
                  cc_nonres = take(Counter::CpuCopyNotResident),
                  cc_partial = take(Counter::CpuCopyPartial),
                  cc_gpu_only = take(Counter::CpuCopyGpuOnlyDwords),
                  cc_equal = take(Counter::CpuCopyCheckEqual),
                  cc_differ = take(Counter::CpuCopyCheckDiffer);
        if (cc + cc_gpu + cc_rejected + cc_nonres != 0) {
            LOG_INFO(Render,
                     "  cpu copy: {} dispatches ({} dwords, {} bindings unmarked; {} in part, "
                     "{} dwords left to the GPU), left to the GPU: GPU-written source {}, not "
                     "resident {}, rejected {}; check: downloaded dwords equal {} differ {}",
                     cc, cc_dwords, cc_bindings, cc_partial, cc_gpu_only, cc_gpu, cc_nonres,
                     cc_rejected, cc_equal, cc_differ);
        }
        const u64 ap_queued = take(Counter::AsyncPipelineQueued),
                  ap_ready = take(Counter::AsyncPipelineReady),
                  ap_skipped = take(Counter::AsyncDrawSkipped),
                  ap_sync = take(Counter::AsyncPipelineSyncWrites),
                  ap_pending = g_async_pipelines_pending.load(std::memory_order_relaxed);
        if (ap_queued + ap_ready + ap_skipped + ap_pending + ap_sync != 0) {
            LOG_INFO(Render,
                     "  async pipelines: queued {} ready {} pending {}; draws skipped {}; built "
                     "on the GPU thread (a stage writes memory) {}",
                     ap_queued, ap_ready, ap_pending, ap_skipped, ap_sync);
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

/// The walker table being read (GPU thread), set by OnSrtTable.
thread_local u64 t_srt_shader = 0, t_srt_table = 0, t_srt_ptr = 0, t_srt_bytes = 0;
thread_local SrtOutcome t_srt_outcome = SrtOutcome::Pending;

void SetSrtOutcome(SrtOutcome outcome) {
    t_srt_outcome = outcome;
}

void OnSrtTable(u64 pgm_hash, u64 table, VAddr ptr, u64 bytes) {
    t_srt_shader = pgm_hash;
    t_srt_table = table;
    t_srt_ptr = ptr;
    t_srt_bytes = bytes;
    t_srt_outcome = SrtOutcome::Pending;
}

void OnFault(VAddr addr, bool is_write, bool gpu_thread, bool gpu_modified, u64 rip) {
    const u32 pm4 = gpu_thread ? t_pm4 : NoPm4;
    std::scoped_lock lk{g_fault_mutex};
    FaultPage* e = FindPage(addr & ~VAddr{0xfff}, true, gpu_thread);
    if (!e) {
        ++g_fault_dropped;
        return;
    }
    ++e->faults;
    ++(gpu_thread ? (is_write ? e->gpu_writes : e->gpu_reads)
                  : (is_write ? e->guest_writes : e->guest_reads));
    e->gpu_modified += gpu_modified ? 1 : 0;
    if (!gpu_thread) {
        e->guest_rip = rip;
    }
    if (gpu_thread && !is_write) {
        const u32 off = u32(addr & 0xfff);
        if (e->gpu_reads == 1) {
            e->gpu_read_min = e->gpu_read_max = off;
        }
        e->gpu_read_min = std::min(e->gpu_read_min, off);
        e->gpu_read_max = std::max(e->gpu_read_max, off);
        e->gpu_read_rip = rip;
        // A fault in the walker's table (extent unknown: within 4 KiB of its start).
        if (t_srt_ptr != 0 && addr >= t_srt_ptr &&
            addr < t_srt_ptr + (t_srt_bytes ? t_srt_bytes : 0x1000)) {
            e->srt_shader = t_srt_shader;
            e->srt_table = t_srt_table;
            e->srt_ptr = t_srt_ptr;
            e->srt_bytes = t_srt_bytes;
            e->srt_outcome = t_srt_outcome;
        }
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

void OnGpuFaultTime(VAddr addr, u64 ns) {
    std::scoped_lock lk{g_fault_mutex};
    if (FaultPage* e = FindPage(addr & ~VAddr{0xfff}, false)) {
        e->gpu_fault_ns += ns;
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

void OnGpuWrite(VAddr addr, u64 size, u64 tick) {
    if (addr == 0) {
        return;
    }
    const u32 pm4 = t_pm4;
    const u64 shader = t_shader;
    std::scoped_lock lk{g_write_mutex};
    if (g_page_writes.size() > (1u << 20)) {
        g_page_writes.clear();
    }
    const u64 seq = ++g_write_seq;
    for (u64 p = addr >> 16, last = std::min((addr + size - 1) >> 16, (addr >> 16) + 255);
         p <= last; ++p) {
        auto& ring = g_page_writes[p];
        GpuWrite* slot = &ring[0];
        for (GpuWrite& e : ring) {
            if (e.addr == addr && e.size == size) {
                slot = &e;
                break;
            }
            if (e.seq < slot->seq) {
                slot = &e;
            }
        }
        *slot = {addr, size, 1, pm4, seq, shader, tick};
    }
    if (shader != 0 && !g_feeders.empty()) {
        if (const auto it = g_feeders.find({shader, size}); it != g_feeders.end()) {
            if (it->second.last_dispatch != t_dispatch_seq) {
                it->second.last_dispatch = t_dispatch_seq;
                ++it->second.dispatches;
            }
        }
    }
    size_t i = size_t(((addr ^ (size << 20)) * 0x9E3779B97F4A7C15ull) >> 52) & (WriteTableSize - 1);
    for (size_t n = 0; n < WriteTableSize; ++n, i = (i + 1) & (WriteTableSize - 1)) {
        GpuWrite& w = g_writes[i];
        if (w.addr == addr && w.size == size) {
            ++w.count;
            w.pm4 = pm4;
            w.seq = seq;
            w.shader = shader;
            w.tick = tick;
            return;
        }
        if (w.addr == 0) {
            if (g_writes_used >= WriteTableSize * 3 / 4) {
                ++g_writes_dropped;
                return;
            }
            ++g_writes_used;
            w = {addr, size, 1, pm4, seq, shader, tick};
            return;
        }
    }
}

void OnFaultDrain(VAddr addr, bool gpu_thread, bool in_batch, bool was_hot, bool hot_full,
                  bool request_in_batch, u64 tick) {
    const u32 pm4 = gpu_thread ? t_pm4 : NoPm4;
    GpuWrite writer{};
    GpuWrite culprit{}; ///< session 17: newest binding written in this batch on the same 64 KiB
    if (in_batch) {
        std::scoped_lock lk{g_write_mutex};
        for (const GpuWrite& w : g_writes) {
            if (w.addr != 0 && w.addr <= addr && addr < w.addr + w.size && w.seq > writer.seq) {
                writer = w;
            }
        }
        if (const auto it = g_page_writes.find(addr >> 16); it != g_page_writes.end()) {
            for (const GpuWrite& w : it->second) {
                if (w.addr == 0) {
                    continue;
                }
                if (w.addr <= addr && addr < w.addr + w.size && w.seq > writer.seq) {
                    writer = w;
                }
                if (w.tick >= tick && w.seq > culprit.seq) {
                    culprit = w;
                }
            }
        }
        if (culprit.addr != 0) {
            ++g_feeders[{culprit.shader, culprit.size}].reads;
        }
    }
    std::scoped_lock lk{g_fault_mutex};
    FaultPage* e = FindPage(addr & ~VAddr{0xfff}, false);
    if (e) {
        ++(gpu_thread ? e->drains_gpu : e->drains_queued);
    }
    if (in_batch) {
        ++g_in_batch_total;
        g_in_batch_hot += was_hot ? 1 : 0;
        g_in_batch_hot_full += hot_full ? 1 : 0;
        g_in_batch_request += request_in_batch ? 1 : 0;
        const u64 rip = !e ? 0 : gpu_thread ? e->gpu_read_rip : e->guest_rip;
        const InBatchDrain key{addr & ~VAddr{0xfff}, gpu_thread, pm4, rip, writer.addr,
                               writer.size, writer.pm4, 0, 0, 0};
        auto it = std::ranges::find_if(g_in_batch, [&](const InBatchDrain& d) {
            return d.page == key.page && d.gpu_thread == key.gpu_thread &&
                   d.reader_pm4 == key.reader_pm4 && d.reader_rip == key.reader_rip &&
                   d.writer_addr == key.writer_addr && d.writer_size == key.writer_size;
        });
        if (it == g_in_batch.end() && g_in_batch.size() < 512) {
            g_in_batch.push_back(key);
            it = g_in_batch.end() - 1;
        }
        if (it != g_in_batch.end()) {
            ++it->count;
            it->was_hot += was_hot ? 1 : 0;
            it->request_in_batch += request_in_batch ? 1 : 0;
            it->writer_pm4 = writer.pm4;
        } else {
            ++g_in_batch_dropped;
        }
        // Session 17: reuse of the line and page, per reader.
        auto r = std::ranges::find_if(g_reuse, [&](const ReuseReader& x) {
            return x.gpu_thread == gpu_thread && x.rip == rip;
        });
        if (r == g_reuse.end()) {
            r = g_reuse.insert(g_reuse.end(), ReuseReader{.gpu_thread = gpu_thread, .rip = rip});
        }
        const u64 frame = g_frame.load(std::memory_order_relaxed);
        const u64 line = addr >> 8, page = addr >> 16;
        if (g_reuse_lines.size() > (1u << 20)) {
            g_reuse_lines.clear();
            g_reuse_pages.clear();
        }
        const auto note = [&](auto& map, u64 key, auto& gaps, u32& within) {
            const auto found = map.find(key);
            const ReuseSeen* seen = found == map.end() ? nullptr : &found->second;
            ++gaps[ReuseBucket(seen, frame)];
            within += seen && tick - seen->tick <= 32 ? 1 : 0;
            map[key] = {frame, tick};
        };
        ++r->drains;
        r->line_hot += was_hot ? 1 : 0;
        r->own_in_batch += writer.addr != 0 && writer.tick >= tick ? 1 : 0;
        r->no_writer += culprit.addr == 0 ? 1 : 0;
        note(g_reuse_lines, line, r->line_gap, r->line_near);
        note(g_reuse_pages, page, r->page_gap, r->page_near);
        r->lines.insert(line);
        r->pages.insert(page);
        auto w = std::ranges::find_if(r->writers, [&](const ReuseWriter& x) {
            return x.shader == culprit.shader && x.size == culprit.size;
        });
        if (w == r->writers.end()) {
            w = r->writers.insert(r->writers.end(), ReuseWriter{culprit.shader, culprit.size, 0});
        }
        ++w->count;
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

void OnDownloadPages(DownloadKind kind, const u64* pages, size_t count) {
    const u64 frame = g_frame.load(std::memory_order_relaxed);
    const u64 second = g_stats_second.load(std::memory_order_relaxed);
    bool recent = count != 0;
    for (size_t i = 0; i < count; ++i) {
        auto [it, inserted] = g_download_pages.try_emplace(pages[i], PageRecency{frame, second});
        if (inserted) {
            recent = false;
            Count(Counter::DownloadPages);
            continue;
        }
        if (it->second.frame + 2 < frame) {
            recent = false;
        }
        if (it->second.second != second) {
            Count(Counter::DownloadPages);
        }
        it->second = {frame, second};
    }
    if (recent) {
        switch (kind) {
        case DownloadKind::InBatch:
            Count(Counter::DownloadRecentInBatch);
            break;
        case DownloadKind::Submitted:
            Count(Counter::DownloadRecentSubmitted);
            break;
        case DownloadKind::Done:
            Count(Counter::DownloadRecentDone);
            break;
        case DownloadKind::Unknown:
            break;
        }
    }
}

void OnFlip(Vulkan::Rasterizer& rasterizer) {
    Count(Counter::Flip);
    g_frame.fetch_add(1, std::memory_order_relaxed);
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

u64 WorkingSetPages(u64 addr, u64 size) {
#ifdef _WIN32
    u64 valid = 0;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info(1024);
    for (u64 page = addr >> 12, last = (addr + size - 1) >> 12; page <= last;) {
        const u64 n = std::min<u64>(info.size(), last - page + 1);
        for (u64 i = 0; i < n; ++i) {
            info[i].VirtualAddress = reinterpret_cast<void*>((page + i) << 12);
        }
        if (!QueryWorkingSetEx(GetCurrentProcess(), info.data(),
                               static_cast<DWORD>(n * sizeof(info[0])))) {
            return 0;
        }
        for (u64 i = 0; i < n; ++i) {
            valid += info[i].VirtualAttributes.Valid;
        }
        page += n;
    }
    return valid;
#else
    return 0;
#endif
}

} // namespace SotcDebug
