// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 62): the command ring of draw-prep shape D (docs/next/draw-prep.md,
// sections 4-9). Stage B appends resolved Vulkan commands (handles, offsets, values) and stage C
// (step 3; in step 2's sync mode B itself, right after each record) reads them in order and
// records them. Vulkan-free, so it has a unit test (gpu/tests/test_cmd_ring.cpp).
// - Records are written in place: Reserve hands out room for a header and payload, the
//   producer fills it, and Publish makes everything reserved so far visible (once per record,
//   not per command: ~41,000 commands per flip, session 61).
// - Each record is an 8-byte header (kind, total bytes including the header, 8-aligned) and its
//   payload. A header of kind WrapKind means the rest of the buffer is unused: continue at 0.
// - Single producer, single consumer. Reserve returns nullptr when the ring is full; the
//   producer decides what to do (sync mode decodes; the thread of step 3 will wait).
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace Vulkan {

class SotcCmdRing {
public:
    static constexpr std::uint32_t WrapKind = ~std::uint32_t{0};

    struct Header {
        std::uint32_t kind;
        std::uint32_t bytes; // header + payload, 8-aligned
    };
    static_assert(sizeof(Header) == 8);

    static constexpr std::size_t Align(std::size_t n) {
        return (n + 7) & ~std::size_t{7};
    }

    explicit SotcCmdRing(std::size_t capacity_pow2)
        : capacity{std::bit_ceil(std::max<std::size_t>(capacity_pow2, 64))},
          buffer{std::make_unique<std::uint64_t[]>(capacity / 8)} {}

    std::size_t Capacity() const {
        return capacity;
    }
    /// The largest payload Reserve can ever accept.
    std::size_t MaxPayload() const {
        return capacity / 2 - sizeof(Header);
    }

    /// Producer: room for a record of `kind` with `payload` bytes, header written, or nullptr
    /// when the ring has no room now (the payload is left for the caller to fill).
    void* Reserve(std::uint32_t kind, std::size_t payload) {
        const std::size_t need = sizeof(Header) + Align(payload);
        std::size_t off = phead & (capacity - 1);
        const std::size_t to_end = capacity - off;
        const std::size_t waste = to_end < need ? to_end : 0;
        if (phead + waste + need - tail.load(std::memory_order_acquire) > capacity ||
            need > capacity / 2) {
            return nullptr;
        }
        std::uint8_t* base = reinterpret_cast<std::uint8_t*>(buffer.get());
        if (waste != 0) {
            const Header wrap{WrapKind, static_cast<std::uint32_t>(waste)};
            std::memcpy(base + off, &wrap, sizeof(wrap));
            phead += waste;
            off = 0;
        }
        const Header h{kind, static_cast<std::uint32_t>(need)};
        std::memcpy(base + off, &h, sizeof(h));
        phead += need;
        ++precords;
        return base + off + sizeof(Header);
    }

    /// Producer: makes every reserved record visible to the consumer.
    void Publish() {
        head.store(phead, std::memory_order_release);
    }

    /// Producer: records reserved so far (published or not).
    std::uint64_t Reserved() const {
        return precords;
    }
    /// Producer: bytes reserved so far, wrap padding included.
    std::uint64_t ReservedBytes() const {
        return phead;
    }

    struct Record {
        std::uint32_t kind;
        const void* payload;
        std::size_t payload_bytes; // 8-aligned
    };

    /// Consumer: the oldest published record; kind WrapKind when none is left.
    Record Front() {
        const std::uint64_t h = head.load(std::memory_order_acquire);
        if (h == ctail) {
            return {WrapKind, nullptr, 0};
        }
        const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(buffer.get());
        std::size_t off = ctail & (capacity - 1);
        Header hdr;
        std::memcpy(&hdr, base + off, sizeof(hdr));
        if (hdr.kind == WrapKind) {
            ctail += hdr.bytes;
            off = 0;
            std::memcpy(&hdr, base, sizeof(hdr));
        }
        front_bytes = hdr.bytes;
        return {hdr.kind, base + off + sizeof(Header), hdr.bytes - sizeof(Header)};
    }

    /// Consumer: releases the record Front returned (its payload is dead after the next
    /// Release).
    void Pop() {
        ctail += front_bytes;
        front_bytes = 0;
    }

    /// Consumer: gives the popped space back to the producer.
    void Release() {
        tail.store(ctail, std::memory_order_release);
    }

    /// Consumer: nothing published is left to read.
    bool Empty() const {
        return head.load(std::memory_order_acquire) == ctail;
    }

private:
    const std::size_t capacity;
    std::unique_ptr<std::uint64_t[]> buffer; // 8-aligned storage
    // Producer side.
    std::uint64_t phead = 0;
    std::uint64_t precords = 0;
    alignas(64) std::atomic<std::uint64_t> head{0};
    // Consumer side.
    alignas(64) std::atomic<std::uint64_t> tail{0};
    std::uint64_t ctail = 0;
    std::uint64_t front_bytes = 0;
};

} // namespace Vulkan
