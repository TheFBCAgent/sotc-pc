/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceUserService: a single local user, always logged in.
 *
 *   user id 1, name from SOTC_USER_NAME (default "Wander").
 *
 * No login/logout events are generated: the initial user is already logged in
 * when the game starts, so sceUserServiceGetEvent always reports NO_EVENT.
 *
 * Semantics follow shadPS4 (src/core/libraries/system/userservice.cpp,
 * GPL-2.0, commit 14ada48). */
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "hle.h"

#define USER_SERVICE_ERROR_INVALID_ARGUMENT    ((int32_t)0x80960005)
#define USER_SERVICE_ERROR_NO_EVENT            ((int32_t)0x80960007)
#define USER_SERVICE_ERROR_BUFFER_TOO_SHORT    ((int32_t)0x8096000A)

#define USER_SERVICE_MAX_LOGIN_USERS 4
#define USER_SERVICE_USER_ID_INVALID (-1)
#define USER_SERVICE_MAX_USER_NAME_LENGTH 16

/* The one local user; pad.c opens controllers for this id. */
#define SOTC_USER_ID 1

static int g_initialized;

static const char *user_name(void) {
    const char *env = getenv("SOTC_USER_NAME");
    return env && *env ? env : "Wander";
}

typedef struct user_service_init_params {
    int32_t priority;
} user_service_init_params;

static SYSV int32_t ps4_sceUserServiceInitialize(const user_service_init_params *params) {
    log_debug("sceUserServiceInitialize(priority %d)", params ? params->priority : -1);
    /* Firmware returns ALREADY_INITIALIZED on a second call; like shadPS4 we
     * accept it, since nothing is lost by initializing twice. */
    if (g_initialized) return SCE_OK;
    g_initialized = 1;
    log_info("userservice: user %d \"%s\" logged in", SOTC_USER_ID, user_name());
    return SCE_OK;
}

static SYSV int32_t ps4_sceUserServiceGetInitialUser(int32_t *user_id) {
    if (!user_id) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
    *user_id = SOTC_USER_ID;
    return SCE_OK;
}

typedef struct user_service_login_list {
    int32_t user_id[USER_SERVICE_MAX_LOGIN_USERS];
} user_service_login_list;

static SYSV int32_t ps4_sceUserServiceGetLoginUserIdList(user_service_login_list *list) {
    if (!list) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
    for (int i = 0; i < USER_SERVICE_MAX_LOGIN_USERS; i++)
        list->user_id[i] = USER_SERVICE_USER_ID_INVALID;
    list->user_id[0] = SOTC_USER_ID;
    return SCE_OK;
}

static SYSV int32_t ps4_sceUserServiceGetUserName(int32_t user_id, char *name, uint64_t size) {
    if (user_id == USER_SERVICE_USER_ID_INVALID || !name) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
    if (user_id != SOTC_USER_ID) log_warn("sceUserServiceGetUserName: unknown user %d", user_id);
    /* PS4 user names are at most 16 characters. */
    char buf[USER_SERVICE_MAX_USER_NAME_LENGTH + 1];
    size_t len = strlen(user_name());
    if (len > USER_SERVICE_MAX_USER_NAME_LENGTH) len = USER_SERVICE_MAX_USER_NAME_LENGTH;
    memcpy(buf, user_name(), len);
    buf[len] = 0;
    if (size < len + 1) return USER_SERVICE_ERROR_BUFFER_TOO_SHORT;
    memcpy(name, buf, len + 1);
    return SCE_OK;
}

typedef struct user_service_event {
    int32_t event;   /* 0 login, 1 logout */
    int32_t user_id;
} user_service_event;

static SYSV int32_t ps4_sceUserServiceGetEvent(user_service_event *event) {
    if (!event) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
    return USER_SERVICE_ERROR_NO_EVENT;
}

HLE_LIBRARY(hle_userservice, "libSceUserService",
    HLE(sceUserServiceInitialize),
    HLE(sceUserServiceGetInitialUser),
    HLE(sceUserServiceGetLoginUserIdList),
    HLE(sceUserServiceGetUserName),
    HLE(sceUserServiceGetEvent),
)
