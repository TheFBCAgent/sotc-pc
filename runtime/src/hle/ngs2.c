/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Null libSceNgs2: Sony's software mixer, minus the mixing.
 *
 * Systems, racks and voices are real host objects so handles validate and
 * buffer bookkeeping (the game's allocator callbacks) balances, but no voice
 * ever plays: sceNgs2SystemRender writes silence and voices report an idle
 * state, as if every sound finished at once. Real Ngs2 (LLE of the firmware
 * module plus Ajm/ATRAC9) is milestone 5.
 *
 * Structure layouts, option checks and error codes follow shadPS4's
 * src/core/libraries/ngs2/ (ngs2.cpp, ngs2_impl.cpp, ngs2.h, ngs2_error.h;
 * GPL-2.0-or-later, shadPS4 Emulator Project). */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#define NGS2_ERR_INVALID_OUT_ADDRESS     ((int32_t)0x804A0053)
#define NGS2_ERR_INVALID_OPTION_SIZE     ((int32_t)0x804A0081)
#define NGS2_ERR_INVALID_MAX_GRAIN       ((int32_t)0x804A0050)
#define NGS2_ERR_INVALID_NUM_GRAIN       ((int32_t)0x804A0051)
#define NGS2_ERR_INVALID_SAMPLE_RATE     ((int32_t)0x804A0201)
#define NGS2_ERR_INVALID_BUFFER_INFO     ((int32_t)0x804A0206)
#define NGS2_ERR_INVALID_BUFFER_ADDRESS  ((int32_t)0x804A0207)
#define NGS2_ERR_INVALID_BUFFER_ALLOCATOR ((int32_t)0x804A020A)
#define NGS2_ERR_INVALID_SYSTEM_HANDLE   ((int32_t)0x804A0230)
#define NGS2_ERR_INVALID_RACK_HANDLE     ((int32_t)0x804A0261)
#define NGS2_ERR_INVALID_VOICE_HANDLE    ((int32_t)0x804A0300)
#define NGS2_ERR_INVALID_VOICE_INDEX     ((int32_t)0x804A0302)
#define NGS2_ERR_INVALID_VOICE_CONTROL_ADDRESS ((int32_t)0x804A0309)

/* Size reported to the game for each context buffer. Our state lives on the
 * host heap; the guest buffer only has to exist. */
#define NGS2_CONTEXT_BUFFER_SIZE 0x1000u

typedef struct ngs2_buffer_info {
    void *host_buffer;
    uint64_t host_buffer_size;
    uint64_t reserved[5];
    uint64_t user_data;
} ngs2_buffer_info;

typedef SYSV int32_t (*ngs2_alloc_fn)(ngs2_buffer_info *info);
typedef SYSV int32_t (*ngs2_free_fn)(ngs2_buffer_info *info);

typedef struct ngs2_allocator {
    ngs2_alloc_fn alloc;
    ngs2_free_fn free;
    uint64_t user_data;
} ngs2_allocator;

typedef struct ngs2_system_option {
    uint64_t size;
    char name[16];
    uint32_t flags;
    uint32_t max_grain_samples;
    uint32_t num_grain_samples;
    uint32_t sample_rate;
    uint32_t reserved[6];
} ngs2_system_option;

typedef struct ngs2_rack_option {
    uint64_t size;
    char name[16];
    uint32_t flags;
    uint32_t max_grain_samples;
    uint32_t max_voices;
    uint32_t max_input_delay_blocks;
    uint32_t max_matrices;
    uint32_t max_ports;
    uint32_t reserved[20];
} ngs2_rack_option;

typedef struct ngs2_render_buffer_info {
    void *buffer;
    uint64_t buffer_size;
    uint32_t waveform_type;
    uint32_t num_channels;
} ngs2_render_buffer_info;

enum { NGS2_SYSTEM = 1, NGS2_RACK = 2, NGS2_VOICE = 3 };
#define NGS2_MAGIC 0x4E475332u /* "NGS2" */

typedef struct ngs2_object {
    uint32_t magic;
    uint32_t type;
    struct ngs2_object *next;    /* registry list */
    ngs2_buffer_info buffer;     /* what to give back on destroy */
    ngs2_free_fn free_fn;        /* set when created with an allocator */
} ngs2_object;

typedef struct ngs2_voice {
    uint32_t magic;
    uint32_t type;
    struct ngs2_rack *rack;
    uint32_t index;
    uint32_t state_flags;
} ngs2_voice;

typedef struct ngs2_system {
    ngs2_object obj;
    uint32_t sample_rate;
    uint32_t num_grain_samples;
    uint64_t render_count;
} ngs2_system;

typedef struct ngs2_rack {
    ngs2_object obj;
    ngs2_system *system;
    uint32_t rack_id;
    uint32_t max_voices;
    ngs2_voice *voices;
} ngs2_rack;

/* All live systems and racks. Handles from the guest are checked against it
 * before being dereferenced. */
static ngs2_object *g_objects;
static plat_mutex *g_lock;

__attribute__((constructor)) static void ngs2_ctor(void) { g_lock = plat_mutex_new(); }

static bool registered(const void *h, uint32_t type) {
    for (ngs2_object *o = g_objects; o; o = o->next)
        if (o == h && o->type == type) return true;
    return false;
}

static void unregister(ngs2_object *obj) {
    for (ngs2_object **pp = &g_objects; *pp; pp = &(*pp)->next) {
        if (*pp == obj) {
            *pp = obj->next;
            return;
        }
    }
}

static ngs2_system *get_system(uint64_t h) {
    return registered((void *)(uintptr_t)h, NGS2_SYSTEM) ? (ngs2_system *)(uintptr_t)h : NULL;
}

static ngs2_rack *get_rack(uint64_t h) {
    return registered((void *)(uintptr_t)h, NGS2_RACK) ? (ngs2_rack *)(uintptr_t)h : NULL;
}

/* A voice handle points into some live rack's voice array. */
static ngs2_voice *get_voice(uint64_t h) {
    for (ngs2_object *o = g_objects; o; o = o->next) {
        if (o->type != NGS2_RACK) continue;
        ngs2_rack *r = (ngs2_rack *)o;
        uintptr_t base = (uintptr_t)r->voices;
        if (h >= base && h < base + (uint64_t)r->max_voices * sizeof(ngs2_voice) &&
            (h - base) % sizeof(ngs2_voice) == 0)
            return (ngs2_voice *)(uintptr_t)h;
    }
    return NULL;
}

/* Get a context buffer from the game's allocator, as the real library does,
 * so its later free callback has something to release. Runs guest code: call
 * without holding g_lock. */
static int32_t alloc_context(const ngs2_allocator *a, ngs2_buffer_info *info) {
    memset(info, 0, sizeof(*info));
    info->host_buffer_size = NGS2_CONTEXT_BUFFER_SIZE;
    info->user_data = a->user_data;
    int32_t r = a->alloc(info);
    if (r < 0) return r;
    if (!info->host_buffer) return NGS2_ERR_INVALID_BUFFER_ADDRESS;
    return SCE_OK;
}

static int32_t check_system_option(const ngs2_system_option *o, uint32_t *rate, uint32_t *grain) {
    *rate = 48000;
    *grain = 256;
    if (!o) return SCE_OK;
    if (o->size != sizeof(ngs2_system_option)) return NGS2_ERR_INVALID_OPTION_SIZE;
    if (o->max_grain_samples < 64 || o->max_grain_samples > 1024 || (o->max_grain_samples & 63))
        return NGS2_ERR_INVALID_MAX_GRAIN;
    if (o->num_grain_samples < 64 || o->num_grain_samples > 1024 || (o->num_grain_samples & 63))
        return NGS2_ERR_INVALID_NUM_GRAIN;
    static const uint32_t rates[] = {11025, 12000, 22050, 24000, 44100,
                                     48000, 88200, 96000, 176400, 192000};
    bool ok = false;
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) ok |= o->sample_rate == rates[i];
    if (!ok) return NGS2_ERR_INVALID_SAMPLE_RATE;
    *rate = o->sample_rate;
    *grain = o->num_grain_samples;
    return SCE_OK;
}

