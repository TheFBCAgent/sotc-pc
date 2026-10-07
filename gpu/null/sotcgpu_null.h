/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only recording build of the null sotcgpu (workstream GI).
 *
 * A test that wants it does
 *     #define SOTCGPU_NULL_RECORDING
 *     #include "../../gpu/null/sotcgpu_null.c"
 * so its own object defines every sotcgpu_* function and the linked sotcgpu
 * library (null or Vulkan) is never pulled in for them. The normal build of
 * sotcgpu_null.c ignores everything here: init returns -1. */
#ifndef SOTCGPU_NULL_H
#define SOTCGPU_NULL_H

#include "sotcgpu.h"

typedef enum sotcgpu_rec_kind {
    REC_INIT, REC_SHUTDOWN, REC_SUBMIT_GFX, REC_SUBMIT_COMPUTE, REC_SUBMIT_DONE,
    REC_REGISTER_BUFFERS, REC_FLIP, REC_MAP, REC_UNMAP, REC_FAULT, REC_SCREENSHOT,
} sotcgpu_rec_kind;

typedef struct sotcgpu_rec {
    sotcgpu_rec_kind kind;
    uint64_t a, b, c; /* addr/size, dcb/dwords, group/count, buf/arg, ... */
    bool flag;        /* after_gpu_work, is_write */
} sotcgpu_rec;

#define SOTCGPU_REC_MAX 4096

typedef struct sotcgpu_recorder {
    int init_result;               /* what sotcgpu_init returns (0: GPU active) */
    bool fault_unprotects;         /* handle_fault lifts the restriction and returns true */
    const sotcgpu_host *host;      /* from sotcgpu_init */
    sotcgpu_config cfg;
    uint64_t buffers[16];          /* last registered table */
    int32_t buffer_count;
    volatile int count;
    sotcgpu_rec recs[SOTCGPU_REC_MAX];
} sotcgpu_recorder;

/* The recorder (zeroed: init_result 0). */
extern sotcgpu_recorder sotcgpu_rec_state;

#endif
