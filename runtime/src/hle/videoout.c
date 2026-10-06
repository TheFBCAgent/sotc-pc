/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceVideoOut without a display: one output port, registered buffers, a
 * flip queue and a 60 Hz vblank thread that performs queued flips and fires
 * flip and vblank events (EVFILT_VIDEO_OUT). Nothing is presented yet (M3).
 *
 * Semantics, structure layouts and error codes follow shadPS4 (commit
 * 14ada48, GPL-2.0): src/core/libraries/videoout/{video_out,driver}.cpp,
 * video_out.h, buffer.h and videoout_error.h. */
#include <string.h>

#include "hle.h"
#include "kobj.h"
#include "pm4.h"

/* ---- constants (shadPS4 videoout_error.h / video_out.h) ----------------- */

#define VO_ERROR_INVALID_VALUE ((int32_t)0x80290001)
#define VO_ERROR_INVALID_ADDRESS ((int32_t)0x80290002)
#define VO_ERROR_INVALID_PITCH ((int32_t)0x80290004)
#define VO_ERROR_INVALID_TILING_MODE ((int32_t)0x80290007)
#define VO_ERROR_INVALID_ASPECT_RATIO ((int32_t)0x80290008)
#define VO_ERROR_RESOURCE_BUSY ((int32_t)0x80290009)
#define VO_ERROR_INVALID_INDEX ((int32_t)0x8029000A)
#define VO_ERROR_INVALID_HANDLE ((int32_t)0x8029000B)
#define VO_ERROR_INVALID_EVENT_QUEUE ((int32_t)0x8029000C)
#define VO_ERROR_INVALID_EVENT ((int32_t)0x8029000D)
#define VO_ERROR_NO_EMPTY_SLOT ((int32_t)0x8029000F)
#define VO_ERROR_SLOT_OCCUPIED ((int32_t)0x80290010)
#define VO_ERROR_FLIP_QUEUE_FULL ((int32_t)0x80290012)

/* Internal event idents (OrbisVideoOutInternalEventId) and public ids. */
enum { VO_IDENT_FLIP = 0x6, VO_IDENT_VBLANK = 0x7, VO_IDENT_SET_MODE = 0x51,
       VO_IDENT_POSITION = 0x58, VO_IDENT_PRE_VBLANK = 0x59, VO_IDENT_SYS_VBLANK = 0x63 };
enum { VO_EVENT_FLIP = 0, VO_EVENT_VBLANK = 1, VO_EVENT_PRE_VBLANK = 2, VO_EVENT_SET_MODE = 8,
       VO_EVENT_POSITION = 12 };

#define VO_MAX_BUFFERS 16
#define VO_MAX_GROUPS 4
#define VO_MAX_EVENTS 8
#define VO_FLIP_QUEUE 32
#define VO_REFRESH_59_94HZ 3
#define VO_VBLANK_NS (1000000000ull / 60)
#define VO_WIDTH 1920
#define VO_HEIGHT 1080

/* ---- guest structures --------------------------------------------------- */

typedef struct vo_buffer_attribute {
    uint32_t pixel_format;
    int32_t tiling_mode;
    int32_t aspect_ratio;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_in_pixel;
    uint32_t option;
    uint32_t reserved0;
    uint64_t reserved1;
} vo_buffer_attribute;
_Static_assert(sizeof(vo_buffer_attribute) == 40, "SceVideoOutBufferAttribute");

typedef struct vo_flip_status {
    uint64_t count;
    uint64_t process_time;
    uint64_t tsc;
    int64_t flip_arg;
    uint64_t submit_tsc;
    uint64_t reserved0;
    int32_t gc_queue_num;
    int32_t flip_pending_num;
    int32_t current_buffer;
    uint32_t reserved1;
} vo_flip_status;
_Static_assert(sizeof(vo_flip_status) == 64, "SceVideoOutFlipStatus");

