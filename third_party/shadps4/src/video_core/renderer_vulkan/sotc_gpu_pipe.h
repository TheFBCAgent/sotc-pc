// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 43): the Vulkan-free parts of the two-stage GPU pipeline
// (docs/next/gpu-pipe.md). Stage A (the GPU command thread: PM4 decode, registers, pipeline
// selection) hands stage B (recording) one packet per draw or dispatch, in stream order:
// - SotcDirtyBlocks: which 32-dword register blocks changed since the last packet. Liverpool
//   marks them in every register write path; a packet carries only those blocks (the register
//   file is 0xD000 dwords, 208 KiB, so never whole) and B replays them into its own copy.
// - SotcPacketWriter/Reader: a packet's bytes, 8-byte aligned fields.
// - SotcPacketRing: a single-producer single-consumer ring of variable-size packets. Release
//   on commit, acquire on read; each side sleeps (atomic wait, a futex on Linux) when the ring
//   is empty or full, with no spinning (the Deck has 4 cores). A side notifies only when the
//   other one announced it sleeps, and the consumer publishes its position every PublishEvery
//   packets and before it sleeps: a seq_cst store per packet waits for the store buffer the
//   recording just filled (~180 ns, 3.7% of the GPU thread in sync mode, session 43).
// - SotcPendingRanges (session 47): A's view of the guest memory packets B hasn't run yet will
//   change: GPU writes (a written buffer binding) and CPU-copy results (with their values, the
//   overlay A reads instead of guest memory until B has committed them).
// Model: bbport's vk_draw_pipe.h (bratdr/bloodborne_pc, docs/parallel_gpu.md).
// Test: gpu/tests/test_gpu_pipe.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Vulkan {

/// The register blocks written since the last Take (32 dwords per block).
template <std::size_t NumWords>
class SotcDirtyBlocks {
public:
    static constexpr std::uint32_t BlockDwords = 32;
    static constexpr std::size_t NumBlocks = (NumWords + BlockDwords - 1) / BlockDwords;

    SotcDirtyBlocks() {
        MarkAll(); // B's copy starts empty: the first packet carries everything
    }

    void Mark(std::uint32_t first_word, std::uint32_t count) {
        if (count == 0 || first_word >= NumWords) {
            return;
        }
        const std::uint32_t last_word =
            std::min<std::uint32_t>(first_word + count, NumWords) - 1;
        for (std::uint32_t b = first_word / BlockDwords; b <= last_word / BlockDwords; ++b) {
            bits[b / 64] |= std::uint64_t{1} << (b % 64);
        }
    }

    void MarkAll() {
        bits.fill(~std::uint64_t{0});
        if constexpr (NumBlocks % 64 != 0) {
            bits.back() = (std::uint64_t{1} << (NumBlocks % 64)) - 1;
        }
    }

    bool Any() const {
        return std::ranges::any_of(bits, [](std::uint64_t w) { return w != 0; });
    }

    /// Calls func(block) for every dirty block in ascending order and clears them.
    template <typename Func>
    void Take(Func&& func) {
        for (std::size_t w = 0; w < bits.size(); ++w) {
            std::uint64_t word = std::exchange(bits[w], 0);
            while (word != 0) {
                const int bit = std::countr_zero(word);
                word &= word - 1;
                func(static_cast<std::uint32_t>(w * 64 + bit));
            }
        }
    }

private:
    std::array<std::uint64_t, (NumBlocks + 63) / 64> bits{};
};

class SotcPacketWriter {
public:
    void Clear() {
        bytes.clear();
    }
    std::span<const std::uint8_t> Bytes() const {
        return bytes;
    }

    template <typename T>
    void Put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        PutRaw(&value, sizeof(T));
    }
    template <typename T>
    void PutSpan(std::span<const T> values) {
        static_assert(std::is_trivially_copyable_v<T>);
        Put<std::uint64_t>(values.size());
        PutRaw(values.data(), values.size_bytes());
    }
    void PutRaw(const void* data, std::size_t size) {
        const std::size_t at = bytes.size();
        bytes.resize(at + Align(size));
        if (size != 0) {
            std::memcpy(bytes.data() + at, data, size);
        }
    }

    static constexpr std::size_t Align(std::size_t n) {
        return (n + 7) & ~std::size_t{7};
    }

private:
    std::vector<std::uint8_t> bytes;
};

class SotcPacketReader {
public:
    explicit SotcPacketReader(std::span<const std::uint8_t> bytes_) : bytes{bytes_} {}

