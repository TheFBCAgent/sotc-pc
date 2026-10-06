/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libScePad: one DualShock 4, connected to user 1, with neutral input (no
 * buttons, centered sticks, level orientation). Real input arrives in M5.
 *
 * Structure layouts, error codes and validation follow shadPS4
 * (src/core/libraries/pad/pad.cpp, pad.h, pad_errors.h; GPL-2.0, commit
 * 14ada48). */
#include <stddef.h>
#include <string.h>

#include "../log.h"
#include "../platform.h"
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
    log_info("pad: opened user %d type %d index %d -> handle %d", user_id, type, index, h);
    return h;
}

static SYSV int32_t ps4_scePadClose(int32_t handle) {
    if (!pad_get(handle)) return PAD_ERROR_INVALID_HANDLE;
    kobj_free(handle);
    g_handle = -1;
    return SCE_OK;
}

static void neutral_state(pad_data *d) {
    memset(d, 0, sizeof(*d));
    d->left_x = d->left_y = d->right_x = d->right_y = 128;
    d->orientation[3] = 1.0f;
    d->acceleration[1] = 1.0f; /* gravity, controller lying flat (in g) */
    d->connected = 1;
    d->connected_count = 1;
    d->timestamp = plat_monotonic_ns() / 1000;
}

static SYSV int32_t ps4_scePadRead(int32_t handle, pad_data *data, int32_t num) {
    if (!data || num < 1 || num > PAD_MAX_DATA_NUM) return PAD_ERROR_INVALID_ARG;
    if (!pad_get(handle)) return PAD_ERROR_INVALID_HANDLE;
    /* Nothing changes between reads, so a single sample covers the period. */
    neutral_state(&data[0]);
    return 1;
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
    log_debug("pad: light bar %02x%02x%02x", param->r, param->g, param->b);
    return SCE_OK;
}

typedef struct pad_vibration_param {
    uint8_t large_motor, small_motor;
} pad_vibration_param;

static SYSV int32_t ps4_scePadSetVibration(int32_t handle, const pad_vibration_param *param) {
    if (!pad_get(handle)) return PAD_ERROR_INVALID_HANDLE;
    if (!param) return PAD_ERROR_INVALID_ARG;
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
