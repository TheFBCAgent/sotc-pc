/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceSystemService: system settings and the (empty) system event queue.
 *
 *   ParamGetInt: language from SOTC_LANGUAGE (PS4 language id, default 1 =
 *   English US), enter button from SOTC_ENTER_BUTTON ("circle" or "cross",
 *   default cross), date DD/MM/YYYY, 24-hour clock, the host's time zone.
 *
 * Semantics follow shadPS4 (src/core/libraries/system/systemservice.cpp,
 * GPL-2.0, commit 14ada48). */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../log.h"
#include "hle.h"

#define SYSTEM_SERVICE_ERROR_PARAMETER ((int32_t)0x80A10003)
#define SYSTEM_SERVICE_ERROR_NO_EVENT  ((int32_t)0x80A10004)

enum {
    PARAM_ID_LANG = 1,
    PARAM_ID_DATE_FORMAT = 2,
    PARAM_ID_TIME_FORMAT = 3,
    PARAM_ID_TIME_ZONE = 4,
    PARAM_ID_SUMMERTIME = 5,
    PARAM_ID_SYSTEM_NAME = 6,
    PARAM_ID_GAME_PARENTAL_LEVEL = 7,
    PARAM_ID_ENTER_BUTTON_ASSIGN = 1000,
};

enum { DATE_FORMAT_DDMMYYYY = 1, TIME_FORMAT_24HOUR = 1, PARENTAL_LEVEL_OFF = 0 };
enum { ENTER_BUTTON_CIRCLE = 0, ENTER_BUTTON_CROSS = 1 };

/* Host UTC offset in minutes (standard time) and whether DST is in effect. */
static void host_timezone(int32_t *offset_min, int32_t *dst) {
    time_t now = time(NULL);
    struct tm local, utc;
#ifdef _WIN32
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
#else
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
#endif
    int is_dst = local.tm_isdst > 0;
    utc.tm_isdst = 0;
    local.tm_isdst = 0;
    /* Both broken-down times reinterpreted as local: their difference is the
     * current offset, DST included. */
    long diff = (long)difftime(mktime(&local), mktime(&utc));
    int32_t minutes = (int32_t)(diff / 60);
    if (is_dst) minutes -= 60;
    if (offset_min) *offset_min = minutes;
    if (dst) *dst = is_dst;
}

static int32_t param_language(void) {
    const char *env = getenv("SOTC_LANGUAGE");
    if (env && *env) return (int32_t)strtol(env, NULL, 0);
    return 1; /* English (United States) */
}

static int32_t param_enter_button(void) {
    const char *env = getenv("SOTC_ENTER_BUTTON");
    if (env && (env[0] == 'c' || env[0] == 'C') && (env[1] == 'i' || env[1] == 'I'))
        return ENTER_BUTTON_CIRCLE;
    return ENTER_BUTTON_CROSS;
}

static SYSV int32_t ps4_sceSystemServiceParamGetInt(int32_t id, int32_t *value) {
    if (!value) return SYSTEM_SERVICE_ERROR_PARAMETER;
    switch (id) {
    case PARAM_ID_LANG: *value = param_language(); break;
    case PARAM_ID_DATE_FORMAT: *value = DATE_FORMAT_DDMMYYYY; break;
    case PARAM_ID_TIME_FORMAT: *value = TIME_FORMAT_24HOUR; break;
    case PARAM_ID_TIME_ZONE: host_timezone(value, NULL); break;
    case PARAM_ID_SUMMERTIME: host_timezone(NULL, value); break;
    case PARAM_ID_GAME_PARENTAL_LEVEL: *value = PARENTAL_LEVEL_OFF; break;
    case PARAM_ID_ENTER_BUTTON_ASSIGN: *value = param_enter_button(); break;
    default:
        log_warn("sceSystemServiceParamGetInt: unknown param %d -> 0", id);
        *value = 0;
        break;
    }
    log_debug("sceSystemServiceParamGetInt(%d) -> %d", id, *value);
    return SCE_OK;
}

/* SceSystemServiceStatus. Only the documented fields are written: the size of
 * the reserved tail differs between SDKs. */
typedef struct system_service_status {
    int32_t event_num;
    uint8_t is_system_ui_overlaid;
    uint8_t is_in_background_execution;
    uint8_t is_cpu_mode7_cpu_normal;
    uint8_t is_game_live_streaming_on_air;
    uint8_t is_out_of_vr_play_area;
} system_service_status;

static SYSV int32_t ps4_sceSystemServiceGetStatus(system_service_status *status) {
    if (!status) return SYSTEM_SERVICE_ERROR_PARAMETER;
    status->event_num = 0;
    status->is_system_ui_overlaid = 0;
    status->is_in_background_execution = 0;
    status->is_cpu_mode7_cpu_normal = 1;
    status->is_game_live_streaming_on_air = 0;
    status->is_out_of_vr_play_area = 0;
    return SCE_OK;
}

typedef struct display_safe_area_info {
    float ratio;
    uint8_t reserved[128];
} display_safe_area_info;

static SYSV int32_t ps4_sceSystemServiceGetDisplaySafeAreaInfo(display_safe_area_info *info) {
    if (!info) return SYSTEM_SERVICE_ERROR_PARAMETER;
    memset(info, 0, sizeof(*info));
    info->ratio = 1.0f;
    return SCE_OK;
}

static SYSV int32_t ps4_sceSystemServiceReceiveEvent(void *event) {
    if (!event) return SYSTEM_SERVICE_ERROR_PARAMETER;
    return SYSTEM_SERVICE_ERROR_NO_EVENT;
}

static SYSV int32_t ps4_sceSystemServiceHideSplashScreen(void) {
    log_info("systemservice: splash screen hidden");
    return SCE_OK;
}

/* The game asks the system to replace it with another executable (a restart,
 * typically). Not supported: log it and carry on. */
static SYSV int32_t ps4_sceSystemServiceLoadExec(const char *path, const char *const *argv) {
    log_warn("sceSystemServiceLoadExec(\"%s\") ignored", path ? path : "(null)");
    for (int i = 0; argv && argv[i] && i < 16; i++) log_warn("  argv[%d] = \"%s\"", i, argv[i]);
    return SCE_OK;
}

HLE_LIBRARY(hle_systemservice, "libSceSystemService",
    HLE(sceSystemServiceParamGetInt),
    HLE(sceSystemServiceGetStatus),
    HLE(sceSystemServiceGetDisplaySafeAreaInfo),
    HLE(sceSystemServiceReceiveEvent),
    HLE(sceSystemServiceHideSplashScreen),
    HLE(sceSystemServiceLoadExec),
)
