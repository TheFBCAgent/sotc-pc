// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (whole-draw memo, W4; docs/next/draw-memo.md): the generic memo table behind the
// draw-memo layers (L1 buffers, L2 textures, L4 barriers). It generalises the session-40
// texture-set memo (SotcTexMemo in vk_rasterizer.h): a direct-mapped table whose entry holds
// key bytes, a generation vector and a payload. A hit is "key bytes equal (memcmp) and every
// generation equal", plus an optional caller validator for state that doesn't fit a fixed
// generation vector (for example W2's per-range CPU write sequence snapshot).
//
// - Single-threaded (stage B, like SotcTexMemo). No locking.
// - No heap allocation on the hit path: the table is allocated once by the constructor;
//   Store copies into an existing slot (a Payload that owns heap memory may allocate there,
//   which is the miss path).
// - Clear() is O(1) (an epoch bump).
// - Verify mode (SOTC_DRAW_MEMO_VERIFY=1): on a hit the caller also runs the full path and calls
//   Verify(hit, fresh); a mismatch is counted, the first one is kept for the log (layer name,
//   slot, key bytes in hex) and the entry is healed with the fresh payload.
//
// Vulkan-free and logging-free (callers log LogLine() themselves); unit test and benchmark:
// gpu/tests/test_draw_memo.cpp.
#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace Vulkan {

// ---------------------------------------------------------------------------------------------
// SOTC_DRAW_MEMO / SOTC_DRAW_MEMO_VERIFY

/// One bit per draw-memo layer (draw-memo.md section 2). L3 is the session-38 render-state memo
/// extension; L5..L8 are spare.
enum SotcMemoLayer : std::uint32_t {
    SotcMemoL1 = 1u << 0, ///< buffers (BindBuffers)
    SotcMemoL2 = 1u << 1, ///< textures, second pass (BindTextures)
    SotcMemoL3 = 1u << 2, ///< render state + BeginRendering
    SotcMemoL4 = 1u << 3, ///< barriers
    SotcMemoAll = 0xFFu,  ///< L1..L8
};

/// Parses SOTC_DRAW_MEMO: a comma (or space/semicolon) separated list of layers "L1,L2,L4"
/// (case-insensitive, L1..L8), "all" (or "1", like other SOTC_ switches: every layer), and
/// "0", "none", "off" or empty for none. A bare digit other than 0/1 is not a layer (write
/// "L2"). Unknown tokens are ignored; `bad` (optional) receives their count.
inline std::uint32_t SotcParseDrawMemo(const char* s, int* bad = nullptr) {
    if (bad) {
        *bad = 0;
    }
    if (!s) {
        return 0;
    }
    std::uint32_t mask = 0;
    const char* p = s;
    while (*p) {
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') {
            ++p;
        }
        const char* b = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') {
            ++p;
        }
        const std::size_t n = static_cast<std::size_t>(p - b);
        if (n == 0) {
            continue;
        }
        char tok[16] = {};
        if (n >= sizeof(tok)) {
            if (bad) {
                ++*bad;
            }
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            tok[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        }
        if (std::strcmp(tok, "all") == 0 || std::strcmp(tok, "1") == 0) {
            mask |= SotcMemoAll;
        } else if (std::strcmp(tok, "0") == 0 || std::strcmp(tok, "none") == 0 ||
                   std::strcmp(tok, "off") == 0) {
            // nothing: "0" alone means none; mixed with layers it adds nothing
        } else if (n == 2 && tok[0] == 'l' && tok[1] >= '1' && tok[1] <= '8') {
            mask |= 1u << (tok[1] - '1');
        } else if (bad) {
            ++*bad;
        }
    }
    return mask;
}

/// "verify" switch: "1" (or any value not starting with '0') is on; unset/empty/"0" is off.
inline bool SotcParseFlag(const char* s) {
    return s && *s && *s != '0';
}

struct SotcDrawMemoConfig {
    std::uint32_t layers = 0; ///< SotcMemoLayer bits
    bool verify = false;      ///< SOTC_DRAW_MEMO_VERIFY=1
    int bad_tokens = 0;       ///< ignored tokens in SOTC_DRAW_MEMO

