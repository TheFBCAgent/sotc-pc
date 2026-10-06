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
