/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Workstream I: input at low frame rates. libScePad latches press edges, so a
 * press that starts and ends between two reads is still seen (the title screen
 * runs at 0-1 fps; SOTC_PAD_SCRIPT holds a button for 150 ms). Input is fed
 * through pad_input_update(), the entry point window.c (gamepad, keyboard and
 * the pad script) uses. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/hle.h"
#include "log.h"
#include "window.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            fflush(stderr);                                                       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

static void *fn(const char *lib, const char *name) {
    void *p = hle_lookup(lib, name);
    if (!p) {
        fprintf(stderr, "FAIL missing %s:%s\n", lib, name);
        exit(1);
    }
    return p;
}
#define CALL(lib, name, type) ((type)fn(lib, name))

/* ScePadData, 120 bytes (the fields the test reads). */
typedef struct pad_data {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2, pad[2];
    float orientation[4];
    float accel[3], gyro[3];
    uint8_t touch[24];
    uint8_t connected;
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t connected_count;
    uint8_t rest[15];
} pad_data;
_Static_assert(sizeof(pad_data) == 120, "ScePadData");

typedef SYSV int32_t (*read_fn)(int32_t, void *, int32_t);
typedef SYSV int32_t (*read_state_fn)(int32_t, void *);
static read_fn g_read;
static read_state_fn g_read_state;
static int32_t g_handle;

static void set(uint32_t buttons, uint8_t lx) {
    pad_input in = {.buttons = buttons, .axes = {lx, 128, 128, 128, 0, 0}};
    pad_input_update(&in);
}

static uint32_t state_buttons(void) {
    pad_data d;
    memset(&d, 0, sizeof(d));
    CHECK(g_read_state(g_handle, &d) == 0);
    return d.buttons;
}

static uint32_t read1_buttons(void) {
    pad_data d;
    memset(&d, 0, sizeof(d));
    CHECK(g_read(g_handle, &d, 1) == 1);
    return d.buttons;
}

static void drain(void) {
    static pad_data d[64];
    while (g_read(g_handle, d, 64) > 0) {}
}

/* A press and its release between two single reads. */
static void test_tap_between_reads(void) {
    set(0, 128);
    (void)state_buttons();
    set(PAD_BTN_CROSS, 128);
    set(0, 128);
    CHECK(state_buttons() == PAD_BTN_CROSS); /* seen once */
    CHECK(state_buttons() == 0);             /* then released */

    /* Same through scePadRead(num == 1). */
    set(PAD_BTN_CIRCLE, 128);
    set(0, 128);
    CHECK(read1_buttons() == PAD_BTN_CIRCLE);
    CHECK(read1_buttons() == 0);
}

/* A held button stays pressed; its release shows at once. */
static void test_hold(void) {
    set(PAD_BTN_SQUARE, 128);
    for (int i = 0; i < 5; i++) {
        set(PAD_BTN_SQUARE, 128); /* the window repeats unchanged snapshots */
        CHECK(state_buttons() == PAD_BTN_SQUARE);
    }
    set(0, 128);
    CHECK(state_buttons() == 0); /* held across the last read: no extra press */

    /* A press while another button is held. */
    set(PAD_BTN_L1, 128);
    CHECK(state_buttons() == PAD_BTN_L1);
    set(PAD_BTN_L1 | PAD_BTN_CROSS, 128);
    set(PAD_BTN_L1, 128);
    CHECK(state_buttons() == (PAD_BTN_L1 | PAD_BTN_CROSS));
    CHECK(state_buttons() == PAD_BTN_L1);
    set(0, 128);
    CHECK(state_buttons() == 0);
}

/* Sticks report the current value, not a latched one. */
static void test_axes_current(void) {
    set(0, 0);
    set(0, 255);
    set(0, 128);
    pad_data d;
    memset(&d, 0, sizeof(d));
    CHECK(g_read_state(g_handle, &d) == 0);
    CHECK(d.lx == 128 && d.buttons == 0);
}

/* SOTC_PAD_SCRIPT: window.c polls every ~8 ms and holds a scripted button for
 * 150 ms; the game reads once per second. Simulate 3 s of polling on a virtual
 * clock with presses at 1.2 s (cross) and 2.5 s (options). */