    template <typename T>
    T Get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        std::memcpy(&value, Take(sizeof(T)), sizeof(T));
        return value;
    }
    /// A view into the packet (valid until the packet is popped).
    template <typename T>
    std::span<const T> GetSpan() {
        const auto n = Get<std::uint64_t>();
        return {reinterpret_cast<const T*>(Take(n * sizeof(T))), static_cast<std::size_t>(n)};
    }
    const std::uint8_t* Take(std::size_t size) {
        const std::uint8_t* p = bytes.data() + pos;
        pos += SotcPacketWriter::Align(size);
        return p;
    }
    bool AtEnd() const {
        return pos >= bytes.size();
    }

private:
    std::span<const std::uint8_t> bytes;
    std::size_t pos = 0;
};

/// Appends the dirty register blocks of words[] to the packet and clears them.
/// Layout: u64 count, then per block u32 index (padded to 8) and its 32 dwords.
template <std::size_t NumWords>
void SotcEncodeRegDelta(SotcDirtyBlocks<NumWords>& dirty, const std::uint32_t* words,
                        SotcPacketWriter& out, std::vector<std::uint32_t>& scratch) {
    constexpr std::uint32_t B = SotcDirtyBlocks<NumWords>::BlockDwords;
    scratch.clear();
    dirty.Take([&](std::uint32_t block) { scratch.push_back(block); });
    out.Put<std::uint64_t>(scratch.size());
    for (const std::uint32_t block : scratch) {
        out.Put<std::uint64_t>(block);
        const std::size_t first = std::size_t{block} * B;
        const std::size_t n = std::min<std::size_t>(B, NumWords - first);
        std::array<std::uint32_t, B> data{};
        std::memcpy(data.data(), words + first, n * sizeof(std::uint32_t));
        out.Put(data);
    }
}

/// Replays a delta written by SotcEncodeRegDelta into words[]. Returns the blocks applied.
template <std::size_t NumWords>
std::size_t SotcApplyRegDelta(SotcPacketReader& in, std::uint32_t* words) {
    constexpr std::uint32_t B = SotcDirtyBlocks<NumWords>::BlockDwords;
    const auto count = in.Get<std::uint64_t>();
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto block = in.Get<std::uint64_t>();
        const std::uint8_t* data = in.Take(B * sizeof(std::uint32_t));
        const std::size_t first = static_cast<std::size_t>(block) * B;
        if (first >= NumWords) {
            continue;
        }
        const std::size_t n = std::min<std::size_t>(B, NumWords - first);
        std::memcpy(words + first, data, n * sizeof(std::uint32_t));
    }
    return static_cast<std::size_t>(count);
}

/// Single-producer single-consumer ring of variable-size packets. Each record is an 8-byte
/// header (payload size, or WrapMarker: the rest of the buffer is unused, continue at 0) and
/// the payload, 8-byte aligned. max_records caps the packets in flight (SOTC_GPU_PIPE_DEPTH).
class SotcPacketRing {
public:
    static constexpr std::uint64_t WrapMarker = ~std::uint64_t{0};

    static constexpr std::size_t PublishEvery = 32;

    explicit SotcPacketRing(std::size_t capacity_pow2, std::size_t max_records_ = ~std::size_t{0})
        : capacity{std::bit_ceil(std::max<std::size_t>(capacity_pow2, 64))},
          max_records{std::max<std::size_t>(max_records_, 1)},
          publish_every{std::clamp<std::size_t>(max_records / 2, 1, PublishEvery)},
          buffer{std::make_unique<std::uint8_t[]>(capacity)} {}

    std::size_t Capacity() const {
        return capacity;
    }
    /// The largest payload Push accepts.
    std::size_t MaxPayload() const {
        return capacity / 2 - 8;
    }

    /// Producer: copies the packet in, waiting while the ring is full.
    void Push(std::span<const std::uint8_t> payload) {
        const std::size_t need = 8 + SotcPacketWriter::Align(payload.size());
        std::uint64_t h = head.load(std::memory_order_relaxed);
        std::size_t off = h & (capacity - 1);
        if (off + need > capacity) {
            // Not contiguous: mark the tail end unused and continue at 0.
            WaitFree(h, capacity - off);
            std::memcpy(buffer.get() + off, &WrapMarker, 8);
            h += capacity - off;
            off = 0;
        }
        WaitFree(h, need);
        for (;;) {
            const std::uint64_t seen = popped.load(std::memory_order_acquire);
            if (pushed - seen < max_records) {
                break;
            }
            producer_waiting.store(true, std::memory_order_seq_cst);
            if (popped.load(std::memory_order_seq_cst) == seen) {
                popped.wait(seen, std::memory_order_acquire);
            }
            producer_waiting.store(false, std::memory_order_relaxed);
        }
        const std::uint64_t size = payload.size();
        std::memcpy(buffer.get() + off, &size, 8);
        if (!payload.empty()) {
            std::memcpy(buffer.get() + off + 8, payload.data(), payload.size());
        }
        ++pushed;
        head.store(h + need, std::memory_order_seq_cst);
        WakeConsumer();
    }