typedef struct vo_resolution_status {
    int32_t full_width;
    int32_t full_height;
    int32_t pane_width;
    int32_t pane_height;
    uint64_t refresh_rate;
    float screen_size_in_inch;
    uint16_t flags;
    uint16_t reserved0;
    uint32_t reserved1[3];
} vo_resolution_status;
_Static_assert(sizeof(vo_resolution_status) == 48, "SceVideoOutResolutionStatus");

typedef struct vo_vblank_status {
    uint64_t count;
    uint64_t process_time;
    uint64_t tsc;
    uint64_t reserved;
    uint8_t flags;
    uint8_t pad[7];
} vo_vblank_status;
_Static_assert(sizeof(vo_vblank_status) == 40, "SceVideoOutVblankStatus");

typedef struct vo_color_settings {
    float gamma;
    uint32_t reserved[3];
} vo_color_settings;

/* SceVideoOutMode (shadPS4 video_out.cpp `Mode`). */
typedef struct vo_mode {
    uint32_t size;
    uint8_t encoding, range, colorimetry, depth;
    uint64_t refresh_rate;
    uint64_t resolution;
    uint8_t reserved[8];
} vo_mode;

/* ---- the port ----------------------------------------------------------- */

typedef struct vo_event_reg {
    equeue *eq;
} vo_event_reg;

typedef struct vo_flip_req {
    int32_t index;
    int64_t flip_arg;
    bool eop;
} vo_flip_req;

typedef struct vo_port {
    plat_mutex *lock;
    plat_cond *vblank_cv;
    plat_thread *thread;
    volatile bool running;
    bool is_open;
    int32_t handle;

    vo_resolution_status resolution;
    struct { int32_t group; uint64_t address; } buffers[VO_MAX_BUFFERS];
    struct { bool occupied; vo_buffer_attribute attr; } groups[VO_MAX_GROUPS];
    vo_flip_status flip_status;
    vo_vblank_status vblank_status;
    int32_t flip_rate;

    vo_event_reg flip_events[VO_MAX_EVENTS];
    int n_flip_events;
    vo_event_reg vblank_events[VO_MAX_EVENTS];
    int n_vblank_events;

    vo_flip_req queue[VO_FLIP_QUEUE];
    uint32_t q_head, q_len;
} vo_port;

static vo_port g_port;
static uint64_t g_start_ns;

__attribute__((constructor)) static void videoout_ctor(void) {
    g_port.lock = plat_mutex_new();
    g_port.vblank_cv = plat_cond_new();
    g_start_ns = plat_monotonic_ns();
}

static uint64_t process_time_us(void) { return (plat_monotonic_ns() - g_start_ns) / 1000; }

/* EVFILT_VIDEO_OUT trigger data is shadPS4's OrbisVideoOutEventHint:
 * event_id (internal ident) | video_id << 8 | flip_arg << 16. The equeue
 * (workstream A) turns it into the delivered data and keeps the per-event
 * count itself, so nothing is pre-counted here. The main port is video 0. */
#define VO_VIDEO_ID 0u
static uint64_t event_hint(uint32_t ident, uint64_t payload) {
    return (uint64_t)(ident & 0xff) | ((uint64_t)VO_VIDEO_ID << 8) | (payload << 16);
}

static vo_port *get_port(int32_t handle) {
    vo_port *p = kobj_get(handle, KOBJ_VIDEOOUT);
    return p && p->is_open ? p : NULL;
}

/* ---- vblank thread (shadPS4 VideoOutDriver::PresentThread / Flip) ------- */

