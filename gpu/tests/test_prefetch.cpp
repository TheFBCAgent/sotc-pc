// SPDX-License-Identifier: GPL-2.0-or-later
// Session 13: VideoCore::SotcPrefetch, the hot-page set, ring and entries of the readback
// prefetch (BufferCache::SotcPrefetchAtBatchEnd / SotcServeFromPrefetch).
#include <cstdio>

#include "video_core/buffer_cache/sotc_prefetch.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using VideoCore::SotcPrefetch;
using Miss = SotcPrefetch::Miss;

static size_t HotPages(const SotcPrefetch& pf) {
    size_t n = 0;
    pf.ForEachHotPage([&](u64) { ++n; });
    return n;
}

static void TestHotSet() {
    SotcPrefetch pf{1 << 20};
    CHECK(!pf.NoteDownload(0x104271, 10));
    CHECK(pf.NoteDownload(0x104271, 12)); // the same page again: still one
    CHECK(!pf.NoteDownload(0x104280, 12));
    CHECK(pf.HotCount() == 2);
    // Not aged while read within HotTicks submits.
    pf.Age(12 + SotcPrefetch::HotTicks);
    CHECK(pf.HotCount() == 2);
    pf.NoteDownload(0x104280, 100);
    pf.Age(12 + SotcPrefetch::HotTicks + 1);
    CHECK(pf.HotCount() == 1); // 0x104271 aged out
    // Capped: a new page replaces the one read longest ago.
    for (u64 i = 0; i < SotcPrefetch::MaxHotPages + 5; ++i) {
        pf.NoteDownload(0x200000 + i, 200 + i);
    }
    CHECK(pf.HotCount() == SotcPrefetch::MaxHotPages);
    bool has_oldest = false, has_newest = false;
    pf.ForEachHotPage([&](u64 page) {
        has_oldest |= page == 0x104280;
        has_newest |= page == 0x200000 + SotcPrefetch::MaxHotPages + 4;
    });
    CHECK(!has_oldest);
    CHECK(has_newest);
    CHECK(HotPages(pf) == SotcPrefetch::MaxHotPages);
}

static void TestLookup() {
    SotcPrefetch pf{1 << 20};
    const u64 page = 0x104271;
    const u64 base = page << SotcPrefetch::PageShift;
    pf.NoteDownload(page, 5);
    std::vector<SotcPrefetch::Segment> out;
    CHECK(pf.Lookup(base + 0x110, 0xb00, out) == Miss::NoEntry);

    // The batch-5 copy of two GPU-written pieces of the page.
    std::vector<SotcPrefetch::Piece> pieces{{base + 0x110, 0xb00, 0}, {base + 0x2000, 0x40, 0}};
    for (auto& p : pieces) {
        p.ring_pos = pf.Allocate(p.size);
    }
    CHECK(pieces[0].ring_pos == 0);
    CHECK(pieces[1].ring_pos == 0xb00); // 64-byte aligned
    pf.Record(page, 5, pieces);

    out.clear();
    CHECK(pf.Lookup(base + 0x110, 0xb00, out) == Miss::None);
    CHECK(out.size() == 1 && out[0].ring_pos == 0 && out[0].tick == 5);
    out.clear();
    CHECK(pf.Lookup(base + 0x200, 0x10, out) == Miss::None); // inside a piece
    CHECK(out.size() == 1 && out[0].ring_pos == 0xf0 && out[0].size == 0x10);
    out.clear();
    CHECK(pf.Lookup(base + 0x100, 0x20, out) == Miss::Partial); // starts before the piece
    CHECK(pf.Lookup(base + 0x2000, 0x41, out) == Miss::Partial);

    // A request across two pages needs both pages' entries.
    const u64 next = page + 1;
    const u64 next_base = next << SotcPrefetch::PageShift;
    std::vector<SotcPrefetch::Piece> tail{{base + 0xff00, 0x100, 0}};
    tail[0].ring_pos = pf.Allocate(0x100);
    pf.Record(page, 6, tail); // replaces the page's entry
    out.clear();
    CHECK(pf.Lookup(base + 0xff80, 0x100, out) == Miss::NoEntry); // next page has none
    std::vector<SotcPrefetch::Piece> head{{next_base, 0x80, 0}};
    head[0].ring_pos = pf.Allocate(0x80);
    pf.Record(next, 7, head);
    out.clear();
    CHECK(pf.Lookup(base + 0xff80, 0x100, out) == Miss::None);
    CHECK(out.size() == 2);
    CHECK(out[0].addr == base + 0xff80 && out[0].size == 0x80 && out[0].tick == 6);
    CHECK(out[0].ring_pos == tail[0].ring_pos + 0x80);
    CHECK(out[1].addr == next_base && out[1].size == 0x80 && out[1].tick == 7);
    // The old pieces are gone with the replaced entry.
    CHECK(pf.Lookup(base + 0x110, 0x10, out) == Miss::Partial);
}

static void TestRing() {
    SotcPrefetch pf{0x1000};
    const u64 a = pf.Allocate(0x800);
    const u64 b = pf.Allocate(0x700);
    CHECK(a == 0 && b == 0x800);
    CHECK(pf.IsLive(a) && pf.IsLive(b));
    // 0x200 doesn't fit before the end (0xf00 + 0x200 > 0x1000): it starts the next lap and
    // reuses a's bytes.
    const u64 c = pf.Allocate(0x200);
    CHECK(c == 0x1000);
    CHECK(pf.RingOffset(c) == 0);
    CHECK(!pf.IsLive(a));
    CHECK(pf.IsLive(b));
    CHECK(pf.IsLive(c));
    // An entry whose piece was reused misses as Overwritten.
    const u64 page = 0x300000;
    const u64 base = page << SotcPrefetch::PageShift;
    pf.NoteDownload(page, 1);
    pf.Record(page, 1, {{base, 0x800, a}});
    std::vector<SotcPrefetch::Segment> out;
    CHECK(pf.Lookup(base, 0x10, out) == Miss::Overwritten);
    pf.Record(page, 2, {{base, 0x700, b}});
    out.clear();
    CHECK(pf.Lookup(base, 0x10, out) == Miss::None);
    (void)pf.Allocate(0x800); // laps b
    out.clear();
    CHECK(pf.Lookup(base, 0x10, out) == Miss::Overwritten);
}

static void TestAgeDropsEntries() {
    SotcPrefetch pf{1 << 20};
    const u64 page = 0x400000;
    const u64 base = page << SotcPrefetch::PageShift;
    pf.NoteDownload(page, 1);
    pf.Record(page, 1, {{base, 0x40, pf.Allocate(0x40)}});
    pf.Age(1 + SotcPrefetch::HotTicks + 1);
    CHECK(pf.HotCount() == 0);
    std::vector<SotcPrefetch::Segment> out;
    CHECK(pf.Lookup(base, 0x40, out) == Miss::NoEntry);
}

int main() {
    TestHotSet();
    TestLookup();
    TestRing();
    TestAgeDropsEntries();
    if (g_failures == 0) {
        std::printf("test_prefetch: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