    [[nodiscard]] bool On(std::uint32_t layer) const {
        return (layers & layer) != 0;
    }
    /// For the caller's one-time LOG_INFO: "draw memo: L1,L2 (verify)" or "draw memo: off".
    [[nodiscard]] std::string Describe() const {
        std::string s = "draw memo: ";
        if (!layers) {
            s += "off";
        }
        for (int i = 0; i < 8; ++i) {
            if (layers & (1u << i)) {
                if (s.back() != ' ') {
                    s += ',';
                }
                s += 'L';
                s += static_cast<char>('1' + i);
            }
        }
        if (verify) {
            s += " (verify, SOTC_DRAW_MEMO_VERIFY=1)";
        }
        if (bad_tokens) {
            s += " (" + std::to_string(bad_tokens) + " unknown token(s) in SOTC_DRAW_MEMO)";
        }
        return s;
    }
};

inline SotcDrawMemoConfig SotcParseDrawMemoConfig(const char* layers, const char* verify) {
    SotcDrawMemoConfig c;
    c.layers = SotcParseDrawMemo(layers, &c.bad_tokens);
    c.verify = SotcParseFlag(verify);
    return c;
}

/// The environment, read once (thread-safe static init; an inline function's static is one
/// object across translation units). Off by default.
inline const SotcDrawMemoConfig& SotcDrawMemoEnv() {
    static const SotcDrawMemoConfig cfg = SotcParseDrawMemoConfig(
        std::getenv("SOTC_DRAW_MEMO"), std::getenv("SOTC_DRAW_MEMO_VERIFY"));
    return cfg;
}

// ---------------------------------------------------------------------------------------------
// The table

/// Cheap 64-bit hash of key bytes for SlotOfKey. Each 8-byte word is mixed with its position
/// and multiplied independently (no dependency chain; a serial multiply-xorshift chain was
/// ~59 ns for 256 bytes), the products are summed and the sum is finalised (splitmix64).
inline std::uint64_t SotcMemoHashBytes(const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t a = len * 0x9E3779B97F4A7C15ull, b = 0;
    std::size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        std::uint64_t w0, w1;
        std::memcpy(&w0, p + i, 8);
        std::memcpy(&w1, p + i + 8, 8);
        // 64x64->128 folded multiply of the two words, each keyed by its position.
        const unsigned __int128 m =
            static_cast<unsigned __int128>(w0 ^ (0xA0761D6478BD642Full + i)) *
            (w1 ^ (0xE7037ED1A0B428DBull - i));
        a += static_cast<std::uint64_t>(m);
        b ^= static_cast<std::uint64_t>(m >> 64);
    }
    if (i < len) {
        std::uint64_t w0 = 0, w1 = 0;
        const std::size_t r = len - i;
        std::memcpy(&w0, p + i, r < 8 ? r : 8);
        if (r > 8) {
            std::memcpy(&w1, p + i + 8, r - 8);
        }
        const unsigned __int128 m =
            static_cast<unsigned __int128>(w0 ^ (0xA0761D6478BD642Full + i)) *
            (w1 ^ (0xE7037ED1A0B428DBull - i));
        a += static_cast<std::uint64_t>(m);
        b ^= static_cast<std::uint64_t>(m >> 64);
    }
    std::uint64_t z = a ^ (b * 0xBF58476D1CE4E5B9ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// Hit/miss counters. misses = cold + evicted + stale + rejected.
struct SotcMemoStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t miss_cold = 0;     ///< slot empty (never stored, or cleared)
    std::uint64_t miss_key = 0;      ///< slot holds another key (a conflict or a changed key)
    std::uint64_t miss_stale = 0;    ///< same key, a generation changed
    std::uint64_t miss_rejected = 0; ///< same key and generations, the validator said no
    std::uint64_t stores = 0;
    std::uint64_t evicts = 0;        ///< a Store replaced a live entry with another key
    std::uint64_t verified = 0;      ///< Verify calls
    std::uint64_t mismatches = 0;    ///< Verify calls whose payloads differed
};

/// Key: trivially copyable, compared as bytes (memcmp) over its first `key_len` bytes
///   (default sizeof(Key)); a caller with a variable-length key (N V#s of a stage) passes the
///   used length, and the bytes past it are never read. Padding inside the compared prefix must
///   be zeroed by the caller (value-initialise the key).
/// Payload: default-constructible and copy-assignable.
/// N: the generation count (std::array<u64, N>, compared exactly; N may be 0).
/// Bits: log2 of the slot count.
template <class Key, class Payload, std::size_t N, unsigned Bits = 10>
class SotcDrawMemo {
    static_assert(std::is_trivially_copyable_v<Key>, "memo keys are compared as bytes");
    static_assert(Bits >= 1 && Bits <= 20);

public:
    using Gens = std::array<std::uint64_t, N>;
    using SameFn = bool (*)(const Payload&, const Payload&);
    static constexpr std::uint32_t Slots = 1u << Bits;
    static constexpr std::uint32_t KeySize = static_cast<std::uint32_t>(sizeof(Key));