    /// Any thread (session 46): wakes a consumer asleep in WaitFront so it re-checks its
    /// `other_work` (stage B's command queue). Call it after making that work visible.
    void Kick() {
        WakeConsumer();
    }

    /// Consumer: the oldest packet, or an empty span when none is queued.
    std::span<const std::uint8_t> Front() {
        if (head.load(std::memory_order_acquire) == ctail) {
            return {};
        }
        std::size_t off = ctail & (capacity - 1);
        std::uint64_t size;
        std::memcpy(&size, buffer.get() + off, 8);
        if (size == WrapMarker) {
            ctail += capacity - off;
            off = 0;
            std::memcpy(&size, buffer.get(), 8);
        }
        front_bytes = 8 + SotcPacketWriter::Align(size);
        return {buffer.get() + off + 8, static_cast<std::size_t>(size)};
    }

    /// Consumer: waits until a packet is queued, then returns it.
    std::span<const std::uint8_t> WaitFront() {
        return WaitFront([] { return false; });
    }

    /// Consumer: waits until a packet is queued (returns it) or `other_work()` is true after a
    /// Kick (returns an empty span). The consumer sleeps on `wakes`, which Push and Kick bump
    /// after their change: if it read `wakes` after a bump, it also sees that change.
    template <typename OtherWork>
    std::span<const std::uint8_t> WaitFront(OtherWork&& other_work) {
        for (;;) {
            const std::uint64_t t = ctail;
            if (head.load(std::memory_order_acquire) != t) {
                return Front();
            }
            if (other_work()) {
                return {};
            }
            Publish(); // the producer may wait for this space
            consumer_waiting.store(true, std::memory_order_seq_cst);
            const std::uint32_t w = wakes.load(std::memory_order_seq_cst);
            if (head.load(std::memory_order_seq_cst) == t && !other_work()) {
                wakes.wait(w, std::memory_order_acquire);
            }
            consumer_waiting.store(false, std::memory_order_relaxed);
        }
    }

    /// Consumer: releases the packet Front returned.
    void Pop() {
        ctail += front_bytes;
        ++cpopped;
        front_bytes = 0;
        if (++unpublished >= publish_every) {
            Publish();
        }
    }

    /// Consumer: makes the popped space available to the producer now.
    void Publish() {
        if (unpublished == 0) {
            return;
        }
        unpublished = 0;
        tail.store(ctail, std::memory_order_seq_cst);
        popped.store(cpopped, std::memory_order_seq_cst);
        WakeProducer();
    }

    /// Consumer: nothing left to read.
    bool Empty() const {
        return head.load(std::memory_order_acquire) == ctail;
    }

private:
    void WaitFree(std::uint64_t h, std::size_t bytes) {
        for (;;) {
            const std::uint64_t t = tail.load(std::memory_order_acquire);
            if (capacity - (h - t) >= bytes) {
                return;
            }
            producer_waiting.store(true, std::memory_order_seq_cst);
            if (tail.load(std::memory_order_seq_cst) == t) {
                tail.wait(t, std::memory_order_acquire);
            }
            producer_waiting.store(false, std::memory_order_relaxed);
        }
    }
    void WakeConsumer() {
        if (consumer_waiting.load(std::memory_order_seq_cst)) {
            wakes.fetch_add(1, std::memory_order_seq_cst);
            wakes.notify_one();
        }
    }
    void WakeProducer() {
        if (producer_waiting.load(std::memory_order_seq_cst)) {
            tail.notify_one();
            popped.notify_one();
        }
    }

    const std::size_t capacity;
    const std::size_t max_records;
    const std::size_t publish_every;
    std::unique_ptr<std::uint8_t[]> buffer;
    alignas(64) std::atomic<std::uint64_t> head{0}; ///< producer position (bytes)
    std::uint64_t pushed = 0;                       ///< producer only
    alignas(64) std::atomic<std::uint64_t> tail{0}; ///< consumer position (bytes)
    std::atomic<std::uint64_t> popped{0};
    alignas(64) std::atomic<bool> consumer_waiting{false};
    std::atomic<std::uint32_t> wakes{0}; ///< the consumer sleeps on this (Push, Kick)
    alignas(64) std::atomic<bool> producer_waiting{false};
    std::uint64_t ctail = 0;       ///< consumer only: position read up to (tail: published)
    std::uint64_t cpopped = 0;     ///< consumer only
    std::size_t unpublished = 0;   ///< consumer only
    std::size_t front_bytes = 0;   ///< consumer only
};

