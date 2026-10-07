/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceGnmDriver: the kernel-side half of Gnm.
 *
 * Most of these functions are packet builders: the statically linked libGnm
 * reserves `size` dwords in a command buffer and asks the driver to fill them
 * with PM4 type-3 packets. Every builder here writes exactly the dwords it
 * was given (padding with NOP packets), so a command buffer is always a
 * well-formed packet stream for the PM4 executor (pm4.h).
 *
 * Submissions go through pm4_submit: to sotcgpu when the GPU bridge is active
 * (M3), else to the null executor, which applies memory side effects and EOP
 * interrupts. Debug, capture and resource-registration calls
 * behave as on retail firmware, where they are unavailable.
 *
 * Ported from shadPS4 (GPL-2.0-or-later, commit 14ada48):
 *   src/core/libraries/gnmdriver/gnmdriver.cpp, gnmdriver_init.h, gnm_error.h
 *   src/video_core/amdgpu/pm4_cmds.h, pm4_opcodes.h
 * Copyright 2024-2026 shadPS4 Emulator Project. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../gpu_bridge.h"
#include "../log.h"
#include "../platform.h"
#include "equeue.h"
#include "hle.h"
#include "pm4.h"
#include "sotcgpu.h"

/* ---- error codes (gnm_error.h) ------------------------------------------ */

#define GNM_ERR(v) ((int32_t)(v))
#define GNM_ERROR_SUBMISSION_FAILED_INVALID_ARGUMENT GNM_ERR(0x80D11000u)
#define GNM_ERROR_SUBMISSION_AND_FLIP_FAILED_INVALID_QUEUE_FULL GNM_ERR(0x80D11081u)
#define GNM_ERROR_CAPTURE_RAZOR_NOT_LOADED GNM_ERR(0x80D15001u)
#define GNM_ERROR_CAPTURE_FAILED_INTERNAL GNM_ERR(0x80D1500Fu)
#define GNM_ERROR_COMPUTEQUEUE_INVALID_PIPE_ID GNM_ERR(0x80D17000u)
#define GNM_ERROR_COMPUTEQUEUE_INVALID_QUEUE_ID GNM_ERR(0x80D17001u)
#define GNM_ERROR_COMPUTEQUEUE_INVALID_RING_SIZE GNM_ERR(0x80D17002u)
#define GNM_ERROR_COMPUTEQUEUE_INVALID_RING_BASE_ADDR GNM_ERR(0x80D17003u)
#define GNM_ERROR_COMPUTEQUEUE_INVALID_READ_PTR_ADDR GNM_ERR(0x80D17004u)
#define GNM_ERROR_COMPUTEQUEUE_INTERNAL GNM_ERR(0x80D170FFu)
#define GNM_ERROR_FAILURE GNM_ERR(0x8EEE00FFu)
#define VIDEOOUT_ERROR_FLIP_QUEUE_FULL GNM_ERR(0x80290012u)

/* ---- PM4 encoding (pm4_cmds.h / pm4_opcodes.h) -------------------------- */

enum {
    OP_NOP = 0x10,
    OP_DISPATCH_DIRECT = 0x15,
    OP_DISPATCH_INDIRECT = 0x16,
    OP_DRAW_INDIRECT = 0x24,
    OP_DRAW_INDEX_INDIRECT = 0x25,
    OP_DRAW_INDEX_2 = 0x27,
    OP_DRAW_INDEX_AUTO = 0x2D,
    OP_DRAW_INDEX_OFFSET_2 = 0x35,
    OP_WRITE_DATA = 0x37,
    OP_WAIT_REG_MEM = 0x3C,
    OP_EVENT_WRITE_EOP = 0x47,
    OP_ACQUIRE_MEM = 0x58,
    OP_SET_CONTEXT_REG = 0x69,
    OP_SET_SH_REG = 0x76,
    OP_SET_UCONFIG_REG = 0x79,
    OP_DRAW_INDEX_INDIRECT_COUNT_MULTI = 0x9D,
};
enum { ST_GFX = 0, ST_CS = 1 };

/* NOP payload markers (PM4CmdNop::PayloadType). */
enum {
    NOP_DEBUG_MARKER_PUSH = 0x68750001u,
    NOP_DEBUG_MARKER_POP = 0x68750002u,
    NOP_DEBUG_SET_MARKER = 0x68750003u,
    NOP_DEBUG_COLOR_MARKER_PUSH = 0x6875000eu,
    NOP_PATCHED_FLIP = 0x68750776u,
    NOP_PREPARE_FLIP = 0x68750777u,
    NOP_PREPARE_FLIP_LABEL = 0x68750778u,
    NOP_PREPARE_FLIP_INTERRUPT = 0x68750780u,
    NOP_PREPARE_FLIP_INTERRUPT_LABEL = 0x68750781u,
};

/* Type-3 header for a packet with `body` (>= 1) dwords after the header. */
static inline uint32_t pm4_type3(uint32_t op, uint32_t body, uint32_t shader_type,
                                 uint32_t predicate) {
    return (3u << 30) | (((body - 1) & 0x3fffu) << 16) | ((op & 0xffu) << 8) |
           ((shader_type & 1u) << 1) | (predicate & 1u);
}

static uint32_t *put_packet(uint32_t *p, uint32_t op, uint32_t shader_type,
                            const uint32_t *body, uint32_t n) {
    *p++ = pm4_type3(op, n, shader_type, 0);
    memcpy(p, body, n * sizeof(uint32_t));
    return p + n;
}

/* WritePacket<op>(cmdbuf, type, args...) */
#define PKT(p, op, st, ...)                                                        \
    put_packet((p), (op), (st), (const uint32_t[]){__VA_ARGS__},                   \
               (uint32_t)(sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t)))
#define SET_SH(p, ...) PKT(p, OP_SET_SH_REG, ST_GFX, __VA_ARGS__)
#define SET_SH_CS(p, ...) PKT(p, OP_SET_SH_REG, ST_CS, __VA_ARGS__)
#define SET_CTX(p, ...) PKT(p, OP_SET_CONTEXT_REG, ST_GFX, __VA_ARGS__)
#define SET_UCONFIG(p, ...) PKT(p, OP_SET_UCONFIG_REG, ST_GFX, __VA_ARGS__)
#define NOP_PKT(p, ...) PKT(p, OP_NOP, ST_GFX, __VA_ARGS__)

/* A NOP whose body is `body` dwords (all zero); returns the end of it. */
static uint32_t *trailing_nop(uint32_t *p, uint32_t body) {
    p[0] = pm4_type3(OP_NOP, body, ST_GFX, 0);
    memset(p + 1, 0, body * sizeof(uint32_t));
    return p + 1 + body;
}

/* Fill [p, p+size) with one NOP packet (size >= 2). */
static void fill_nop(uint32_t *p, uint32_t size) { trailing_nop(p, size - 1); }

static uint32_t *header(uint32_t *p, uint32_t op, uint32_t body, uint32_t st, uint32_t pred) {
    *p = pm4_type3(op, body, st, pred);
    return p + 1;
}

static bool is_neo(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("SOTC_NEO");
        cached = env && *env == '1';
    }
    return cached != 0;
}

/* ---- init sequences (gnmdriver_init.h) ---------------------------------- */

#define HW_INIT_PACKET_SIZE 0x100u
#define COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static const uint32_t k_clear_state_sequence[] = {
    0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0055800u,
    0x2ec47fc0u, 0xffffffffu, 0u,          0u,          0u, 10u,
};
_Static_assert(sizeof(k_clear_state_sequence) == 0xc * 4, "clear state");

/* InitSequence350 without its fake two-dword CLEAR_STATE preamble. */
static const uint32_t k_init350[] = {
    0xc0017600u, 0x216u, 0xffffffffu,
    0xc0017600u, 0x217u, 0xffffffffu,
    0xc0017600u, 0x215u, 0u,
    0xc0016900u, 0x2f9u, 0x2du,
    0xc0016900u, 0x282u, 8u,
    0xc0016900u, 0x280u, 0x80008u,
    0xc0016900u, 0x281u, 0xffff0000u,
    0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu,
    0xc0016900u, 0x83u,  0xffffu,
    0xc0016900u, 0x317u, 0x10u,
    0xc0016900u, 0x2fau, 0x3f800000u,
    0xc0016900u, 0x2fcu, 0x3f800000u,
    0xc0016900u, 0x2fbu, 0x3f800000u,
    0xc0016900u, 0x2fdu, 0x3f800000u,
    0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu,
    0xc0016900u, 0x30fu, 0xffffffffu,
    0xc0002f00u, 1u,
    0xc0017600u, 7u,     0x1701ffu,
    0xc0017600u, 0x46u,  0x1701fdu,
    0xc0017600u, 0x87u,  0x1701ffu,
    0xc0017600u, 0xc7u,  0x1701fdu,
    0xc0017600u, 0x107u, 0x17u,
    0xc0017600u, 0x147u, 0x1701fdu,
    0xc0017600u, 0x47u,  0x1cu,
    0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu,
    0xc0016900u, 0x103u, 0u,
    0xc0016900u, 0x284u, 0u,
    0xc0016900u, 0x290u, 0u,
    0xc0016900u, 0x2aeu, 0u,
    0xc0016900u, 0x102u, 0u,
    0xc0016900u, 0x292u, 0u,
    0xc0016900u, 0x293u, 0x6020000u,
    0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x200u, 0xe0000000u,
    0xc0016900u, 0x2aau, 0xffu,
};
_Static_assert(sizeof(k_init350) == 0x7c * 4, "InitSequence350");

