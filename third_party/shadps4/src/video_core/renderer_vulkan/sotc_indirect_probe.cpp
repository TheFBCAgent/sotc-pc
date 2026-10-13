// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 66): the indirect-draw probe; see sotc_indirect_probe.h.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/sotc_indirect_probe.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
constexpr std::uint64_t SlabBytes = std::uint64_t{256} << 10;
constexpr std::uint64_t MaxArgBytes = std::uint64_t{16} << 10; // per capture
constexpr std::size_t MaxSlabs = 256;                          // in flight, then captures drop
} // namespace

bool SotcIndirectProbe::Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("SOTC_INDIRECT_PROBE");
        return env && *env && *env != '0';
    }();
    return on;
}

SotcIndirectProbe::SotcIndirectProbe(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    LOG_INFO(Render_Vulkan, "SOTC_INDIRECT_PROBE on: indirect draws end their render pass");
}

SotcIndirectProbe::~SotcIndirectProbe() = default;

SotcIndirectProbe::Slab* SotcIndirectProbe::CurrentSlab(std::uint64_t bytes) {
    const std::uint64_t tick = scheduler.CurrentTick();
    // A slab closes when it is full or a submit went by (so it becomes readable soon).
    if (current && (current->used + bytes > SlabBytes || current->tick != tick)) {
        closed.push_back(std::move(current));
    }
    if (!current) {
        if (!free_slabs.empty()) {
            current = std::move(free_slabs.back());
            free_slabs.pop_back();
        } else if (closed.size() < MaxSlabs) {
            current = std::make_unique<Slab>();
            current->buffer = std::make_unique<VideoCore::Buffer>(
                instance, 0, SlabBytes, VideoCore::MemoryType::HostCached, "SotcIndirectProbe");
        } else {
            return nullptr;
        }
        current->used = 0;
        current->entries.clear();
    }
    current->tick = tick;
    return current.get();
}

void SotcIndirectProbe::Capture(Kind kind, vk::Buffer buffer, std::uint64_t offset,
                                std::uint32_t stride, std::uint32_t max_count,
                                vk::Buffer count_buffer, std::uint64_t count_offset,
                                std::uint64_t key, std::uint64_t shader) {
    Poll();
    const std::uint32_t rec = kind == Kind::Dispatch ? 12u : stride;
    std::uint32_t captured = kind == Kind::Dispatch ? 1u : max_count;
    if (std::uint64_t{captured} * rec > MaxArgBytes) {
        captured = static_cast<std::uint32_t>(MaxArgBytes / rec);
        ++truncated;
    }
    const std::uint64_t arg_bytes = std::uint64_t{captured} * rec;
    const std::uint64_t bytes = ((arg_bytes + 3) & ~std::uint64_t{3}) + (count_buffer ? 4 : 0);
    if (captured == 0 && !count_buffer) {
        Entry e{kind, stride, max_count, 0, 0, -1, key, shader, flip};
        // Nothing to copy: max_count 0, empty by construction. Account it at once.
        Slab tmp;
        tmp.entries.push_back(e);
        Read(tmp);
        return;
    }
    Slab* slab = CurrentSlab(bytes);
    if (!slab) {
        ++dropped;
        return;
    }
    Entry e{kind,
            stride,
            max_count,
            captured,
            static_cast<std::uint32_t>(slab->used),
            count_buffer ? static_cast<std::int64_t>(slab->used + bytes - 4) : -1,
            key,
            shader,
            flip};

    const auto cmdbuf = scheduler.Cmd();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &before,
    });
    const vk::Buffer dst = slab->buffer->Handle();
    if (arg_bytes) {
        cmdbuf.copyBuffer(buffer, dst,
                          vk::BufferCopy{.srcOffset = offset, .dstOffset = e.args_at,
                                         .size = arg_bytes});
    }
    if (count_buffer) {
        cmdbuf.copyBuffer(count_buffer, dst,
                          vk::BufferCopy{.srcOffset = count_offset,
                                         .dstOffset = static_cast<vk::DeviceSize>(e.count_at),
                                         .size = 4});
    }
    // The host reads the slab; and later commands that overwrite the arguments wait for the
    // copy (the runtime's tracking doesn't know about it).
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &after,
    });
    slab->used += bytes;
    slab->entries.push_back(e);
}

void SotcIndirectProbe::Poll() {
    // A slab still current after its tick was submitted closes here too.
    if (current && current->tick != scheduler.CurrentTick()) {
        closed.push_back(std::move(current));
    }
    while (!closed.empty() && scheduler.IsFree(closed.front()->tick)) {
        auto slab = std::move(closed.front());
        closed.pop_front();
        Read(*slab);
        free_slabs.push_back(std::move(slab));
    }
    if (std::chrono::steady_clock::now() - period_start >= std::chrono::seconds{10}) {
        Report();
    }
}