/// Session 45: the label writes of the fence packets stage B hasn't run yet (stage A only).
/// A WaitRegMem on a label a pending fence writes is decided from the newest such write: B
/// runs that fence before any later packet, so the wait is met in stream order. ~250 of ~296
/// waits per flip wait for a fence a few packets back (session 42).
class SotcPendingFences {
public:
    enum class Lookup { None, Value, Unknown };

    /// A fence in packet `seq` writes `bytes` (<= 8) at `addr`; `known` false: a clock, a GDS
    /// copy, any value A can't predict.
    void Add(std::uint64_t seq, std::uint64_t addr, std::uint32_t bytes, std::uint64_t value,
             bool known) {
        writes.push_back({seq, addr, bytes, value, known});
    }

    /// Forgets the writes of packets up to `retired` (B ran them: memory holds the values).
    void Retire(std::uint64_t retired) {
        std::size_t n = 0;
        while (n < writes.size() && writes[n].seq <= retired) {
            ++n;
        }
        if (n != 0) {
            writes.erase(writes.begin(), writes.begin() + std::ptrdiff_t(n));
        }
    }

    /// The u32 at `addr` once every pending write has run: Value (in *out), None (no pending
    /// write touches it), or Unknown (the newest one is not predictable or covers it partly).
    Lookup Find(std::uint64_t addr, std::uint32_t* out) const {
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            if (it->addr >= addr + 4 || addr >= it->addr + it->bytes) {
                continue;
            }
            if (!it->known || addr < it->addr || addr + 4 > it->addr + it->bytes) {
                return Lookup::Unknown;
            }
            *out = std::uint32_t(it->value >> (8 * (addr - it->addr)));
            return Lookup::Value;
        }
        return Lookup::None;
    }

    bool Empty() const {
        return writes.empty();
    }

private:
    struct Write {
        std::uint64_t seq;
        std::uint64_t addr;
        std::uint32_t bytes;
        std::uint64_t value;
        bool known;
    };
    std::vector<Write> writes; ///< in stream order
};

/// Session 47 (docs/next/gpu-pipe.md, step 4): guest ranges that packets B hasn't run yet will
/// write, on stage A. Two kinds: a GPU write (a written buffer binding; A only knows "changes",
/// so a read that hits one waits for B) and an overlay (a CPU copy A computed: the values the
/// dispatch writes, which A reads instead of guest memory until B has committed them).
/// A GPU write of a range already pending only moves its packet number (the GPU writes the same
/// bindings over and over), so the set stays small. Lookups ignore entries of packets up to
/// `retired` (B ran them); Retire frees them, and A calls it only between walks, because a
/// walker may still read an overlay a lookup handed out. 64 KiB block counters keep the common
/// miss to a hash lookup. A only, no locking.
class SotcPendingRanges {
public:
    static constexpr unsigned BlockShift = 16;
    static constexpr std::uint64_t BigBytes = std::uint64_t{16} << 20;

    struct Entry {
        std::uint64_t seq;
        std::uint64_t addr;
        std::uint64_t size;
        std::vector<std::uint32_t> words; ///< an overlay: size / 4 dwords; empty for a GPU write
        bool Overlay() const {
            return !words.empty();
        }
    };

    /// Packet `seq` writes [addr, addr + size) on the GPU.
    void AddWrite(std::uint64_t seq, std::uint64_t addr, std::uint64_t size) {
        if (size == 0) {
            return;
        }
        const auto [it, inserted] = writes.try_emplace(Key{addr, size}, entries.size());
        if (!inserted) {
            entries[it->second].seq = seq;
            return;
        }
        entries.push_back({seq, addr, size, {}});
        Count(addr, size, +1);
    }

    /// Packet `seq` writes `words` at addr (a CPU copy's result).
    void AddOverlay(std::uint64_t seq, std::uint64_t addr, std::vector<std::uint32_t>&& words) {
        if (words.empty()) {
            return;
        }
        const std::uint64_t size = words.size() * 4;
        entries.push_back({seq, addr, size, std::move(words)});
        Count(addr, size, +1);
    }

