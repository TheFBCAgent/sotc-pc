# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(shadow-pc C)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "shadow-pc needs clang (clang or clang-cl): guest calls use "
                        "__attribute__((sysv_abi)), which MSVC does not support.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# HLE libraries register themselves (HLE_LIBRARY), so a new runtime/src/hle/*.c
# file is picked up without editing this list. An OBJECT library keeps every
# object file in the link even when nothing references it directly.
file(GLOB SOTC_HLE_SOURCES CONFIGURE_DEPENDS runtime/src/hle/*.c)
add_library(sotc_core OBJECT
    runtime/src/log.c
    runtime/src/loader.c
    runtime/src/tls.c
    runtime/src/sse4a.c
    ${SOTC_HLE_SOURCES}
    $<IF:$<BOOL:${WIN32}>,runtime/src/platform_win.c,runtime/src/platform_posix.c>
)
target_include_directories(sotc_core PUBLIC runtime/include runtime/src)
target_compile_options(sotc_core PUBLIC
    $<IF:$<C_COMPILER_FRONTEND_VARIANT:MSVC>,/W4,-Wall -Wextra> -Wno-unused-parameter)
if(WIN32)
    target_compile_definitions(sotc_core PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
                                                WIN32_LEAN_AND_MEAN)
else()
    target_link_libraries(sotc_core PUBLIC pthread)
endif()

add_executable(sotc-runtime runtime/src/main.c)
target_link_libraries(sotc-runtime PRIVATE sotc_core)

# Keep the host executable out of the guest's address range (below 1 TiB),
# like shadPS4: fixed high base on Windows, PIE on Linux.
if(WIN32)
    target_link_options(sotc-runtime PRIVATE
        $<$<C_COMPILER_FRONTEND_VARIANT:MSVC>:/BASE:0x700000000000 /DYNAMICBASE:NO /STACK:0x200000>
        $<$<C_COMPILER_FRONTEND_VARIANT:GNU>:-Wl,--image-base=0x700000000000>)
else()
    set_target_properties(sotc-runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_options(sotc-runtime PRIVATE -pie)
endif()

# One test executable per runtime/tests/test_*.c (each with its own main()).
enable_testing()
file(GLOB SOTC_TESTS CONFIGURE_DEPENDS runtime/tests/test_*.c)
foreach(test_source ${SOTC_TESTS})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE sotc_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