static void test_script_at_1fps(void) {
    drain();
    (void)state_buttons();
    const uint32_t hold_ms = 150, poll_ms = 8;
    struct { uint32_t at_ms, bit; } script[] = {{1200, PAD_BTN_CROSS}, {2500, PAD_BTN_OPTIONS}};
    uint32_t seen_single[4] = {0}, seen_multi[4] = {0};
    int reads = 0;
    for (uint32_t t = 0; t <= 3000; t += poll_ms) {
        uint32_t bits = 0;
        for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++)
            if (t >= script[i].at_ms && t < script[i].at_ms + hold_ms) bits |= script[i].bit;
        set(bits, 128);
        if (t % 1000 < poll_ms && t > 0 && reads < 4) { /* a frame, once per second */
            seen_single[reads] = state_buttons();
            static pad_data d[64];
            int32_t n = g_read(g_handle, d, 64);
            CHECK(n >= 0);
            for (int32_t i = 0; i < n; i++) seen_multi[reads] |= d[i].buttons;
            reads++;
        }
    }
    CHECK(reads == 3);
    CHECK(seen_single[0] == 0);
    CHECK(seen_single[1] == PAD_BTN_CROSS);   /* read at 2.0 s: cross at 1.2-1.35 s */
    CHECK(seen_single[2] == PAD_BTN_OPTIONS); /* read at 3.0 s: options at 2.5-2.65 s */
    CHECK(seen_multi[0] == 0);
    CHECK(seen_multi[1] == PAD_BTN_CROSS);
    CHECK(seen_multi[2] == PAD_BTN_OPTIONS);
    CHECK(state_buttons() == 0);
}

/* Multi reads return the press and release samples; a press lost to a queue
 * overflow (more than 64 changes between reads) is ORed into the first one. */
static void test_multi_overflow(void) {
    static pad_data d[64];
    drain();
    set(PAD_BTN_TRIANGLE, 128);
    set(0, 128);
    CHECK(g_read(g_handle, d, 64) == 2);
    CHECK(d[0].buttons == PAD_BTN_TRIANGLE && d[1].buttons == 0);

    set(PAD_BTN_R1, 128);
    set(0, 128);
    for (int i = 0; i < 100; i++) set(0, (uint8_t)(i & 1 ? 0 : 255)); /* stick noise */
    int32_t n = g_read(g_handle, d, 64);
    CHECK(n == 64);
    CHECK(d[0].buttons == PAD_BTN_R1);
    uint32_t rest = 0;
    for (int32_t i = 1; i < n; i++) rest |= d[i].buttons;
    CHECK(rest == 0);
    CHECK(g_read(g_handle, d, 64) == 0);
    set(0, 128);
    CHECK(g_read(g_handle, d, 64) == 1 && d[0].buttons == 0); /* latch consumed */
}

/* Reopening the pad clears the latches. */
static void test_reopen_clears(void) {
    set(PAD_BTN_CROSS, 128);
    set(0, 128);
    SYSV int32_t (*close)(int32_t) = CALL("libScePad", "scePadClose", SYSV int32_t (*)(int32_t));
    CHECK(close(g_handle) == 0);
    g_handle = CALL("libScePad", "scePadOpen", SYSV int32_t (*)(int32_t, int32_t, int32_t, void *))(
        1, 0, 0, NULL);
    CHECK(g_handle > 0);
    CHECK(state_buttons() == 0);
}

int main(void) {
    log_init();
    hle_init();
    g_read = CALL("libScePad", "scePadRead", read_fn);
    g_read_state = CALL("libScePad", "scePadReadState", read_state_fn);
    CHECK(CALL("libScePad", "scePadInit", SYSV int32_t (*)(void))() == 0);
    g_handle = CALL("libScePad", "scePadOpen", SYSV int32_t (*)(int32_t, int32_t, int32_t, void *))(
        1, 0, 0, NULL);
    CHECK(g_handle > 0);

    test_tap_between_reads();
    test_hold();
    test_axes_current();
    test_script_at_1fps();
    test_multi_overflow();
    test_reopen_clears();

    if (g_failures) {
        printf("pad: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("pad: passed\n");
    return 0;
}