static void vblank_tick(vo_port *p) {
    equeue *flip_eqs[VO_MAX_EVENTS], *vblank_eqs[VO_MAX_EVENTS];
    int n_flip = 0, n_vblank = 0;
    uint64_t flip_data = 0, vblank_data, frame = 0;

    plat_mutex_lock(p->lock);
    vo_vblank_status *vs = &p->vblank_status;
    if (vs->count % (uint64_t)(p->flip_rate + 1) == 0 && p->q_len > 0) {
        vo_flip_req r = p->queue[p->q_head];
        p->q_head = (p->q_head + 1) % VO_FLIP_QUEUE;
        p->q_len--;
        vo_flip_status *fs = &p->flip_status;
        fs->count++;
        fs->process_time = process_time_us();
        fs->tsc = plat_tsc();
        fs->flip_arg = r.flip_arg;
        fs->current_buffer = r.index;
        if (r.eop) fs->gc_queue_num--;
        fs->flip_pending_num--;
        if (fs->count % 60 == 0) frame = fs->count;
        flip_data = event_hint(VO_IDENT_FLIP, (uint64_t)r.flip_arg);
        n_flip = p->n_flip_events;
        for (int i = 0; i < n_flip; i++) flip_eqs[i] = p->flip_events[i].eq;
    }
    vblank_data = event_hint(VO_IDENT_VBLANK, vs->count);
    n_vblank = p->n_vblank_events;
    for (int i = 0; i < n_vblank; i++) vblank_eqs[i] = p->vblank_events[i].eq;
    vs->count++;
    vs->process_time = process_time_us();
    vs->tsc = plat_tsc();
    plat_cond_broadcast(p->vblank_cv);
    plat_mutex_unlock(p->lock);

    for (int i = 0; i < n_flip; i++)
        equeue_trigger(flip_eqs[i], VO_IDENT_FLIP, EVFILT_VIDEO_OUT, flip_data);
    for (int i = 0; i < n_vblank; i++)
        equeue_trigger(vblank_eqs[i], VO_IDENT_VBLANK, EVFILT_VIDEO_OUT, vblank_data);
    if (frame) log_info("videoout: frame %llu", (unsigned long long)frame);
}

static void vblank_thread(void *arg) {
    vo_port *p = arg;
    plat_thread_set_name("sotc:vblank");
    /* Absolute deadlines: host sleeps are coarse (Sleep() rounds to the timer
     * tick on Windows), so a late wakeup runs the missed vblanks back to back
     * and the average rate stays at 60 Hz. */
    uint64_t next = plat_monotonic_ns() + VO_VBLANK_NS;
    while (p->running) {
        uint64_t now = plat_monotonic_ns();
        if (now < next) {
            plat_sleep_us((next - now) / 1000 + 1);
            continue;
        }
        if (now - next > 10 * VO_VBLANK_NS) next = now; /* stalled: resync, don't burst */
        next += VO_VBLANK_NS;
        vblank_tick(p);
    }
}

/* ---- flips -------------------------------------------------------------- */

static int32_t submit_flip(vo_port *p, int32_t index, int64_t flip_arg, bool eop) {
    int32_t rc = SCE_OK;
    plat_mutex_lock(p->lock);
    if (index < -1 || index >= VO_MAX_BUFFERS || (index >= 0 && p->buffers[index].group < 0)) {
        rc = VO_ERROR_INVALID_INDEX;
    } else if ((index != -1 && p->flip_status.flip_pending_num > 16) || p->q_len >= VO_FLIP_QUEUE) {
        rc = VO_ERROR_FLIP_QUEUE_FULL;
    } else {
        p->queue[(p->q_head + p->q_len) % VO_FLIP_QUEUE] = (vo_flip_req){index, flip_arg, eop};
        p->q_len++;
        if (eop) p->flip_status.gc_queue_num++;
        p->flip_status.flip_pending_num++;
        p->flip_status.submit_tsc = plat_tsc();
    }
    plat_mutex_unlock(p->lock);
    if (rc == VO_ERROR_INVALID_INDEX) log_error("videoout: flip to bad buffer index %d", index);
    else if (rc != SCE_OK) log_error("videoout: flip queue full");
    return rc;
}

int32_t videoout_submit_flip(int32_t handle, int32_t buf_index, uint32_t flip_mode,
                             int64_t flip_arg) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    log_trace("videoout: EOP flip buffer %d mode %u arg %lld", buf_index, flip_mode,
              (long long)flip_arg);
    return submit_flip(p, buf_index, flip_arg, true);
}

