// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/libraries/system/systemservice.h: the presenter's
// splash check. The runtime's SystemService owns the splash; sotcgpu never draws it.
#pragma once

namespace Libraries::SystemService {

inline bool IsSplashVisible() {
    return false;
}

} // namespace Libraries::SystemService