void SotcIndirectProbe::Read(Slab& slab) {
    const std::uint8_t* base = nullptr;
    if (slab.buffer) {
        slab.buffer->Invalidate(0, slab.used);
        base = slab.buffer->mapped_data.data();
    }
    const auto word = [&](std::uint64_t at) {
        std::uint32_t v;
        std::memcpy(&v, base + at, 4);
        return v;
    };
    for (const Entry& e : slab.entries) {
        bool empty;
        if (e.kind == Kind::Dispatch) {
            ++dispatches;
            empty = word(e.args_at) == 0 || word(e.args_at + 4) == 0 || word(e.args_at + 8) == 0;
            dispatches_empty += empty;
        } else {
            ++draws;
            ++draws_by_shader[e.shader];
            multi += e.max_count > 1;
            std::uint32_t n = std::min(e.captured, e.max_count);
            if (e.count_at >= 0) {
                const std::uint32_t count = word(static_cast<std::uint64_t>(e.count_at));
                n = std::min(n, count);
                draws_count0 += count == 0;
            }
            std::uint32_t live = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint64_t at = e.args_at + std::uint64_t{i} * e.stride;
                // Both layouts start {vertex or index count, instance count}.
                live += word(at) != 0 && word(at + 4) != 0;
            }
            subdraws += n;
            subdraws_empty += n - live;
            empty = live == 0;
            if (empty) {
                ++draws_empty;
                ++empty_by_shader[e.shader];
            }
        }
        // Does the record at this address one flip earlier predict this one?
        const std::uint64_t key = e.key ^ (e.kind == Kind::Dispatch ? 1ULL << 63 : 0);
        auto [it, inserted] = last.try_emplace(key, Last{e.flip, empty});
        if (!inserted) {
            if (it->second.flip + 1 == e.flip) {
                ++pred[it->second.empty][empty];
            } else if (it->second.flip != e.flip) {
                ++pred_none;
            }
            // Several records at one address in a flip: keep the flip's first, so the next
            // flip compares like with like.
            if (it->second.flip != e.flip) {
                it->second = Last{e.flip, empty};
            }
        } else {
            ++pred_none;
        }
    }
}

void SotcIndirectProbe::Report() {
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - period_start).count();
    const double flips = std::max<std::uint64_t>(period_flips, 1);
    const std::uint64_t zpass = g_sotc_zpass_dumps.load(std::memory_order_relaxed);
    const auto pct = [](std::uint64_t a, std::uint64_t b) { return b ? 100.0 * a / b : 0.0; };

    std::vector<std::pair<std::uint64_t, std::uint64_t>> top(empty_by_shader.begin(),
                                                              empty_by_shader.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string tops;
    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 6); ++i) {
        tops += fmt::format(" {:016x} {}/{},", top[i].first, top[i].second,
                            draws_by_shader[top[i].first]);
    }
    LOG_INFO(Render_Vulkan,
             "probe: indirect: {:.1f} s, {} flips: draws {} ({:.0f}/flip), empty {} ({:.1f}%; "
             "count 0: {}), multi {}, sub-draws {} empty {}; dispatches {} ({:.0f}/flip), empty "
             "{} ({:.1f}%); last flip at the same address: empty->empty {}, live->empty {}, "
             "empty->live {}, live->live {}, none {}; ZPASS_DONE dumps {} ({:.1f}/flip); "
             "skipped {}, dropped {}, truncated {}, slabs {} in flight",
             secs, period_flips, draws, draws / flips, draws_empty, pct(draws_empty, draws),
             draws_count0, multi, subdraws, subdraws_empty, dispatches, dispatches / flips,
             dispatches_empty, pct(dispatches_empty, dispatches), pred[1][1], pred[0][1],
             pred[1][0], pred[0][0], pred_none, zpass - zpass_seen, (zpass - zpass_seen) / flips,
             skipped, dropped, truncated, closed.size());
    if (!tops.empty()) {
        LOG_INFO(Render_Vulkan, "probe: indirect: empty by vertex shader (empty/draws):{}", tops);
    }
    zpass_seen = zpass;
    period_flips = 0;
    draws = draws_empty = draws_count0 = multi = subdraws = subdraws_empty = 0;
    dispatches = dispatches_empty = 0;
    std::memset(pred, 0, sizeof(pred));
    pred_none = skipped = dropped = truncated = 0;
    empty_by_shader.clear();
    draws_by_shader.clear();
    period_start = std::chrono::steady_clock::now();
}

} // namespace Vulkan
