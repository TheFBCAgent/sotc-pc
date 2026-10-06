/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PlayStation Network: libSceNpManager and libSceNpTrophy, offline.
 *
 * NpManager only records the title id and content restriction. NpTrophy hands
 * out real context and handle ids (so the game's bookkeeping works) and
 * validates them like the firmware; unlocking a trophy only logs it. Trophies
 * are not persisted.
 *
 * Semantics and error codes follow shadPS4 (src/core/libraries/np/
 * np_manager.cpp, np_trophy.cpp, np_error.h; GPL-2.0, commit 14ada48). */
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "hle.h"
#include "kobj.h"

/* ---- libSceNpManager ---------------------------------------------------- */

#define NP_ERROR_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_ERROR_INVALID_SIZE     ((int32_t)0x80550011)

typedef struct np_title_id {
    char id[13];
    uint8_t padding[3];
} np_title_id;

typedef struct np_content_restriction {
    uint64_t size;
    int8_t default_age_restriction;
    uint8_t padding[3];
    int32_t age_restriction_count;
    const void *age_restriction;
} np_content_restriction;

static SYSV int32_t ps4_sceNpSetNpTitleId(const np_title_id *title_id, const void *secret) {
    if (!title_id || !secret) return NP_ERROR_INVALID_ARGUMENT;
    char id[sizeof(title_id->id) + 1];
    memcpy(id, title_id->id, sizeof(title_id->id));
    id[sizeof(title_id->id)] = 0;
    log_info("np: title id %s", id);
    return SCE_OK;
}

static SYSV int32_t ps4_sceNpSetContentRestriction(const np_content_restriction *r) {
    if (!r) return NP_ERROR_INVALID_ARGUMENT;
    if (r->size != sizeof(np_content_restriction)) return NP_ERROR_INVALID_SIZE;
    if (r->default_age_restriction < 0 || r->age_restriction_count > 0x100 ||
        (r->age_restriction_count > 0 && !r->age_restriction))
        return NP_ERROR_INVALID_ARGUMENT;
    log_debug("np: content restriction, default age %d, %d region(s)",
              r->default_age_restriction, r->age_restriction_count);
    return SCE_OK;
}

HLE_LIBRARY(hle_npmanager, "libSceNpManager",
    HLE(sceNpSetNpTitleId),
    HLE(sceNpSetContentRestriction),
)

/* ---- libSceNpTrophy ----------------------------------------------------- */

#define NP_TROPHY_ERROR_INVALID_ARGUMENT       ((int32_t)0x80551604)
#define NP_TROPHY_ERROR_INVALID_HANDLE         ((int32_t)0x80551608)
#define NP_TROPHY_ERROR_INVALID_CONTEXT        ((int32_t)0x80551609)
#define NP_TROPHY_ERROR_INVALID_TROPHY_ID      ((int32_t)0x8055160A)
#define NP_TROPHY_ERROR_NOT_REGISTERED         ((int32_t)0x8055160F)
#define NP_TROPHY_ERROR_ALREADY_REGISTERED     ((int32_t)0x80551610)
#define NP_TROPHY_ERROR_CONTEXT_ALREADY_EXISTS ((int32_t)0x80551613)
#define NP_TROPHY_ERROR_CONTEXT_EXCEEDS_MAX    ((int32_t)0x80551623)
#define NP_TROPHY_ERROR_HANDLE_EXCEEDS_MAX     ((int32_t)0x80551624)

#define NP_TROPHY_NUM_MAX 128
#define NP_TROPHY_INVALID_TROPHY_ID (-1)
#define MAX_TROPHY_CONTEXTS 8
#define MAX_TROPHY_HANDLES 4

typedef struct trophy_context {
    int32_t user_id;
    uint32_t service_label;
    int registered;
    uint8_t logged[NP_TROPHY_NUM_MAX / 8]; /* unlocks already logged */
} trophy_context;

static trophy_context *g_contexts[MAX_TROPHY_CONTEXTS];
static int g_handle_count;
static volatile int g_trophy_lock;

static void trophy_lock(void) {
    while (__atomic_exchange_n(&g_trophy_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}
static void trophy_unlock(void) { __atomic_store_n(&g_trophy_lock, 0, __ATOMIC_RELEASE); }

static SYSV int32_t ps4_sceNpTrophyCreateContext(int32_t *context, int32_t user_id,
                                                 uint32_t service_label, uint64_t options) {
    if (!context || options) return NP_TROPHY_ERROR_INVALID_ARGUMENT;
    int32_t ret;
    trophy_lock();
    int free_slot = -1;
    ret = SCE_OK;
    for (int i = 0; i < MAX_TROPHY_CONTEXTS; i++) {
        if (!g_contexts[i]) {
            if (free_slot < 0) free_slot = i;
        } else if (g_contexts[i]->user_id == user_id &&
                   g_contexts[i]->service_label == service_label) {
            ret = NP_TROPHY_ERROR_CONTEXT_ALREADY_EXISTS;
        }
    }
    if (ret == SCE_OK && free_slot < 0) ret = NP_TROPHY_ERROR_CONTEXT_EXCEEDS_MAX;
    if (ret == SCE_OK) {
        trophy_context *ctx = calloc(1, sizeof(*ctx));
        int32_t id = ctx ? kobj_alloc(KOBJ_TROPHY_CONTEXT, ctx) : -1;
        if (id < 0) {
            free(ctx);
            ret = NP_TROPHY_ERROR_CONTEXT_EXCEEDS_MAX;
        } else {
            ctx->user_id = user_id;
            ctx->service_label = service_label;
            g_contexts[free_slot] = ctx;
            *context = id;
            log_debug("np trophy: context %d (user %d, service label %u)", id, user_id,
                      service_label);
        }
    }
    trophy_unlock();
    return ret;
}

static SYSV int32_t ps4_sceNpTrophyDestroyContext(int32_t context) {
    trophy_lock();
    trophy_context *ctx = kobj_get(context, KOBJ_TROPHY_CONTEXT);
    if (ctx) {
        kobj_free(context);
        for (int i = 0; i < MAX_TROPHY_CONTEXTS; i++)
            if (g_contexts[i] == ctx) g_contexts[i] = NULL;
        free(ctx);
    }
    trophy_unlock();
    return ctx ? SCE_OK : NP_TROPHY_ERROR_INVALID_CONTEXT;
}

/* Trophy handles carry no state; the kobj slot only proves they exist. */
static int g_handle_tag;

static SYSV int32_t ps4_sceNpTrophyCreateHandle(int32_t *handle) {
    if (!handle) return NP_TROPHY_ERROR_INVALID_ARGUMENT;
    int32_t ret = SCE_OK;
    trophy_lock();
    if (g_handle_count >= MAX_TROPHY_HANDLES) {
        ret = NP_TROPHY_ERROR_HANDLE_EXCEEDS_MAX;
    } else {
        int32_t id = kobj_alloc(KOBJ_TROPHY_HANDLE, &g_handle_tag);
        if (id < 0) {
            ret = NP_TROPHY_ERROR_HANDLE_EXCEEDS_MAX;
        } else {
            g_handle_count++;
            *handle = id;
        }
    }
    trophy_unlock();
    return ret;
}

static SYSV int32_t ps4_sceNpTrophyDestroyHandle(int32_t handle) {
    trophy_lock();
    int ok = kobj_get(handle, KOBJ_TROPHY_HANDLE) != NULL;
    if (ok) {
        kobj_free(handle);
        g_handle_count--;
    }
    trophy_unlock();
    return ok ? SCE_OK : NP_TROPHY_ERROR_INVALID_HANDLE;
}

static SYSV int32_t ps4_sceNpTrophyRegisterContext(int32_t context, int32_t handle,
                                                   uint64_t options) {
    if (options) return NP_TROPHY_ERROR_INVALID_ARGUMENT;
    int32_t ret = SCE_OK;
    trophy_lock();
    trophy_context *ctx = kobj_get(context, KOBJ_TROPHY_CONTEXT);
    if (!ctx) ret = NP_TROPHY_ERROR_INVALID_CONTEXT;
    else if (!kobj_get(handle, KOBJ_TROPHY_HANDLE)) ret = NP_TROPHY_ERROR_INVALID_HANDLE;
    else if (ctx->registered) ret = NP_TROPHY_ERROR_ALREADY_REGISTERED;
    else ctx->registered = 1;
    trophy_unlock();
    if (ret == SCE_OK) log_debug("np trophy: context %d registered", context);
    return ret;
}

static SYSV int32_t ps4_sceNpTrophyUnlockTrophy(int32_t context, int32_t handle, int32_t trophy_id,
                                                int32_t *platinum_id) {
    if (trophy_id < 0 || trophy_id >= NP_TROPHY_NUM_MAX) return NP_TROPHY_ERROR_INVALID_TROPHY_ID;
    if (!platinum_id) return NP_TROPHY_ERROR_INVALID_ARGUMENT;
    int32_t ret = SCE_OK;
    int first = 0;
    trophy_lock();
    trophy_context *ctx = kobj_get(context, KOBJ_TROPHY_CONTEXT);
    if (!ctx) ret = NP_TROPHY_ERROR_INVALID_CONTEXT;
    else if (!kobj_get(handle, KOBJ_TROPHY_HANDLE)) ret = NP_TROPHY_ERROR_INVALID_HANDLE;
    else if (!ctx->registered) ret = NP_TROPHY_ERROR_NOT_REGISTERED;
    else {
        uint8_t bit = (uint8_t)(1u << (trophy_id & 7));
        first = !(ctx->logged[trophy_id >> 3] & bit);
        ctx->logged[trophy_id >> 3] |= bit;
    }
    trophy_unlock();
    if (ret != SCE_OK) return ret;
    if (first) log_info("np trophy: unlocked trophy %d", trophy_id);
    *platinum_id = NP_TROPHY_INVALID_TROPHY_ID;
    return SCE_OK;
}

HLE_LIBRARY(hle_nptrophy, "libSceNpTrophy",
    HLE(sceNpTrophyCreateContext),
    HLE(sceNpTrophyDestroyContext),
    HLE(sceNpTrophyCreateHandle),
    HLE(sceNpTrophyDestroyHandle),
    HLE(sceNpTrophyRegisterContext),
    HLE(sceNpTrophyUnlockTrophy),
)
