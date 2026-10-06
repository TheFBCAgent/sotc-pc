/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "kobj.h"

#include <stddef.h>

#include "../platform.h"

#define MAX_KOBJ 4096

typedef struct slot {
    uint32_t type;   /* 0 = free */
    void *obj;
} slot;

static slot g_slots[MAX_KOBJ];
static plat_mutex *g_lock;

/* Lazily created; kobj may be used before hle_init by constructors' callers. */
static plat_mutex *lock(void) {
    if (!g_lock) g_lock = plat_mutex_new();
    return g_lock;
}

int32_t kobj_alloc(uint32_t type, void *obj) {
    plat_mutex *m = lock();
    plat_mutex_lock(m);
    for (int32_t i = 1; i < MAX_KOBJ; i++)
        if (!g_slots[i].type) {
            g_slots[i].type = type;
            g_slots[i].obj = obj;
            plat_mutex_unlock(m);
            return i;
        }
    plat_mutex_unlock(m);
    return -1;
}

void *kobj_get(int32_t handle, uint32_t type) {
    if (handle <= 0 || handle >= MAX_KOBJ) return NULL;
    plat_mutex *m = lock();
    plat_mutex_lock(m);
    void *obj = g_slots[handle].type == type ? g_slots[handle].obj : NULL;
    plat_mutex_unlock(m);
    return obj;
}

void *kobj_free(int32_t handle) {
    if (handle <= 0 || handle >= MAX_KOBJ) return NULL;
    plat_mutex *m = lock();
    plat_mutex_lock(m);
    void *obj = g_slots[handle].obj;
    g_slots[handle].type = 0;
    g_slots[handle].obj = NULL;
    plat_mutex_unlock(m);
    return obj;
}
