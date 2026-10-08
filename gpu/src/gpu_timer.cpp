// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 12): GPU busy time per second from timestamp queries. See sotc_gpu_timer.h.

#include <array>
#include <mutex>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "sotc_debug.h"
#include "sotc_gpu_busy.h"
#include "sotc_gpu_timer.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace SotcDebug::GpuTimer {
namespace {

/// Slots (two queries each). ~150-400 submissions/s on both queues; the stats thread frees the
/// finished ones every second, so a full ring means the GPU is seconds behind.
constexpr u32 NumSlots = 4096;

enum class State : u8 { Free, Recording, Pending, Done };

struct Slot {
    State state = State::Free;
    Queue queue = Queue::Main;
    vk::Semaphore semaphore{};
    u64 tick = 0;
};

std::mutex g_mutex;
bool g_init_tried = false;
bool g_ok = false;
vk::Device g_device{};
vk::QueryPool g_pool{};
u32 g_valid_bits = 0;
float g_period_ns = 1.0f;
std::array<Slot, NumSlots> g_slots{};
u32 g_cursor = 0;
u32 g_untimed = 0;
u64 g_floor_all = 0, g_floor_main = 0, g_floor_side = 0;

bool InitLocked(const Vulkan::Instance& instance) {
    if (g_init_tried) {
        return g_ok;
    }
    g_init_tried = true;
    const vk::PhysicalDevice physical = instance.GetPhysicalDevice();
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    g_valid_bits = family < families.size() ? families[family].timestampValidBits : 0;
    g_period_ns = physical.getProperties().limits.timestampPeriod;
    if (g_valid_bits == 0 || g_period_ns <= 0.0f) {
        LOG_WARNING(Render, "gpu timer: the graphics queue family has no timestamps; no gpu busy");
        return false;
    }
    g_device = instance.GetDevice();
    auto [result, pool] = g_device.createQueryPool({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlots * 2,
    });
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render, "gpu timer: createQueryPool failed ({}); no gpu busy",
                    vk::to_string(result));
        return false;
    }
    g_pool = pool;
    g_ok = true;
    LOG_INFO(Render, "gpu timer: {} timestamp slots, {} valid bits, {} ns per tick", NumSlots,
             g_valid_bits, g_period_ns);
    return true;
}

} // namespace

int Begin(const Vulkan::Instance& instance, vk::CommandBuffer cmd, Queue queue) {
    if (!StatsEnabled()) {
        return -1;
    }
    std::scoped_lock lk{g_mutex};
    if (!InitLocked(instance)) {
        return -1;
    }
    Slot& s = g_slots[g_cursor];
    if (s.state != State::Free) {
        ++g_untimed;
        return -1;
    }
    const u32 slot = g_cursor;
    g_cursor = (g_cursor + 1) % NumSlots;
    s = Slot{.state = State::Recording, .queue = queue};
    cmd.resetQueryPool(g_pool, slot * 2, 2);
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, g_pool, slot * 2);
    return int(slot);
}

void End(vk::CommandBuffer cmd, int slot) {
    if (slot < 0) {
        return;
    }
    std::scoped_lock lk{g_mutex};
    if (g_ok) {
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, g_pool, u32(slot) * 2 + 1);
    }
}

void Submitted(int slot, vk::Semaphore semaphore, u64 tick) {
    if (slot < 0) {
        return;
    }
    std::scoped_lock lk{g_mutex};
    Slot& s = g_slots[slot];
    s.semaphore = semaphore;
    s.tick = tick;
    s.state = State::Pending;
}

void Completed(int slot) {
    if (slot < 0) {
        return;
    }
    std::scoped_lock lk{g_mutex};
    g_slots[slot].state = State::Done;
}

Report Collect() {
    Report report{};
    std::vector<SotcGpuBusy::Interval> all, main, side;
    std::scoped_lock lk{g_mutex};
    report.untimed = std::exchange(g_untimed, 0);
    if (!g_ok) {
        return report;
    }
    // Timeline values read once per semaphore per call (one per scheduler).
    std::vector<std::pair<vk::Semaphore, u64>> values;
    const auto semaphore_value = [&](vk::Semaphore semaphore) {
        for (const auto& [sem, value] : values) {
            if (sem == semaphore) {
                return value;
            }
        }
        auto [result, value] = g_device.getSemaphoreCounterValue(semaphore);
        const u64 v = result == vk::Result::eSuccess ? value : 0;
        values.emplace_back(semaphore, v);
        return v;
    };
    for (u32 i = 0; i < NumSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.state == State::Free) {
            continue;
        }
        if (s.state == State::Recording ||
            (s.state == State::Pending && semaphore_value(s.semaphore) < s.tick)) {
            ++report.in_flight;
            continue;
        }
        // Finished: the results are written. Never wait here.
        std::array<u64, 4> data{}; // begin, available, end, available
        const vk::Result result = g_device.getQueryPoolResults(
            g_pool, i * 2, 2, sizeof(data), data.data(), sizeof(u64) * 2,
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if ((result != vk::Result::eSuccess && result != vk::Result::eNotReady) || !data[1] ||
            !data[3]) {
            ++report.in_flight;
            continue;
        }
        const SotcGpuBusy::Interval iv{SotcGpuBusy::MaskTicks(data[0], g_valid_bits),
                                       SotcGpuBusy::MaskTicks(data[2], g_valid_bits)};
        all.push_back(iv);
        (s.queue == Queue::Main ? main : side).push_back(iv);
        s.state = State::Free;
        ++report.timed;
    }
    report.busy_ms = SotcGpuBusy::TicksToMs(SotcGpuBusy::UnionTicks(all, g_floor_all), g_period_ns);
    report.main_ms =
        SotcGpuBusy::TicksToMs(SotcGpuBusy::UnionTicks(main, g_floor_main), g_period_ns);
    report.side_ms =
        SotcGpuBusy::TicksToMs(SotcGpuBusy::UnionTicks(side, g_floor_side), g_period_ns);
    return report;
}

void Destroy() {
    std::scoped_lock lk{g_mutex};
    if (g_pool) {
        g_device.destroyQueryPool(g_pool);
    }
    g_pool = vk::QueryPool{};
    g_ok = false;
    g_init_tried = false;
    g_slots.fill(Slot{});
    g_cursor = 0;
    g_untimed = 0;
    g_floor_all = g_floor_main = g_floor_side = 0;
}

} // namespace SotcDebug::GpuTimer
