// SPDX-License-Identifier: GPL-2.0-or-later
// Session 36: VideoCore::SotcFindCache, the cache of TextureCache::FindImage's perfect matches
// (video_core/texture_cache/sotc_find_cache.h). A model texture cache (images register and
// unregister, ids are reused like SlotVector's) answers lookups through the cache the way
// FindImage does; every answer must equal the uncached perfect-match search.
#include <cstdio>
#include <random>
#include <vector>

#include "video_core/texture_cache/sotc_find_cache.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using VideoCore::SotcFindCache;
using Key = SotcFindCache::Key;
constexpr std::uint32_t None = SotcFindCache::Invalid;

static Key MakeKey(std::uint64_t addr, std::uint32_t size, std::uint32_t w, std::uint32_t fmt) {
    Key k{};
    k.guest_address = addr;
    k.guest_size = size;
    k.width = w;
    k.height = w;
    k.depth = 1;
    k.pixel_format = fmt;
    k.levels = 1;
    k.layers = 1;
    k.num_samples = 1;
    return k;
}

// The model: slot ids reused after a free, a generation bump on every register/unregister.
struct Model {
    struct Image {
        Key info{}; // only the image fields (guest range, size, format)
        bool live = false;
    };
    std::vector<Image> slots;
    SotcFindCache cache;
    std::uint64_t searches = 0;

    std::uint32_t Register(const Key& info) {
        std::uint32_t id = 0;
        while (id < slots.size() && slots[id].live) {
            ++id;
        }
        if (id == slots.size()) {
            slots.emplace_back();
        }
        slots[id] = {info, true};
        cache.Bump();
        return id;
    }
    void Unregister(std::uint32_t id) {
        slots[id].live = false;
        cache.Bump();
    }

    // FindImage's perfect match (last match wins), ignoring the request-only fields.
    std::uint32_t Search(const Key& req) {
        ++searches;
        std::uint32_t found = None;
        for (std::uint32_t id = 0; id < slots.size(); ++id) {
            const Key& im = slots[id].info;
            if (slots[id].live && im.guest_address == req.guest_address &&
                im.guest_size == req.guest_size && im.width == req.width &&
                im.height == req.height && im.pixel_format == req.pixel_format) {
                found = id;
            }
        }
        return found;
    }

    std::uint32_t Find(const Key& req) {
        if (const std::uint32_t hit = cache.Lookup(req); hit != None) {
            return hit;
        }
        const std::uint64_t gen = cache.Generation();
        const std::uint32_t id = Search(req);
        if (id != None) {
            cache.Store(req, id, gen);
        }
        return id;
    }
};

static void TestBasics() {
    Model m;
    const Key a = MakeKey(0x1042000000, 0x40000, 256, 37);
    CHECK(m.cache.Lookup(a) == None); // a fresh cache holds nothing
    const std::uint32_t ia = m.Register(a);
    CHECK(m.Find(a) == ia);
    const std::uint64_t searched = m.searches;
    CHECK(m.Find(a) == ia);
    CHECK(m.searches == searched); // served from the cache

    // Any request field differing misses.
    Key b = a;
    b.binding_type = 2;
    CHECK(m.cache.Lookup(b) == None);
    b = a;
    b.exact_fmt = 1;
    CHECK(m.cache.Lookup(b) == None);
    b = a;
    b.levels = 2;
    CHECK(m.cache.Lookup(b) == None);

    // A store from a lookup that saw the generation move is dropped.
    const std::uint64_t old_gen = m.cache.Generation();
    m.cache.Bump();
    const Key c = MakeKey(0x1043000000, 0x40000, 256, 37);
    m.cache.Store(c, 7, old_gen);
    CHECK(m.cache.Lookup(c) == None);

    // A freed image is never returned; its reused slot id is found again only by a search.
    m.Unregister(ia);
    CHECK(m.Find(a) == None);
    const std::uint32_t ic = m.Register(c);
    CHECK(ic == ia); // id reused
    CHECK(m.Find(a) == None);
    CHECK(m.Find(c) == ic);
}

static void TestRandom() {
    Model m;
    std::mt19937_64 rng{36};
    std::vector<Key> pool;
    for (std::uint32_t i = 0; i < 64; ++i) {
        // Few addresses, so images share ranges and several match one request.
        pool.push_back(MakeKey(0x1042000000 + (i % 16) * 0x40000, 0x40000, 128u << (i % 2),
                               37 + (i / 32)));
    }
    std::vector<std::uint32_t> live;
    std::uint64_t hits = 0, lookups = 0;
    for (int step = 0; step < 200000; ++step) {
        const auto r = rng() % 1000;
        if (r < 2 || live.empty()) {
            live.push_back(m.Register(pool[rng() % pool.size()]));
        } else if (r < 4) {
            const std::size_t i = rng() % live.size();
            m.Unregister(live[i]);
            live.erase(live.begin() + i);
        } else {
            Key req = pool[rng() % pool.size()];
            req.binding_type = static_cast<std::uint8_t>(rng() % 2);
            const std::uint64_t before = m.searches;
            const std::uint32_t got = m.Find(req);
            hits += m.searches == before;
            ++lookups;
            const std::uint32_t want = m.Search(req);
            CHECK(got == want);
            if (got != want) {
                break;
            }
        }
    }
    std::printf("random: %llu lookups, %llu from the cache\n", (unsigned long long)lookups,
                (unsigned long long)hits);
    CHECK(hits > lookups / 4); // the cache path is exercised
}

int main() {
    TestBasics();
    TestRandom();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_find_cache: ok\n");
    return 0;
}
