/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GnmDriver (workstream G1): every import resolves, packet builders write
 * exactly the reserved dwords as well-formed PM4 type-3 packets, and the
 * compute-queue, submission and flip-patch logic behaves. The PM4 executor
 * itself is G2's and is not tested here. */
#include <stdio.h>
#include <string.h>

#include "hle/equeue.h"
#include "hle/hle.h"
#include "hle/pm4.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

/* All functions are called through one SysV signature with spare arguments:
 * extra integer arguments are harmless under the SysV calling convention. */
typedef SYSV uint64_t (*gfn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t);

static uint64_t call_(const char *name, const uint64_t *a) {
    gfn f = (gfn)hle_lookup(strncmp(name, "sceVideoOut", 11) ? "libSceGnmDriver" : "libSceVideoOut",
                            name);
    if (!f) {
        fprintf(stderr, "FAIL missing %s\n", name);
        g_failures++;
        return 0;
    }
    return f(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
}
#define CALL(name, ...) call_(name, (const uint64_t[10]){__VA_ARGS__})
#define CALLI(name, ...) ((int32_t)CALL(name, __VA_ARGS__))
#define P(x) ((uint64_t)(uintptr_t)(x))

static const char *const k_imports[] = {
    "sceGnmAddEqEvent", "sceGnmAreSubmitsAllowed", "sceGnmBeginWorkload",
    "sceGnmComputeWaitOnAddress", "sceGnmComputeWaitSemaphore", "sceGnmCreateWorkloadStream",
    "sceGnmDebugHardwareStatus", "sceGnmDeleteEqEvent", "sceGnmDestroyWorkloadStream",
    "sceGnmDingDong", "sceGnmDingDongForWorkload", "sceGnmDisableMipStatsReport",
    "sceGnmDispatchDirect", "sceGnmDispatchIndirect", "sceGnmDispatchIndirectOnMec",
    "sceGnmDispatchInitDefaultHardwareState", "sceGnmDrawIndex", "sceGnmDrawIndexAuto",
    "sceGnmDrawIndexIndirect", "sceGnmDrawIndexIndirectCountMulti",
    "sceGnmDrawIndexMultiInstanced", "sceGnmDrawIndexOffset", "sceGnmDrawIndirect",
    "sceGnmDrawIndirectCountMulti", "sceGnmDrawInitDefaultHardwareState350",
    "sceGnmDrawInitToDefaultContextState400", "sceGnmDrawOpaqueAuto",
    "sceGnmDriverCaptureInProgress", "sceGnmDriverTraceInProgress",
    "sceGnmDriverTriggerCapture", "sceGnmEndWorkload", "sceGnmFindResourcesPublic",
    "sceGnmFlushGarlic", "sceGnmGetEqEventType", "sceGnmGetEqTimeStamp",
    "sceGnmGetGpuBlockStatus", "sceGnmGetGpuInfoStatus", "sceGnmGetLastWaitedAddress",
    "sceGnmGetNumTcaUnits", "sceGnmGetOffChipTessellationBufferSize", "sceGnmGetOwnerName",
    "sceGnmGetPhysicalCounterFromVirtualized", "sceGnmGetProtectionFaultTimeStamp",
    "sceGnmGetResourceBaseAddressAndSizeInBytes", "sceGnmGetResourceName",
    "sceGnmGetResourceShaderGuid", "sceGnmGetResourceType", "sceGnmGetResourceUserData",
    "sceGnmGetShaderProgramBaseAddress", "sceGnmGetShaderStatus",
    "sceGnmGetTheTessellationFactorRingBufferBaseAddress", "sceGnmInsertDingDongMarker",
    "sceGnmInsertPopMarker", "sceGnmInsertPushColorMarker", "sceGnmInsertPushMarker",
    "sceGnmInsertSetColorMarker", "sceGnmInsertSetMarker", "sceGnmInsertThreadTraceMarker",
    "sceGnmInsertWaitFlipDone", "sceGnmIsUserPaEnabled",
    "sceGnmLogicalCuIndexToPhysicalCuIndex", "sceGnmLogicalCuMaskToPhysicalCuMask",
    "sceGnmLogicalTcaUnitToPhysical", "sceGnmMapComputeQueue",
    "sceGnmMapComputeQueueWithPriority",
    "sceGnmQueryResourceRegistrationUserMemoryRequirements", "sceGnmRegisterGdsResource",
    "sceGnmRegisterOwner", "sceGnmRegisterResource", "sceGnmRequestFlipAndSubmitDone",
    "sceGnmRequestFlipAndSubmitDoneForWorkload", "sceGnmRequestMipStatsReportAndReset",
    "sceGnmResetVgtControl", "sceGnmSetCsShaderWithModifier", "sceGnmSetEmbeddedPsShader",
    "sceGnmSetEmbeddedVsShader", "sceGnmSetEsShader", "sceGnmSetGsRingSizes",
    "sceGnmSetGsShader", "sceGnmSetHsShader", "sceGnmSetLsShader", "sceGnmSetPsShader350",
    "sceGnmSetResourceRegistrationUserMemory", "sceGnmSetResourceUserData",
    "sceGnmSetVgtControl", "sceGnmSetVsShader", "sceGnmSetWaveLimitMultipliers",
    "sceGnmSetupMipStatsReport", "sceGnmSubmitAndFlipCommandBuffers",
    "sceGnmSubmitAndFlipCommandBuffersForWorkload", "sceGnmSubmitCommandBuffers",
    "sceGnmSubmitCommandBuffersForWorkload", "sceGnmSubmitDone", "sceGnmUnmapComputeQueue",
    "sceGnmUnregisterAllResourcesForOwner", "sceGnmUnregisterOwnerAndResources",
    "sceGnmUnregisterResource", "sceGnmUpdateGsShader", "sceGnmUpdateHsShader",
    "sceGnmUpdatePsShader350", "sceGnmUpdateVsShader", "sceRazorCaptureImmediate",
    "sceRazorIsLoaded",
};

/* ---- PM4 checking -------------------------------------------------------- */

#define CANARY 0xdeadbeefu
#define BUF 0x200
static uint32_t g_buf[BUF + 16];
static uint8_t g_ops[256];
static int g_nops;

static void reset_buf(void) {
    for (int i = 0; i < BUF + 16; i++) g_buf[i] = CANARY;
}

static uint32_t op_of(uint32_t h) { return (h >> 8) & 0xff; }
static uint32_t body_of(uint32_t h) { return ((h >> 16) & 0x3fff) + 1; }

/* True if [buf, buf+n) is exactly a sequence of type-3 packets and the dword
 * after it is untouched. Records the opcodes in g_ops. */
static bool well_formed(const uint32_t *buf, uint32_t n) {
    uint32_t i = 0;
    g_nops = 0;
    while (i < n) {
        uint32_t h = buf[i];
        if ((h >> 30) != 3) {
            fprintf(stderr, "  dword %u: not a type-3 header (%#x)\n", i, h);
            return false;
        }
        if (g_nops < 256) g_ops[g_nops++] = (uint8_t)op_of(h);
        i += 1 + body_of(h);
    }
    if (i != n) {
        fprintf(stderr, "  packets end at %u, expected %u\n", i, n);
        return false;
    }
    return buf[n] == CANARY;
}

static bool has_op(uint32_t op) {
    for (int i = 0; i < g_nops; i++)
        if (g_ops[i] == op) return true;
    return false;
}

static const uint32_t k_regs[16] = {0x1234, 0, 0x55, 0x66, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static void test_builders(void) {
    reset_buf();
    CHECK(CALLI("sceGnmDispatchDirect", P(g_buf), 9, 4, 2, 1, 0) == 0);
    CHECK(well_formed(g_buf, 9) && g_ops[0] == 0x15 && g_buf[1] == 4 && g_buf[3] == 1);
    CHECK((g_buf[0] & 2) != 0); /* compute shader type */
    CHECK(CALLI("sceGnmDispatchDirect", P(g_buf), 8, 1, 1, 1, 0) == -1);

    reset_buf();
    CHECK(CALLI("sceGnmDispatchIndirect", P(g_buf), 7, 0x40, 0) == 0);
    CHECK(well_formed(g_buf, 7) && g_ops[0] == 0x16);

    reset_buf();
    CHECK(CALLI("sceGnmDispatchIndirectOnMec", P(g_buf), 8, 0x123450ull, 0) == 0);
    CHECK(well_formed(g_buf, 8) && g_buf[1] == 0x123450);
    CHECK(CALLI("sceGnmDispatchIndirectOnMec", P(g_buf), 8, 0x123452ull, 0) == -1);

    reset_buf();
    CHECK((uint32_t)CALL("sceGnmDispatchInitDefaultHardwareState", P(g_buf), BUF) == 0x100);
    CHECK(well_formed(g_buf, 0x100) && has_op(0x76) && has_op(0x58));
    CHECK((uint32_t)CALL("sceGnmDispatchInitDefaultHardwareState", P(g_buf), 0xff) == 0);

    reset_buf();
    CHECK(CALLI("sceGnmComputeWaitOnAddress", P(g_buf), 0xe, 0x10000040ull, ~0u, 3, 1) == 0);
    CHECK(well_formed(g_buf, 0xe) && has_op(0x3c));

    reset_buf();
    CHECK(CALLI("sceGnmDrawIndex", P(g_buf), 10, 36, 0x20000000ull, 0, 0) == 0);
    CHECK(well_formed(g_buf, 10) && g_ops[0] == 0x27 && g_buf[1] == 36 && g_buf[2] == 0x20000000);
    CHECK(CALLI("sceGnmDrawIndex", P(g_buf), 10, 36, 0x20000001ull, 0, 0) == -1);

    reset_buf();
    CHECK(CALLI("sceGnmDrawIndexAuto", P(g_buf), 7, 3, 0) == 0);
    CHECK(well_formed(g_buf, 7) && g_ops[0] == 0x2d && g_buf[1] == 3 && g_buf[2] == 2);

    reset_buf();
    CHECK(CALLI("sceGnmDrawIndexOffset", P(g_buf), 9, 6, 12, 0) == 0);
    CHECK(well_formed(g_buf, 9) && g_ops[0] == 0x35);

    reset_buf();
    CHECK(CALLI("sceGnmDrawIndexIndirect", P(g_buf), 9, 0, 2 /* VS */, 1, 2, 0) == 0);
    CHECK(well_formed(g_buf, 9) && g_ops[0] == 0x25 && g_buf[2] == 0x4d);
    reset_buf();
    CHECK(CALLI("sceGnmDrawIndirect", P(g_buf), 9, 0, 2, 0, 0, 0) == 0);
    CHECK(well_formed(g_buf, 9) && g_ops[0] == 0x24);
    CHECK(CALLI("sceGnmDrawIndirect", P(g_buf), 9, 0, 7, 0, 0, 0) == -1);

    reset_buf();
    CHECK(CALLI("sceGnmDrawIndexIndirectCountMulti", P(g_buf), 16, 0, 4, 0x3000ull, 2, 0, 0,
                0) == 0);
    CHECK(well_formed(g_buf, 16) && has_op(0x9d));
    CHECK(CALLI("sceGnmDrawIndexIndirectCountMulti", P(g_buf), 16, 0, 4, 0, 1 /* PS */, 0, 0,
                0) == -1);

    /* Unknown-layout builders fill the reserved space with one NOP. */
    reset_buf();
    CHECK(CALLI("sceGnmDrawOpaqueAuto", P(g_buf), 7, 0) == 0);
    CHECK(well_formed(g_buf, 7));
    reset_buf();
    CHECK(CALLI("sceGnmInsertSetColorMarker", P(g_buf), 12, P("x"), 0xff) == 0);
    CHECK(well_formed(g_buf, 12));

    reset_buf();
    CHECK((uint32_t)CALL("sceGnmDrawInitDefaultHardwareState350", P(g_buf), BUF) == 0x100);
    CHECK(well_formed(g_buf, 0x100) && g_buf[0] == 0xc0012800u && has_op(0x69));
    CHECK((uint32_t)CALL("sceGnmDrawInitDefaultHardwareState350", P(g_buf), 0x80) == 0);

    reset_buf();
    CHECK((uint32_t)CALL("sceGnmDrawInitToDefaultContextState400", P(g_buf), 0x100) == 0x100);
    CHECK(well_formed(g_buf, 0x100));
    CHECK((uint32_t)CALL("sceGnmDrawInitToDefaultContextState400", P(g_buf), 0x200) == 0);

    reset_buf();
    CHECK(CALLI("sceGnmResetVgtControl", P(g_buf), 3) == 0);
    CHECK(well_formed(g_buf, 3) && g_buf[1] == 0x2aa && g_buf[2] == 0xff);
    reset_buf();
    CHECK(CALLI("sceGnmSetVgtControl", P(g_buf), 3, 0x7f, 1, 0) == 0);
    CHECK(well_formed(g_buf, 3) && g_buf[2] == 0x1007f);
    CHECK(CALLI("sceGnmSetVgtControl", P(g_buf), 3, 0x100, 0, 0) == -1);
}

static void test_shaders(void) {
    struct { const char *name; uint32_t size; } cases[] = {
        {"sceGnmSetCsShaderWithModifier", 0x19}, {"sceGnmSetVsShader", 0x1d},
        {"sceGnmUpdateVsShader", 0x1d},          {"sceGnmSetPsShader350", 0x28},
        {"sceGnmUpdatePsShader350", 0x28},       {"sceGnmSetEsShader", 0x14},
        {"sceGnmSetGsShader", 0x1d},             {"sceGnmUpdateGsShader", 0x1d},
        {"sceGnmSetHsShader", 0x1e},             {"sceGnmUpdateHsShader", 0x1e},
        {"sceGnmSetLsShader", 0x17},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_buf();
        int32_t r = CALLI(cases[i].name, P(g_buf), cases[i].size, P(k_regs), 0);
        if (r != 0 || !well_formed(g_buf, cases[i].size)) {
            fprintf(stderr, "FAIL shader builder %s\n", cases[i].name);
            g_failures++;
        }
        CHECK(g_buf[0] == 0xc0017600u || g_buf[0] == 0xc0027600u); /* SET_SH_REG first */
        CHECK(CALLI(cases[i].name, P(g_buf), cases[i].size - 1, P(k_regs), 0) == -1);
        /* A shader address with high bits is rejected. */
        uint32_t bad[16];
        memcpy(bad, k_regs, sizeof bad);
        bad[1] = 1;
        CHECK(CALLI(cases[i].name, P(g_buf), cases[i].size, P(bad), 0) == -1);
    }
    /* PS 3.50 with no shader. */
    reset_buf();
    CHECK(CALLI("sceGnmSetPsShader350", P(g_buf), 0x28, 0) == 0 && well_formed(g_buf, 0x28));
    reset_buf();
    CHECK(CALLI("sceGnmUpdatePsShader350", P(g_buf), 0x28, 0) == 0 && well_formed(g_buf, 0x28));
    /* Invalid modifier bits. */
    CHECK(CALLI("sceGnmSetVsShader", P(g_buf), 0x1d, P(k_regs), 1) == -1);

    reset_buf();
    CHECK(CALLI("sceGnmSetEmbeddedPsShader", P(g_buf), 0x28, 0, 0) == 0);
    CHECK(well_formed(g_buf, 0x28));
    reset_buf();
    CHECK(CALLI("sceGnmSetEmbeddedPsShader", P(g_buf), 0x28, 1, 0) == 0);
    CHECK(well_formed(g_buf, 0x28));
    CHECK(CALLI("sceGnmSetEmbeddedPsShader", P(g_buf), 0x28, 2, 0) == (int32_t)0x8EEE00FFu);
    reset_buf();
    CHECK(CALLI("sceGnmSetEmbeddedVsShader", P(g_buf), 0x1d, 0, 0) == 0);
    CHECK(well_formed(g_buf, 0x1d));
}

static void test_markers(void) {
    reset_buf();
    CHECK(CALLI("sceGnmInsertDingDongMarker", P(g_buf), 4) == 0 && well_formed(g_buf, 4));
    reset_buf();
    CHECK(CALLI("sceGnmInsertPopMarker", P(g_buf), 6) == 0 && well_formed(g_buf, 6));
    CHECK(g_buf[1] == 0x68750002u);

    /* "abc": packet_size = (11 >> 2) + (15 >> 3) * 2 = 4, so 6 dwords. */
    reset_buf();
    CHECK(CALLI("sceGnmInsertPushMarker", P(g_buf), 6, P("abc")) == 0);
    CHECK(well_formed(g_buf, 6) && g_buf[1] == 0x68750001u);
    CHECK(memcmp(&g_buf[2], "abc", 4) == 0);
    CHECK(CALLI("sceGnmInsertPushMarker", P(g_buf), 7, P("abc")) == -1);
    reset_buf();
    CHECK(CALLI("sceGnmInsertSetMarker", P(g_buf), 6, P("abc")) == 0);
    CHECK(well_formed(g_buf, 6) && g_buf[1] == 0x68750003u);

    /* Color: packet_size = (15 >> 2) + (19 >> 3) * 2 = 7, so 9 dwords. */
    reset_buf();
    CHECK(CALLI("sceGnmInsertPushColorMarker", P(g_buf), 9, P("abc"), 0xff00ff) == 0);
    CHECK(well_formed(g_buf, 9) && g_buf[1] == 0x6875000eu);
    uint32_t color;
    memcpy(&color, (uint8_t *)&g_buf[2] + 4 + 8, 4);
    CHECK(color == 0xff00ff);

    reset_buf();
    CHECK(CALLI("sceGnmInsertWaitFlipDone", P(g_buf), 7, 1, 0) == 0 && well_formed(g_buf, 7));
}

static uint32_t g_ring[256] __attribute__((aligned(256)));

static void test_compute_queues(void) {
    uint32_t rptr = 0xffffffffu;
    CHECK(CALLI("sceGnmMapComputeQueue", 7, 0, P(g_ring), 256, P(&rptr)) == (int32_t)0x80D17000u);
    CHECK(CALLI("sceGnmMapComputeQueue", 0, 8, P(g_ring), 256, P(&rptr)) == (int32_t)0x80D17001u);
    CHECK(CALLI("sceGnmMapComputeQueue", 0, 0, P(g_ring), 100, P(&rptr)) == (int32_t)0x80D17002u);
    CHECK(CALLI("sceGnmMapComputeQueue", 0, 0, P(g_ring) + 4, 256, P(&rptr)) ==
          (int32_t)0x80D17003u);
    CHECK(CALLI("sceGnmMapComputeQueue", 0, 0, P(g_ring), 256, P(&rptr) + 1) ==
          (int32_t)0x80D17004u);

    int32_t vqid = CALLI("sceGnmMapComputeQueue", 0, 0, P(g_ring), 256, P(&rptr));
    CHECK(vqid >= 1 && vqid <= 56);
    CHECK(rptr == 0);
    int32_t vqid2 = CALLI("sceGnmMapComputeQueueWithPriority", 1, 0, P(g_ring), 256, P(&rptr), 0);
    CHECK(vqid2 >= 1 && vqid2 != vqid);
    CHECK(CALLI("sceGnmUnmapComputeQueue", vqid2) == 0);

    CALL("sceGnmDingDong", (uint64_t)vqid, 16);
    pm4_wait_idle(); /* the GPU queue advances the read pointer */
    CHECK(rptr == 16);
    CALL("sceGnmDingDongForWorkload", (uint64_t)vqid, 200, 0);
    pm4_wait_idle(); /* the GPU queue advances the read pointer */
    CHECK(rptr == 200);
    CALL("sceGnmDingDong", (uint64_t)vqid, 8); /* wraps */
    pm4_wait_idle(); /* the GPU queue advances the read pointer */
    CHECK(rptr == 8);
    CALL("sceGnmDingDong", (uint64_t)vqid, 0); /* up to the end of the ring */
    pm4_wait_idle();
    CHECK(rptr == 0);
    CALL("sceGnmDingDong", 0, 4); /* vqid 0 is ignored */
    CHECK(CALLI("sceGnmUnmapComputeQueue", vqid) == 0);
}

static void test_submit(void) {
    static uint32_t dcb[128];
    const uint32_t *dcbs[1] = {dcb};
    uint32_t sizes[1] = {sizeof dcb};
    uint32_t zero[1] = {0};
    for (int i = 0; i < 128; i++) dcb[i] = 0;
    dcb[0] = 0xc07e1000u; /* NOP, 127-dword body */

    CHECK(CALLI("sceGnmSubmitCommandBuffers", 1, 0, P(sizes), 0, 0) == (int32_t)0x80D11000u);
    CHECK(CALLI("sceGnmSubmitCommandBuffers", 1, P(dcbs), P(zero), 0, 0) ==
          (int32_t)0x80D11000u);
    CHECK(CALLI("sceGnmSubmitCommandBuffers", 1, P(dcbs), P(sizes), 0, P(sizes)) ==
          (int32_t)0x80D11000u);
    CHECK(CALLI("sceGnmSubmitCommandBuffers", 1, P(dcbs), P(sizes), 0, 0) == 0);
    CHECK(CALLI("sceGnmSubmitCommandBuffersForWorkload", 0, 1, P(dcbs), P(sizes), P(dcbs),
                P(sizes)) == 0);
    CHECK(CALLI("sceGnmSubmitDone") == 0);
    CHECK(CALLI("sceGnmAreSubmitsAllowed") == 1);

    /* A VideoOut port with two registered buffers for the flips below. */
    static uint8_t vo_attr[64];
    static uint8_t vo_mem[2][64];
    void *vo_bufs[2] = {vo_mem[0], vo_mem[1]};
    int32_t vo = CALLI("sceVideoOutOpen", 255, 0, 0, 0);
    CHECK(vo > 0);
    CALL("sceVideoOutSetBufferAttribute", P(vo_attr), 0x80000000u, 0, 0, 1920, 1080, 1920);
    CHECK(CALLI("sceVideoOutRegisterBuffers", vo, 0, P(vo_bufs), 2, P(vo_attr)) == 0);

    /* Flip with a prepareFlip-with-label packet in the last 64 dwords. */
    static uint32_t flip[68];
    const uint32_t *flips[1] = {flip};
    uint32_t fsizes[1] = {sizeof flip};
    for (int i = 0; i < 68; i++) flip[i] = 0;
    flip[0] = 0xc0021000u; /* NOP, 3-dword body */
    uint32_t *p = flip + 4;
    p[0] = 0xc03e1000u;
    p[1] = 0x68750778u; /* PrepareFlipLabel */
    p[2] = 0x20001000u;
    p[3] = 0x10;
    p[4] = 0xabcd;
    CHECK(CALLI("sceGnmSubmitAndFlipCommandBuffers", 1, P(flips), P(fsizes), 0, 0, vo, 0, 1, 0) ==
          0);
    CHECK((p[0] >> 30) == 3 && op_of(p[0]) == 0x10 && body_of(p[0]) == 58);
    CHECK(op_of(p[59]) == 0x37 && body_of(p[59]) == 4);
    CHECK(p[61] == 0x20001000u && p[62] == 0x10 && p[63] == 0xabcd);

    /* prepareFlip with interrupt and label: EVENT_WRITE_EOP at the end. */
    for (int i = 4; i < 68; i++) flip[i] = 0;
    p[0] = 0xc03e1000u;
    p[1] = 0x68750781u;
    p[2] = 0x20002000u;
    p[3] = 0x10;
    p[4] = 7;
    p[5] = 0x04; /* event type */
    p[6] = 0x00;
    CHECK(CALLI("sceGnmSubmitAndFlipCommandBuffersForWorkload", 0, 1, P(flips), P(fsizes), 0, 0,
                vo, 1, 1, 0) == 0);
    CHECK(body_of(p[0]) == 57 && op_of(p[58]) == 0x47 && body_of(p[58]) == 5);
    CHECK(p[59] == 0x504 && p[60] == 0x20002000u && p[61] == 0x22000010u && p[62] == 7);
    CHECK(CALLI("sceVideoOutClose", vo) == 0);
}

static void test_misc(void) {
    CHECK(CALLI("sceGnmAddEqEvent", 0, 0x40, 0) == SCE_KERNEL_ERROR(PS4_EBADF));
    CHECK(CALLI("sceGnmDeleteEqEvent", 0, 0x40) == SCE_KERNEL_ERROR(PS4_EBADF));
    ps4_kevent ev = {.ident = 0x40, .filter = EVFILT_GRAPHICS_CORE, .data = 0x40};
    CHECK(CALLI("sceGnmGetEqEventType", P(&ev)) == 0x40);

    uint64_t tf = CALL("sceGnmGetTheTessellationFactorRingBufferBaseAddress");
    CHECK(tf != 0 && (tf & 0xffff) == 0);
    CHECK(CALLI("sceGnmGetOffChipTessellationBufferSize") == 0x800000);
    CHECK(CALLI("sceGnmLogicalCuMaskToPhysicalCuMask", 0, 0x3ff) == 0x3ff);

    uint32_t stream = 0;
    CHECK(CALLI("sceGnmCreateWorkloadStream", 1, P(&stream)) == 0 && stream == 1);
    uint64_t workload = 99;
    CHECK(CALLI("sceGnmBeginWorkload", 1, P(&workload)) == 0 && workload == 1);
    CHECK(CALLI("sceGnmEndWorkload", workload) == 0);

    CHECK(CALLI("sceGnmRegisterOwner", 0, P("owner")) == (int32_t)0x8EEE00FFu);
    CHECK(CALLI("sceGnmDriverTriggerCapture") == (int32_t)0x80D15001u);
    CHECK((uint8_t)CALL("sceRazorIsLoaded") == 0);
    CHECK((uint8_t)CALL("sceGnmIsUserPaEnabled") == 0);
    CHECK(CALLI("sceGnmSetGsRingSizes", 0, 0) == 0);
}

int main(void) {
    log_init();
    hle_init();
    for (size_t i = 0; i < sizeof k_imports / sizeof k_imports[0]; i++) {
        if (!hle_lookup("libSceGnmDriver", k_imports[i])) {
            fprintf(stderr, "FAIL unresolved %s\n", k_imports[i]);
            g_failures++;
        }
    }
    CHECK(sizeof k_imports / sizeof k_imports[0] == 103);

    test_builders();
    test_shaders();
    test_markers();
    test_compute_queues();
    test_submit();
    test_misc();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("gnm tests passed\n");
    return 0;
}