/* Release a context buffer: through the allocator's free callback, or by
 * handing it back in `out` for buffers the game passed in directly. */
static void release_context(const ngs2_object *obj, ngs2_buffer_info *out) {
    ngs2_buffer_info info = obj->buffer;
    if (out) *out = info;
    if (obj->free_fn) obj->free_fn(&info);
}

/* ---- system ------------------------------------------------------------ */

static SYSV int32_t ps4_sceNgs2SystemCreateWithAllocator(const ngs2_system_option *option,
                                                         const ngs2_allocator *allocator,
                                                         uint64_t *out_handle) {
    if (!allocator || !allocator->alloc) return NGS2_ERR_INVALID_BUFFER_ALLOCATOR;
    if (!out_handle) return NGS2_ERR_INVALID_OUT_ADDRESS;
    uint32_t rate, grain;
    int32_t r = check_system_option(option, &rate, &grain);
    if (r < 0) return r;
    ngs2_buffer_info info;
    if ((r = alloc_context(allocator, &info)) < 0) return r;

    ngs2_system *s = calloc(1, sizeof(*s));
    s->obj.magic = NGS2_MAGIC;
    s->obj.type = NGS2_SYSTEM;
    s->obj.buffer = info;
    s->obj.free_fn = allocator->free;
    s->sample_rate = rate;
    s->num_grain_samples = grain;
    plat_mutex_lock(g_lock);
    s->obj.next = g_objects;
    g_objects = &s->obj;
    plat_mutex_unlock(g_lock);
    *out_handle = (uint64_t)(uintptr_t)s;
    log_info("ngs2: system %p rate=%u grain=%u (null renderer)", (void *)s, rate, grain);
    return SCE_OK;
}