static SYSV int32_t ps4_sceVideoOutSubmitFlip(int32_t handle, int32_t index, int32_t flip_mode,
                                              int64_t flip_arg) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    log_trace("videoout: flip buffer %d mode %d arg %lld", index, flip_mode, (long long)flip_arg);
    return submit_flip(p, index, flip_arg, false);
}

/* ---- open / close (shadPS4 VideoOutDriver::Open / Close) ---------------- */

static void reset_port_locked(vo_port *p) {
    memset(p->buffers, 0, sizeof(p->buffers));
    for (int i = 0; i < VO_MAX_BUFFERS; i++) p->buffers[i].group = -1;
    memset(p->groups, 0, sizeof(p->groups));
    memset(&p->flip_status, 0, sizeof(p->flip_status));
    p->flip_status.flip_arg = -1;
    p->flip_status.current_buffer = -1;
    memset(&p->vblank_status, 0, sizeof(p->vblank_status));
    p->flip_rate = 0;
    p->q_head = p->q_len = 0;
    p->n_flip_events = p->n_vblank_events = 0;
    p->resolution = (vo_resolution_status){
        .full_width = VO_WIDTH, .full_height = VO_HEIGHT,
        .pane_width = VO_WIDTH, .pane_height = VO_HEIGHT,
        .refresh_rate = VO_REFRESH_59_94HZ, .screen_size_in_inch = 50.0f,
    };
}

static SYSV int32_t ps4_sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index,
                                        const void *param) {
    log_info("videoout: open (user %d, bus %d, index %d)", user_id, bus_type, index);
    if (bus_type != 0 || index != 0) return VO_ERROR_INVALID_VALUE;
    vo_port *p = &g_port;
    plat_mutex_lock(p->lock);
    if (p->is_open) {
        plat_mutex_unlock(p->lock);
        return VO_ERROR_RESOURCE_BUSY;
    }
    int32_t h = kobj_alloc(KOBJ_VIDEOOUT, p);
    if (h < 0) {
        plat_mutex_unlock(p->lock);
        return VO_ERROR_RESOURCE_BUSY;
    }
    reset_port_locked(p);
    p->handle = h;
    p->is_open = true;
    p->running = true;
    plat_mutex_unlock(p->lock);
    p->thread = plat_thread_create(vblank_thread, p, 256 * 1024);
    if (!p->thread) log_error("videoout: cannot start the vblank thread");
    return h;
}

static SYSV int32_t ps4_sceVideoOutClose(int32_t handle) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    log_info("videoout: close");
    p->running = false;
    if (p->thread) plat_thread_join(p->thread);
    p->thread = NULL;

    equeue *flip_eqs[VO_MAX_EVENTS], *vblank_eqs[VO_MAX_EVENTS];
    plat_mutex_lock(p->lock);
    int n_flip = p->n_flip_events, n_vblank = p->n_vblank_events;
    for (int i = 0; i < n_flip; i++) flip_eqs[i] = p->flip_events[i].eq;
    for (int i = 0; i < n_vblank; i++) vblank_eqs[i] = p->vblank_events[i].eq;
    reset_port_locked(p);
    p->is_open = false;
    kobj_free(p->handle);
    p->handle = 0;
    plat_cond_broadcast(p->vblank_cv); /* release WaitVblank callers */
    plat_mutex_unlock(p->lock);

    for (int i = 0; i < n_flip; i++)
        equeue_remove_event(flip_eqs[i], VO_IDENT_FLIP, EVFILT_VIDEO_OUT);
    for (int i = 0; i < n_vblank; i++)
        equeue_remove_event(vblank_eqs[i], VO_IDENT_VBLANK, EVFILT_VIDEO_OUT);
    return SCE_OK;
}

/* ---- buffers ------------------------------------------------------------ */

