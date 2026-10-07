// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's emulator.h. common/assert.cpp calls
// Emulator::Shutdown() before trapping; Common::ElfInfo befriends Core::Emulator,
// which is how sotcgpu_init fills the game's SDK version.
#pragma once

#include "common/types.h"

namespace Core {

class Emulator {
public:
    /// Flushes the log before an assertion trap.
    void Shutdown();
    /// Fills Common::ElfInfo (SDK version, serial) for video_core.
    static void FillElfInfo();
};

} // namespace Core