    /// `name`: the layer's name in logs ("buffer" -> "buffer memo: H hits, M misses").
    /// `same`: the verify comparison (required for Verify; may be null otherwise).
    explicit SotcDrawMemo(const char* name, SameFn same = nullptr)
        : name_(name), same_(same), table_(std::make_unique<Entry[]>(Slots)) {}

    SotcDrawMemo(const SotcDrawMemo&) = delete;
    SotcDrawMemo& operator=(const SotcDrawMemo&) = delete;

    /// Slot from a stable pointer (a stage's Shader::Info*), the SotcTexMemo hash.
    static std::uint32_t SlotOf(const void* p) {
        return static_cast<std::uint32_t>(
            (reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - Bits));
    }
    /// Slot from the key bytes (when there is no stable owner pointer).
    static std::uint32_t SlotOfKey(const Key& key, std::uint32_t key_len = KeySize) {
        return static_cast<std::uint32_t>(SotcMemoHashBytes(&key, key_len) >> (64 - Bits));
    }

    /// The payload stored for (key, gens) in `slot`, or nullptr. `valid(const Payload&)` is
    /// asked last, only when key and generations match (e.g. W2's write-sequence snapshot kept
    /// in the payload is still current); it must not touch the table.
    template <class Valid>
    const Payload* Lookup(std::uint32_t slot, const Key& key, const Gens& gens,
                          std::uint32_t key_len, Valid&& valid) {
        Entry& e = table_[slot & (Slots - 1)];
        if (e.epoch != epoch_) {
            ++stats_.misses;
            ++stats_.miss_cold;
            return nullptr;
        }
        if (e.key_len != key_len || std::memcmp(&e.key, &key, key_len) != 0) {
            ++stats_.misses;
            ++stats_.miss_key;
            return nullptr;
        }
        if constexpr (N > 0) {
            if (std::memcmp(e.gens.data(), gens.data(), sizeof(Gens)) != 0) {
                ++stats_.misses;
                ++stats_.miss_stale;
                return nullptr;
            }
        }
        if (!valid(static_cast<const Payload&>(e.payload))) {
            ++stats_.misses;
            ++stats_.miss_rejected;
            return nullptr;
        }
        ++stats_.hits;
        return &e.payload;
    }
    const Payload* Lookup(std::uint32_t slot, const Key& key, const Gens& gens,
                          std::uint32_t key_len = KeySize) {
        return Lookup(slot, key, gens, key_len, [](const Payload&) { return true; });
    }

    /// Stores (key, gens, payload) in `slot`, replacing what was there. Returns the stored
    /// payload (a caller can fill it in place to avoid a copy: Store(...) then edit).
    Payload& Store(std::uint32_t slot, const Key& key, const Gens& gens, const Payload& payload,
                   std::uint32_t key_len = KeySize) {
        Entry& e = Claim(slot, key, gens, key_len);
        e.payload = payload;
        return e.payload;
    }
    Payload& Store(std::uint32_t slot, const Key& key, const Gens& gens, Payload&& payload,
                   std::uint32_t key_len = KeySize) {
        Entry& e = Claim(slot, key, gens, key_len);
        e.payload = std::move(payload);
        return e.payload;
    }

    /// Drops one slot (e.g. the caller learned its payload went bad).
    void Invalidate(std::uint32_t slot) {
        table_[slot & (Slots - 1)].epoch = 0;
    }
    /// Drops every entry, O(1).
    void Clear() {
        if (++epoch_ == 0) { // 2^64 clears: never in practice, but stay correct
            for (std::uint32_t i = 0; i < Slots; ++i) {
                table_[i].epoch = 0;
            }
            epoch_ = 1;
        }
    }

    /// Verify mode: `hit` is the pointer Lookup returned, `fresh` what the full path produced.
    /// Returns true when they are the same. A mismatch is counted, the first one is described
    /// for FirstMismatch(), and the entry is healed with `fresh`. `describe` (optional) adds a
    /// payload description to the first mismatch record.
    bool Verify(const Payload* hit, const Payload& fresh,
                std::string (*describe)(const Payload& hit, const Payload& fresh) = nullptr) {
        ++stats_.verified;
        if (!hit || !same_ || same_(*hit, fresh)) {
            return true;
        }
        ++stats_.mismatches;
        Entry* e = EntryOf(hit);
        if (first_mismatch_.empty()) {
            char head[96];
            std::snprintf(head, sizeof(head), "%s memo: verify mismatch #1 (after %llu hits)",
                          name_, static_cast<unsigned long long>(total_hits_ + stats_.hits));
            first_mismatch_ = head;
            if (e) {
                first_mismatch_ += " slot " + std::to_string(e - table_.get());
                first_mismatch_ += " key " + Hex(&e->key, e->key_len);
                if constexpr (N > 0) {
                    first_mismatch_ += " gens";
                    for (std::uint64_t g : e->gens) {
                        char b[24];
                        std::snprintf(b, sizeof(b), " %llx", static_cast<unsigned long long>(g));
                        first_mismatch_ += b;
                    }
                }
            }
            if (describe) {
                first_mismatch_ += ": " + describe(*hit, fresh);
            }
        }
        if (e) {
            e->payload = fresh;
        }
        return false;
    }