static SYSV void ps4_sceVideoOutSetBufferAttribute(vo_buffer_attribute *attr,
                                                   uint32_t pixel_format, uint32_t tiling_mode,
                                                   uint32_t aspect_ratio, uint32_t width,
                                                   uint32_t height, uint32_t pitch_in_pixel) {
    log_debug("videoout: buffer attribute format %#x tiling %u aspect %u %ux%u pitch %u",
              pixel_format, tiling_mode, aspect_ratio, width, height, pitch_in_pixel);
    if (!attr) return;
    memset(attr, 0, sizeof(*attr));
    attr->pixel_format = pixel_format;
    attr->tiling_mode = (int32_t)tiling_mode;
    attr->aspect_ratio = (int32_t)aspect_ratio;
    attr->width = width;
    attr->height = height;
    attr->pitch_in_pixel = pitch_in_pixel;
    attr->option = 0;
}

static int32_t check_attribute(const vo_buffer_attribute *a) {
    if (a->reserved0 != 0 || a->reserved1 != 0) return VO_ERROR_INVALID_VALUE;
    if (a->aspect_ratio != 0) return VO_ERROR_INVALID_ASPECT_RATIO;
    if (a->width > a->pitch_in_pixel) return VO_ERROR_INVALID_PITCH;
    if (a->tiling_mode < 0 || a->tiling_mode > 1) return VO_ERROR_INVALID_TILING_MODE;
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutRegisterBuffers(int32_t handle, int32_t start,
                                                   void *const *addresses, int32_t num,
                                                   const vo_buffer_attribute *attr) {
    if (!addresses || !attr) return VO_ERROR_INVALID_ADDRESS;
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    if (start < 0 || num < 0 || start > VO_MAX_BUFFERS || num > VO_MAX_BUFFERS ||
        start + num > VO_MAX_BUFFERS)
        return VO_ERROR_INVALID_VALUE;
    int32_t rc = check_attribute(attr);
    if (rc != SCE_OK) return rc;

    plat_mutex_lock(p->lock);
    int32_t group = 0;
    while (group < VO_MAX_GROUPS && p->groups[group].occupied) group++;
    if (group == VO_MAX_GROUPS) {
        rc = VO_ERROR_NO_EMPTY_SLOT;
    } else {
        for (int32_t i = start; i < start + num; i++)
            if (p->buffers[i].group != -1) rc = VO_ERROR_SLOT_OCCUPIED;
    }
    if (rc == SCE_OK) {
        p->groups[group].occupied = true;
        p->groups[group].attr = *attr;
        for (int32_t i = 0; i < num; i++) {
            p->buffers[start + i].group = group;
            p->buffers[start + i].address = (uint64_t)(uintptr_t)addresses[i];
        }
    }
    plat_mutex_unlock(p->lock);
    if (rc != SCE_OK) return rc;
    log_info("videoout: registered buffers %d..%d as group %d (format %#x, %ux%u, pitch %u, "
             "tiling %d)",
             start, start + num - 1, group, attr->pixel_format, attr->width, attr->height,
             attr->pitch_in_pixel, attr->tiling_mode);
    return group;
}

static SYSV int32_t ps4_sceVideoOutUnregisterBuffers(int32_t handle, int32_t group) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    int32_t rc = SCE_OK;
    plat_mutex_lock(p->lock);
    if (group < 0 || group >= VO_MAX_GROUPS || !p->groups[group].occupied) {
        rc = VO_ERROR_INVALID_VALUE;
    } else {
        p->groups[group].occupied = false;
        for (int i = 0; i < VO_MAX_BUFFERS; i++)
            if (p->buffers[i].group == group) p->buffers[i].group = -1;
    }
    plat_mutex_unlock(p->lock);
    return rc;
}

