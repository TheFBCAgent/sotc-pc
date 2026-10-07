// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc shim for shadPS4's core/memory.h (+ the bits of core/address_space.h that
// video_core uses). Every call forwards to the runtime through sotcgpu_host; the
// implementations are in gpu/src/shim_core.cpp.
#pragma once

#include <boost/icl/interval_set.hpp>
#include "common/enum.h"
#include "common/singleton.h"
#include "common/types.h"

namespace Vulkan {
class Rasterizer;
}

namespace Core {

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/// Page protection for PageManager (host->protect).
class AddressSpace {
public:
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);
    /// Only used by the Linux userfaultfd tracker, which shadow-pc never selects.
    boost::icl::interval_set<VAddr> GetUsableRegions() {
        return {};
    }
};

class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }
    Vulkan::Rasterizer* GetRasterizer() const {
        return rasterizer;
    }
    AddressSpace& GetAddressSpace() {
        return impl;
    }

    bool IsValidGpuMapping(VAddr virtual_addr, u64 size);
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};

using Memory = Common::Singleton<MemoryManager>;

} // namespace Core