static SYSV int32_t ps4_sceNgs2SystemDestroy(uint64_t handle, ngs2_buffer_info *out_info) {
    plat_mutex_lock(g_lock);
    ngs2_system *s = get_system(handle);
    if (!s) {
        plat_mutex_unlock(g_lock);
        return NGS2_ERR_INVALID_SYSTEM_HANDLE;
    }
    /* Racks of a destroyed system are invalid too; free their host state
     * (their guest buffers are the game's business, as on hardware). */
    for (ngs2_object **pp = &g_objects; *pp;) {
        ngs2_object *o = *pp;
        if (o->type == NGS2_RACK && ((ngs2_rack *)o)->system == s) {
            *pp = o->next;
            free(((ngs2_rack *)o)->voices);
            free(o);
        } else {
            pp = &o->next;
        }
    }
    unregister(&s->obj);
    plat_mutex_unlock(g_lock);
    release_context(&s->obj, out_info);
    free(s);
    return SCE_OK;
}

/* Fill every output buffer with silence. Waveform type and channel count only
 * describe the buffer's layout, so a full clear is correct for all of them. */
static SYSV int32_t ps4_sceNgs2SystemRender(uint64_t handle, const ngs2_render_buffer_info *infos,
                                            uint32_t count) {
    plat_mutex_lock(g_lock);
    ngs2_system *s = get_system(handle);
    if (s) s->render_count++;
    plat_mutex_unlock(g_lock);
    if (!s) return NGS2_ERR_INVALID_SYSTEM_HANDLE;
    if (count && !infos) return NGS2_ERR_INVALID_BUFFER_INFO;
    for (uint32_t i = 0; i < count; i++) {
        if (infos[i].buffer && infos[i].buffer_size) memset(infos[i].buffer, 0, infos[i].buffer_size);
    }
    return SCE_OK;
}

/* ---- rack -------------------------------------------------------------- */

