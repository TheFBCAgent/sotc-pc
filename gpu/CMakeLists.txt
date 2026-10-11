# SPDX-License-Identifier: GPL-2.0-or-later
# sotcgpu: the GPU behind gpu/include/sotcgpu.h.
# - SOTC_VULKAN=OFF: the null implementation (gpu/null), so the runtime keeps
#   its null PM4 executor.
# - SOTC_VULKAN=ON: shadPS4's common + shader_recompiler + video_core (vendored in
#   third_party/shadps4), with gpu/shim/ standing in for shadPS4's core/, and
#   gpu/src/sotcgpu.cpp implementing the C contract. Workstream GL.

if(NOT SOTC_VULKAN)
    add_library(sotcgpu STATIC null/sotcgpu_null.c)
    target_include_directories(sotcgpu PUBLIC include)
    return()
endif()

set(SHADPS4_SRC ${PROJECT_SOURCE_DIR}/third_party/shadps4/src)

# --- Host shaders (glslang-standalone -> SPIR-V headers), shadPS4's own CMake file.
add_subdirectory(${SHADPS4_SRC}/video_core/host_shaders ${CMAKE_CURRENT_BINARY_DIR}/host_shaders)

# --- Settings shared by every sotcgpu object library.
add_library(sotcgpu_options INTERFACE)
target_compile_features(sotcgpu_options INTERFACE cxx_std_23)
target_include_directories(sotcgpu_options INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim      # first: replaces shadPS4's core/, imgui
    # Tracy: stubs unless SOTC_TRACY (then Tracy::TracyClient's public/ provides tracy/*.hpp)
    $<$<NOT:$<BOOL:${SOTC_TRACY}>>:${CMAKE_CURRENT_SOURCE_DIR}/shim/tracy_off>
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SHADPS4_SRC}
    ${SHADPS4_SRC}/..                     # a few files include "src/common/..."
    ${HOST_SHADERS_INCLUDE})
target_compile_definitions(sotcgpu_options INTERFACE
    SOTC_NO_IMGUI
    BOOST_ASIO_STANDALONE
    $<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE _SCL_SECURE_NO_WARNINGS _TIMESPEC_DEFINED
        NTDDI_VERSION=0x0A000006 _WIN32_WINNT=0x0A00 WINVER=0x0A00>)
# shadPS4 builds with -march=x86-64-v3 (AVX2, BMI2, F16C); the Ryzen 5 3600 has them.
target_compile_options(sotcgpu_options INTERFACE
    -march=x86-64-v3
    $<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>:/EHsc /bigobj /utf-8>
    -Wno-invalid-offsetof -Wno-unused-parameter -Wno-unused-variable
    -Wno-unused-private-field -Wno-missing-field-initializers -Wno-deprecated-declarations
    -Wno-unknown-pragmas -Wno-sign-compare -Wno-microsoft-cast -Wno-switch
    -Wno-unused-function -Wno-nonportable-include-path -Wno-language-extension-token
    -Wno-c++20-designator -Wno-reorder-ctor)
target_link_libraries(sotcgpu_options INTERFACE
    Boost::headers fmt::fmt Vulkan::Headers GPUOpen::VulkanMemoryAllocator sirit
    tsl::robin_map xxHash::xxhash magic_enum::magic_enum half::half xbyak::xbyak Zydis
    miniz sotc_stb sotc_renderdoc SDL3::SDL3
    $<$<BOOL:${SOTC_TRACY}>:Tracy::TracyClient>)

# --- 1. common (a subset: no spdlog logging, no path_util/user dirs, no patches/keys/discord).
set(SOTCGPU_COMMON
    ${SHADPS4_SRC}/common/logging/classes.cpp
    ${SHADPS4_SRC}/common/assert.cpp
    ${SHADPS4_SRC}/common/decoder.cpp
    ${SHADPS4_SRC}/common/error.cpp
    ${SHADPS4_SRC}/common/io_file.cpp
    ${SHADPS4_SRC}/common/native_clock.cpp
    ${SHADPS4_SRC}/common/ntapi.cpp
    ${SHADPS4_SRC}/common/number_utils.cpp
    ${SHADPS4_SRC}/common/rdtsc.cpp
    ${SHADPS4_SRC}/common/recursive_lock.cpp
    ${SHADPS4_SRC}/common/signal_context.cpp
    ${SHADPS4_SRC}/common/slot_vector.cpp
    ${SHADPS4_SRC}/common/spin_lock.cpp
    ${SHADPS4_SRC}/common/stb.cpp
    ${SHADPS4_SRC}/common/string_util.cpp
    ${SHADPS4_SRC}/common/thread.cpp
    src/log.cpp          # replaces common/logging/log.cpp
    src/path_util.cpp)   # replaces common/path_util.cpp