static SYSV int32_t ps4_sceVideoOutSubmitChangeBufferAttribute(int32_t handle, int32_t group,
                                                               const vo_buffer_attribute *attr) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    if (!attr) return VO_ERROR_INVALID_ADDRESS;
    int32_t rc = check_attribute(attr);
    if (rc != SCE_OK) return rc;
    plat_mutex_lock(p->lock);
    if (group < 0 || group >= VO_MAX_GROUPS || !p->groups[group].occupied)
        rc = VO_ERROR_INVALID_VALUE;
    else
        p->groups[group].attr = *attr;
    plat_mutex_unlock(p->lock);
    return rc;
}

/* ---- events ------------------------------------------------------------- */

static int32_t add_event(vo_event_reg *list, int *n, equeue *eq, uint64_t ident, void *udata) {
    if (!equeue_add_event(eq, ident, EVFILT_VIDEO_OUT, EV_ADD | EV_CLEAR, udata))
        return VO_ERROR_INVALID_EVENT_QUEUE;
    for (int i = 0; i < *n; i++)
        if (list[i].eq == eq) return SCE_OK; /* re-add: equeue updated the udata */
    if (*n == VO_MAX_EVENTS) return VO_ERROR_INVALID_EVENT_QUEUE;
    list[(*n)++].eq = eq;
    return SCE_OK;
}

static int32_t delete_event(vo_event_reg *list, int *n, equeue *eq, uint64_t ident) {
    for (int i = 0; i < *n; i++)
        if (list[i].eq == eq) {
            list[i] = list[--*n];
            equeue_remove_event(eq, ident, EVFILT_VIDEO_OUT);
            return SCE_OK;
        }
    return VO_ERROR_INVALID_EVENT_QUEUE;
}

static SYSV int32_t ps4_sceVideoOutAddFlipEvent(equeue *eq, int32_t handle, void *udata) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    if (!eq) return VO_ERROR_INVALID_EVENT_QUEUE;
    plat_mutex_lock(p->lock);
    int32_t rc = add_event(p->flip_events, &p->n_flip_events, eq, VO_IDENT_FLIP, udata);
    plat_mutex_unlock(p->lock);
    log_debug("videoout: flip event on eq %p -> %#x", (void *)eq, rc);
    return rc;
}

static SYSV int32_t ps4_sceVideoOutDeleteFlipEvent(equeue *eq, int32_t handle) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    plat_mutex_lock(p->lock);
    int32_t rc = delete_event(p->flip_events, &p->n_flip_events, eq, VO_IDENT_FLIP);
    plat_mutex_unlock(p->lock);
    return rc;
}

static SYSV int32_t ps4_sceVideoOutAddVblankEvent(equeue *eq, int32_t handle, void *udata) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    if (!eq) return VO_ERROR_INVALID_EVENT_QUEUE;
    plat_mutex_lock(p->lock);
    int32_t rc = add_event(p->vblank_events, &p->n_vblank_events, eq, VO_IDENT_VBLANK, udata);
    plat_mutex_unlock(p->lock);
    return rc;
}

static SYSV int32_t ps4_sceVideoOutDeleteVblankEvent(equeue *eq, int32_t handle) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    plat_mutex_lock(p->lock);
    int32_t rc = delete_event(p->vblank_events, &p->n_vblank_events, eq, VO_IDENT_VBLANK);
    plat_mutex_unlock(p->lock);
    return rc;
}

static SYSV int32_t ps4_sceVideoOutGetEventId(const ps4_kevent *ev) {
    if (!ev) return VO_ERROR_INVALID_ADDRESS;
    if (ev->filter != EVFILT_VIDEO_OUT) return VO_ERROR_INVALID_EVENT;
    switch (ev->ident) {
    case VO_IDENT_FLIP: return VO_EVENT_FLIP;
    case VO_IDENT_VBLANK:
    case VO_IDENT_SYS_VBLANK: return VO_EVENT_VBLANK;
    case VO_IDENT_PRE_VBLANK: return VO_EVENT_PRE_VBLANK;
    case VO_IDENT_SET_MODE: return VO_EVENT_SET_MODE;
    case VO_IDENT_POSITION: return VO_EVENT_POSITION;
    default: return VO_ERROR_INVALID_EVENT;
    }
}