    [[nodiscard]] const SotcMemoStats& Stats() const {
        return stats_;
    }
    [[nodiscard]] const char* Name() const {
        return name_;
    }
    /// Empty until a verify mismatch happened (kept across ResetStats).
    [[nodiscard]] const std::string& FirstMismatch() const {
        return first_mismatch_;
    }

    /// True every 2^20 lookups (SotcTexMemo's cadence): the caller then logs LogLine() and
    /// calls ResetStats().
    [[nodiscard]] bool LogDue(std::uint64_t mask = 0xFFFFF) const {
        return ((stats_.hits + stats_.misses) & mask) == mask;
    }

    /// "<name> memo: H hits, M misses" (the texture-set memo's format), then the miss split,
    /// evictions and, when Verify ran, "V verified, X mismatches".
    [[nodiscard]] std::string LogLine() const {
        char buf[320];
        int n = std::snprintf(
            buf, sizeof(buf),
            "%s memo: %llu hits, %llu misses (cold %llu, key %llu, stale %llu, rejected %llu; "
            "%llu evicts)",
            name_, U(stats_.hits), U(stats_.misses), U(stats_.miss_cold), U(stats_.miss_key),
            U(stats_.miss_stale), U(stats_.miss_rejected), U(stats_.evicts));
        if (stats_.verified && n > 0 && n < static_cast<int>(sizeof(buf))) {
            std::snprintf(buf + n, sizeof(buf) - n, ", %llu verified, %llu mismatches",
                          U(stats_.verified), U(stats_.mismatches));
        }
        return buf;
    }
    void ResetStats() {
        total_hits_ += stats_.hits;
        stats_ = {};
    }

    /// Lower-case hex of `len` bytes, 8-byte groups separated by '.' (for logs).
    static std::string Hex(const void* data, std::size_t len) {
        static constexpr char digits[] = "0123456789abcdef";
        const auto* p = static_cast<const unsigned char*>(data);
        std::string s;
        s.reserve(len * 2 + len / 8);
        for (std::size_t i = 0; i < len; ++i) {
            if (i && (i & 7) == 0) {
                s += '.';
            }
            s += digits[p[i] >> 4];
            s += digits[p[i] & 15];
        }
        return s;
    }

private:
    struct Entry {
        std::uint64_t epoch = 0; ///< live when equal to the table's epoch_
        std::uint32_t key_len = 0;
        Gens gens{};
        Key key{};
        Payload payload{};
    };

    static unsigned long long U(std::uint64_t v) {
        return static_cast<unsigned long long>(v);
    }

    Entry& Claim(std::uint32_t slot, const Key& key, const Gens& gens, std::uint32_t key_len) {
        Entry& e = table_[slot & (Slots - 1)];
        key_len = key_len < KeySize ? key_len : KeySize; // Lookup then never reads past Key
        ++stats_.stores;
        if (e.epoch == epoch_ &&
            (e.key_len != key_len || std::memcmp(&e.key, &key, key_len) != 0)) {
            ++stats_.evicts;
        }
        e.epoch = epoch_;
        e.key_len = key_len;
        std::memcpy(&e.key, &key, key_len);
        e.gens = gens;
        return e;
    }

    Entry* EntryOf(const Payload* p) {
        const auto* base = reinterpret_cast<const unsigned char*>(table_.get());
        const auto* q = reinterpret_cast<const unsigned char*>(p);
        if (q < base || q >= base + sizeof(Entry) * Slots) {
            return nullptr;
        }
        Entry* e = &table_[static_cast<std::size_t>(q - base) / sizeof(Entry)];
        return &e->payload == p ? e : nullptr;
    }

    const char* name_;
    SameFn same_;
    std::unique_ptr<Entry[]> table_;
    std::uint64_t epoch_ = 1;
    SotcMemoStats stats_{};
    std::uint64_t total_hits_ = 0;
    std::string first_mismatch_;
};

} // namespace Vulkan
