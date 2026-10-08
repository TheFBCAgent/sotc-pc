# SPDX-License-Identifier: GPL-2.0-or-later
# Build glue for the submodules under third_party/. Workstream GL owns this
# file from M3 step 0 on (it adds the Vulkan, recompiler and fmt targets).

# SDL3: the window and input (workstream GW). Linked statically so the runtime
# stays one executable. Settings follow shadPS4's externals/CMakeLists.txt.
if(NOT TARGET SDL3::SDL3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
    set(SDL_AVX512F OFF CACHE BOOL "" FORCE)
    add_subdirectory(sdl3 EXCLUDE_FROM_ALL)
endif()

# Tracy (session 18, SOTC_TRACY=ON): the client, with shadPS4's settings (externals/CMakeLists.txt,
# commit 14ada48); on demand, so it only records while a viewer is connected. No crash handler:
# the runtime and the GPU page tracker own access violations.
if(SOTC_TRACY AND NOT TARGET Tracy::TracyClient)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
    set(TRACY_FIBERS OFF CACHE BOOL "" FORCE)
    set(TRACY_NO_SYSTEM_TRACING ON CACHE BOOL "" FORCE)
    set(TRACY_NO_CALLSTACK ON CACHE BOOL "" FORCE)
    set(TRACY_NO_CODE_TRANSFER ON CACHE BOOL "" FORCE)
    set(TRACY_NO_SAMPLING ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    set(TRACY_NO_CONTEXT_SWITCH ON CACHE BOOL "" FORCE)
    set(TRACY_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory(tracy EXCLUDE_FROM_ALL SYSTEM)
endif()

if(NOT SOTC_VULKAN)
    return()
endif()

# ---------------------------------------------------------------------------
# SOTC_VULKAN=ON: everything shadPS4's common, shader_recompiler and
# video_core need. Settings follow shadPS4's externals/CMakeLists.txt
# (commit 14ada48). Nothing here is installed; all of it is static.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS)
endif()

# Boost (header subset)
if(NOT TARGET Boost::headers)
    add_subdirectory(ext-boost EXCLUDE_FROM_ALL SYSTEM)
endif()

# fmt
if(NOT TARGET fmt::fmt)
    add_subdirectory(fmt EXCLUDE_FROM_ALL SYSTEM)
endif()

# Vulkan headers (the loader is opened at run time; nothing links vulkan-1)
if(NOT TARGET Vulkan::Headers)
    set(VULKAN_HEADERS_ENABLE_MODULE OFF)
    add_subdirectory(vulkan-headers EXCLUDE_FROM_ALL SYSTEM)
endif()

# VMA
if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
    add_subdirectory(vma EXCLUDE_FROM_ALL SYSTEM)
endif()

# glslang: only the standalone compiler, for video_core's host shaders.
if(NOT TARGET glslang-standalone)
    set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(glslang EXCLUDE_FROM_ALL SYSTEM)
endif()

# sirit (SPIR-V emitter used by the recompiler)
if(NOT TARGET sirit)
    add_subdirectory(sirit EXCLUDE_FROM_ALL SYSTEM)
    if(WIN32)
        target_compile_options(sirit PRIVATE
            $<$<CXX_COMPILER_ID:Clang>:-Wno-error=unused-command-line-argument>)
    endif()
endif()

# robin-map
if(NOT TARGET tsl::robin_map)
    add_subdirectory(robin-map EXCLUDE_FROM_ALL SYSTEM)
endif()

# xxHash
if(NOT TARGET xxHash::xxhash)
    add_library(xxhash STATIC xxhash/xxhash.h xxhash/xxhash.c)
    target_include_directories(xxhash SYSTEM PUBLIC xxhash)
    add_library(xxHash::xxhash ALIAS xxhash)
endif()

# magic_enum
if(NOT TARGET magic_enum::magic_enum)
    add_subdirectory(magic_enum EXCLUDE_FROM_ALL SYSTEM)
endif()

# half
if(NOT TARGET half::half)
    add_library(half INTERFACE)
    target_include_directories(half SYSTEM INTERFACE half/include)
    add_library(half::half ALIAS half)
endif()

# xbyak (the recompiler JITs the SRT walker)
if(NOT TARGET xbyak::xbyak)
    add_subdirectory(xbyak EXCLUDE_FROM_ALL SYSTEM)
endif()

# Zydis (common/decoder: the SRT walker's fault fix-up)
if(NOT TARGET Zydis)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
    add_subdirectory(zydis EXCLUDE_FROM_ALL SYSTEM)
endif()

# miniz (video_core's pipeline cache archive)
if(NOT TARGET miniz)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
    add_subdirectory(miniz EXCLUDE_FROM_ALL SYSTEM)
endif()

# stb (stb_image_write for screenshots) and renderdoc_app.h: single headers
# copied from shadPS4's externals/ (public domain / MIT).
add_library(sotc_stb INTERFACE)
target_include_directories(sotc_stb SYSTEM INTERFACE stb)
add_library(sotc_renderdoc INTERFACE)
target_include_directories(sotc_renderdoc SYSTEM INTERFACE renderdoc)
