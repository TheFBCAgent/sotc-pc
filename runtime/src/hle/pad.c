/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libScePad: one DualShock 4, connected to user 1. Its buttons, sticks and
 * triggers come from the host window (window.c: an SDL gamepad, or the
 * keyboard), which hands complete snapshots over with pad_input_update().
 * Motion stays neutral (level orientation, gravity on +y) and the touchpad
 * reports no touches (its click is a button).
 *
 * Structure layouts, error codes, validation and the sample semantics follow
 * shadPS4 (src/core/libraries/pad/pad.cpp, pad.h, pad_errors.h and
 * src/input/controller.cpp; GPL-2.0, commit 14ada48):
 *  - every input change is a sample stamped with sceKernelGetProcessTime()
 *    (microseconds) and queued, 64 deep, dropping the oldest;
 *  - scePadRead(num == 1) and scePadReadState return the current state; its
 *    timestamp is that of the last change, not of the read;
 *  - scePadRead(num > 1) drains the queue and may return 0 samples;
 *  - opening the pad acts like shadPS4's ConnectController: the queue is
 *    cleared and the current state is queued once.
 * One addition (workstream I, docs/next/i.md): presses are latched, so a
 * press that starts and ends between two reads is not lost when the game
 * reads rarely (the title screen runs at 0-1 fps; SOTC_PAD_SCRIPT holds a
 * button for only 150 ms):
 *  - single reads (scePadReadState, scePadRead(num == 1)) report the current
 *    buttons ORed with every button whose press edge came after the previous
 *    single read; the latch is then cleared, so the next read shows the
 *    release. A held button simply stays pressed. Sticks and triggers are the
 *    current values.
 *  - multi reads already return the press and release samples from the queue;
 *    if the queue overflowed, the press edges of the dropped samples are ORed
 *    into the first sample returned.
 * Each path keeps its own latch, so mixing both read styles loses nothing.
 * The pad is always connected (the keyboard is the fallback when no gamepad
 * is plugged in), so connected = 1 and connected_count = 1. */
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "../log.h"
#include "../platform.h"
#include "../window.h"
#include "hle.h"
#include "kobj.h"

#define PAD_ERROR_INVALID_ARG              ((int32_t)0x80920001)
#define PAD_ERROR_INVALID_HANDLE           ((int32_t)0x80920003)
#define PAD_ERROR_ALREADY_OPENED           ((int32_t)0x80920004)
#define PAD_ERROR_NOT_INITIALIZED          ((int32_t)0x80920005)
#define PAD_ERROR_INVALID_LIGHTBAR_SETTING ((int32_t)0x80920006)
#define DEVICE_SERVICE_ERROR_INVALID_USER   ((int32_t)0x809B0001)
#define DEVICE_SERVICE_ERROR_USER_NOT_LOGIN ((int32_t)0x809B0081)

#define PAD_MAX_DATA_NUM 64
#define PAD_PORT_TYPE_STANDARD 0
#define PAD_PORT_TYPE_SPECIAL 2
#define PAD_PORT_TYPE_REMOTE_CONTROL 16
#define PAD_CONNECTION_TYPE_LOCAL 0
#define PAD_DEVICE_CLASS_STANDARD 0
#define SOTC_USER_ID 1 /* userservice.c */

/* ScePadData, 120 bytes. */
typedef struct pad_data {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_pad[2];
    float orientation[4];        /* quaternion x, y, z, w */
    float acceleration[3];
    float angular_velocity[3];
    struct {
        uint8_t touch_num;
        uint8_t reserve[3];
        uint32_t reserve1;
        struct {
            uint16_t x, y;
            uint8_t id;
            uint8_t reserve[3];
        } touch[2];
    } touch_data;
    uint8_t connected;
    uint64_t timestamp;          /* microseconds */
    struct {
        uint32_t extension_unit_id;
        uint8_t reserve;
        uint8_t data_length;
        uint8_t data[10];
    } extension_unit;
    uint8_t connected_count;
    uint8_t reserve[2];
    uint8_t device_unique_data_len;
    uint8_t device_unique_data[12];
} pad_data;
_Static_assert(sizeof(pad_data) == 120, "ScePadData is 120 bytes");
_Static_assert(offsetof(pad_data, touch_data) == 52, "ScePadData layout");
_Static_assert(offsetof(pad_data, connected) == 76, "ScePadData layout");
_Static_assert(offsetof(pad_data, timestamp) == 80, "ScePadData layout");

/* ScePadControllerInformation, 28 bytes. */
typedef struct pad_controller_info {
    float touch_pixel_density;
    uint16_t touch_resolution_x, touch_resolution_y;
    uint8_t dead_zone_left, dead_zone_right;
    uint8_t connection_type;
    uint8_t connected_count;
    uint8_t connected;
    int32_t device_class;
    uint8_t reserve[8];
} pad_controller_info;
_Static_assert(sizeof(pad_controller_info) == 28, "ScePadControllerInformation is 28 bytes");

