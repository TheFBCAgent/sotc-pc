/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AMD SSE4a EXTRQ/INSERTQ for CPUs without it (Intel). The PS4's Jaguar has them.
 * Only used from a SIGILL handler on Linux, where the kernel preserves the red zone. */
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "runtime.h"

static const bootimg_patch *g_sites;
static uint32_t g_count;

static uint64_t field_mask(uint8_t len) {
    return len == 0 || len >= 64 ? ~0ull : (1ull << len) - 1;
}

void sse4a_extrq(uint64_t *dst, uint8_t len, uint8_t idx) {
    len &= 63;
    idx &= 63;
    *dst = (*dst >> idx) & field_mask(len);
}

void sse4a_insertq(uint64_t *dst, uint64_t src, uint8_t len, uint8_t idx) {
    len &= 63;
    idx &= 63;
    uint64_t mask = field_mask(len) << idx;
    *dst = (*dst & ~mask) | ((src << idx) & mask);
}

void sse4a_set_sites(const bootimg_patch *sites, uint32_t count) {
    /* Patch records are sorted by the converter within each module, and modules
     * are laid out in order, so the table is sorted by offset. */
    g_sites = sites;
    g_count = count;
}

static const bootimg_patch *find_site(uint64_t offset) {
    uint32_t lo = 0, hi = g_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_sites[mid].offset < offset) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_count && g_sites[lo].offset == offset ? &g_sites[lo] : NULL;
}

bool sse4a_handle_fault(plat_fault *f) {
    if (f->kind != PLAT_FAULT_ILLEGAL || !g_sites || !f->xmm) return false;
    uint8_t *rip = (uint8_t *)(uintptr_t)f->rip;
    if (rip < g_rt.image || rip >= g_rt.image + g_rt.image_size) return false;
    const bootimg_patch *s = find_site((uint64_t)(rip - g_rt.image));
    if (!s || s->kind == BOOTIMG_PATCH_FS) return false;

    uint64_t (*xmm)[2] = f->xmm; /* 16 registers, low and high qwords */
    uint64_t *dst = &xmm[s->a][0];
    switch (s->kind) {
    case BOOTIMG_PATCH_EXTRQ_IMM: sse4a_extrq(dst, s->c, s->d); break;
    case BOOTIMG_PATCH_EXTRQ_REG: {
        uint64_t ctl = xmm[s->b][0];
        sse4a_extrq(dst, ctl & 63, (ctl >> 8) & 63);
        break;
    }
    case BOOTIMG_PATCH_INSERTQ_IMM: sse4a_insertq(dst, xmm[s->b][0], s->c, s->d); break;
    case BOOTIMG_PATCH_INSERTQ_REG: {
        uint64_t ctl = xmm[s->b][1];
        sse4a_insertq(dst, xmm[s->b][0], ctl & 63, (ctl >> 8) & 63);
        break;
    }
    default: return false;
    }
    *f->rip_ptr += s->length;
    return true;
}
