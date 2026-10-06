/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows: memory, the guest arena and physical memory. */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>

#include "log.h"
#include "platform.h"

/* ---- memory ------------------------------------------------------------ */

static DWORD win_prot(int prot) {
    bool r = prot & PLAT_PROT_R, w = prot & PLAT_PROT_W, x = prot & PLAT_PROT_X;
    if (x) return w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

static void *try_alloc(uint64_t addr, size_t size, int prot) {
    return VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
}

void *plat_map(uint64_t addr, size_t size, int prot, bool fixed) {
    if (fixed) return try_alloc(addr, size, prot);

    /* Walk the address space for a free range below the guest limit. */
    const uint64_t gran = 0x10000;
    uint64_t cur = addr ? addr : 0x10000000ull;
    while (cur + size <= PLAT_GUEST_LIMIT) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)cur, &mbi, sizeof(mbi))) break;
        uint64_t region_end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            uint64_t start = (cur + gran - 1) & ~(gran - 1);
            if (start + size <= region_end) {
                void *p = try_alloc(start, size, prot);
                if (p) return p;
            }
        }
        cur = region_end;
    }
    return NULL;
}

void plat_unmap(void *addr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.AllocationBase == addr &&
        mbi.RegionSize >= size) {
        VirtualFree(addr, 0, MEM_RELEASE);
    } else {
        VirtualFree(addr, size, MEM_DECOMMIT);
    }
}

void *plat_reserve(uint64_t addr, size_t size) {
    return VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool plat_commit(void *addr, size_t size, int prot) {
    /* Decommit first so recommitted pages come back zeroed, like a fresh mmap. */
    VirtualFree(addr, size, MEM_DECOMMIT);
    return VirtualAlloc(addr, size, MEM_COMMIT, win_prot(prot)) != NULL;
}

void plat_decommit(void *addr, size_t size) { VirtualFree(addr, size, MEM_DECOMMIT); }

/* ---- guest arena: placeholders, section views, private units -------------
 * Needs Windows 10 1803+ (VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFile2),
 * looked up at run time so no newer import library is needed. The same scheme
 * as shadPS4's Windows backend (src/core/address_space.cpp). */

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

typedef PVOID(WINAPI *virtual_alloc2_fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *map_view3_fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *,
                                    ULONG);
typedef BOOL(WINAPI *unmap_view2_fn)(HANDLE, PVOID, ULONG);

static virtual_alloc2_fn p_VirtualAlloc2;
static map_view3_fn p_MapViewOfFile3;
static unmap_view2_fn p_UnmapViewOfFile2;
static HANDLE g_phys_section;
static uint64_t g_arena_lo, g_arena_hi;

static bool load_placeholder_api(void) {
    if (p_VirtualAlloc2) return true;
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) return false;
    p_VirtualAlloc2 = (virtual_alloc2_fn)(void *)GetProcAddress(kb, "VirtualAlloc2");
    p_MapViewOfFile3 = (map_view3_fn)(void *)GetProcAddress(kb, "MapViewOfFile3");
    p_UnmapViewOfFile2 = (unmap_view2_fn)(void *)GetProcAddress(kb, "UnmapViewOfFile2");
    return p_VirtualAlloc2 && p_MapViewOfFile3 && p_UnmapViewOfFile2;
}

bool plat_phys_init(uint64_t size) {
    if (!load_placeholder_api()) {
        log_error("memory: placeholder APIs missing (needs Windows 10 1803+)");
        return false;
    }
    /* SEC_RESERVE: pages count against the commit limit only once used. */
    g_phys_section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                        PAGE_EXECUTE_READWRITE | SEC_RESERVE,
                                        (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!g_phys_section) log_error("memory: cannot create the physical memory section (%lu)",
                                   GetLastError());
    return g_phys_section != NULL;
}