/* Event data: bits 0-11 time, 12-15 count, 16-63 flip_arg / vblank count.
 * The public value is the top 48 bits, sign-extended (shadPS4). */
static SYSV int32_t ps4_sceVideoOutGetEventData(const ps4_kevent *ev, int64_t *data) {
    if (!ev || !data) return VO_ERROR_INVALID_ADDRESS;
    if (ev->filter != EVFILT_VIDEO_OUT) return VO_ERROR_INVALID_EVENT;
    *data = (int64_t)ev->data >> 16;
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutGetEventCount(const ps4_kevent *ev) {
    if (!ev) return VO_ERROR_INVALID_ADDRESS;
    if (ev->filter != EVFILT_VIDEO_OUT) return VO_ERROR_INVALID_EVENT;
    return (int32_t)((ev->data >> 12) & 0xf);
}

/* ---- status ------------------------------------------------------------- */

static SYSV int32_t ps4_sceVideoOutSetFlipRate(int32_t handle, int32_t rate) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    if (rate < 0 || rate > 2) return VO_ERROR_INVALID_VALUE;
    log_debug("videoout: flip rate %d (%d Hz)", rate, 60 / (rate + 1));
    plat_mutex_lock(p->lock);
    p->flip_rate = rate;
    plat_mutex_unlock(p->lock);
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutIsFlipPending(int32_t handle) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    plat_mutex_lock(p->lock);
    int32_t n = p->flip_status.flip_pending_num;
    plat_mutex_unlock(p->lock);
    return n;
}