    /// The newest entry of a packet after `retired` that intersects [addr, addr + size), or null.
    const Entry* Newest(std::uint64_t addr, std::uint64_t size, std::uint64_t retired) const {
        if (entries.empty() || !MaybeAny(addr, size)) {
            return nullptr;
        }
        const Entry* newest = nullptr;
        for (const Entry& e : entries) {
            if (e.seq > retired && e.addr < addr + size && addr < e.addr + e.size &&
                (!newest || e.seq > newest->seq)) {
                newest = &e;
            }
        }
        return newest;
    }

    /// Where A reads [addr, addr + size) once the pending packets have run: None (guest memory
    /// is current), Overlay (*out points at the values: the newest pending writer is an overlay
    /// holding every byte), or Wait (a GPU write or a partial overlay: wait for B).
    enum class Read { None, Overlay, Wait };
    Read Find(std::uint64_t addr, std::uint64_t size, std::uint64_t retired,
              const std::uint8_t** out) const {
        const Entry* e = Newest(addr, size, retired);
        if (!e) {
            return Read::None;
        }
        if (!e->Overlay() || addr < e->addr || addr + size > e->addr + e->size) {
            return Read::Wait;
        }
        *out = reinterpret_cast<const std::uint8_t*>(e->words.data()) + (addr - e->addr);
        return Read::Overlay;
    }

    /// Copies the overlays' bytes inside [addr, addr + size) over dst (which holds guest memory),
    /// oldest packet first. False (dst partly patched) if a pending GPU write intersects.
    bool Merge(std::uint64_t addr, std::uint8_t* dst, std::uint64_t size,
               std::uint64_t retired) const {
        if (entries.empty() || !MaybeAny(addr, size)) {
            return true;
        }
        thread_local std::vector<const Entry*> hits;
        hits.clear();
        for (const Entry& e : entries) {
            if (e.seq > retired && e.addr < addr + size && addr < e.addr + e.size) {
                if (!e.Overlay()) {
                    return false;
                }
                hits.push_back(&e);
            }
        }
        std::sort(hits.begin(), hits.end(),
                  [](const Entry* a, const Entry* b) { return a->seq < b->seq; });
        for (const Entry* e : hits) {
            const std::uint64_t lo = std::max(addr, e->addr);
            const std::uint64_t hi = std::min(addr + size, e->addr + e->size);
            std::memcpy(dst + (lo - addr),
                        reinterpret_cast<const std::uint8_t*>(e->words.data()) + (lo - e->addr),
                        hi - lo);
        }
        return true;
    }

    /// Frees the entries of packets up to `retired`.
    void Retire(std::uint64_t retired) {
        for (std::size_t i = 0; i < entries.size();) {
            Entry& e = entries[i];
            if (e.seq > retired) {
                ++i;
                continue;
            }
            Count(e.addr, e.size, -1);
            if (!e.Overlay()) {
                writes.erase(Key{e.addr, e.size});
            }
            if (i + 1 != entries.size()) {
                e = std::move(entries.back());
                if (!e.Overlay()) {
                    writes[Key{e.addr, e.size}] = i;
                }
            }
            entries.pop_back();
        }
    }

    bool Empty() const {
        return entries.empty();
    }
    std::size_t Size() const {
        return entries.size();
    }
    std::size_t Big() const {
        return std::size_t(big);
    }

private:
    struct Key {
        std::uint64_t addr, size;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return std::size_t(k.addr * 0x9E3779B97F4A7C15ULL ^ k.size);
        }
    };

    void Count(std::uint64_t addr, std::uint64_t size, int delta) {
        if (size > BigBytes) {
            big += delta; // too many blocks to count: lookups scan while one is pending
            return;
        }
        const std::uint64_t last = (addr + size - 1) >> BlockShift;
        for (std::uint64_t b = addr >> BlockShift; b <= last; ++b) {
            auto& n = blocks[b];
            n += delta;
            if (n == 0) {
                blocks.erase(b);
            }
        }
    }

    bool MaybeAny(std::uint64_t addr, std::uint64_t size) const {
        if (big != 0) {
            return true;
        }
        const std::uint64_t last = (addr + std::max<std::uint64_t>(size, 1) - 1) >> BlockShift;
        for (std::uint64_t b = addr >> BlockShift; b <= last; ++b) {
            if (blocks.contains(b)) {
                return true;
            }
        }
        return false;
    }

    std::vector<Entry> entries;
    std::unordered_map<Key, std::size_t, KeyHash> writes; ///< GPU write range -> entries index
    std::unordered_map<std::uint64_t, std::int64_t> blocks; ///< 64 KiB block -> entries in it
    std::int64_t big = 0; ///< entries over BigBytes (not in blocks)
};

} // namespace Vulkan
