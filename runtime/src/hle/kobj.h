/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Integer handles for guest-visible kernel objects (VideoOut ports, pad
 * handles, ...). Many PS4 handles are pointers instead (event flags, equeues,
 * mutexes); those just hand the host object's address to the guest. */
#ifndef SOTC_KOBJ_H
#define SOTC_KOBJ_H

#include <stdint.h>

/* Allocate a handle (> 0) for `obj` tagged with `type`; -1 when full. */
int32_t kobj_alloc(uint32_t type, void *obj);
/* The object for `handle` if it exists and has `type`, else NULL. */
void *kobj_get(int32_t handle, uint32_t type);
/* Release `handle`; returns the object it held (or NULL). */
void *kobj_free(int32_t handle);

/* Type tags: one per object kind, so a handle of the wrong kind is rejected. */
enum {
    KOBJ_VIDEOOUT = 1,
    KOBJ_PAD,
    KOBJ_AUDIOOUT,
    KOBJ_TROPHY_CONTEXT,
    KOBJ_TROPHY_HANDLE,
    KOBJ_SAVEDATA_MOUNT,
    KOBJ_DIALOG,
    KOBJ_NET,
    KOBJ_USER_FIRST = 0x100, /* workstreams may add their own from here */
};

#endif
