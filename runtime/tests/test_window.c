/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Workstream GW: the window's main loop, window_sdl_handle() and the pad
 * input hand-off.
 *
 * host_main_loop runs once per process, so each scenario runs in a child
 * process (this executable with a mode argument):
 *   pad       input snapshots -> scePadRead / scePadReadState semantics
 *   headless  SOTC_HEADLESS=1: no window, the handle is NULL right away, the
 *             loop returns when the guest main thread does
 *   window    a guest thread waits for the handle (Vulkan window), the loop
 *             returns when that thread ends; SKIPs without a display
 *   close     a quit event (window close) makes the loop return while the
 *             guest thread still runs; SKIPs without a display */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "hle/hle.h"
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

static int finish(const char *what) {
    if (g_failures) {
        printf("%s: %d check(s) failed\n", what, g_failures);
        return 1;
    }
    printf("%s: passed\n", what);
    return 0;
}

/* ---- pad ------------------------------------------------------------------ */

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

static pad_input neutral(void) {
    pad_input in = {.axes = {128, 128, 128, 128, 0, 0}};
    return in;
}

static int mode_pad(void) {
    const char *L = "libScePad";
    SYSV int32_t (*read)(int32_t, void *, int32_t) =
        CALL(L, "scePadRead", SYSV int32_t (*)(int32_t, void *, int32_t));
    SYSV int32_t (*read_state)(int32_t, void *) =
        CALL(L, "scePadReadState", SYSV int32_t (*)(int32_t, void *));

    /* Input before the pad is opened is kept (the window starts first). */
    pad_input in = neutral();
    in.buttons = PAD_BTN_OPTIONS;
    pad_input_update(&in);

    CHECK(CALL(L, "scePadInit", SYSV int32_t (*)(void))() == 0);
    int32_t h = CALL(L, "scePadOpen", SYSV int32_t (*)(int32_t, int32_t, int32_t, void *))(
        1, 0, 0, NULL);
    CHECK(h > 0);

    /* Opening queues the current state once. */
    static pad_data d[64];
    CHECK(read(h, d, 4) == 1);
    CHECK(d[0].buttons == PAD_BTN_OPTIONS && d[0].lx == 128 && d[0].connected == 1 &&
          d[0].connected_count == 1 && d[0].orientation[3] == 1.0f && d[0].timestamp > 0);
    uint64_t t0 = d[0].timestamp;
    CHECK(read(h, d, 4) == 0); /* nothing new */

    /* A change is one sample with a new timestamp. */
    SDL_DelayNS(2000000);
    in = neutral();
    in.buttons = PAD_BTN_CROSS | PAD_BTN_L2;
    in.axes[PAD_AXIS_LEFT_X] = 0;
    in.axes[PAD_AXIS_RIGHT_Y] = 255;
    in.axes[PAD_AXIS_L2] = 200;
    pad_input_update(&in);
    pad_input_update(&in); /* unchanged: no sample */
    memset(d, 0xAB, sizeof(d[0]) * 4);
    CHECK(read(h, d, 4) == 1);
    CHECK(d[0].buttons == (PAD_BTN_CROSS | PAD_BTN_L2) && d[0].lx == 0 && d[0].ly == 128 &&
          d[0].rx == 128 && d[0].ry == 255 && d[0].l2 == 200 && d[0].r2 == 0);
    CHECK(d[0].timestamp > t0);
    uint64_t t1 = d[0].timestamp;

    /* Single reads return the current state; its timestamp is the change's. */
    SDL_DelayNS(2000000);
    memset(d, 0, sizeof(d[0]));
    CHECK(read(h, d, 1) == 1);
    CHECK(d[0].buttons == (PAD_BTN_CROSS | PAD_BTN_L2) && d[0].timestamp == t1);
    memset(d, 0, sizeof(d[0]));
    CHECK(read_state(h, d) == 0);
    CHECK(d[0].lx == 0 && d[0].timestamp == t1 && d[0].connected == 1);

    /* The queue keeps the newest 64 samples, oldest first. */
    for (int i = 0; i < 70; i++) {
        in = neutral();
        in.axes[PAD_AXIS_RIGHT_X] = (uint8_t)i;
        pad_input_update(&in);
    }
    CHECK(read(h, d, 64) == 64);
    CHECK(d[0].rx == 6 && d[63].rx == 69);
    for (int i = 1; i < 64; i++) CHECK(d[i].timestamp >= d[i - 1].timestamp);
    CHECK(read(h, d, 64) == 0);
    CHECK(read(h, d, 65) == (int32_t)0x80920001);

    /* Reopening clears the queue and queues the current state. */
    in.axes[PAD_AXIS_RIGHT_X] = 77;
    pad_input_update(&in);
    SYSV int32_t (*close)(int32_t) = CALL(L, "scePadClose", SYSV int32_t (*)(int32_t));
    CHECK(close(h) == 0);
    h = CALL(L, "scePadOpen", SYSV int32_t (*)(int32_t, int32_t, int32_t, void *))(1, 0, 0, NULL);
    CHECK(h > 0);
    CHECK(read(h, d, 8) == 1 && d[0].rx == 77);

    /* Vibration and light bar reach the window without one existing. */
    const uint8_t vib[2] = {10, 20}, light[4] = {255, 0, 0, 0};
    CHECK(CALL(L, "scePadSetVibration", SYSV int32_t (*)(int32_t, const uint8_t *))(h, vib) == 0);
    CHECK(CALL(L, "scePadSetLightBar", SYSV int32_t (*)(int32_t, const uint8_t *))(h, light) == 0);
    CHECK(close(h) == 0);
    return finish("pad");
}