static const uint32_t k_init350_neo[] = {
    0xc0017600u, 0x216u, 0xffffffffu,
    0xc0017600u, 0x217u, 0xffffffffu,
    0xc0017600u, 0x219u, 0xffffffffu,
    0xc0017600u, 0x21au, 0xffffffffu,
    0xc0017600u, 0x215u, 0u,
    0xc0016900u, 0x2f9u, 0x2du,
    0xc0016900u, 0x282u, 8u,
    0xc0016900u, 0x280u, 0x80008u,
    0xc0016900u, 0x281u, 0xffff0000u,
    0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu,
    0xc0016900u, 0x83u,  0xffffu,
    0xc0016900u, 0x317u, 0x10u,
    0xc0016900u, 0x2fau, 0x3f800000u,
    0xc0016900u, 0x2fcu, 0x3f800000u,
    0xc0016900u, 0x2fbu, 0x3f800000u,
    0xc0016900u, 0x2fdu, 0x3f800000u,
    0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu,
    0xc0016900u, 0x30fu, 0xffffffffu,
    0xc0002f00u, 1u,
    0xc0017600u, 7u,     0x1701ffu,
    0xc0017600u, 0x46u,  0x1701fdu,
    0xc0017600u, 0x87u,  0x1701ffu,
    0xc0017600u, 0xc7u,  0x1701fdu,
    0xc0017600u, 0x107u, 0x17u,
    0xc0017600u, 0x147u, 0x1701fdu,
    0xc0017600u, 0x47u,  0x1cu,
    0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu,
    0xc0016900u, 0x103u, 0u,
    0xc0016900u, 0x284u, 0u,
    0xc0016900u, 0x290u, 0u,
    0xc0016900u, 0x2aeu, 0u,
    0xc0016900u, 0x102u, 0u,
    0xc0016900u, 0x292u, 0u,
    0xc0016900u, 0x293u, 0x6020000u,
    0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u,
    0xc0026900u, 0xebu,  0xff00ff00u, 0xff00u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x200u, 0xe0000000u,
    0xc0017900u, 0x40000258u, 0x6d007fu,
};
_Static_assert(sizeof(k_init350_neo) == 0x86 * 4, "InitSequence350Neo");

/* CtxInitSequence400: ends with a NOP header whose body runs to 0x100 dwords. */
static const uint32_t k_ctx_init400[] = {
    0xc0012800u, 0x80000000u, 0x80000000u,
    0xc0001200u, 0u,
    0xc0016900u, 0x2f9u, 0x2du,
    0xc0016900u, 0x282u, 8u,
    0xc0016900u, 0x280u, 0x80008u,
    0xc0016900u, 0x281u, 0xffff0000u,
    0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu,
    0xc0016900u, 0x83u,  0xffffu,
    0xc0016900u, 0x317u, 0x10u,
    0xc0016900u, 0x2fau, 0x3f800000u,
    0xc0016900u, 0x2fcu, 0x3f800000u,
    0xc0016900u, 0x2fbu, 0x3f800000u,
    0xc0016900u, 0x2fdu, 0x3f800000u,
    0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu,
    0xc0016900u, 0x30fu, 0xffffffffu,
    0xc0002f00u, 1u,
    0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu,
    0xc0016900u, 0x103u, 0u,
    0xc0016900u, 0x284u, 0u,
    0xc0016900u, 0x290u, 0u,
    0xc0016900u, 0x2aeu, 0u,
    0xc0016900u, 0x102u, 0u,
    0xc0016900u, 0x292u, 0u,
    0xc0016900u, 0x293u, 0x6020000u,
    0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0016900u, 0x2aau, 0xffu,
    0xc09e1000u, 0u,
};
_Static_assert(sizeof(k_ctx_init400) == 0x62 * 4, "CtxInitSequence400");

static const uint32_t k_ctx_init400_neo[] = {
    0xc0012800u, 0x80000000u, 0x80000000u,
    0xc0001200u, 0u,
    0xc0016900u, 0x2f9u, 0x2du,
    0xc0016900u, 0x282u, 8u,
    0xc0016900u, 0x280u, 0x80008u,
    0xc0016900u, 0x281u, 0xffff0000u,
    0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu,
    0xc0016900u, 0x83u,  0xffffu,
    0xc0016900u, 0x317u, 0x10u,
    0xc0016900u, 0x2fau, 0x3f800000u,
    0xc0016900u, 0x2fcu, 0x3f800000u,
    0xc0016900u, 0x2fbu, 0x3f800000u,
    0xc0016900u, 0x2fdu, 0x3f800000u,
    0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu,
    0xc0016900u, 0x30fu, 0xffffffffu,
    0xc0002f00u, 1u,
    0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu,
    0xc0016900u, 0x103u, 0u,
    0xc0016900u, 0x284u, 0u,
    0xc0016900u, 0x290u, 0u,
    0xc0016900u, 0x2aeu, 0u,
    0xc0016900u, 0x102u, 0u,
    0xc0016900u, 0x292u, 0u,
    0xc0016900u, 0x293u, 0x6020000u,
    0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u,
    0xc0026900u, 0xebu,  0xff00ff00u, 0xff00u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x40000258u, 0x6d007fu,
    0xc09a1000u, 0u,
};
_Static_assert(sizeof(k_ctx_init400_neo) == 0x66 * 4, "CtxInitSequence400Neo");

/* ---- shared state ------------------------------------------------------- */

static _Atomic(plat_mutex *) g_lock_ptr;

static plat_mutex *gnm_lock(void) {
    plat_mutex *m = atomic_load(&g_lock_ptr);
    if (!m) {
        plat_mutex *fresh = plat_mutex_new();
        if (atomic_compare_exchange_strong(&g_lock_ptr, &m, fresh)) m = fresh;
        else plat_mutex_free(fresh);
    }
    return m;
}

/* Compute queues (MapComputeQueue / DingDong). vqid = slot + 1, in [1..56]. */
#define NUM_COMPUTE_PIPES 7u /* actually 8, #7 is reserved by system software */
#define NUM_QUEUES_PER_PIPE 8u
#define NUM_COMPUTE_RINGS (NUM_COMPUTE_PIPES * NUM_QUEUES_PER_PIPE)

typedef struct compute_queue {
    bool used;
    uint32_t *ring;
    uint32_t ring_size_dw;
    uint32_t *read_ptr;
    uint32_t next_offs_dw;
    uint32_t pipe, queue;
} compute_queue;
static compute_queue g_cq[NUM_COMPUTE_RINGS];

static uint64_t g_frames_submitted;

/* Where the game maps its tessellation factor ring (MAP_FIXED). shadPS4 puts
 * it 256 MiB below the end of the system-reserved range; we use the top of the
 * guest arena in memory.c (0x1000000000 + 0x4000000000), far above the bump
 * allocator. */
#define TESS_FACTOR_RING_ADDR (0x1000000000ull + 0x4000000000ull - 0x10000000ull)
#define TESS_OFFCHIP_BUFFER_SIZE 0x800000u

/* ---- events ------------------------------------------------------------- */

