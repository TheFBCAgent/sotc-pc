// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (whole-draw memo plan, W1): the draw-memo probe; see sotc_memo_probe.h and
// docs/next/draw-memo-W1.md (the log format).

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/sotc_memo_probe.h"

namespace SotcMemoProbe {

namespace {
bool ReadEnv() {
    const char* env = std::getenv("SOTC_DRAW_MEMO_PROBE");
    return env && *env && *env != '0';
}
} // namespace

extern const bool g_on = ReadEnv();

namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using s64 = std::int64_t;

constexpr int NumKinds = int(Kind::Count);
constexpr const char* KindNames[NumKinds] = {"draw", "draw-ind", "disp", "disp-ind"};
constexpr const char* PhaseNames[NumPh] = {"L3 rstate", "L3 beginrt", "L1 buf", "L2 tex",
                                           "L4 barr"};
constexpr auto Window = std::chrono::seconds(10);
constexpr u32 TableBits = 12; // direct-mapped, like SotcTexMemo (which uses 10)

static_assert(sizeof(AmdGpu::Buffer) == 16);
static_assert(sizeof(AmdGpu::Image) == 32);
static_assert(sizeof(AmdGpu::Sampler) == 16);

struct PhaseStats {
    u64 calls = 0, hits = 0, clean = 0, conflicts = 0;
    u64 cyc_hit = 0, cyc_clean = 0, cyc_miss = 0;
};
struct KindStats {
    u64 records = 0, all_hit = 0, all_hit_clean = 0;
    u64 cyc_hit = 0, cyc_clean = 0, cyc_miss = 0;
    std::array<PhaseStats, NumPh> ph{};
};

struct Entry {
    const void* info = nullptr;
    u64 key = 0;
};

// One buffer binding (Info* + binding index). `last*` persist across windows; the rest is
// the window's.
struct Slot {
    u64 pgm_hash = 0;
    u32 idx = 0;
    bool have = false;
    u64 last[2]{};
    u64 last_base = 0;
    // window
    u64 binds = 0, same = 0, base_only = 0, other = 0;
    u64 moves = 0, fwd = 0, back = 0;
    u64 minb = 0, maxb = 0;
    u64 size = 0;
    std::array<u64, 4> bases{}; // distinct bases seen (up to 4)
    u32 nbases = 0;
    bool many_bases = false;
    std::array<std::pair<s64, u64>, 4> deltas{}; // forward deltas, space-saving top 4
};

enum Class { OneShot, Static, Multi, RingFixed, RingVar, Irregular, NumClasses };
constexpr const char* ClassNames[NumClasses] = {"one-shot",  "static",   "multi(<=4)",
                                                "ring-fixed", "ring-var", "irregular"};

struct Current {
    bool active = false;
    bool graphics = false;
    int kind = 0;
    u64 t0 = 0, ov0 = 0, dirty0 = 0;
    bool l1_all = true, l2_all = true, l3_hit = false;
    // the key of the phase in progress (L1/L2)
    bool pending_set = false, pending_hit = false, pending_conflict = false;
};

// All of it is stage B's.
struct State {
    std::array<KindStats, NumKinds> ks{};
    Current cur{};
    std::vector<Entry> l1_tab = std::vector<Entry>(1u << TableBits);
    std::vector<Entry> l2_tab = std::vector<Entry>(1u << TableBits);
    std::unordered_set<const void*> l1_stages, l2_stages; // seen in the window
    u64 l3_last = 0;
    bool l3_valid = false;
    std::unordered_map<u64, Slot> slots;
    u64 bind_same = 0, bind_base_only = 0, bind_other = 0, bind_first = 0;
    u64 records_since_check = 0;
    u64 overhead_seen = 0;
    u64 flips_seen = 0;
    u64 window_tsc = 0;
    std::chrono::steady_clock::time_point window_start{};
};

State& S() {
    static State* state = new State(); // never destroyed: B may record during shutdown
    return *state;
}

inline u64 Mix(u64 h, u64 v) {
    h ^= v * 0xbf58476d1ce4e5b9ull;
    h = std::rotl(h, 31) * 0x94d049bb133111ebull;
    return h;
}

template <typename T>
inline u64 MixObj(u64 h, const T& obj) {
    const auto* p = reinterpret_cast<const unsigned char*>(&obj);
    std::size_t n = sizeof(T);
    while (n >= 8) {
        u64 w;
        std::memcpy(&w, p, 8);
        h = Mix(h, w);
        p += 8;
        n -= 8;
    }
    if (n) {
        u64 w = 0;
        std::memcpy(&w, p, n);
        h = Mix(h, w);
    }
    return h;
}

void TableCheck(std::vector<Entry>& tab, const void* info, u64 key) {
    auto& st = S();
    const auto slot =
        (reinterpret_cast<uintptr_t>(info) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - TableBits);
    Entry& e = tab[slot];
    st.cur.pending_set = true;
    st.cur.pending_hit = e.info == info && e.key == key;
    st.cur.pending_conflict = e.info != nullptr && e.info != info;
    e.info = info;
    e.key = key;
}

constexpr u64 BaseMask = (u64{1} << 40) - 1; // V# dword 0-1: base_address is bits 0..39

void ClassifyBinding(const Shader::Info& stage, u32 idx, const AmdGpu::Buffer& v) {
    if (v.base_address == 0 || v.GetSize() == 0) {
        return;
    }
    auto& st = S();
    const u64 key = (u64(reinterpret_cast<uintptr_t>(&stage)) << 8) | idx;
    Slot& s = st.slots[key];
    u64 w[2];
    std::memcpy(w, &v, sizeof(w));
    const u64 base = v.base_address;
    if (s.binds++ == 0) {
        s.minb = s.maxb = base;
    } else {
        s.minb = std::min(s.minb, base);
        s.maxb = std::max(s.maxb, base);
    }
    s.size = v.GetSize();
    if (!s.many_bases) {
        bool found = false;
        for (u32 i = 0; i < s.nbases; ++i) {
            found |= s.bases[i] == base;
        }
        if (!found) {
            if (s.nbases < s.bases.size()) {
                s.bases[s.nbases++] = base;
            } else {
                s.many_bases = true;
            }
        }
    }
    if (!s.have) {
        s.have = true;
        s.pgm_hash = stage.pgm_hash;
        s.idx = idx;
        ++st.bind_first;
    } else {
        if (w[0] == s.last[0] && w[1] == s.last[1]) {
            ++s.same;
            ++st.bind_same;
        } else if ((w[0] & ~BaseMask) == (s.last[0] & ~BaseMask) && w[1] == s.last[1]) {
            ++s.base_only;
            ++st.bind_base_only;
        } else {
            ++s.other;
            ++st.bind_other;
        }
        if (base != s.last_base) {
            ++s.moves;
            const s64 delta = s64(base) - s64(s.last_base);
            if (delta > 0) {
                ++s.fwd;
                // Space-saving top 4 of the forward deltas.
                auto it = std::find_if(s.deltas.begin(), s.deltas.end(),
                                       [&](const auto& d) { return d.second && d.first == delta; });
                if (it == s.deltas.end()) {
                    it = std::min_element(s.deltas.begin(), s.deltas.end(),
                                          [](const auto& a, const auto& b) {
                                              return a.second < b.second;
                                          });
                    it->first = delta;
                }
                ++it->second;
            } else {
                ++s.back;
            }
        }
    }
    s.last[0] = w[0];
    s.last[1] = w[1];
    s.last_base = base;
}

std::pair<s64, u64> TopDelta(const Slot& s) {
    const auto it = std::max_element(s.deltas.begin(), s.deltas.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
    return *it;
}

Class Classify(const Slot& s) {
    if (s.binds <= 1) {
        return OneShot;
    }
    if (s.moves == 0) {
        return Static;
    }
    if (!s.many_bases) {
        return Multi;
    }
    if (s.moves >= 8) {
        if (TopDelta(s).second * 10 >= s.moves * 8) {
            return RingFixed;
        }
        if (s.fwd * 10 >= s.moves * 8) {
            return RingVar;
        }
    }
    return Irregular;
}

double Pct(u64 a, u64 b) {
    return b ? 100.0 * double(a) / double(b) : 0.0;
}

void Report(std::chrono::steady_clock::time_point now) {
    auto& st = S();
    const u64 tsc = __rdtsc();
    const double secs = std::chrono::duration<double>(now - st.window_start).count();
    const double ns_per_cyc = secs * 1e9 / double(std::max<u64>(tsc - st.window_tsc, 1));
    const u64 flips_now = g_flips.load(std::memory_order_relaxed);
    const u64 flips = flips_now - st.flips_seen;
    const double fl = double(std::max<u64>(flips, 1));
    const auto ms = [&](u64 cyc) { return double(cyc) * ns_per_cyc / 1e6; };
    const auto mspf = [&](u64 cyc) { return ms(cyc) / fl; };
    const auto us = [&](u64 cyc, u64 n) { return n ? double(cyc) * ns_per_cyc / 1e3 / n : 0.0; };

    u64 all_records = 0;
    for (const auto& k : st.ks) {
        all_records += k.records;
    }
    LOG_INFO(Render_Vulkan,
             "probe: draw-memo: {:.1f} s, {} flips, {} records ({:.0f}/flip), tsc {:.2f} GHz, "
             "probe overhead {:.1f} ms ({:.2f} ms/flip), stages seen L1 {} L2 {} (table {} "
             "entries)",
             secs, flips, all_records, all_records / fl, 1.0 / ns_per_cyc,
             ms(g_overhead - st.overhead_seen), mspf(g_overhead - st.overhead_seen),
             st.l1_stages.size(), st.l2_stages.size(), 1u << TableBits);

    std::array<u64, NumPh> sum_hit{}, sum_clean{}, sum_miss{};
    u64 rec_hit = 0, rec_clean = 0, rec_all = 0;
    for (int k = 0; k < NumKinds; ++k) {
        const auto& ks = st.ks[k];
        if (!ks.records) {
            continue;
        }
        const u64 total = ks.cyc_hit + ks.cyc_miss;
        rec_hit += ks.cyc_hit;
        rec_clean += ks.cyc_clean;
        rec_all += total;
        LOG_INFO(Render_Vulkan,
                 "probe: draw-memo: {}: {} records ({:.0f}/flip), {:.2f} us each, {:.2f} ms/flip; "
                 "all layers hit {:.1f}% (clean {:.1f}%): hit {:.2f} ms/flip (clean {:.2f}), "
                 "miss {:.2f} ms/flip",
                 KindNames[k], ks.records, ks.records / fl, us(total, ks.records), mspf(total),
                 Pct(ks.all_hit, ks.records), Pct(ks.all_hit_clean, ks.records),
                 mspf(ks.cyc_hit), mspf(ks.cyc_clean), mspf(ks.cyc_miss));
        for (int p = 0; p < NumPh; ++p) {
            const auto& ph = ks.ph[p];
            if (!ph.calls) {
                continue;
            }
            sum_hit[p] += ph.cyc_hit;
            sum_clean[p] += ph.cyc_clean;
            sum_miss[p] += ph.cyc_miss;
            const u64 misses = ph.calls - ph.hits;
            LOG_INFO(Render_Vulkan,
                     "probe: draw-memo: {} {}: {} calls, would-hit {:.1f}% (clean {:.1f}%), "
                     "conflict misses {}; hit {:.2f} ms/flip ({:.2f} us/call), clean hit {:.2f} "
                     "ms/flip, miss {:.2f} ms/flip ({:.2f} us/call)",
                     KindNames[k], PhaseNames[p], ph.calls, Pct(ph.hits, ph.calls),
                     Pct(ph.clean, ph.calls), ph.conflicts, mspf(ph.cyc_hit),
                     us(ph.cyc_hit, ph.hits), mspf(ph.cyc_clean), mspf(ph.cyc_miss),
                     us(ph.cyc_miss, misses));
        }
    }
    LOG_INFO(Render_Vulkan,
             "probe: draw-memo: summary, ms/flip the would-hit calls spend in the phases a layer "
             "skips (clean = no upload/stream/image upload in the phase): L1 buf {:.2f} (clean "
             "{:.2f}), L2 tex {:.2f} (clean {:.2f}), L3 rstate+beginrt {:.2f} (clean {:.2f}), L4 "
             "barr {:.2f} (clean {:.2f}), whole record {:.2f} (clean {:.2f}) of {:.2f} ms/flip "
             "in records",
             mspf(sum_hit[Buf]), mspf(sum_clean[Buf]), mspf(sum_hit[Tex]), mspf(sum_clean[Tex]),
             mspf(sum_hit[RState] + sum_hit[BeginRt]),
             mspf(sum_clean[RState] + sum_clean[BeginRt]), mspf(sum_hit[Barr]),
             mspf(sum_clean[Barr]), mspf(rec_hit), mspf(rec_clean), mspf(rec_all));

    // Buffer bindings.
    std::array<u64, NumClasses> cls_slots{}, cls_binds{};
    std::vector<const Slot*> rings, irregular;
    u64 win_binds = 0;
    for (const auto& [key, s] : st.slots) {
        if (!s.binds) {
            continue;
        }
        win_binds += s.binds;
        const Class c = Classify(s);
        ++cls_slots[c];
        cls_binds[c] += s.binds;
        if (c == RingFixed || c == RingVar) {
            rings.push_back(&s);
        } else if (c == Irregular) {
            irregular.push_back(&s);
        }
    }
    std::string cls;
    for (int c = 0; c < NumClasses; ++c) {
        cls += fmt::format(" {} {} ({:.1f}% of binds),", ClassNames[c], cls_slots[c],
                           Pct(cls_binds[c], win_binds));
    }
    const u64 compared = st.bind_same + st.bind_base_only + st.bind_other;
    LOG_INFO(Render_Vulkan,
             "probe: draw-memo: L1 bindings: {} binds ({:.0f}/flip) vs the slot's last: same "
             "{:.1f}%, base-only change {:.1f}%, other change {:.1f}%, first {}; slots by "
             "class:{}",
             win_binds, win_binds / fl, Pct(st.bind_same, compared),
             Pct(st.bind_base_only, compared), Pct(st.bind_other, compared), st.bind_first, cls);

    // Ring regions: the ring slots' address ranges, merged when they overlap or lie within
    // 64 KiB of each other.
    struct Region {
        u64 lo, hi, binds, slots;
    };
    std::vector<Region> regions;
    {
        std::vector<Region> spans;
        for (const auto* s : rings) {
            spans.push_back({s->minb, s->maxb + s->size, s->binds, 1});
        }
        std::sort(spans.begin(), spans.end(), [](auto& a, auto& b) { return a.lo < b.lo; });
        for (const auto& r : spans) {
            if (!regions.empty() && r.lo <= regions.back().hi + (64u << 10)) {
                auto& b = regions.back();
                b.hi = std::max(b.hi, r.hi);
                b.binds += r.binds;
                b.slots += r.slots;
            } else {
                regions.push_back(r);
            }
        }
        std::sort(regions.begin(), regions.end(),
                  [](auto& a, auto& b) { return a.binds > b.binds; });
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(regions.size(), 6); ++i) {
        const auto& r = regions[i];
        LOG_INFO(Render_Vulkan,
                 "probe: draw-memo: ring region {}: {:#x}-{:#x} ({} KiB), {} slots, {} binds "
                 "({:.0f}/flip)",
                 i, r.lo, r.hi, (r.hi - r.lo) >> 10, r.slots, r.binds, r.binds / fl);
    }
    const auto by_binds = [](const Slot* a, const Slot* b) { return a->binds > b->binds; };
    std::sort(rings.begin(), rings.end(), by_binds);
    std::sort(irregular.begin(), irregular.end(), by_binds);
    const auto slot_line = [&](const char* what, std::size_t i, const Slot& s) {
        const auto [stride, count] = TopDelta(s);
        LOG_INFO(Render_Vulkan,
                 "probe: draw-memo: {} slot {}: pgm {:016x} b{} {}, {} binds ({:.0f}/flip), base "
                 "{:#x}-{:#x} ({} KiB), size {}, moves {} (fwd {}, back/wraps {}), top stride "
                 "{} ({:.0f}% of moves), V# same {} base-only {} other {}",
                 what, i, s.pgm_hash, s.idx, ClassNames[Classify(s)], s.binds, s.binds / fl,
                 s.minb, s.maxb, (s.maxb + s.size - s.minb) >> 10, s.size, s.moves, s.fwd,
                 s.back, stride, Pct(count, s.moves), s.same, s.base_only, s.other);
    };
    for (std::size_t i = 0; i < std::min<std::size_t>(rings.size(), 8); ++i) {
        slot_line("ring", i, *rings[i]);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(irregular.size(), 4); ++i) {
        slot_line("irregular", i, *irregular[i]);
    }

    // Next window.
    for (auto& [key, s] : st.slots) {
        const u64 pgm = s.pgm_hash;
        const u32 idx = s.idx;
        const bool have = s.have;
        const u64 last0 = s.last[0], last1 = s.last[1], last_base = s.last_base;
        s = Slot{};
        s.pgm_hash = pgm;
        s.idx = idx;
        s.have = have;
        s.last[0] = last0;
        s.last[1] = last1;
        s.last_base = last_base;
    }
    st.ks = {};
    st.l1_stages.clear();
    st.l2_stages.clear();
    st.bind_same = st.bind_base_only = st.bind_other = st.bind_first = 0;
    st.flips_seen = flips_now;
    st.overhead_seen = g_overhead;
    st.window_start = std::chrono::steady_clock::now();
    st.window_tsc = __rdtsc();
}

} // namespace

void BeginRecord(Kind kind) {
    auto& st = S();
    auto& c = st.cur;
    if (st.window_tsc == 0) {
        st.window_start = std::chrono::steady_clock::now();
        st.window_tsc = __rdtsc();
        st.flips_seen = g_flips.load(std::memory_order_relaxed);
        st.overhead_seen = g_overhead;
        LOG_INFO(Render_Vulkan, "SOTC_DRAW_MEMO_PROBE on: a `probe: draw-memo:` block every 10 s");
    }
    c = Current{};
    c.active = true;
    c.kind = int(kind);
    c.graphics = kind == Kind::Draw || kind == Kind::DrawInd;
    c.ov0 = g_overhead;
    c.dirty0 = g_dirty.load(std::memory_order_relaxed);
    c.t0 = __rdtsc();
}

void EndRecord() {
    const u64 now = __rdtsc();
    auto& st = S();
    auto& c = st.cur;
    if (!c.active) {
        return;
    }
    c.active = false;
    const u64 ov = g_overhead - c.ov0;
    const u64 dt = now - c.t0 > ov ? now - c.t0 - ov : 0;
    const bool hit = c.l1_all && c.l2_all && (!c.graphics || c.l3_hit);
    const bool clean = g_dirty.load(std::memory_order_relaxed) == c.dirty0;
    auto& ks = st.ks[c.kind];
    ++ks.records;
    if (hit) {
        ++ks.all_hit;
        ks.cyc_hit += dt;
        if (clean) {
            ++ks.all_hit_clean;
            ks.cyc_clean += dt;
        }
    } else {
        ks.cyc_miss += dt;
    }
    if ((++st.records_since_check & 255) == 0) {
        const auto t = std::chrono::steady_clock::now();
        if (t - st.window_start >= Window) {
            Report(t);
        }
    }
}

void PhaseEnd(Ph ph, u64 t0, u64 ov0, u64 d0) {
    const u64 now = __rdtsc();
    auto& st = S();
    auto& c = st.cur;
    if (!c.active) {
        c.pending_set = false;
        return;
    }
    const u64 ov = g_overhead - ov0;
    const u64 dt = now - t0 > ov ? now - t0 - ov : 0;
    const u64 dirty_now = g_dirty.load(std::memory_order_relaxed);
    bool dirty = dirty_now != d0;
    bool hit = false;
    bool conflict = false;
    switch (ph) {
    case Buf:
    case Tex:
        hit = c.pending_set && c.pending_hit;
        conflict = c.pending_set && c.pending_conflict;
        (ph == Buf ? c.l1_all : c.l2_all) &= hit;
        c.pending_set = false;
        break;
    case RState:
    case BeginRt:
        hit = c.l3_hit;
        break;
    case Barr:
        hit = c.l1_all && c.l2_all && (!c.graphics || c.l3_hit);
        dirty = dirty_now != c.dirty0; // the whole record so far
        break;
    default:
        break;
    }
    auto& ps = st.ks[c.kind].ph[ph];
    ++ps.calls;
    if (hit) {
        ++ps.hits;
        ps.cyc_hit += dt;
        if (!dirty) {
            ++ps.clean;
            ps.cyc_clean += dt;
        }
    } else {
        ps.cyc_miss += dt;
        ps.conflicts += conflict;
    }
    g_overhead += __rdtsc() - now;
}

void KeyRenderState(const AmdGpu::Regs& regs, u32 mrt_mask, const AmdGpu::CbDbExtent* cb_extents,
                    const AmdGpu::CbDbExtent& db_extent) {
    const u64 t = __rdtsc();
    auto& st = S();
    if (!st.cur.active) {
        return;
    }
    u64 h = Mix(0x4c33, mrt_mask);
    h = MixObj(h, regs.color_control);
    h = MixObj(h, regs.color_target_mask);
    for (u32 cb = 0; cb < u32(std::bit_width(mrt_mask)); ++cb) {
        if ((mrt_mask >> cb) & 1) {
            h = MixObj(h, regs.color_buffers[cb]);
            h = Mix(h, cb_extents[cb].raw);
        }
    }
    h = MixObj(h, regs.depth_control);
    h = MixObj(h, regs.depth_buffer);
    h = MixObj(h, regs.depth_view);
    h = MixObj(h, regs.depth_htile_data_base);
    h = MixObj(h, regs.depth_render_control);
    h = Mix(h, db_extent.raw);
    st.cur.l3_hit = st.l3_valid && h == st.l3_last;
    st.l3_last = h;
    st.l3_valid = true;
    g_overhead += __rdtsc() - t;
}

void KeyBuffers(const Shader::Info& stage) {
    const u64 t = __rdtsc();
    auto& st = S();
    if (!st.cur.active) {
        return;
    }
    u64 h = 0x4c31;
    u32 idx = 0;
    for (const auto& desc : stage.buffers) {
        if (desc.IsSpecial()) {
            // Flatbuf/clip planes/LDS/...: today's path runs anyway (not part of the key).
            h = Mix(h, 0xF000u | u32(desc.buffer_type));
        } else {
            const AmdGpu::Buffer v = desc.GetSharp(stage);
            h = MixObj(h, v);
            ClassifyBinding(stage, idx, v);
        }
        ++idx;
    }
    TableCheck(st.l1_tab, &stage, h);
    st.l1_stages.insert(&stage);
    g_overhead += __rdtsc() - t;
}

void KeyTextures(const Shader::Info& stage, const AmdGpu::Regs& regs) {
    const u64 t = __rdtsc();
    auto& st = S();
    if (!st.cur.active) {
        return;
    }
    u64 h = 0x4c32;
    for (const auto& desc : stage.images) {
        h = MixObj(h, desc.GetSharp(stage));
    }
    for (const auto& desc : stage.samplers) {
        h = MixObj(h, desc.GetSharp(stage));
    }
    if (!stage.samplers.empty()) {
        h = MixObj(h, regs.ta_bc_base); // custom border colours
    }
    TableCheck(st.l2_tab, &stage, h);
    st.l2_stages.insert(&stage);
    g_overhead += __rdtsc() - t;
}

} // namespace SotcMemoProbe