add_library(sotcgpu_common OBJECT ${SOTCGPU_COMMON})
target_link_libraries(sotcgpu_common PRIVATE sotcgpu_options)

# --- 2. shader_recompiler (every .cpp, as shadPS4's SHADER_RECOMPILER list).
file(GLOB_RECURSE SOTCGPU_RECOMPILER CONFIGURE_DEPENDS ${SHADPS4_SRC}/shader_recompiler/*.cpp)
add_library(sotcgpu_recompiler OBJECT ${SOTCGPU_RECOMPILER})
target_link_libraries(sotcgpu_recompiler PRIVATE sotcgpu_options)

# --- 3. video_core (every .cpp, as shadPS4's VIDEO_CORE list).
file(GLOB_RECURSE SOTCGPU_VIDEO_CORE CONFIGURE_DEPENDS ${SHADPS4_SRC}/video_core/*.cpp)
add_library(sotcgpu_video_core OBJECT ${SOTCGPU_VIDEO_CORE})
target_link_libraries(sotcgpu_video_core PRIVATE sotcgpu_options)
add_dependencies(sotcgpu_video_core host_shaders)

# --- 4. sotcgpu: the C contract + the shim implementations, linked with everything.
add_library(sotcgpu STATIC
    src/sotcgpu.cpp
    src/shim_core.cpp
    src/debug_dump.cpp   # workstream X debug tools (sotc_debug.h)
    src/gpu_timer.cpp    # session 12: gpu busy from timestamps (sotc_gpu_timer.h)
    $<TARGET_OBJECTS:sotcgpu_common>
    $<TARGET_OBJECTS:sotcgpu_recompiler>
    $<TARGET_OBJECTS:sotcgpu_video_core>)
target_link_libraries(sotcgpu PRIVATE sotcgpu_options)
target_include_directories(sotcgpu PUBLIC include)
# Static dependencies the objects above reference.
target_link_libraries(sotcgpu PUBLIC fmt::fmt sirit xxHash::xxhash Zydis miniz SDL3::SDL3)
if(WIN32)
    target_link_libraries(sotcgpu PUBLIC ntdll mincore) # mincore: CreateFileMapping2 (common/io_file)
endif()

# --- Tests (SOTC_VULKAN=ON only).
enable_testing()
add_executable(test_recompiler tests/test_recompiler.cpp)
target_link_libraries(test_recompiler PRIVATE sotcgpu sotcgpu_options)
add_test(NAME test_recompiler COMMAND test_recompiler)

# Workstream R: the residency suballocator's core, with fake device memory (no Vulkan device).
add_executable(test_residency tests/test_residency.cpp
    ${SHADPS4_SRC}/video_core/buffer_cache/residency_allocator.cpp)
target_link_libraries(test_residency PRIVATE sotcgpu_options)
add_test(NAME test_residency COMMAND test_residency)

# Workstream T: the texture cache's image GC policy (header-only, no Vulkan device).
add_executable(test_gc_policy tests/test_gc_policy.cpp)
target_link_libraries(test_gc_policy PRIVATE sotcgpu_options)
add_test(NAME test_gc_policy COMMAND test_gc_policy)

# Session 14: the image-free worker (header-only, no Vulkan).
add_executable(test_async_free tests/test_async_free.cpp)
target_link_libraries(test_async_free PRIVATE sotcgpu_options)
add_test(NAME test_async_free COMMAND test_async_free)

# Crash C (session 6): hashing guest memory through a reader, as MarkAsMaybeDirty does (no Vulkan).
add_executable(test_guest_hash tests/test_guest_hash.cpp)
target_link_libraries(test_guest_hash PRIVATE sotcgpu_options xxHash::xxhash)
add_test(NAME test_guest_hash COMMAND test_guest_hash)

# Session 10: the SRT walker load emulation (header-only, Zydis, no Vulkan).
add_executable(test_srt_load tests/test_srt_load.cpp)
target_link_libraries(test_srt_load PRIVATE sotcgpu_options Zydis)
add_test(NAME test_srt_load COMMAND test_srt_load)

# Session 10: the per-64 KiB GPU write ticks (header-only, no Vulkan).
add_executable(test_write_ticks tests/test_write_ticks.cpp)
target_link_libraries(test_write_ticks PRIVATE sotcgpu_options)
add_test(NAME test_write_ticks COMMAND test_write_ticks)

# Session 33: the hot lines behind the hot flush, aged by flip (header-only, no Vulkan).
add_executable(test_hot_lines tests/test_hot_lines.cpp)
target_link_libraries(test_hot_lines PRIVATE sotcgpu_options)
add_test(NAME test_hot_lines COMMAND test_hot_lines)

# Session 21: SOTC_CPU_COPY's plan and copy for the copy kernel (header-only, no Vulkan).
add_executable(test_cpu_copy tests/test_cpu_copy.cpp)
target_link_libraries(test_cpu_copy PRIVATE sotcgpu_options)
add_test(NAME test_cpu_copy COMMAND test_cpu_copy)

# Session 22: the pages SOTC_CPU_COPY may write (header-only, no Vulkan).
add_executable(test_resident_pages tests/test_resident_pages.cpp)
target_link_libraries(test_resident_pages PRIVATE sotcgpu_options)
add_test(NAME test_resident_pages COMMAND test_resident_pages)

# Session 23: the staging ring policy, on a model of the ring (header-only, no Vulkan).
add_executable(test_staging_ring tests/test_staging_ring.cpp)
target_link_libraries(test_staging_ring PRIVATE sotcgpu_options)
add_test(NAME test_staging_ring COMMAND test_staging_ring)

# Session 31: the PM4 dispatch scan for compute pipelines ahead (header-only, no Vulkan).
add_executable(test_dispatch_scan tests/test_dispatch_scan.cpp)
target_link_libraries(test_dispatch_scan PRIVATE sotcgpu_options)
add_test(NAME test_dispatch_scan COMMAND test_dispatch_scan)

# Session 32: the log of streamed buffer ranges (header-only, no Vulkan).
add_executable(test_range_log tests/test_range_log.cpp)
target_link_libraries(test_range_log PRIVATE sotcgpu_options)
add_test(NAME test_range_log COMMAND test_range_log)

# Session 34: the page prefilter in front of IsRangeGpuWritten (header-only, no Vulkan).
add_executable(test_written_pages tests/test_written_pages.cpp)
target_link_libraries(test_written_pages PRIVATE sotcgpu_options)
add_test(NAME test_written_pages COMMAND test_written_pages)

# Session 36: the cache of FindImage's perfect matches (header-only, no Vulkan).
add_executable(test_find_cache tests/test_find_cache.cpp)
target_link_libraries(test_find_cache PRIVATE sotcgpu_options)
add_test(NAME test_find_cache COMMAND test_find_cache)

# Session 16: SOTC_TEX_DOWNSCALE's eligibility and size/level mapping (header-only, no Vulkan).
add_executable(test_tex_downscale tests/test_tex_downscale.cpp)
target_link_libraries(test_tex_downscale PRIVATE sotcgpu_options)
add_test(NAME test_tex_downscale COMMAND test_tex_downscale)

# Session 13: the readback prefetch's hot set, ring and entries (header-only, no Vulkan).
add_executable(test_prefetch tests/test_prefetch.cpp)
target_link_libraries(test_prefetch PRIVATE sotcgpu_options)
add_test(NAME test_prefetch COMMAND test_prefetch)

# Session 12: the union of GPU timestamp intervals (header-only, no Vulkan).
add_executable(test_gpu_busy tests/test_gpu_busy.cpp)
target_link_libraries(test_gpu_busy PRIVATE sotcgpu_options)
add_test(NAME test_gpu_busy COMMAND test_gpu_busy)

# Session 29: a guest unmapping a DCB as soon as its EOP label lands (needs a Vulkan device).
add_executable(test_cmd_lifetime tests/test_cmd_lifetime.cpp)
target_link_libraries(test_cmd_lifetime PRIVATE sotcgpu sotcgpu_options)
add_test(NAME test_cmd_lifetime COMMAND test_cmd_lifetime)
set_tests_properties(test_cmd_lifetime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

add_executable(test_sotcgpu_headless tests/test_sotcgpu_headless.cpp)
target_link_libraries(test_sotcgpu_headless PRIVATE sotcgpu sotcgpu_options)
add_test(NAME test_sotcgpu_headless COMMAND test_sotcgpu_headless)
# Exit code 77: no Vulkan driver or device on this machine.
set_tests_properties(test_sotcgpu_headless PROPERTIES SKIP_RETURN_CODE 77)

# With a real SDL window: Presenter, the VideoOut texture path and screenshots (Windows).
if(WIN32)
    add_executable(test_sotcgpu_window tests/test_sotcgpu_window.cpp)
    target_link_libraries(test_sotcgpu_window PRIVATE sotcgpu sotcgpu_options)
    add_test(NAME test_sotcgpu_window COMMAND test_sotcgpu_window)
    set_tests_properties(test_sotcgpu_window PROPERTIES SKIP_RETURN_CODE 77)
endif()
