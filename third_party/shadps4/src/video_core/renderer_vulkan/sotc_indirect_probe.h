// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 66): the indirect-draw probe (SOTC_INDIRECT_PROBE=1, debug only). How many
// of the indirect draws and dispatches stage B records draw nothing? Their arguments are written
// by the game's compute shaders on the GPU (likely GPU culling), so the CPU can't see them when
// it records. For each one the probe copies the argument words (and the count, for the count
// form) into a host-visible slab in the same command buffer, as encoded commands, and reads them
// once the slab's tick is free (Scheduler::IsFree, never waiting). It logs a `probe: indirect:`
// line every 10 s. The copies need a transfer outside a render pass, so a probed draw ends the
// current pass first (the frames stay the same; the GPU does more work: not for measuring speed).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
struct Buffer;
}

namespace Vulkan {

class Instance;
class Scheduler;

/// ZPASS_DONE dumps (EVENT_WRITE PixelPipeStatDump), counted by Liverpool (stage A) whatever
/// the probe's setting; the probe line reports them per flip.
inline std::atomic<std::uint64_t> g_sotc_zpass_dumps{0};

class SotcIndirectProbe {
public:
    /// SOTC_INDIRECT_PROBE, read once.
    static bool Enabled();

    SotcIndirectProbe(const Instance& instance, Scheduler& scheduler);
    ~SotcIndirectProbe();

    enum class Kind : std::uint8_t { Draw, DrawIndexed, Dispatch };

    /// Records the copies for one indirect draw or dispatch. No render pass may be active (the
    /// caller ends it). `count_buffer` is null for the forms without a count. `key` is the guest
    /// address of the arguments, `shader` the program hash it is reported under (the vertex
    /// shader's for a draw).
    void Capture(Kind kind, vk::Buffer buffer, std::uint64_t offset, std::uint32_t stride,
                 std::uint32_t max_count, vk::Buffer count_buffer, std::uint64_t count_offset,
                 std::uint64_t key, std::uint64_t shader);

    /// A draw the probe could not capture (it would have ended a pass that clears).
    void Skip() {
        ++skipped;
    }

    /// Session 66b: a direct draw or dispatch, whose counts B knows. `clears`: the draw starts
    /// a render pass that clears (work a skip would have to keep).
    void Direct(bool dispatch, bool empty, bool clears, std::uint64_t shader) {
        if (dispatch) {
            ++direct_dispatches;
            direct_dispatches_empty += empty;
        } else {
            ++direct_draws;
            if (empty) {
                ++direct_draws_empty;
                direct_empty_clears += clears;
                ++direct_empty_by_shader[shader];
            }
        }
        Poll();
    }

    /// Stage B ran a flip.
    void OnFlip() {
        ++flip;
        ++period_flips;
    }

    /// Reads the slabs the GPU finished, and logs the line every 10 s.
    void Poll();

private:
    struct Entry {
        Kind kind;
        std::uint32_t stride;
        std::uint32_t max_count;
        std::uint32_t captured; // argument records copied (max_count, capped)
        std::uint32_t args_at;  // byte offset in the slab
        std::int64_t count_at;  // -1: no count
        std::uint64_t key;
        std::uint64_t shader;
        std::uint64_t flip;
    };
    struct Slab {
        std::unique_ptr<VideoCore::Buffer> buffer;
        std::uint64_t used = 0;
        std::uint64_t tick = 0; // the last tick that copied into it
        std::vector<Entry> entries;
    };
    struct Last {
        std::uint64_t flip;
        bool empty;
    };

    Slab* CurrentSlab(std::uint64_t bytes);
    void Read(Slab& slab);
    void Report();

    const Instance& instance;
    Scheduler& scheduler;
    std::vector<std::unique_ptr<Slab>> free_slabs;
    std::unique_ptr<Slab> current;
    std::deque<std::unique_ptr<Slab>> closed; // oldest first
    std::unordered_map<std::uint64_t, Last> last; // by key
    std::uint64_t flip = 0;

    // Counters since the last line.
    std::uint64_t period_flips = 0;
    std::uint64_t draws = 0, draws_empty = 0, draws_count0 = 0, multi = 0;
    std::uint64_t subdraws = 0, subdraws_empty = 0;
    std::uint64_t dispatches = 0, dispatches_empty = 0;
    std::uint64_t pred[2][2]{};   // [last flip's empty][this one's empty], same key
    std::uint64_t pred_none = 0;  // no record at the key last flip
    std::uint64_t skipped = 0, dropped = 0, truncated = 0;
    std::uint64_t zpass_seen = 0;
    std::uint64_t direct_draws = 0, direct_draws_empty = 0, direct_empty_clears = 0;
    std::uint64_t direct_dispatches = 0, direct_dispatches_empty = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> direct_empty_by_shader;
    std::unordered_map<std::uint64_t, std::uint64_t> empty_by_shader;
    std::unordered_map<std::uint64_t, std::uint64_t> draws_by_shader;
    std::chrono::steady_clock::time_point period_start = std::chrono::steady_clock::now();
};

} // namespace Vulkan
