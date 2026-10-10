// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 31): while a SotcAssertGuard lives on a thread, a failed ASSERT/UNREACHABLE
// on that thread throws SotcAssertFailure instead of stopping the process. Only for speculative
// work that may run on stale inputs and is thrown away on failure (compute pipelines compiled
// ahead of their dispatch, vk_pipeline_cache.cpp).
#pragma once

#include <exception>

namespace Common {

struct SotcAssertFailure : std::exception {
    const char* what() const noexcept override {
        return "assertion failed in speculative work";
    }
};

extern thread_local int g_sotc_assert_throws;

struct SotcAssertGuard {
    SotcAssertGuard() {
        ++g_sotc_assert_throws;
    }
    ~SotcAssertGuard() {
        --g_sotc_assert_throws;
    }
    SotcAssertGuard(const SotcAssertGuard&) = delete;
    SotcAssertGuard& operator=(const SotcAssertGuard&) = delete;
};

} // namespace Common