static SYSV int32_t ps4_sceVideoOutGetFlipStatus(int32_t handle, vo_flip_status *status) {
    if (!status) return VO_ERROR_INVALID_ADDRESS;
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    plat_mutex_lock(p->lock);
    *status = p->flip_status;
    plat_mutex_unlock(p->lock);
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutGetVblankStatus(int32_t handle, vo_vblank_status *status) {
    if (!status) return VO_ERROR_INVALID_ADDRESS;
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    plat_mutex_lock(p->lock);
    *status = p->vblank_status;
    plat_mutex_unlock(p->lock);
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutGetResolutionStatus(int32_t handle,
                                                       vo_resolution_status *status) {
    if (!status) return VO_ERROR_INVALID_ADDRESS;
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    plat_mutex_lock(p->lock);
    *status = p->resolution;
    plat_mutex_unlock(p->lock);
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutWaitVblank(int32_t handle) {
    vo_port *p = get_port(handle);
    if (!p) return VO_ERROR_INVALID_HANDLE;
    plat_mutex_lock(p->lock);
    uint64_t start = p->vblank_status.count;
    /* Bounded waits so a closed port or a dead vblank thread can't hang. */
    for (int i = 0; i < 10 && p->is_open && p->vblank_status.count == start; i++)
        plat_cond_wait(p->vblank_cv, p->lock, 100000);
    plat_mutex_unlock(p->lock);
    return SCE_OK;
}

/* ---- output mode and color (no display yet: accept and log) ------------- */

static SYSV int32_t ps4_sceVideoOutGetDeviceCapabilityInfo_(int32_t handle, uint64_t *info) {
    if (!info) return VO_ERROR_INVALID_ADDRESS;
    if (!get_port(handle)) return VO_ERROR_INVALID_HANDLE;
    *info = 0; /* no HDR (BT2020 PQ) */
    return SCE_OK;
}

static SYSV void ps4_sceVideoOutModeSetAny_(vo_mode *mode, uint32_t size) {
    if (!mode) return;
    if (size > sizeof(vo_mode)) size = sizeof(vo_mode);
    memset(mode, 0xff, size);
    mode->size = size;
}

static SYSV int32_t ps4_sceVideoOutConfigureOutputMode_(int32_t handle, uint32_t reserved,
                                                        const vo_mode *mode, const void *options,
                                                        uint32_t size_mode,
                                                        uint32_t size_options) {
    if (!get_port(handle)) return VO_ERROR_INVALID_HANDLE;
    if (reserved != 0 || !mode) return VO_ERROR_INVALID_VALUE;
    /* Colorimetry 0xff (any) = SDR, 12 = BT2020 PQ (HDR): we report no HDR. */
    if (mode->colorimetry != 0xff && mode->colorimetry != 12) return VO_ERROR_INVALID_VALUE;
    log_debug("videoout: configure output mode (colorimetry %u)", mode->colorimetry);
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutColorSettingsSetGamma_(vo_color_settings *settings,
                                                          float gamma) {
    if (!settings) return VO_ERROR_INVALID_ADDRESS;
    if (gamma < 0.1f || gamma > 2.0f) return VO_ERROR_INVALID_VALUE;
    settings->gamma = gamma;
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutAdjustColor_(int32_t handle,
                                                const vo_color_settings *settings) {
    if (!settings) return VO_ERROR_INVALID_ADDRESS;
    if (!get_port(handle)) return VO_ERROR_INVALID_HANDLE;
    log_debug("videoout: gamma %f", (double)settings->gamma);
    return SCE_OK;
}

static SYSV int32_t ps4_sceVideoOutSetWindowModeMargins(int32_t handle, int32_t top,
                                                        int32_t bottom) {
    log_debug("videoout: window margins %d/%d", top, bottom);
    return SCE_OK;
}

/* The trailing-underscore names are how the NID database spells these
 * imports (see build/game/boot.txt); the plain names are aliases. */
HLE_LIBRARY(hle_videoout, "libSceVideoOut",
    HLE(sceVideoOutOpen),
    HLE(sceVideoOutClose),
    HLE(sceVideoOutSetBufferAttribute),
    HLE(sceVideoOutRegisterBuffers),
    HLE(sceVideoOutUnregisterBuffers),
    HLE(sceVideoOutSubmitChangeBufferAttribute),
    HLE(sceVideoOutSubmitFlip),
    HLE(sceVideoOutSetFlipRate),
    HLE(sceVideoOutIsFlipPending),
    HLE(sceVideoOutGetFlipStatus),
    HLE(sceVideoOutGetVblankStatus),
    HLE(sceVideoOutGetResolutionStatus),
    HLE_AS("sceVideoOutSysGetResolutionStatus", ps4_sceVideoOutGetResolutionStatus),
    HLE(sceVideoOutWaitVblank),
    HLE(sceVideoOutAddFlipEvent),
    HLE(sceVideoOutDeleteFlipEvent),
    HLE(sceVideoOutAddVblankEvent),
    HLE(sceVideoOutDeleteVblankEvent),
    HLE_AS("sceVideoOutSysAddVblankEvent", ps4_sceVideoOutAddVblankEvent),
    HLE(sceVideoOutGetEventId),
    HLE(sceVideoOutGetEventData),
    HLE(sceVideoOutGetEventCount),
    HLE(sceVideoOutGetDeviceCapabilityInfo_),
    HLE_AS("sceVideoOutGetDeviceCapabilityInfo", ps4_sceVideoOutGetDeviceCapabilityInfo_),
    HLE(sceVideoOutModeSetAny_),
    HLE_AS("sceVideoOutModeSetAny", ps4_sceVideoOutModeSetAny_),
    HLE(sceVideoOutConfigureOutputMode_),
    HLE_AS("sceVideoOutConfigureOutputMode", ps4_sceVideoOutConfigureOutputMode_),
    HLE(sceVideoOutColorSettingsSetGamma_),
    HLE_AS("sceVideoOutColorSettingsSetGamma", ps4_sceVideoOutColorSettingsSetGamma_),
    HLE(sceVideoOutAdjustColor_),
    HLE_AS("sceVideoOutAdjustColor", ps4_sceVideoOutAdjustColor_),
    HLE(sceVideoOutSetWindowModeMargins),
)
