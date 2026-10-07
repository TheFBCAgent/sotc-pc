// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim: video_core's texture cache only uses absl::flat_hash_map/set as plain
// hash containers (find/emplace/try_emplace/erase/contains), so std::unordered_* stand
// in for Abseil, which is not vendored.
#pragma once

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace absl {
template <class K, class V, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
using flat_hash_map = std::unordered_map<K, V, Hash, Eq>;
template <class K, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
using flat_hash_set = std::unordered_set<K, Hash, Eq>;
} // namespace absl