static SYSV int32_t ps4_sceGnmAddEqEvent(equeue *eq, uint64_t id, void *udata) {
    log_debug("sceGnmAddEqEvent(%p, %#llx)", (void *)eq, (unsigned long long)id);
    if (!eq) return SCE_KERNEL_ERROR(PS4_EBADF);
    pm4_add_eop_equeue(eq, id, udata);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmDeleteEqEvent(equeue *eq, uint64_t id) {
    if (!eq) return SCE_KERNEL_ERROR(PS4_EBADF);
    pm4_remove_eop_equeue(eq, id);
    return SCE_OK;
}

/* The event type (0x00-0x06 compute RELEASE_MEM, 0x40 graphics EOP) is the
 * ident the event was registered with; shadPS4 also stores it in `data`. */
static SYSV int32_t ps4_sceGnmGetEqEventType(const ps4_kevent *ev) {
    return ev ? (int32_t)ev->ident : 0;
}

static SYSV uint64_t ps4_sceGnmGetEqTimeStamp(const ps4_kevent *ev) {
    (void)ev;
    return 0;
}

/* ---- submission --------------------------------------------------------- */

static SYSV int32_t ps4_sceGnmAreSubmitsAllowed(void) { return 1; }

static int32_t validate_submit(uint32_t count, void *const *dcb, const uint32_t *dcb_sizes,
                               void *const *ccb, const uint32_t *ccb_sizes) {
    if (count != 0) {
        if (!dcb || !dcb_sizes) {
            log_error("gnm: dcbGpuAddrs and dcbSizesInBytes must not be NULL");
            return GNM_ERROR_SUBMISSION_FAILED_INVALID_ARGUMENT;
        }
        if (ccb_sizes && !ccb) {
            log_error("gnm: ccbGpuAddrs must not be NULL if ccbSizesInBytes is non-NULL");
            return GNM_ERROR_SUBMISSION_FAILED_INVALID_ARGUMENT;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (dcb_sizes[i] == 0) {
            log_error("gnm: submitting a null DCB %u", i);
            return GNM_ERROR_SUBMISSION_FAILED_INVALID_ARGUMENT;
        }
        if (dcb_sizes[i] > 0x3ffffc || (ccb_sizes && ccb_sizes[i] > 0x3ffffc)) {
            log_error("gnm: command buffer %u is over (2^20)-1 dwords", i);
            return GNM_ERROR_SUBMISSION_FAILED_INVALID_ARGUMENT;
        }
    }
    return SCE_OK;
}

static void perform_submit(uint32_t count, void *const *dcb, const uint32_t *dcb_sizes,
                           void *const *ccb, const uint32_t *ccb_sizes) {
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t *c = ccb ? (const uint32_t *)ccb[i] : NULL;
        uint32_t c_dw = ccb_sizes ? ccb_sizes[i] / 4 : 0;
        if (!c) c_dw = 0;
        pm4_submit(PM4_QUEUE_GFX, (const uint32_t *)dcb[i], dcb_sizes[i] / 4, c, c_dw, NULL,
                   NULL);
    }
}

/* A flip queued behind its command buffers: runs on the GPU queue thread. */
typedef struct pending_flip {
    int32_t handle, buf_idx;
    uint32_t mode;
    int64_t arg;
} pending_flip;

static void do_flip(void *p) {
    pending_flip *f = p;
    int32_t r = videoout_submit_flip(f->handle, f->buf_idx, f->mode, f->arg);
    if (r != SCE_OK) log_debug("gnm: flip of buffer %d failed: %#x", f->buf_idx, (uint32_t)r);
    free(f);
}

/* libGnm ends a flip submission with a 64-dword "prepareFlip" NOP. Rewrite it
 * like shadPS4's PatchFlipRequest so the executor sees the label write or EOP
 * interrupt the game asked for. shadPS4 also writes a VideoOut buffer label
 * ("surface locked") first; that needs the label address from VideoOut, so
 * here the leading part stays a NOP. */
static void patch_flip_request(uint32_t *cmdbuf, uint32_t size_dw) {
    if (size_dw < 64 || cmdbuf[size_dw - 64] != 0xc03e1000u) {
        log_warn("gnm: no prepareFlip packet at the end of the flip DCB");
        return;
    }
    uint32_t *p = cmdbuf + size_dw - 64;
    uint32_t b[7];
    memcpy(b, p, sizeof b);
    switch (b[1]) {
    case NOP_PREPARE_FLIP:
        trailing_nop(p, 63);
        p[1] = NOP_PATCHED_FLIP;
        break;
    case NOP_PREPARE_FLIP_LABEL: /* NOP p[0..58], WRITE_DATA p[59..63] */
        trailing_nop(p, 58);
        p[1] = NOP_PATCHED_FLIP;
        p[59] = pm4_type3(OP_WRITE_DATA, 4, ST_GFX, 0);
        p[60] = 0x500u; /* dst_sel memory */
        p[61] = b[2] & 0xfffffffcu;
        p[62] = b[3];
        p[63] = b[4];
        break;
    case NOP_PREPARE_FLIP_INTERRUPT_LABEL: /* NOP p[0..57], EVENT_WRITE_EOP p[58..63] */
    case NOP_PREPARE_FLIP_INTERRUPT:
        trailing_nop(p, 57);
        p[1] = NOP_PATCHED_FLIP;
        p[58] = pm4_type3(OP_EVENT_WRITE_EOP, 5, ST_GFX, 0);
        p[59] = (b[5] & 0x3fu) + 0x500u + (b[6] & 0x3fu) * 0x1000u;
        if (b[1] == NOP_PREPARE_FLIP_INTERRUPT_LABEL) {
            p[60] = b[2] & 0xfffffffcu;
            p[61] = (b[3] & 0xffffu) | 0x22000000u; /* data_sel 32-bit, int_sel irq */
            p[62] = b[4];
        } else {
            p[60] = 0;
            p[61] = 0x01000000u; /* interrupt only */
            p[62] = 0;
        }
        p[63] = 0;
        break;
    default:
        log_warn("gnm: unknown prepareFlip payload %#x", b[1]);
        break;
    }
}

static SYSV int32_t ps4_sceGnmSubmitCommandBuffersForWorkload(
    uint32_t workload, uint32_t count, void *const *dcb, const uint32_t *dcb_sizes,
    void *const *ccb, const uint32_t *ccb_sizes) {
    (void)workload;
    log_trace("sceGnmSubmitCommandBuffers(%u)", count);
    if (!dcb || !dcb_sizes) {
        log_error("gnm: dcbGpuAddrs and dcbSizesInBytes must not be NULL");
        return GNM_ERROR_SUBMISSION_FAILED_INVALID_ARGUMENT;
    }
    int32_t r = validate_submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
    if (r != SCE_OK) return r;
    gpu_bridge_init(); /* no-op after the first call; may wait for the window */
    plat_mutex *m = gnm_lock();
    plat_mutex_lock(m);
    perform_submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
    plat_mutex_unlock(m);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSubmitCommandBuffers(uint32_t count, void *const *dcb,
                                                   const uint32_t *dcb_sizes, void *const *ccb,
                                                   const uint32_t *ccb_sizes) {
    return ps4_sceGnmSubmitCommandBuffersForWorkload(count, count, dcb, dcb_sizes, ccb,
                                                     ccb_sizes);
}

static SYSV int32_t ps4_sceGnmSubmitAndFlipCommandBuffersForWorkload(
    uint32_t workload, uint32_t count, void *const *dcb, const uint32_t *dcb_sizes,
    void *const *ccb, const uint32_t *ccb_sizes, uint32_t vo_handle, uint32_t buf_idx,
    uint32_t flip_mode, int64_t flip_arg) {
    (void)workload;
    log_trace("sceGnmSubmitAndFlipCommandBuffers(%u, buf %u)", count, buf_idx);
    int32_t r = validate_submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
    if (r != SCE_OK) return r;

    gpu_bridge_init();
    plat_mutex *m = gnm_lock();
    plat_mutex_lock(m);
    if (count != 0) {
        patch_flip_request((uint32_t *)dcb[count - 1], dcb_sizes[count - 1] / 4);
        perform_submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
    }
    if (gpu_bridge_active()) {
        /* sotcgpu orders the flip after the work above (sotcgpu_flip with
         * after_gpu_work); VideoOut accounts it as a pending EOP flip now. */
        r = videoout_submit_flip((int32_t)vo_handle, (int32_t)buf_idx, flip_mode, flip_arg);
        plat_mutex_unlock(m);
        return r == SCE_OK ? SCE_OK : GNM_ERROR_SUBMISSION_AND_FLIP_FAILED_INVALID_QUEUE_FULL;
    }
    /* The flip happens once the GPU has run the buffers, like the real
     * hardware; a failure then can only be logged. */
    pending_flip *f = malloc(sizeof(*f));
    *f = (pending_flip){(int32_t)vo_handle, (int32_t)buf_idx, flip_mode, flip_arg};
    pm4_submit(PM4_QUEUE_GFX, NULL, 0, NULL, 0, do_flip, f);
    plat_mutex_unlock(m);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSubmitAndFlipCommandBuffers(
    uint32_t count, void *const *dcb, const uint32_t *dcb_sizes, void *const *ccb,
    const uint32_t *ccb_sizes, uint32_t vo_handle, uint32_t buf_idx, uint32_t flip_mode,
    int64_t flip_arg) {
    return ps4_sceGnmSubmitAndFlipCommandBuffersForWorkload(
        count, count, dcb, dcb_sizes, ccb, ccb_sizes, vo_handle, buf_idx, flip_mode, flip_arg);
}

/* Submissions run asynchronously on the pm4 queue threads; SubmitDone only
 * marks the end of a frame. */
static SYSV int32_t ps4_sceGnmSubmitDone(void) {
    uint64_t n = ++g_frames_submitted;
    log_trace("sceGnmSubmitDone #%llu", (unsigned long long)n);
    if (gpu_bridge_active()) sotcgpu_submit_done();
    return SCE_OK;
}

/* Used only with the "flip request on submitDone" mode; we flip at submit. */
HLE_STUB_OK(sceGnmRequestFlipAndSubmitDone)
HLE_STUB_OK(sceGnmRequestFlipAndSubmitDoneForWorkload)

/* ---- compute queues ----------------------------------------------------- */

static SYSV int32_t ps4_sceGnmMapComputeQueue(uint32_t pipe_id, uint32_t queue_id,
                                              void *ring_base, uint32_t ring_size_dw,
                                              uint32_t *read_ptr) {
    if (pipe_id >= NUM_COMPUTE_PIPES) return GNM_ERROR_COMPUTEQUEUE_INVALID_PIPE_ID;
    if (queue_id >= NUM_QUEUES_PER_PIPE) return GNM_ERROR_COMPUTEQUEUE_INVALID_QUEUE_ID;
    if ((uintptr_t)ring_base % 256 != 0) return GNM_ERROR_COMPUTEQUEUE_INVALID_RING_BASE_ADDR;
    if (ring_size_dw == 0 || (ring_size_dw & (ring_size_dw - 1)) != 0)
        return GNM_ERROR_COMPUTEQUEUE_INVALID_RING_SIZE;
    if ((uintptr_t)read_ptr % 4 != 0) return GNM_ERROR_COMPUTEQUEUE_INVALID_READ_PTR_ADDR;

    plat_mutex *m = gnm_lock();
    plat_mutex_lock(m);
    int32_t vqid = GNM_ERROR_COMPUTEQUEUE_INTERNAL;
    for (uint32_t i = 0; i < NUM_COMPUTE_RINGS; i++) {
        if (g_cq[i].used) continue;
        g_cq[i] = (compute_queue){true, ring_base, ring_size_dw, read_ptr, 0, pipe_id, queue_id};
        vqid = (int32_t)i + 1;
        if (read_ptr) *read_ptr = 0;
        break;
    }
    plat_mutex_unlock(m);
    log_info("gnm: compute pipe %u queue %u (ring %p, %u dwords) -> vqid %d", pipe_id, queue_id,
             ring_base, ring_size_dw, vqid);
    return vqid;
}

static SYSV int32_t ps4_sceGnmMapComputeQueueWithPriority(uint32_t pipe_id, uint32_t queue_id,
                                                          void *ring_base,
                                                          uint32_t ring_size_dw,
                                                          uint32_t *read_ptr,
                                                          uint32_t priority) {
    (void)priority;
    return ps4_sceGnmMapComputeQueue(pipe_id, queue_id, ring_base, ring_size_dw, read_ptr);
}

static SYSV int32_t ps4_sceGnmUnmapComputeQueue(uint32_t vqid) {
    if (vqid == 0 || vqid > NUM_COMPUTE_RINGS) return SCE_OK;
    plat_mutex *m = gnm_lock();
    plat_mutex_lock(m);
    g_cq[vqid - 1].used = false;
    plat_mutex_unlock(m);
    return SCE_OK;
}

typedef struct ring_progress {
    uint32_t *read_ptr;
    uint32_t offs;
} ring_progress;

static void advance_read_ptr(void *p) {
    ring_progress *rp = p;
    if (rp->read_ptr) __atomic_store_n(rp->read_ptr, rp->offs, __ATOMIC_RELEASE);
    free(rp);
}

/* The game wrote packets into the ring up to `next_offs_dw` (0 = the end of
 * the ring): run everything since the previous doorbell, then report it all
 * consumed through the read pointer. */
static SYSV void ps4_sceGnmDingDong(uint32_t vqid, uint32_t next_offs_dw) {
    log_trace("sceGnmDingDong(%u, %u)", vqid, next_offs_dw);
    if (vqid == 0 || vqid > NUM_COMPUTE_RINGS) return;
    gpu_bridge_init();
    plat_mutex *m = gnm_lock();
    plat_mutex_lock(m);
    compute_queue *q = &g_cq[vqid - 1];
    if (!q->used || next_offs_dw > q->ring_size_dw || next_offs_dw == q->next_offs_dw) {
        plat_mutex_unlock(m);
        return;
    }
    uint32_t offs = q->next_offs_dw;
    if (next_offs_dw < offs && next_offs_dw != 0) { /* wrapped: run the tail first */
        pm4_submit(vqid, q->ring + offs, q->ring_size_dw - offs, NULL, 0, NULL, NULL);
        offs = 0;
    }
    uint32_t end = next_offs_dw ? next_offs_dw : q->ring_size_dw;
    /* The read pointer advances once the GPU has consumed the packets. */
    ring_progress *rp = malloc(sizeof(*rp));
    *rp = (ring_progress){q->read_ptr, next_offs_dw % q->ring_size_dw};
    pm4_submit(vqid, end > offs ? q->ring + offs : NULL, end > offs ? end - offs : 0, NULL, 0,
               advance_read_ptr, rp);
    q->next_offs_dw = next_offs_dw;
    plat_mutex_unlock(m);
}

static SYSV void ps4_sceGnmDingDongForWorkload(uint32_t vqid, uint32_t next_offs_dw,
                                               uint64_t workload) {
    (void)workload;
    ps4_sceGnmDingDong(vqid, next_offs_dw);
}

/* ---- workloads ---------------------------------------------------------- */

static SYSV int32_t ps4_sceGnmCreateWorkloadStream(uint64_t name, uint32_t *workload_stream) {
    if (name != 0 && workload_stream) {
        *workload_stream = 1;
        return 0;
    }
    return 3;
}

static SYSV int32_t ps4_sceGnmBeginWorkload(uint32_t workload_stream, uint64_t *workload) {
    if (workload) {
        *workload = (uint64_t)(-(uint32_t)(workload_stream < 0x10) & 1);
        return 0xf < workload_stream;
    }
    return 3;
}

static SYSV int32_t ps4_sceGnmEndWorkload(uint64_t workload) {
    if (workload != 0) return (0xf < ((workload >> 0x38) & 0xff)) * 2;
    return 2;
}

HLE_STUB_OK(sceGnmDestroyWorkloadStream)

/* ---- dispatch ----------------------------------------------------------- */

static SYSV int32_t ps4_sceGnmDispatchDirect(uint32_t *cmdbuf, uint32_t size, uint32_t x,
                                             uint32_t y, uint32_t z, uint32_t flags) {
    if (!cmdbuf || size != 9 || (int32_t)(x | y | z) < 0) return -1;
    uint32_t *p = header(cmdbuf, OP_DISPATCH_DIRECT, 4, ST_CS, flags & 1);
    p[0] = x;
    p[1] = y;
    p[2] = z;
    p[3] = (flags & 0x18) + 1; /* ordered append mode */
    trailing_nop(p + 4, 3);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmDispatchIndirect(uint32_t *cmdbuf, uint32_t size,
                                               uint32_t data_offset, uint32_t flags) {
    if (!cmdbuf || size != 7) return -1;
    uint32_t *p = header(cmdbuf, OP_DISPATCH_INDIRECT, 2, ST_CS, flags & 1);
    p[0] = data_offset;
    p[1] = (flags & 0x18) + 1;
    trailing_nop(p + 2, 3);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmDispatchIndirectOnMec(uint32_t *cmdbuf, uint32_t size,
                                                    uint64_t args, uint32_t modifier) {
    if (!cmdbuf || size != 8 || args == 0 || (args & 3) != 0) return -1;
    cmdbuf[0] = 0xc0021602u | (modifier & 1u); /* DISPATCH_INDIRECT, 3 dwords, compute */
    cmdbuf[1] = (uint32_t)args;
    cmdbuf[2] = (uint32_t)(args >> 32);
    cmdbuf[3] = (modifier & 0x18) | 1u;
    trailing_nop(cmdbuf + 4, 3);
    return SCE_OK;
}

static SYSV uint32_t ps4_sceGnmDispatchInitDefaultHardwareState(uint32_t *cmdbuf, uint32_t size) {
    if (!cmdbuf || size < HW_INIT_PACKET_SIZE) return 0;
    uint32_t *p = cmdbuf;
    p = SET_SH_CS(p, 0x216u, 0xffffffffu); /* COMPUTE_STATIC_THREAD_MGMT_SE0 */
    p = SET_SH_CS(p, 0x217u, 0xffffffffu); /* COMPUTE_STATIC_THREAD_MGMT_SE1 */
    if (is_neo()) {
        p = SET_SH_CS(p, 0x219u, 0xffffffffu); /* SE2 */
        p = SET_SH_CS(p, 0x21au, 0xffffffffu); /* SE3 */
    }
    p = SET_SH_CS(p, 0x215u, 0x170u); /* COMPUTE_RESOURCE_LIMITS */
    p = PKT(p, OP_ACQUIRE_MEM, ST_GFX, 0x28000000u, 0u, 0u, 0u, 0u, 0xau);
    trailing_nop(p, (uint32_t)(cmdbuf + HW_INIT_PACKET_SIZE - p) - 1);
    return HW_INIT_PACKET_SIZE;
}

static SYSV int32_t ps4_sceGnmComputeWaitOnAddress(uint32_t *cmdbuf, uint32_t size, uint64_t addr,
                                                   uint32_t mask, uint32_t cmp_func,
                                                   uint32_t ref) {
    if (!cmdbuf || size != 0xe) return -1;
    uint32_t *p = trailing_nop(cmdbuf, 3);
    const uint32_t is_mem = addr > 0xffffu;
    const uint32_t addr_mask = is_mem ? 0xfffffffcu : 0xffffu;
    p[0] = pm4_type3(OP_WAIT_REG_MEM, 6, ST_GFX, 0);
    p[1] = (is_mem << 4) | (cmp_func & 7u);
    p[2] = (uint32_t)addr & addr_mask;
    p[3] = (uint32_t)(addr >> 32);
    p[4] = ref;
    p[5] = mask;
    p[6] = 10u; /* poll interval */
    trailing_nop(p + 7, 2);
    return SCE_OK;
}

/* ---- draws -------------------------------------------------------------- */

enum { STAGE_CS, STAGE_PS, STAGE_VS, STAGE_GS, STAGE_ES, STAGE_HS, STAGE_LS, STAGE_MAX };
static const uint32_t k_indirect_sgpr_offsets[STAGE_MAX] = {0, 0, 0x4c, 0, 0xcc, 0, 0x14c};

static uint32_t neo_flags(uint32_t flags) { return is_neo() ? flags & 0xe0000000u : 0u; }

static uint32_t sgpr(uint32_t off, uint32_t stage) {
    return off == 0 ? 0 : (off & 0xffffu) + k_indirect_sgpr_offsets[stage];
}

static SYSV int32_t ps4_sceGnmDrawIndex(uint32_t *cmdbuf, uint32_t size, uint32_t index_count,
                                        uint64_t index_addr, uint32_t flags, uint32_t type) {
    (void)type;
    if (!cmdbuf || size != 10 || index_addr == 0 || (index_addr & 1) != 0 ||
        (flags & 0x1ffffffe) != 0)
        return -1;
    cmdbuf = PKT(cmdbuf, OP_DRAW_INDEX_2, ST_GFX, index_count, (uint32_t)index_addr,
                 (uint32_t)(index_addr >> 32), index_count, neo_flags(flags));
    trailing_nop(cmdbuf, 3);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmDrawIndexAuto(uint32_t *cmdbuf, uint32_t size,
                                            uint32_t index_count, uint32_t flags) {
    if (!cmdbuf || size != 7 || (flags & 0x1ffffffe) != 0) return -1;
    cmdbuf = PKT(cmdbuf, OP_DRAW_INDEX_AUTO, ST_GFX, index_count, neo_flags(flags) | 2u);
    trailing_nop(cmdbuf, 3);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmDrawIndexOffset(uint32_t *cmdbuf, uint32_t size,
                                              uint32_t index_offset, uint32_t index_count,
                                              uint32_t flags) {
    if (!cmdbuf || size != 9) return -1;
    uint32_t *p = header(cmdbuf, OP_DRAW_INDEX_OFFSET_2, 4, ST_GFX, flags & 1);
    p[0] = index_count;
    p[1] = index_offset;
    p[2] = index_count;
    p[3] = neo_flags(flags);
    trailing_nop(p + 4, 3);
    return SCE_OK;
}

static int32_t draw_indirect(uint32_t *cmdbuf, uint32_t size, uint32_t op, uint32_t data_offset,
                             uint32_t stage, uint32_t vtx_sgpr, uint32_t inst_sgpr,
                             uint32_t flags, uint32_t last) {
    if (!cmdbuf || size != 9 || stage >= STAGE_MAX || vtx_sgpr >= 0x10 || inst_sgpr >= 0x10)
        return -1;
    uint32_t *p = header(cmdbuf, op, 4, ST_GFX, flags & 1);
    p[0] = data_offset;
    p[1] = sgpr(vtx_sgpr, stage);
    p[2] = sgpr(inst_sgpr, stage);
    p[3] = last;
    trailing_nop(p + 4, 3);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmDrawIndexIndirect(uint32_t *cmdbuf, uint32_t size,
                                                uint32_t data_offset, uint32_t stage,
                                                uint32_t vtx_sgpr, uint32_t inst_sgpr,
                                                uint32_t flags) {
    return draw_indirect(cmdbuf, size, OP_DRAW_INDEX_INDIRECT, data_offset, stage, vtx_sgpr,
                         inst_sgpr, flags, neo_flags(flags));
}

static SYSV int32_t ps4_sceGnmDrawIndirect(uint32_t *cmdbuf, uint32_t size, uint32_t data_offset,
                                           uint32_t stage, uint32_t vtx_sgpr, uint32_t inst_sgpr,
                                           uint32_t flags) {
    return draw_indirect(cmdbuf, size, OP_DRAW_INDIRECT, data_offset, stage, vtx_sgpr,
                         inst_sgpr, flags, neo_flags(flags) | 2u /* auto index */);
}

static SYSV int32_t ps4_sceGnmDrawIndexIndirectCountMulti(
    uint32_t *cmdbuf, uint32_t size, uint32_t data_offset, uint32_t max_count,
    uint64_t count_addr, uint32_t stage, uint32_t vtx_sgpr, uint32_t inst_sgpr, uint32_t flags) {
    if (!cmdbuf || size != 16 || vtx_sgpr >= 0x10 || inst_sgpr >= 0x10 ||
        (stage != STAGE_VS && stage != STAGE_ES && stage != STAGE_LS))
        return -1;
    uint32_t *p = trailing_nop(cmdbuf, 2);
    p = header(p, OP_DRAW_INDEX_INDIRECT_COUNT_MULTI, 9, ST_GFX, flags & 1);
    p[0] = data_offset;
    p[1] = sgpr(vtx_sgpr, stage);
    p[2] = sgpr(inst_sgpr, stage);
    p[3] = (count_addr != 0 ? 1u : 0u) << 30;
    p[4] = max_count;
    p[5] = (uint32_t)count_addr;
    p[6] = (uint32_t)(count_addr >> 32);
    p[7] = 20; /* sizeof(DrawIndexedIndirectArgs) */
    p[8] = neo_flags(flags);
    trailing_nop(p + 9, 2);
    return SCE_OK;
}

/* Builders shadPS4 leaves unimplemented (SotC imports but may never call
 * them). They follow the usual (cmdbuf, size, ...) shape, so fill the
 * reserved space with a NOP to keep the stream well-formed, and log the first
 * call so the arguments can be checked. */
static int32_t nop_builder(const char *name, atomic_int *logged, uint32_t *cmdbuf,
                           uint32_t size, uint64_t a2, uint64_t a3) {
    if (!atomic_exchange(logged, 1))
        log_warn("gnm: %s(%p, %u, %#llx, %#llx) -> NOP fill (logged once)", name,
                 (void *)cmdbuf, size, (unsigned long long)a2, (unsigned long long)a3);
    if ((uintptr_t)cmdbuf < 0x10000 || size < 2 || size > 0x4000) return -1;
    fill_nop(cmdbuf, size);
    return SCE_OK;
}

#define NOP_BUILDER(n)                                                                    \
    static SYSV int32_t ps4_##n(uint32_t *cmdbuf, uint32_t size, uint64_t a2, uint64_t a3) { \
        static atomic_int logged;                                                         \
        return nop_builder(#n, &logged, cmdbuf, size, a2, a3);                            \
    }
NOP_BUILDER(sceGnmDrawIndexMultiInstanced)
NOP_BUILDER(sceGnmDrawIndirectCountMulti)
NOP_BUILDER(sceGnmDrawOpaqueAuto)
NOP_BUILDER(sceGnmComputeWaitSemaphore)
NOP_BUILDER(sceGnmInsertSetColorMarker)

/* ---- hardware state ----------------------------------------------------- */

static SYSV uint32_t ps4_sceGnmDrawInitDefaultHardwareState350(uint32_t *cmdbuf, uint32_t size) {
    if (!cmdbuf || size < HW_INIT_PACKET_SIZE) return 0;
    uint32_t *p = cmdbuf;
    memcpy(p, k_clear_state_sequence, sizeof k_clear_state_sequence);
    p += COUNT(k_clear_state_sequence);
    if (is_neo()) {
        memcpy(p, k_init350_neo, sizeof k_init350_neo);
        p += COUNT(k_init350_neo);
    } else {
        memcpy(p, k_init350, sizeof k_init350);
        p += COUNT(k_init350);
    }
    trailing_nop(p, (uint32_t)(cmdbuf + HW_INIT_PACKET_SIZE - p) - 1);
    return HW_INIT_PACKET_SIZE;
}

static SYSV uint32_t ps4_sceGnmDrawInitToDefaultContextState400(uint32_t *cmdbuf,
                                                                uint32_t size) {
    if (!cmdbuf || size != 0x100) return 0;
    const uint32_t *seq = is_neo() ? k_ctx_init400_neo : k_ctx_init400;
    const uint32_t n = is_neo() ? COUNT(k_ctx_init400_neo) : COUNT(k_ctx_init400);
    memcpy(cmdbuf, seq, n * sizeof(uint32_t));
    /* The final NOP's body runs to 0x100 dwords; clear it for determinism. */
    memset(cmdbuf + n, 0, (0x100 - n) * sizeof(uint32_t));
    return 0x100;
}

static SYSV int32_t ps4_sceGnmResetVgtControl(uint32_t *cmdbuf, uint32_t size) {
    if (!cmdbuf || size != 3) return -1;
    if (is_neo()) SET_UCONFIG(cmdbuf, 0x40000258u, 0x6d007fu); /* IA_MULTI_VGT_PARAM */
    else SET_CTX(cmdbuf, 0x2aau, 0xffu);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSetVgtControl(uint32_t *cmdbuf, uint32_t size,
                                            uint32_t prim_group_sz_minus_one,
                                            uint32_t partial_vs_wave_mode,
                                            uint32_t wd_switch_only_on_eop_mode) {
    if (!cmdbuf || size != 3 || prim_group_sz_minus_one >= 0x100 ||
        (wd_switch_only_on_eop_mode | partial_vs_wave_mode) >= 2)
        return -1;
    if (is_neo()) {
        const uint32_t wd_switch_on_eop = (uint32_t)(wd_switch_only_on_eop_mode != 0) << 20;
        const uint32_t switch_on_eoi = (uint32_t)(wd_switch_only_on_eop_mode == 0) << 19;
        const uint32_t v =
            wd_switch_only_on_eop_mode != 0
                ? ((partial_vs_wave_mode & 1) << 16) | prim_group_sz_minus_one |
                      wd_switch_on_eop | switch_on_eoi | 0x40000u
                : (prim_group_sz_minus_one & 0x1cffffu) | wd_switch_on_eop | switch_on_eoi |
                      0x50000u;
        SET_UCONFIG(cmdbuf, 0x40000258u, v | 0x600000u);
    } else {
        SET_CTX(cmdbuf, 0x2aau, ((partial_vs_wave_mode & 1) << 16) |
                                    (prim_group_sz_minus_one & 0xffffu));
    }
    return SCE_OK;
}

/* ---- shaders ------------------------------------------------------------ */

static SYSV int32_t ps4_sceGnmSetCsShaderWithModifier(uint32_t *cmdbuf, uint32_t size,
                                                      const uint32_t *cs, uint32_t modifier) {
    if (!cmdbuf || size <= 0x18 || !cs) return -1;
    if ((modifier & 0xfffffc3fu) != 0 || cs[1] != 0) return -1;
    const uint32_t rsrc1 = modifier == 0 ? cs[2] : (cs[2] & 0xfffffc3fu) | modifier;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x20cu, cs[0], 0u);           /* COMPUTE_PGM_LO/HI */
    p = SET_SH(p, 0x212u, rsrc1, cs[3]);        /* COMPUTE_PGM_RSRC1/2 */
    p = SET_SH(p, 0x207u, cs[4], cs[5], cs[6]); /* COMPUTE_NUM_THREAD_X/Y/Z */
    trailing_nop(p, 11);
    return SCE_OK;
}

static uint32_t *ps_regs_full(uint32_t *p, const uint32_t *ps) {
    p = SET_SH(p, 8u, ps[0], ps[1]);     /* SPI_SHADER_PGM_LO/HI_PS */
    p = SET_SH(p, 10u, ps[2], ps[3]);    /* SPI_SHADER_PGM_RSRC1/2_PS */
    p = SET_CTX(p, 0x1c4u, ps[4], ps[5]); /* SPI_SHADER_Z_FORMAT/COL_FORMAT */
    p = SET_CTX(p, 0x1b3u, ps[6], ps[7]); /* SPI_PS_INPUT_ENA/ADDR */
    p = SET_CTX(p, 0x1b6u, ps[8]);        /* SPI_PS_IN_CONTROL */
    p = SET_CTX(p, 0x1b8u, ps[9]);        /* SPI_BARYC_CNTL */
    p = SET_CTX(p, 0x203u, ps[10]);       /* DB_SHADER_CONTROL */
    p = SET_CTX(p, 0x8fu, ps[11]);        /* CB_SHADER_MASK */
    return p;
}

static SYSV int32_t ps4_sceGnmSetPsShader350(uint32_t *cmdbuf, uint32_t size,
                                             const uint32_t *ps) {
    if (!cmdbuf || size <= 0x27) return -1;
    uint32_t *p = cmdbuf;
    if (!ps) {
        p = SET_SH(p, 8u, 0u, 0u);
        p = SET_CTX(p, 0x203u, 0u);
        p = SET_CTX(p, 0x8fu, 0xfu);
        trailing_nop(p, 0x1d);
        return SCE_OK;
    }
    if (ps[1] != 0) return -1;
    const uint32_t regs[12] = {ps[0], 0u, ps[2], ps[3], ps[4],  ps[5],
                               ps[6], ps[7], ps[8], ps[9], ps[10], ps[11]};
    p = ps_regs_full(p, regs);
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmUpdatePsShader350(uint32_t *cmdbuf, uint32_t size,
                                                const uint32_t *ps) {
    if (!cmdbuf || size <= 0x27) return -1;
    uint32_t *p = cmdbuf;
    if (!ps) {
        p = SET_SH(p, 8u, 0u, 0u);
        p = NOP_PKT(p, 0xc01e0203u, 0u); /* DB_SHADER_CONTROL update */
        p = SET_CTX(p, 0x8fu, 0xfu);
        trailing_nop(p, 0x1d);
        return SCE_OK;
    }
    if (ps[1] != 0) return -1;
    p = SET_SH(p, 8u, ps[0], 0u);
    p = SET_SH(p, 10u, ps[2], ps[3]);
    p = NOP_PKT(p, 0xc01e01c4u, ps[4], ps[5]);
    p = NOP_PKT(p, 0xc01e01b3u, ps[6], ps[7]);
    p = NOP_PKT(p, 0xc01e01b6u, ps[8]);
    p = NOP_PKT(p, 0xc01e01b8u, ps[9]);
    p = NOP_PKT(p, 0xc01e0203u, ps[10]);
    p = NOP_PKT(p, 0xc01e008fu, ps[11]);
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSetVsShader(uint32_t *cmdbuf, uint32_t size, const uint32_t *vs,
                                          uint32_t modifier) {
    if (!cmdbuf || size <= 0x1c || !vs) return -1;
    if ((modifier & 0xfcfffc3fu) != 0 || vs[1] != 0) return -1;
    const uint32_t rsrc1 = modifier == 0 ? vs[2] : (vs[2] & 0xfcfffc3fu) | modifier;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x48u, vs[0], 0u);    /* SPI_SHADER_PGM_LO/HI_VS */
    p = SET_SH(p, 0x4au, rsrc1, vs[3]); /* SPI_SHADER_PGM_RSRC1/2_VS */
    p = SET_CTX(p, 0x207u, vs[6]);      /* PA_CL_VS_OUT_CNTL */
    p = SET_CTX(p, 0x1b1u, vs[4]);      /* SPI_VS_OUT_CONFIG */
    p = SET_CTX(p, 0x1c3u, vs[5]);      /* SPI_SHADER_POS_FORMAT */
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmUpdateVsShader(uint32_t *cmdbuf, uint32_t size,
                                             const uint32_t *vs, uint32_t modifier) {
    if (!cmdbuf || size <= 0x1c || !vs) return -1;
    if ((modifier & 0xfcfffc3fu) != 0 || vs[1] != 0) return -1;
    const uint32_t rsrc1 = modifier == 0 ? vs[2] : (vs[2] & 0xfcfffc3fu) | modifier;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x48u, vs[0], 0u);
    p = SET_SH(p, 0x4au, rsrc1, vs[3]);
    p = NOP_PKT(p, 0xc01e0207u, vs[6]);
    p = NOP_PKT(p, 0xc01e01b1u, vs[4]);
    p = NOP_PKT(p, 0xc01e01c3u, vs[5]);
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSetEsShader(uint32_t *cmdbuf, uint32_t size, const uint32_t *es,
                                          uint32_t modifier) {
    if (!cmdbuf || size < 0x14 || !es) return -1;
    if ((modifier & 0xfcfffc3fu) != 0 || es[1] != 0) return -1;
    const uint32_t rsrc1 = modifier == 0 ? es[2] : (es[2] & 0xfcfffc3fu) | modifier;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0xc8u, es[0], 0u);    /* SPI_SHADER_PGM_LO/HI_ES */
    p = SET_SH(p, 0xcau, rsrc1, es[3]); /* SPI_SHADER_PGM_RSRC1/2_ES */
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSetGsShader(uint32_t *cmdbuf, uint32_t size, const uint32_t *gs) {
    if (!cmdbuf || size < 0x1d || !gs || gs[1] != 0) return -1;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x88u, gs[0], 0u);    /* SPI_SHADER_PGM_LO/HI_GS */
    p = SET_SH(p, 0x8au, gs[2], gs[3]); /* SPI_SHADER_PGM_RSRC1/2_GS */
    p = SET_CTX(p, 0x2e5u, gs[4]);      /* VGT_STRMOUT_CONFIG */
    p = SET_CTX(p, 0x29bu, gs[5]);      /* VGT_GS_OUT_PRIM_TYPE */
    p = SET_CTX(p, 0x2e4u, gs[6]);      /* VGT_GS_INSTANCE_CNT */
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmUpdateGsShader(uint32_t *cmdbuf, uint32_t size,
                                             const uint32_t *gs) {
    if (!cmdbuf || size < 0x1d || !gs || gs[1] != 0) return -1;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x88u, gs[0], 0u);
    p = SET_SH(p, 0x8au, gs[2], gs[3]);
    p = NOP_PKT(p, 0xc01e02e5u, gs[4]);
    p = NOP_PKT(p, 0xc01e029bu, gs[5]);
    p = NOP_PKT(p, 0xc01e02e4u, gs[6]);
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSetHsShader(uint32_t *cmdbuf, uint32_t size, const uint32_t *hs,
                                          uint32_t ls_hs_config) {
    if (!cmdbuf || size < 0x1e || !hs || hs[1] != 0) return -1;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x108u, hs[0], 0u);       /* SPI_SHADER_PGM_LO/HI_HS */
    p = SET_SH(p, 0x10au, hs[2], hs[3]);    /* SPI_SHADER_PGM_RSRC1/2_HS */
    p = SET_CTX(p, 0x286u, hs[5], hs[6]);   /* VGT_HOS_MAX/MIN_TESS_LEVEL */
    p = SET_CTX(p, 0x2dbu, hs[4]);          /* VGT_TF_PARAM */
    p = SET_CTX(p, 0x2d6u, ls_hs_config);   /* VGT_LS_HS_CONFIG */
    trailing_nop(p, 11);
    return SCE_OK;
}

/* shadPS4 accepts size 0x1d here but writes 0x1e dwords; require 0x1e. */
static SYSV int32_t ps4_sceGnmUpdateHsShader(uint32_t *cmdbuf, uint32_t size, const uint32_t *hs,
                                             uint32_t ls_hs_config) {
    if (!cmdbuf || size < 0x1e || !hs || hs[1] != 0) return -1;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x108u, hs[0], 0u);
    p = SET_SH(p, 0x10au, hs[2], hs[3]);
    p = NOP_PKT(p, 0xc01e0286u, hs[5], hs[6]);
    p = NOP_PKT(p, 0xc01e02dbu, hs[4]);
    p = NOP_PKT(p, 0xc01e02d6u, ls_hs_config);
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSetLsShader(uint32_t *cmdbuf, uint32_t size, const uint32_t *ls,
                                          uint32_t modifier) {
    if (!cmdbuf || size < 0x17 || !ls) return -1;
    const uint32_t mask = (modifier & 0xfffffc3fu) == 0 ? 0xfffffc3fu : 0xfcfffc3fu;
    if ((modifier & mask) != 0 || ls[1] != 0) return -1;
    const uint32_t rsrc1 = modifier == 0 ? ls[2] : (ls[2] & mask) | modifier;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x148u, ls[0], 0u);    /* SPI_SHADER_PGM_LO/HI_LS */
    p = SET_SH(p, 0x14bu, ls[3]);        /* SPI_SHADER_PGM_RSRC2_LS */
    p = SET_SH(p, 0x14au, rsrc1, ls[3]); /* SPI_SHADER_PGM_RSRC1/2_LS */
    trailing_nop(p, 11);
    return SCE_OK;
}

/* Embedded shaders (shadPS4): PS 0 writes zero to mrt0, PS 1 a single
 * channel; VS 0 is a fullscreen triangle. The code lives in host memory, so
 * the register writes are done here instead of via SetPsShader350, which
 * rejects addresses with high bits. */
static const uint32_t k_ps0_code[] __attribute__((aligned(256))) = {
    0xbeeb03ffu, 0x00000003u, 0x7e000280u, 0x5e000100u, 0xbf800000u, 0xf8001c0fu,
    0x00000000u, 0xbf810000u,
    0x5362724fu, 0x07726468u, 0x00002043u, 0u, 0xb0a45b2bu, 0x1d39766du, 0x72044b7bu,
    0x0000000fu,
    0x0fe000f0u, 0u, 0xc0000u, 4u, 0u, 4u, 2u, 2u, 0u, 0u, 0x10u, 0xfu, 0xcu, 0u, 0u, 0u,
};
static const uint32_t k_ps1_code[] __attribute__((aligned(256))) = {
    0xbeeb03ffu, 0x00000003u, 0x7e040280u, 0xf8001803u, 0x02020202u, 0xbf810000u,
    0x5362724fu, 0x07726468u, 0x00001841u, 0x04080002u, 0x98b9cb94u, 0u, 0x6f130734u,
    0x0000000fu,
    0x0fe000f2u, 0u, 0x2000u, 0u, 0u, 2u, 2u, 2u, 0u, 0u, 0x10u, 3u, 0xcu,
};
static const uint32_t k_vs0_code[] __attribute__((aligned(256))) = {
    0xbeeb03ffu, 0x00000007u, 0x36020081u, 0x34020281u, 0x360000c2u, 0x4a0202c1u,
    0x4a0000c1u, 0x7e020b01u, 0x7e000b00u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu,
    0x03020001u, 0xf800020fu, 0x03030303u, 0xbf810000u,
    0x5362724fu, 0x07726468u, 0x00004047u, 0u, 0x47f8c29fu, 0x9b2da5cfu, 0xff7c5b7du,
    0x00000017u,
    0x0fe000f1u, 0u, 0x000c0000u, 4u, 0u, 4u, 0u, 7u,
};

static SYSV int32_t ps4_sceGnmSetEmbeddedPsShader(uint32_t *cmdbuf, uint32_t size,
                                                  uint32_t shader_id, uint32_t modifier) {
    (void)modifier;
    if (shader_id > 1) {
        log_error("gnm: unknown embedded PS %u", shader_id);
        return GNM_ERROR_FAILURE;
    }
    if (!cmdbuf || size < 0x28) return -1;
    const uintptr_t a = (uintptr_t)(shader_id == 0 ? k_ps0_code : k_ps1_code);
    const uint32_t regs0[12] = {(uint32_t)(a >> 8), (uint32_t)((uint64_t)a >> 40), 0xc0000u, 4u,
                                0u, 4u, 2u, 2u, 0u, 0u, 0x10u, 0xfu};
    const uint32_t regs1[12] = {(uint32_t)(a >> 8), (uint32_t)((uint64_t)a >> 40), 0x2000u, 0u,
                                0u, 2u, 2u, 2u, 0u, 0u, 0x10u, 3u};
    uint32_t *p = ps_regs_full(cmdbuf, shader_id == 0 ? regs0 : regs1);
    trailing_nop(p, 11);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmSetEmbeddedVsShader(uint32_t *cmdbuf, uint32_t size,
                                                  uint32_t shader_id, uint32_t modifier) {
    (void)modifier;
    if (shader_id != 0) {
        log_error("gnm: unknown embedded VS %u", shader_id);
        return GNM_ERROR_FAILURE;
    }
    if (!cmdbuf || size < 0x1d) return -1;
    const uintptr_t a = (uintptr_t)k_vs0_code;
    uint32_t *p = cmdbuf;
    p = SET_SH(p, 0x48u, (uint32_t)(a >> 8), (uint32_t)((uint64_t)a >> 40));
    p = SET_SH(p, 0x4au, 0xc0000u, 4u);
    p = SET_CTX(p, 0x207u, 0u); /* PA_CL_VS_OUT_CNTL */
    p = SET_CTX(p, 0x1b1u, 0u); /* SPI_VS_OUT_CONFIG */
    p = SET_CTX(p, 0x1c3u, 4u); /* SPI_SHADER_POS_FORMAT */
    trailing_nop(p, 11);
    return SCE_OK;
}

/* ---- markers ------------------------------------------------------------ */

static SYSV int32_t ps4_sceGnmInsertDingDongMarker(uint32_t *cmdbuf, uint32_t size) {
    if (!cmdbuf || size != 4) return -1;
    NOP_PKT(cmdbuf, 0u, 0u, 0u);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmInsertPopMarker(uint32_t *cmdbuf, uint32_t size) {
    if (!cmdbuf || size != 6) return -1;
    NOP_PKT(cmdbuf, NOP_DEBUG_MARKER_POP, 0u, 0u, 0u, 0u);
    return SCE_OK;
}

/* NOP { payload, marker string, zero padding }: packet_size + 2 dwords. */
static int32_t string_marker(uint32_t *cmdbuf, uint32_t size, const char *marker,
                             uint32_t payload) {
    if (!cmdbuf || !marker) return -1;
    const size_t len = strlen(marker);
    const uint32_t packet_size = (uint32_t)(((len + 8) >> 2) + ((len + 0xc) >> 3) * 2);
    if (packet_size + 2 != size) return -1;
    trailing_nop(cmdbuf, packet_size + 1);
    cmdbuf[1] = payload;
    memcpy(cmdbuf + 2, marker, len + 1);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmInsertPushMarker(uint32_t *cmdbuf, uint32_t size,
                                               const char *marker) {
    return string_marker(cmdbuf, size, marker, NOP_DEBUG_MARKER_PUSH);
}

static SYSV int32_t ps4_sceGnmInsertSetMarker(uint32_t *cmdbuf, uint32_t size,
                                              const char *marker) {
    return string_marker(cmdbuf, size, marker, NOP_DEBUG_SET_MARKER);
}

static SYSV int32_t ps4_sceGnmInsertPushColorMarker(uint32_t *cmdbuf, uint32_t size,
                                                    const char *marker, uint32_t color) {
    if (!cmdbuf || !marker) return -1;
    const size_t len = strlen(marker);
    const uint32_t packet_size = (uint32_t)(((len + 0xc) >> 2) + ((len + 0x10) >> 3) * 2);
    if (packet_size + 2 != size) return -1;
    trailing_nop(cmdbuf, packet_size + 1);
    cmdbuf[1] = NOP_DEBUG_COLOR_MARKER_PUSH;
    uint8_t *s = (uint8_t *)(cmdbuf + 2);
    memcpy(s, marker, len + 1);
    memcpy(s + len + 1 + 8, &color, sizeof color);
    return SCE_OK;
}

/* Waits for VideoOut buffer `buf_idx` to stop being displayed. shadPS4 emits a
 * WAIT_REG_MEM on the VideoOut buffer label; pm4.h has no way to get that
 * address yet, and the null GPU never has a flip in flight while the CPU
 * builds commands, so this is a 7-dword NOP. */
static SYSV int32_t ps4_sceGnmInsertWaitFlipDone(uint32_t *cmdbuf, uint32_t size,
                                                 int32_t vo_handle, uint32_t buf_idx) {
    (void)vo_handle; (void)buf_idx;
    if (!cmdbuf || size != 7) return -1;
    fill_nop(cmdbuf, 7);
    return SCE_OK;
}

static SYSV int32_t ps4_sceGnmInsertThreadTraceMarker(void) { return SCE_OK; }

/* ---- queries ------------------------------------------------------------ */

static SYSV uint64_t ps4_sceGnmGetTheTessellationFactorRingBufferBaseAddress(void) {
    log_debug("sceGnmGetTheTessellationFactorRingBufferBaseAddress -> %#llx",
              (unsigned long long)TESS_FACTOR_RING_ADDR);
    return TESS_FACTOR_RING_ADDR;
}

static SYSV uint32_t ps4_sceGnmGetOffChipTessellationBufferSize(void) {
    return TESS_OFFCHIP_BUFFER_SIZE;
}

static SYSV int32_t ps4_sceGnmLogicalCuMaskToPhysicalCuMask(int64_t se, int32_t mask) {
    (void)se;
    return mask;
}

static SYSV int32_t ps4_sceGnmLogicalCuIndexToPhysicalCuIndex(void) { return SCE_OK; }
static SYSV int32_t ps4_sceGnmGetGpuBlockStatus(void) { return SCE_OK; }
static SYSV int32_t ps4_sceGnmGetGpuInfoStatus(void) { return SCE_OK; }
static SYSV int32_t ps4_sceGnmGetLastWaitedAddress(void) { return SCE_OK; }
static SYSV int32_t ps4_sceGnmGetShaderProgramBaseAddress(void) { return SCE_OK; }
static SYSV int32_t ps4_sceGnmGetShaderStatus(void) { return SCE_OK; }
static SYSV uint32_t ps4_sceGnmGetProtectionFaultTimeStamp(void) { return 0; }
static SYSV bool ps4_sceGnmIsUserPaEnabled(void) { return false; }
static SYSV void ps4_sceGnmFlushGarlic(void) {}

HLE_STUB_OK(sceGnmGetNumTcaUnits)
HLE_STUB_OK(sceGnmLogicalTcaUnitToPhysical)
HLE_STUB_OK(sceGnmDebugHardwareStatus)
HLE_STUB_OK(sceGnmSetGsRingSizes)
HLE_STUB_OK(sceGnmSetWaveLimitMultipliers)
HLE_STUB_OK(sceGnmSetupMipStatsReport)
HLE_STUB_OK(sceGnmDisableMipStatsReport)
HLE_STUB_OK(sceGnmRequestMipStatsReportAndReset)

/* ---- debug, capture and resource registration ---------------------------
 * Not available in retail firmware: they fail exactly as there. */

static SYSV bool ps4_sceGnmDriverCaptureInProgress(void) { return false; }
static SYSV bool ps4_sceGnmDriverTraceInProgress(void) { return false; }
static SYSV int32_t ps4_sceGnmDriverTriggerCapture(void) {
    return GNM_ERROR_CAPTURE_RAZOR_NOT_LOADED;
}
static SYSV int32_t ps4_sceRazorCaptureImmediate(void) { return GNM_ERROR_CAPTURE_FAILED_INTERNAL; }
static SYSV bool ps4_sceRazorIsLoaded(void) { return false; }

#define GNM_UNAVAILABLE(n)                                                               \
    static SYSV int32_t ps4_##n(void) {                                                  \
        log_debug(#n " -> unavailable on retail");                                       \
        return GNM_ERROR_FAILURE;                                                        \
    }
GNM_UNAVAILABLE(sceGnmRegisterOwner)
GNM_UNAVAILABLE(sceGnmRegisterResource)
GNM_UNAVAILABLE(sceGnmRegisterGdsResource)
GNM_UNAVAILABLE(sceGnmUnregisterResource)
GNM_UNAVAILABLE(sceGnmUnregisterAllResourcesForOwner)
GNM_UNAVAILABLE(sceGnmUnregisterOwnerAndResources)
GNM_UNAVAILABLE(sceGnmFindResourcesPublic)
GNM_UNAVAILABLE(sceGnmGetOwnerName)
GNM_UNAVAILABLE(sceGnmGetPhysicalCounterFromVirtualized)
GNM_UNAVAILABLE(sceGnmGetResourceBaseAddressAndSizeInBytes)
GNM_UNAVAILABLE(sceGnmGetResourceName)
GNM_UNAVAILABLE(sceGnmGetResourceShaderGuid)
GNM_UNAVAILABLE(sceGnmGetResourceType)
GNM_UNAVAILABLE(sceGnmGetResourceUserData)
GNM_UNAVAILABLE(sceGnmSetResourceUserData)
GNM_UNAVAILABLE(sceGnmQueryResourceRegistrationUserMemoryRequirements)
GNM_UNAVAILABLE(sceGnmSetResourceRegistrationUserMemory)

HLE_LIBRARY(hle_gnmdriver, "libSceGnmDriver",
    HLE(sceGnmAddEqEvent),
    HLE(sceGnmAreSubmitsAllowed),
    HLE(sceGnmBeginWorkload),
    HLE(sceGnmComputeWaitOnAddress),
    HLE(sceGnmComputeWaitSemaphore),
    HLE(sceGnmCreateWorkloadStream),
    HLE(sceGnmDebugHardwareStatus),
    HLE(sceGnmDeleteEqEvent),
    HLE(sceGnmDestroyWorkloadStream),
    HLE(sceGnmDingDong),
    HLE(sceGnmDingDongForWorkload),
    HLE(sceGnmDisableMipStatsReport),
    HLE(sceGnmDispatchDirect),
    HLE(sceGnmDispatchIndirect),
    HLE(sceGnmDispatchIndirectOnMec),
    HLE(sceGnmDispatchInitDefaultHardwareState),
    HLE(sceGnmDrawIndex),
    HLE(sceGnmDrawIndexAuto),
    HLE(sceGnmDrawIndexIndirect),
    HLE(sceGnmDrawIndexIndirectCountMulti),
    HLE(sceGnmDrawIndexMultiInstanced),
    HLE(sceGnmDrawIndexOffset),
    HLE(sceGnmDrawIndirect),
    HLE(sceGnmDrawIndirectCountMulti),
    HLE(sceGnmDrawInitDefaultHardwareState350),
    HLE(sceGnmDrawInitToDefaultContextState400),
    HLE(sceGnmDrawOpaqueAuto),
    HLE(sceGnmDriverCaptureInProgress),
    HLE(sceGnmDriverTraceInProgress),
    HLE(sceGnmDriverTriggerCapture),
    HLE(sceGnmEndWorkload),
    HLE(sceGnmFindResourcesPublic),
    HLE(sceGnmFlushGarlic),
    HLE(sceGnmGetEqEventType),
    HLE(sceGnmGetEqTimeStamp),
    HLE(sceGnmGetGpuBlockStatus),
    HLE(sceGnmGetGpuInfoStatus),
    HLE(sceGnmGetLastWaitedAddress),
    HLE(sceGnmGetNumTcaUnits),
    HLE(sceGnmGetOffChipTessellationBufferSize),
    HLE(sceGnmGetOwnerName),
    HLE(sceGnmGetPhysicalCounterFromVirtualized),
    HLE(sceGnmGetProtectionFaultTimeStamp),
    HLE(sceGnmGetResourceBaseAddressAndSizeInBytes),
    HLE(sceGnmGetResourceName),
    HLE(sceGnmGetResourceShaderGuid),
    HLE(sceGnmGetResourceType),
    HLE(sceGnmGetResourceUserData),
    HLE(sceGnmGetShaderProgramBaseAddress),
    HLE(sceGnmGetShaderStatus),
    HLE(sceGnmGetTheTessellationFactorRingBufferBaseAddress),
    HLE(sceGnmInsertDingDongMarker),
    HLE(sceGnmInsertPopMarker),
    HLE(sceGnmInsertPushColorMarker),
    HLE(sceGnmInsertPushMarker),
    HLE(sceGnmInsertSetColorMarker),
    HLE(sceGnmInsertSetMarker),
    HLE(sceGnmInsertThreadTraceMarker),
    HLE(sceGnmInsertWaitFlipDone),
    HLE(sceGnmIsUserPaEnabled),
    HLE(sceGnmLogicalCuIndexToPhysicalCuIndex),
    HLE(sceGnmLogicalCuMaskToPhysicalCuMask),
    HLE(sceGnmLogicalTcaUnitToPhysical),
    HLE(sceGnmMapComputeQueue),
    HLE(sceGnmMapComputeQueueWithPriority),
    HLE(sceGnmQueryResourceRegistrationUserMemoryRequirements),
    HLE(sceGnmRegisterGdsResource),
    HLE(sceGnmRegisterOwner),
    HLE(sceGnmRegisterResource),
    HLE(sceGnmRequestFlipAndSubmitDone),
    HLE(sceGnmRequestFlipAndSubmitDoneForWorkload),
    HLE(sceGnmRequestMipStatsReportAndReset),
    HLE(sceGnmResetVgtControl),
    HLE(sceGnmSetCsShaderWithModifier),
    HLE(sceGnmSetEmbeddedPsShader),
    HLE(sceGnmSetEmbeddedVsShader),
    HLE(sceGnmSetEsShader),
    HLE(sceGnmSetGsRingSizes),
    HLE(sceGnmSetGsShader),
    HLE(sceGnmSetHsShader),
    HLE(sceGnmSetLsShader),
    HLE(sceGnmSetPsShader350),
    HLE(sceGnmSetResourceRegistrationUserMemory),
    HLE(sceGnmSetResourceUserData),
    HLE(sceGnmSetVgtControl),
    HLE(sceGnmSetVsShader),
    HLE(sceGnmSetWaveLimitMultipliers),
    HLE(sceGnmSetupMipStatsReport),
    HLE(sceGnmSubmitAndFlipCommandBuffers),
    HLE(sceGnmSubmitAndFlipCommandBuffersForWorkload),
    HLE(sceGnmSubmitCommandBuffers),
    HLE(sceGnmSubmitCommandBuffersForWorkload),
    HLE(sceGnmSubmitDone),
    HLE(sceGnmUnmapComputeQueue),
    HLE(sceGnmUnregisterAllResourcesForOwner),
    HLE(sceGnmUnregisterOwnerAndResources),
    HLE(sceGnmUnregisterResource),
    HLE(sceGnmUpdateGsShader),
    HLE(sceGnmUpdateHsShader),
    HLE(sceGnmUpdatePsShader350),
    HLE(sceGnmUpdateVsShader),
    HLE(sceRazorCaptureImmediate),
    HLE(sceRazorIsLoaded),
)
