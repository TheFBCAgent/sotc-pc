# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
# C++ (SDL3 on Windows, and sotcgpu) uses the same compiler as C, so the one
# configure line in CLAUDE.md (-DCMAKE_C_COMPILER=clang-cl) is enough.
if(CMAKE_C_COMPILER MATCHES "clang-cl" AND NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${CMAKE_C_COMPILER}")
endif()
# A fresh build directory without -DCMAKE_BUILD_TYPE used to be an unoptimised Debug build
# (session 18: build-tracy ran the game ~7x slower than build-vk). Set before project():
# with clang-cl, project() itself sets Debug (session 37: build-vk-pgo ran ~8x slower).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_GENERATOR MATCHES "Multi-Config|Visual Studio|Xcode")
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type")
endif()
project(shadow-pc C CXX)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "shadow-pc needs clang (clang or clang-cl): guest calls use "
                        "__attribute__((sysv_abi)), which MSVC does not support.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Linux: the host executables are PIE (see sotc_host_executable), so every
# object linked into them, including the sotc_core OBJECT library, must be PIC.
if(NOT WIN32)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(SOTC_VULKAN "Build the Vulkan GPU (sotcgpu)" OFF)
# Session 18: the Tracy profiler client (third_party/tracy, v0.14.1; the viewer and
# tracy-capture must be 0.14.1 too, the protocol is checked). Off: gpu/shim/tracy_off stubs and runtime/src/prof.h compile every zone out.
option(SOTC_TRACY "Build with the Tracy profiler client" OFF)
# Session 37: thin LTO over everything built here (the runtime, sotcgpu and the submodules),
# linked with lld (bbport: -1.8% per draw). Static archives hold bitcode: llvm-ar, or on
# clang-cl llvm-lib (CMake's MSVC archive rule passes lib.exe flags, which llvm-ar rejects).
option(SOTC_LTO "Build with thin LTO (needs lld)" OFF)
# Session 37: PGO. "generate" instruments the build; the runtime writes
# <build>/pgo/sotc-<pid>.profraw every 30 s (smoke kills the game, so there is no clean exit).
# A path to a merged .profdata uses it. Empty: off. Regeneration: docs (CLAUDE.md, Commands).
set(SOTC_PGO "" CACHE STRING "PGO: empty (off), generate, or a merged .profdata path")
set(SOTC_CLANG_CL OFF)
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(SOTC_CLANG_CL ON)
endif()
if(SOTC_LTO)
    add_compile_options(-flto=thin)
    if(SOTC_CLANG_CL)
        # clang-cl: CMake runs the linker itself, which must read bitcode (not link.exe).
        if(NOT CMAKE_LINKER MATCHES "lld-link")
            message(FATAL_ERROR "SOTC_LTO with clang-cl needs -DCMAKE_LINKER=lld-link (now ${CMAKE_LINKER})")
        endif()
        if(NOT CMAKE_AR MATCHES "llvm-lib")
            message(FATAL_ERROR "SOTC_LTO with clang-cl needs llvm-lib as CMAKE_AR (now ${CMAKE_AR})")
        endif()
        add_link_options(/opt:lldltojobs=all)
    else()
        find_program(SOTC_LLVM_AR NAMES llvm-ar REQUIRED)
        find_program(SOTC_LLVM_RANLIB NAMES llvm-ranlib REQUIRED)
        set(CMAKE_AR "${SOTC_LLVM_AR}" CACHE FILEPATH "" FORCE)
        set(CMAKE_RANLIB "${SOTC_LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
        add_link_options(-fuse-ld=lld -flto=thin -Wl,--thinlto-jobs=all)
    endif()
endif()
if(SOTC_PGO STREQUAL "generate")
    add_compile_options(-fprofile-generate)
    if(SOTC_CLANG_CL)
        # clang-cl names clang_rt.profile in the objects (/DEFAULTLIB); lld-link, run directly,
        # needs clang's runtime directory to find it.
        execute_process(COMMAND "${CMAKE_C_COMPILER}" /clang:-print-resource-dir
                        OUTPUT_VARIABLE SOTC_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        add_link_options("/libpath:${SOTC_CLANG_RESOURCE_DIR}/lib/windows")
    else()
        add_link_options(-fprofile-generate)
    endif()
    # The runtime names the file (no %m: online merging would add up the cumulative
    # counters of every 30 s write).
    add_compile_definitions(SOTC_PGO_GENERATE SOTC_PGO_DIR="${CMAKE_BINARY_DIR}/pgo")
elseif(SOTC_PGO)
    if(NOT EXISTS "${SOTC_PGO}")
        message(FATAL_ERROR "SOTC_PGO: no profile at ${SOTC_PGO}")
    endif()
    add_compile_options(-fprofile-use=${SOTC_PGO} -Wno-profile-instr-out-of-date
                        -Wno-profile-instr-unprofiled -Wno-backend-plugin)
    if(NOT SOTC_CLANG_CL)
        add_link_options(-fprofile-use=${SOTC_PGO})
    endif()
endif()
add_subdirectory(third_party)
add_subdirectory(gpu)

# HLE libraries register themselves (HLE_LIBRARY), so a new runtime/src/hle/*.c
# file is picked up without editing this list. An OBJECT library keeps every
# object file in the link even when nothing references it directly.
file(GLOB SOTC_HLE_SOURCES CONFIGURE_DEPENDS runtime/src/hle/*.c)
if(WIN32)
    file(GLOB SOTC_PLATFORM_SOURCES CONFIGURE_DEPENDS runtime/src/platform_win_*.c)
else()
    file(GLOB SOTC_PLATFORM_SOURCES CONFIGURE_DEPENDS runtime/src/platform_posix_*.c)
endif()
add_library(sotc_core OBJECT
    runtime/src/log.c
    runtime/src/loader.c
    runtime/src/tls.c
    runtime/src/sse4a.c
    runtime/src/window.c
    runtime/src/gpu_bridge.c
    ${SOTC_HLE_SOURCES}
    ${SOTC_PLATFORM_SOURCES}
)
target_include_directories(sotc_core PUBLIC runtime/include runtime/src)
# A fiber can resume on another thread, but the compiler computes a thread_local's address once
# per function: with LTO, tls_current() (t_current) inlined into the fiber calls read the old
# thread's value after sotc_fiber_swap (test_fiber crashed). Native code keeps those calls opaque.
if(SOTC_LTO)
    set_source_files_properties(runtime/src/hle/fiber.c PROPERTIES COMPILE_OPTIONS -fno-lto)
endif()
target_link_libraries(sotc_core PUBLIC sotcgpu SDL3::SDL3)
if(SOTC_TRACY)
    target_link_libraries(sotc_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(sotc_core PUBLIC SOTC_TRACY)
endif()
target_compile_options(sotc_core PUBLIC
    $<IF:$<C_COMPILER_FRONTEND_VARIANT:MSVC>,/W4,-Wall -Wextra> -Wno-unused-parameter)
if(WIN32)
    target_compile_definitions(sotc_core PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
                                                WIN32_LEAN_AND_MEAN)
else()
    target_link_libraries(sotc_core PUBLIC pthread)
endif()

# Keep a host executable out of the guest's address range (below 1 TiB),
# like shadPS4: fixed high base on Windows, PIE on Linux. On Windows,
# /DYNAMICBASE:NO also turns off bottom-up ASLR, which otherwise scatters
# heaps and DLLs over the low 1 TiB and can take the guest arena's range.
function(sotc_host_executable target)
    if(WIN32)
        target_link_options(${target} PRIVATE
            $<$<C_COMPILER_FRONTEND_VARIANT:MSVC>:/BASE:0x700000000000 /DYNAMICBASE:NO /STACK:0x200000>
            $<$<C_COMPILER_FRONTEND_VARIANT:GNU>:-Wl,--image-base=0x700000000000>)
    else()
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_link_options(${target} PRIVATE -pie)
    endif()
endfunction()

add_executable(sotc-runtime runtime/src/main.c)
target_link_libraries(sotc-runtime PRIVATE sotc_core)
sotc_host_executable(sotc-runtime)

# One test executable per runtime/tests/test_*.c (each with its own main()).
enable_testing()
file(GLOB SOTC_TESTS CONFIGURE_DEPENDS runtime/tests/test_*.c)
foreach(test_source ${SOTC_TESTS})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE sotc_core)
    sotc_host_executable(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# sotc-padprobe: lists joysticks and prints their raw input (docs/m3/pad.md).
add_executable(sotc-padprobe tools/padprobe/padprobe.c)
target_link_libraries(sotc-padprobe PRIVATE SDL3::SDL3)