static SYSV int32_t ps4_sceNgs2RackQueryBufferSize(uint32_t rack_id, const ngs2_rack_option *option,
                                                   ngs2_buffer_info *out_info) {
    (void)rack_id;
    if (!out_info) return NGS2_ERR_INVALID_BUFFER_ADDRESS;
    if (option && option->size < sizeof(ngs2_rack_option)) return NGS2_ERR_INVALID_OPTION_SIZE;
    memset(out_info, 0, sizeof(*out_info));
    out_info->host_buffer_size = NGS2_CONTEXT_BUFFER_SIZE;
    return SCE_OK;
}

static int32_t rack_create(uint64_t sys_handle, uint32_t rack_id, const ngs2_rack_option *option,
                           const ngs2_buffer_info *buffer, ngs2_free_fn free_fn,
                           uint64_t *out_handle) {
    /* Rack-type options extend the common header; only maxVoices matters. */
    uint32_t max_voices = 0;
    if (option) {
        if (option->size < sizeof(ngs2_rack_option)) return NGS2_ERR_INVALID_OPTION_SIZE;
        max_voices = option->max_voices;
    }
    if (max_voices == 0) max_voices = 64; /* lenient default; the guest normally passes one */
    if (max_voices > 4096) max_voices = 4096;

    ngs2_rack *r = calloc(1, sizeof(*r));
    r->obj.magic = NGS2_MAGIC;
    r->obj.type = NGS2_RACK;
    r->obj.buffer = *buffer;
    r->obj.free_fn = free_fn;
    r->rack_id = rack_id;
    r->max_voices = max_voices;
    r->voices = calloc(max_voices, sizeof(ngs2_voice));
    for (uint32_t i = 0; i < max_voices; i++) {
        r->voices[i].magic = NGS2_MAGIC;
        r->voices[i].type = NGS2_VOICE;
        r->voices[i].rack = r;
        r->voices[i].index = i;
    }
    plat_mutex_lock(g_lock);
    ngs2_system *s = get_system(sys_handle);
    if (!s) {
        plat_mutex_unlock(g_lock);
        free(r->voices);
        free(r);
        return NGS2_ERR_INVALID_SYSTEM_HANDLE;
    }
    r->system = s;
    r->obj.next = g_objects;
    g_objects = &r->obj;
    plat_mutex_unlock(g_lock);
    *out_handle = (uint64_t)(uintptr_t)r;
    log_debug("ngs2: rack %p id=%#x voices=%u", (void *)r, rack_id, max_voices);
    return SCE_OK;
}

static SYSV int32_t ps4_sceNgs2RackCreate(uint64_t sys_handle, uint32_t rack_id,
                                          const ngs2_rack_option *option,
                                          const ngs2_buffer_info *buffer, uint64_t *out_handle) {
    if (!out_handle) return NGS2_ERR_INVALID_OUT_ADDRESS;
    if (!buffer) return NGS2_ERR_INVALID_BUFFER_INFO;
    if (!buffer->host_buffer) return NGS2_ERR_INVALID_BUFFER_ADDRESS;
    return rack_create(sys_handle, rack_id, option, buffer, NULL, out_handle);
}

static SYSV int32_t ps4_sceNgs2RackCreateWithAllocator(uint64_t sys_handle, uint32_t rack_id,
                                                       const ngs2_rack_option *option,
                                                       const ngs2_allocator *allocator,
                                                       uint64_t *out_handle) {
    if (!allocator || !allocator->alloc) return NGS2_ERR_INVALID_BUFFER_ALLOCATOR;
    if (!out_handle) return NGS2_ERR_INVALID_OUT_ADDRESS;
    if (option && option->size < sizeof(ngs2_rack_option)) return NGS2_ERR_INVALID_OPTION_SIZE;
    plat_mutex_lock(g_lock);
    bool sys_ok = get_system(sys_handle) != NULL;
    plat_mutex_unlock(g_lock);
    if (!sys_ok) return NGS2_ERR_INVALID_SYSTEM_HANDLE;
    ngs2_buffer_info info;
    int32_t r = alloc_context(allocator, &info);
    if (r < 0) return r;
    r = rack_create(sys_handle, rack_id, option, &info, allocator->free, out_handle);
    if (r < 0 && allocator->free) allocator->free(&info);
    return r;
}