/* ---- main loop scenarios ---------------------------------------------------- */

typedef struct guest_arg {
    int mode; /* 0 headless, 1 window, 2 close */
    void *handle;
    uint64_t wait_ns;
    bool vulkan;
} guest_arg;

static void guest_main(void *p) {
    guest_arg *a = p;
    uint64_t t = plat_monotonic_ns();
    a->handle = window_sdl_handle();
    a->wait_ns = plat_monotonic_ns() - t;
    CHECK(window_sdl_handle() == a->handle); /* stable, and immediate now */
    if (a->handle) {
        a->vulkan = (SDL_GetWindowFlags(a->handle) & SDL_WINDOW_VULKAN) != 0;
        int w = 0, h = 0;
        CHECK(SDL_GetWindowSizeInPixels(a->handle, &w, &h) && w > 0 && h > 0);
    }
    if (a->mode == 2 && a->handle) {
        /* Close the window; the loop must return while this thread runs. */
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_QUIT;
        CHECK(SDL_PushEvent(&e));
        plat_sleep_us(10 * 1000 * 1000);
        fprintf(stderr, "FAIL: host_main_loop did not return after a quit event\n");
        fflush(stderr);
        _Exit(1);
    }
    plat_sleep_us(200 * 1000);
}

static int mode_loop(int mode) {
    static const char *names[] = {"headless", "window", "close"};
    if (mode == 0) {
#ifdef _WIN32
        _putenv("SOTC_HEADLESS=1");
#else
        setenv("SOTC_HEADLESS", "1", 1);
#endif
    }
    guest_arg a = {.mode = mode};
    plat_thread *t = plat_thread_create(guest_main, &a, 256 * 1024);
    CHECK(t);
    uint64_t start = plat_monotonic_ns();
    int code = host_main_loop(t);
    uint64_t took = plat_monotonic_ns() - start;
    CHECK(code == 0);
    if (mode == 0) {
        CHECK(a.handle == NULL);
        CHECK(a.wait_ns < 2000000000ull);
        CHECK(took >= 150000000ull); /* waited for the guest thread */
    } else if (!a.handle) {
        printf("%s: SKIP (no display or no Vulkan window)\n", names[mode]);
        return g_failures ? 1 : 0;
    } else {
        CHECK(a.vulkan);
        if (mode == 1) CHECK(took >= 150000000ull);
    }
    return finish(names[mode]);
}

/* ---- driver ------------------------------------------------------------------ */

static int run_child(const char *self, const char *mode) {
    fflush(NULL);
#ifdef _WIN32
    intptr_t r = _spawnl(_P_WAIT, self, "test_window", mode, NULL);
    return r == -1 ? 1 : (int)r;
#else
    pid_t pid = fork();
    if (pid == 0) {
        execl(self, self, mode, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
#endif
}

int main(int argc, char **argv) {
    log_init();
    hle_init();
    if (argc > 1) {
        if (!strcmp(argv[1], "pad")) return mode_pad();
        if (!strcmp(argv[1], "headless")) return mode_loop(0);
        if (!strcmp(argv[1], "window")) return mode_loop(1);
        if (!strcmp(argv[1], "close")) return mode_loop(2);
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    static const char *modes[] = {"pad", "headless", "window", "close"};
    int failed = 0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        int r = run_child(argv[0], modes[i]);
        if (r) {
            printf("%s: FAILED (exit %d)\n", modes[i], r);
            failed++;
        }
    }
    if (failed) return 1;
    printf("window tests passed\n");
    return 0;
}
