// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/libraries/videoout/driver.h. VideoOut itself lives
// in the runtime (runtime/src/hle/videoout.c); Liverpool only asks the port whether a
// WAIT_REG_MEM targets a flip label. The runtime's labels are in guest memory, not
// here, so IsVoLabel is always false and such waits take the generic polling path.
#pragma once

#include <array>
#include <condition_variable>
#include <mutex>
#include "common/types.h"
#include "core/libraries/videoout/buffer.h"

namespace Libraries::VideoOut {

struct VideoOutPort {
    std::array<VideoOutBuffer, MaxDisplayBuffers> buffer_slots;
    std::array<u64, MaxDisplayBuffers> buffer_labels{};
    std::array<BufferAttributeGroup, MaxDisplayBufferGroups> groups{};
    std::mutex vo_mutex;
    std::condition_variable vo_cv;

    bool IsVoLabel(const u64* address) const {
        const u64* start = &buffer_labels[0];
        const u64* end = &buffer_labels[MaxDisplayBuffers - 1];
        return address >= start && address <= end;
    }

    void WaitVoLabel(auto&& pred) {
        std::unique_lock lk{vo_mutex};
        vo_cv.wait(lk, pred);
    }

    void SignalVoLabel() {
        std::scoped_lock lk{vo_mutex};
        vo_cv.notify_one();
    }
};

} // namespace Libraries::VideoOut