static SYSV int32_t ps4_sceNgs2RackDestroy(uint64_t handle, ngs2_buffer_info *out_info) {
    plat_mutex_lock(g_lock);
    ngs2_rack *r = get_rack(handle);
    if (r) unregister(&r->obj);
    plat_mutex_unlock(g_lock);
    if (!r) return NGS2_ERR_INVALID_RACK_HANDLE;
    release_context(&r->obj, out_info);
    free(r->voices);
    free(r);
    return SCE_OK;
}

static SYSV int32_t ps4_sceNgs2RackGetVoiceHandle(uint64_t handle, uint32_t index,
                                                  uint64_t *out_handle) {
    if (!out_handle) return NGS2_ERR_INVALID_OUT_ADDRESS;
    plat_mutex_lock(g_lock);
    ngs2_rack *r = get_rack(handle);
    int32_t ret = SCE_OK;
    if (!r) ret = NGS2_ERR_INVALID_RACK_HANDLE;
    else if (index >= r->max_voices) ret = NGS2_ERR_INVALID_VOICE_INDEX;
    else *out_handle = (uint64_t)(uintptr_t)&r->voices[index];
    plat_mutex_unlock(g_lock);
    return ret;
}

/* ---- voice ------------------------------------------------------------- */

/* Parameters (patches, matrices, events, waveform blocks...) are accepted and
 * dropped: with nothing rendered, a voice that is told to play has already
 * finished. */
static SYSV int32_t ps4_sceNgs2VoiceControl(uint64_t handle, const void *param_list) {
    plat_mutex_lock(g_lock);
    ngs2_voice *v = get_voice(handle);
    plat_mutex_unlock(g_lock);
    if (!v) return NGS2_ERR_INVALID_VOICE_HANDLE;
    if (!param_list) return NGS2_ERR_INVALID_VOICE_CONTROL_ADDRESS;
    return SCE_OK;
}

/* Every voice is idle (no INUSE/PLAYING flags). */
static SYSV int32_t ps4_sceNgs2VoiceGetStateFlags(uint64_t handle, uint32_t *out_flags) {
    if (!out_flags) return NGS2_ERR_INVALID_OUT_ADDRESS;
    plat_mutex_lock(g_lock);
    ngs2_voice *v = get_voice(handle);
    uint32_t flags = v ? v->state_flags : 0;
    plat_mutex_unlock(g_lock);
    if (!v) return NGS2_ERR_INVALID_VOICE_HANDLE;
    *out_flags = flags;
    return SCE_OK;
}

/* The state struct depends on the rack type (sampler, submixer, mastering...);
 * all of them start with the 32-bit state flags and are zero when idle. */
static SYSV int32_t ps4_sceNgs2VoiceGetState(uint64_t handle, void *out_state, uint64_t size) {
    if (!out_state) return NGS2_ERR_INVALID_OUT_ADDRESS;
    plat_mutex_lock(g_lock);
    ngs2_voice *v = get_voice(handle);
    uint32_t flags = v ? v->state_flags : 0;
    plat_mutex_unlock(g_lock);
    if (!v) return NGS2_ERR_INVALID_VOICE_HANDLE;
    if (size > 0x10000) size = 0x10000;
    memset(out_state, 0, size);
    if (size >= 4) memcpy(out_state, &flags, 4);
    return SCE_OK;
}

HLE_LIBRARY(hle_ngs2, "libSceNgs2",
    HLE(sceNgs2SystemCreateWithAllocator),
    HLE(sceNgs2SystemDestroy),
    HLE(sceNgs2SystemRender),
    HLE(sceNgs2RackQueryBufferSize),
    HLE(sceNgs2RackCreate),
    HLE(sceNgs2RackCreateWithAllocator),
    HLE(sceNgs2RackDestroy),
    HLE(sceNgs2RackGetVoiceHandle),
    HLE(sceNgs2VoiceControl),
    HLE(sceNgs2VoiceGetState),
    HLE(sceNgs2VoiceGetStateFlags),
)