/* ---- Input hand-off: host main thread -> guest threads -------------------- */

typedef struct pad_sample {
    pad_input in;
    uint64_t time; /* process time, microseconds */
} pad_sample;

#define PAD_QUEUE_LEN 64
/* A spinlock: the critical sections only copy a few samples, and it needs no
 * initialization (the window may report input before scePadInit). */
static atomic_flag g_input_lock = ATOMIC_FLAG_INIT;
static pad_sample g_cur = {.in = {.axes = {128, 128, 128, 128, 0, 0}}};
static pad_sample g_queue[PAD_QUEUE_LEN];
static unsigned g_queue_head, g_queue_count;
/* Press edges not yet seen by a single read / lost to a queue overflow. */
static uint32_t g_latch_single, g_latch_dropped;

static void input_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_input_lock, memory_order_acquire)) plat_yield();
}
static void input_unlock(void) { atomic_flag_clear_explicit(&g_input_lock, memory_order_release); }

static uint64_t process_time_us(void) {
    typedef SYSV uint64_t (*time_fn)(void);
    static _Atomic(time_fn) get;
    time_fn f = atomic_load_explicit(&get, memory_order_relaxed);
    if (!f) {
        f = (time_fn)hle_lookup("libkernel", "sceKernelGetProcessTime");
        atomic_store_explicit(&get, f, memory_order_relaxed);
    }
    return f ? f() : plat_monotonic_ns() / 1000;
}

/* With the lock held. */
static void queue_current(void) {
    if (g_queue_count == PAD_QUEUE_LEN) { /* drop the oldest; keep its presses */
        g_latch_dropped |= g_queue[g_queue_head].in.buttons;
        g_queue_head = (g_queue_head + 1) % PAD_QUEUE_LEN;
        g_queue_count--;
    }
    g_queue[(g_queue_head + g_queue_count) % PAD_QUEUE_LEN] = g_cur;
    g_queue_count++;
}

void pad_input_update(const pad_input *in) {
    uint64_t now = process_time_us();
    input_lock();
    if (memcmp(&g_cur.in, in, sizeof(*in)) != 0) {
        g_latch_single |= in->buttons & ~g_cur.in.buttons; /* press edges */
        g_cur.in = *in;
        g_cur.time = now;
        queue_current();
    }
    input_unlock();
}

static void input_connect(void) {
    uint64_t now = process_time_us();
    input_lock();
    g_queue_head = g_queue_count = 0;
    g_latch_single = g_latch_dropped = 0;
    g_cur.time = now;
    queue_current();
    input_unlock();
}

/* ---- libScePad ------------------------------------------------------------ */

typedef struct pad {
    int32_t user_id, type, index;
    uint8_t light[3];
    uint8_t motion_sensor;
} pad;

/* One controller: a single port, opened at most once. */
static pad g_pad;
static int32_t g_handle = -1;
static int g_initialized;

static pad *pad_get(int32_t handle) { return kobj_get(handle, KOBJ_PAD); }

static SYSV int32_t ps4_scePadInit(void) {
    g_initialized = 1;
    return SCE_OK;
}

static SYSV int32_t ps4_scePadOpen(int32_t user_id, int32_t type, int32_t index,
                                   const void *param) {
    (void)param;
    if (!g_initialized) return PAD_ERROR_NOT_INITIALIZED;
    if (user_id < 0) return DEVICE_SERVICE_ERROR_INVALID_USER;
    if (type == PAD_PORT_TYPE_REMOTE_CONTROL) return PAD_ERROR_INVALID_ARG;
    if (user_id != SOTC_USER_ID) return DEVICE_SERVICE_ERROR_USER_NOT_LOGIN;
    if (type != PAD_PORT_TYPE_STANDARD && type != PAD_PORT_TYPE_SPECIAL)
        return PAD_ERROR_INVALID_ARG;
    if (g_handle > 0) return PAD_ERROR_ALREADY_OPENED;
    g_pad = (pad){.user_id = user_id, .type = type, .index = index, .light = {0, 0, 255}};
    int32_t h = kobj_alloc(KOBJ_PAD, &g_pad);
    if (h < 0) return PAD_ERROR_INVALID_ARG;
    g_handle = h;
    input_connect();
    log_info("pad: opened user %d type %d index %d -> handle %d", user_id, type, index, h);
    return h;
}

static SYSV int32_t ps4_scePadClose(int32_t handle) {
    if (!pad_get(handle)) return PAD_ERROR_INVALID_HANDLE;
    kobj_free(handle);
    g_handle = -1;
    return SCE_OK;
}