void *plat_arena_reserve(uint64_t addr, uint64_t size) {
    if (!load_placeholder_api()) return NULL;
    void *p = p_VirtualAlloc2(GetCurrentProcess(), (void *)(uintptr_t)addr, size,
                              MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (p) {
        g_arena_lo = addr;
        g_arena_hi = addr + size;
    }
    return p;
}

/* Is [a, a+1) inside a free placeholder? (All free arena space is placeholders.) */
static bool free_at(uint64_t a, MEMORY_BASIC_INFORMATION *mbi) {
    if (a < g_arena_lo || a >= g_arena_hi) return false;
    return VirtualQuery((void *)(uintptr_t)a, mbi, sizeof(*mbi)) && mbi->State == MEM_RESERVE;
}

/* Make [addr, addr+size) exactly one placeholder. VirtualQuery reports each
 * placeholder as its own region, so merge the placeholders the range touches
 * first, then carve the range out. */
static bool carve_placeholder(uint64_t addr, uint64_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!free_at(addr, &mbi)) return false;
    uint64_t start = (uint64_t)(uintptr_t)mbi.AllocationBase;
    uint64_t end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    bool multi = false;
    while (end < addr + size) {
        if (!free_at(end, &mbi)) return false; /* something is mapped in the way */
        end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        multi = true;
    }
    log_trace("carve %#llx+%#llx in placeholder %#llx-%#llx%s", (unsigned long long)addr,
              (unsigned long long)size, (unsigned long long)start, (unsigned long long)end,
              multi ? " (coalesced)" : "");
    if (multi && !VirtualFree((void *)(uintptr_t)start, end - start,
                              MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
        return false;
    if ((start != addr || end != addr + size) &&
        !VirtualFree((void *)(uintptr_t)addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        return false;
    return true;
}

bool plat_arena_map(uint64_t addr, uint64_t size, int prot, int64_t phys, bool commit) {
    if (!carve_placeholder(addr, size)) {
        log_error("memory: %#llx+%#llx is not free arena space", (unsigned long long)addr,
                  (unsigned long long)size);
        return false;
    }
    HANDLE proc = GetCurrentProcess();
    void *a = (void *)(uintptr_t)addr;
    if (phys >= 0) {
        if (!p_MapViewOfFile3(g_phys_section, proc, a, (ULONG64)phys, size,
                              MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL, 0)) {
            log_error("memory: MapViewOfFile3 %#llx+%#llx phys %#llx failed (%lu)",
                      (unsigned long long)addr, (unsigned long long)size,
                      (unsigned long long)phys, GetLastError());
            return false;
        }
        /* SEC_RESERVE sections are committed through any view; already
         * committed pages keep their contents. */
        if (commit && !VirtualAlloc(a, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
            log_error("memory: committing physical pages failed (%lu)", GetLastError());
            p_UnmapViewOfFile2(proc, a, MEM_PRESERVE_PLACEHOLDER);
            return false;
        }
        DWORD old;
        if (win_prot(prot) != PAGE_EXECUTE_READWRITE) VirtualProtect(a, size, win_prot(prot), &old);
        return true;
    }
    if (!p_VirtualAlloc2(proc, a, size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                         win_prot(prot), NULL, 0)) {
        log_error("memory: VirtualAlloc2 %#llx+%#llx failed (%lu)", (unsigned long long)addr,
                  (unsigned long long)size, GetLastError());
        return false;
    }
    return true;
}

void plat_arena_unmap(uint64_t addr, uint64_t size, bool is_view) {
    void *a = (void *)(uintptr_t)addr;
    log_trace("arena unmap %#llx+%#llx %s", (unsigned long long)addr, (unsigned long long)size,
              is_view ? "view" : "private");
    if (is_view) p_UnmapViewOfFile2(GetCurrentProcess(), a, MEM_PRESERVE_PLACEHOLDER);
    else VirtualFree(a, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

bool plat_protect(void *addr, size_t size, int prot) {
    DWORD old;
    return VirtualProtect(addr, size, win_prot(prot), &old) != 0;
}

void plat_flush_icache(void *addr, size_t size) {
    FlushInstructionCache(GetCurrentProcess(), addr, size);
}

#endif
