# SPDX-License-Identifier: GPL-2.0-or-later
# sotcgpu: the GPU behind gpu/include/sotcgpu.h.
# - SOTC_VULKAN=OFF: the null implementation (gpu/null), so the runtime keeps
#   its null PM4 executor.
# - SOTC_VULKAN=ON: shadPS4's video_core (workstream GL fills this in).

if(SOTC_VULKAN)
    message(FATAL_ERROR "SOTC_VULKAN=ON: the Vulkan sotcgpu is not built yet (workstream GL).")
endif()

add_library(sotcgpu STATIC null/sotcgpu_null.c)
target_include_directories(sotcgpu PUBLIC include)