static void fill_data(pad_data *d, const pad_sample *s) {
    memset(d, 0, sizeof(*d));
    d->buttons = s->in.buttons;
    d->left_x = s->in.axes[PAD_AXIS_LEFT_X];
    d->left_y = s->in.axes[PAD_AXIS_LEFT_Y];
    d->right_x = s->in.axes[PAD_AXIS_RIGHT_X];
    d->right_y = s->in.axes[PAD_AXIS_RIGHT_Y];
    d->l2 = s->in.axes[PAD_AXIS_L2];
    d->r2 = s->in.axes[PAD_AXIS_R2];
    d->orientation[3] = 1.0f;
    d->acceleration[1] = 1.0f; /* gravity, controller lying flat (in g) */
    d->connected = 1;
    d->connected_count = 1;
    d->timestamp = s->time;
}

static SYSV int32_t ps4_scePadRead(int32_t handle, pad_data *data, int32_t num) {
    if (!data || num < 1 || num > PAD_MAX_DATA_NUM) return PAD_ERROR_INVALID_ARG;
    if (!pad_get(handle)) return PAD_ERROR_INVALID_HANDLE;
    /* Copy out under the lock (small, 1 KiB at most on the guest stack), then
     * write guest memory without it. */
    pad_sample samples[PAD_MAX_DATA_NUM];
    int32_t n = 0;
    input_lock();
    if (num == 1) {
        samples[n] = g_cur;
        samples[n++].in.buttons |= g_latch_single;
        g_latch_single = 0;
    } else {
        while (n < num && g_queue_count) {
            samples[n++] = g_queue[g_queue_head];
            g_queue_head = (g_queue_head + 1) % PAD_QUEUE_LEN;
            g_queue_count--;
        }
        if (n) {
            samples[0].in.buttons |= g_latch_dropped;
            g_latch_dropped = 0;
        }
    }
    input_unlock();
    for (int32_t i = 0; i < n; i++) fill_data(&data[i], &samples[i]);
    return n;
}

static SYSV int32_t ps4_scePadReadState(int32_t handle, pad_data *data) {
    int32_t r = ps4_scePadRead(handle, data, 1);
    return r < 0 ? r : SCE_OK;
}

static SYSV int32_t ps4_scePadGetControllerInformation(int32_t handle, pad_controller_info *info) {
    if (!pad_get(handle)) return PAD_ERROR_INVALID_HANDLE;
    if (!info) return PAD_ERROR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->touch_pixel_density = 44.86f; /* DS4: 1920 points over ~42.8 mm */
    info->touch_resolution_x = 1920;
    info->touch_resolution_y = 943;
    info->dead_zone_left = 13;
    info->dead_zone_right = 13;
    info->connection_type = PAD_CONNECTION_TYPE_LOCAL;
    info->connected_count = 1;
    info->connected = 1;
    info->device_class = PAD_DEVICE_CLASS_STANDARD;
    return SCE_OK;
}

typedef struct pad_light_bar_param {
    uint8_t r, g, b, reserve;
} pad_light_bar_param;

static SYSV int32_t ps4_scePadSetLightBar(int32_t handle, const pad_light_bar_param *param) {
    pad *p = pad_get(handle);
    if (!p) return PAD_ERROR_INVALID_HANDLE;
    if (!param) return PAD_ERROR_INVALID_ARG;
    /* The firmware rejects colors too dark to see. */
    if (param->r < 0xD && param->g < 0xD && param->b < 0xD) return PAD_ERROR_INVALID_LIGHTBAR_SETTING;
    p->light[0] = param->r;
    p->light[1] = param->g;
    p->light[2] = param->b;
    window_pad_light(param->r, param->g, param->b);
    log_debug("pad: light bar %02x%02x%02x", param->r, param->g, param->b);
    return SCE_OK;
}

typedef struct pad_vibration_param {
    uint8_t large_motor, small_motor;
} pad_vibration_param;

static SYSV int32_t ps4_scePadSetVibration(int32_t handle, const pad_vibration_param *param) {
    if (!pad_get(handle)) return PAD_ERROR_INVALID_HANDLE;
    if (!param) return PAD_ERROR_INVALID_ARG;
    window_pad_rumble(param->large_motor, param->small_motor);
    log_trace("pad: vibration %u/%u", param->large_motor, param->small_motor);
    return SCE_OK;
}

static SYSV int32_t ps4_scePadSetMotionSensorState(int32_t handle, int32_t enable) {
    pad *p = pad_get(handle);
    if (!p) return PAD_ERROR_INVALID_HANDLE;
    p->motion_sensor = enable != 0;
    return SCE_OK;
}

HLE_LIBRARY(hle_pad, "libScePad",
    HLE(scePadInit),
    HLE(scePadOpen),
    HLE(scePadClose),
    HLE(scePadRead),
    HLE(scePadReadState),
    HLE(scePadGetControllerInformation),
    HLE(scePadSetLightBar),
    HLE(scePadSetVibration),
    HLE(scePadSetMotionSensorState),
)
