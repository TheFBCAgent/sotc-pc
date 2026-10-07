/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Null audio: libSceAudioOut and libSceAudio3d.
 *
 * Nothing is played. What matters is timing: games pace their audio thread on
 * sceAudioOutOutput, which on hardware blocks until the previous buffer has
 * been consumed. Each port keeps an absolute deadline advanced by one buffer's
 * duration (length / 48 kHz) per call, so oversleeping on one call is made up
 * on the next and the long-term rate does not drift.
 *
 * Semantics (handle encoding, port ranges, error codes, Audio3d queue model)
 * follow shadPS4's src/core/libraries/audio/audioout.cpp and
 * src/core/libraries/audio3d/audio3d.cpp (GPL-2.0-or-later, shadPS4 Emulator
 * Project). */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

/* ---- error codes ------------------------------------------------------- */

#define AO_ERR_NOT_OPENED        ((int32_t)0x80260001)
#define AO_ERR_INVALID_PORT      ((int32_t)0x80260003)
#define AO_ERR_INVALID_POINTER   ((int32_t)0x80260004)
#define AO_ERR_PORT_FULL         ((int32_t)0x80260005)
#define AO_ERR_INVALID_SIZE      ((int32_t)0x80260006)
#define AO_ERR_INVALID_FORMAT    ((int32_t)0x80260007)
#define AO_ERR_INVALID_FREQ      ((int32_t)0x80260008)
#define AO_ERR_INVALID_PORT_TYPE ((int32_t)0x8026000A)
#define AO_ERR_ALREADY_INIT      ((int32_t)0x8026000E)
#define AO_ERR_NOT_INIT          ((int32_t)0x8026000F)
#define AO_ERR_INVALID_MIXLEVEL  ((int32_t)0x80260014)

#define A3D_ERR_INVALID_PORT      ((int32_t)0x80EA0002)
#define A3D_ERR_INVALID_OBJECT    ((int32_t)0x80EA0003)
#define A3D_ERR_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define A3D_ERR_OUT_OF_RESOURCES  ((int32_t)0x80EA0006)
#define A3D_ERR_NOT_READY         ((int32_t)0x80EA0007)
#define A3D_ERR_NOT_SUPPORTED     ((int32_t)0x80EA0008)

/* ---- pacing ------------------------------------------------------------ */

#define AUDIO_RATE 48000u
/* A caller this far behind its deadline (a hitch, a debugger) resyncs instead
 * of returning immediately for many buffers to catch up. Larger than the
 * Windows Sleep() granularity so ordinary jitter never triggers it. */
#define PACE_RESYNC_NS 100000000ull

static uint64_t frames_to_ns(uint32_t frames) {
    return (uint64_t)frames * 1000000000ull / AUDIO_RATE;
}

/* Compute until when (monotonic ns) the caller must wait for its buffer to be
 * "played" and advance the deadline. Call with the owning lock held; sleep
 * after unlocking. The first buffer is accepted immediately (hardware
 * double-buffers), the next blocks until the first one's duration has
 * elapsed, and so on. */
static uint64_t pace_reserve(uint64_t *deadline, uint64_t dur_ns, bool advance) {
    uint64_t now = plat_monotonic_ns();
    if (*deadline == 0 || now > *deadline + PACE_RESYNC_NS) *deadline = now;
    uint64_t wait_until = *deadline;
    if (advance) *deadline += dur_ns;
    return wait_until;
}

/* Sleep to the absolute deadline (M3, workstream P): no per-call rounding, so
 * each Output returns within ~0.1 ms of its buffer boundary. */
static void pace_sleep(uint64_t until_ns) {
    if (until_ns > plat_monotonic_ns()) plat_sleep_until_ns(until_ns);
}

/* ---- libSceAudioOut ---------------------------------------------------- */

enum { AO_NUM_PORTS = 25 };
enum { AO_MAIN = 0, AO_BGM = 1, AO_VOICE = 2, AO_PERSONAL = 3, AO_PADSPK = 4, AO_AUDIO3D = 126,
       AO_AUX = 127 };

typedef struct ao_port {
    bool open;
    int32_t type;
    uint32_t frames;     /* samples per channel per buffer */
    uint32_t channels;
    uint32_t sample_size;
    uint64_t deadline;   /* monotonic ns when the queued buffer finishes */
    uint64_t last_output_ns;
    int32_t mix_level;
} ao_port;

/* Ports are never freed, so a thread blocked in Output while another closes
 * the port touches valid memory. */
static ao_port g_ao_ports[AO_NUM_PORTS];
static plat_mutex *g_ao_lock;
static atomic_int g_ao_init;

__attribute__((constructor)) static void audio_ctor(void) { g_ao_lock = plat_mutex_new(); }

/* Port index ranges by type (shadPS4 port_ranges). */
static bool ao_range(int32_t type, int *start, int *end) {
    switch (type) {
    case AO_MAIN: *start = 0; *end = 7; return true;
    case AO_BGM: *start = 8; *end = 8; return true;
    case AO_VOICE: *start = 9; *end = 12; return true;
    case AO_PERSONAL: *start = 13; *end = 16; return true;
    case AO_PADSPK: *start = 17; *end = 20; return true;
    case 5: case 6: case 7: case 8: *start = 21; *end = 21; return true;
    case AO_AUDIO3D: *start = 22; *end = 22; return true;
    case AO_AUX: *start = 23; *end = 23; return true;
    case 125: *start = 24; *end = 24; return true;
    default: return false;
    }
}

/* Handles are 0x20000000 | type << 16 | port index. Returns the index or an
 * error code. */
static int32_t ao_port_id(int32_t handle) {
    int32_t id = handle & 0xFF;
    if (id >= AO_NUM_PORTS || (handle & 0x3F000000) != 0x20000000) return AO_ERR_INVALID_PORT;
    int32_t type = (handle >> 16) & 0xFF;
    if (type >= 5 && type <= 13) return AO_ERR_INVALID_PORT_TYPE;
    if (!((type >= 0 && type <= 4) || type == 14 || type == 126 || type == 127))
        return AO_ERR_INVALID_PORT_TYPE;
    return id;
}

static SYSV int32_t ps4_sceAudioOutInit(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_ao_init, &expected, 1)) return AO_ERR_ALREADY_INIT;
    log_info("audioout: null output initialized");
    return SCE_OK;
}

static SYSV int32_t ps4_sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index,
                                        uint32_t length, uint32_t sample_rate, uint32_t param) {
    if (!atomic_load(&g_ao_init)) return AO_ERR_NOT_INIT;
    if (length == 0 || length > 2048 || (length & 0xFF) != 0) return AO_ERR_INVALID_SIZE;
    if (type != AO_PERSONAL && (param & 0x00020000)) return AO_ERR_INVALID_FORMAT; /* MIX_TO_MAIN */
    if (type != AO_MAIN && (param & 0x70000000)) return AO_ERR_INVALID_FORMAT;
    if ((type < AO_MAIN || type > AO_PADSPK) && type != AO_AUDIO3D && type != AO_AUX)
        return AO_ERR_INVALID_PORT_TYPE;
    if (sample_rate != AUDIO_RATE) return AO_ERR_INVALID_FREQ;
    uint32_t format = param & 0xFF;
    if (format > 7) return AO_ERR_INVALID_FORMAT;
    if (((param >> 16) & 0xF) > 2) return AO_ERR_INVALID_FORMAT;
    static const uint8_t channels[8] = {1, 2, 8, 1, 2, 8, 8, 8};
    static const uint8_t sizes[8] = {2, 2, 2, 4, 4, 4, 2, 4};

    int start, end;
    if (!ao_range(type, &start, &end)) return AO_ERR_INVALID_PORT_TYPE;
    plat_mutex_lock(g_ao_lock);
    int id = -1;
    for (int i = start; i <= end; i++) {
        if (!g_ao_ports[i].open) { id = i; break; }
    }
    if (id < 0) {
        plat_mutex_unlock(g_ao_lock);
        return AO_ERR_PORT_FULL;
    }
    ao_port *p = &g_ao_ports[id];
    p->open = true;
    p->type = type;
    p->frames = length;
    p->channels = channels[format];
    p->sample_size = sizes[format];
    p->deadline = 0;
    p->last_output_ns = 0;
    p->mix_level = 11626; /* PADSPK default, -9 dB */
    plat_mutex_unlock(g_ao_lock);

    int32_t handle = (type << 16) | id | 0x20000000;
    log_info("audioout: open user=%d type=%d index=%d len=%u fmt=%u -> %#x", user_id, type, index,
             length, format, (unsigned)handle);
    return handle;
}

static SYSV int32_t ps4_sceAudioOutClose(int32_t handle) {
    if (!atomic_load(&g_ao_init)) return AO_ERR_NOT_INIT;
    int32_t id = ao_port_id(handle);
    if (id < 0) return id;
    plat_mutex_lock(g_ao_lock);
    bool was_open = g_ao_ports[id].open;
    g_ao_ports[id].open = false;
    plat_mutex_unlock(g_ao_lock);
    log_debug("audioout: close %#x", (unsigned)handle);
    return was_open ? SCE_OK : AO_ERR_NOT_OPENED;
}

/* Block for one buffer on port `id`. NULL `ptr` only waits for the queued
 * buffer to finish. Returns the sample count accepted or an error. */
static int32_t ao_output(int32_t id, const void *ptr) {
    plat_mutex_lock(g_ao_lock);
    ao_port *p = &g_ao_ports[id];
    if (!p->open) {
        plat_mutex_unlock(g_ao_lock);
        return AO_ERR_NOT_OPENED;
    }
    uint64_t wait = pace_reserve(&p->deadline, frames_to_ns(p->frames), ptr != NULL);
    int32_t samples = ptr ? (int32_t)(p->frames * p->channels) : 0;
    if (ptr) p->last_output_ns = p->deadline;
    plat_mutex_unlock(g_ao_lock);
    pace_sleep(wait);
    return samples;
}

static SYSV int32_t ps4_sceAudioOutOutput(int32_t handle, const void *ptr) {
    if (!atomic_load(&g_ao_init)) return AO_ERR_NOT_INIT;
    int32_t id = ao_port_id(handle);
    if (id < 0) return id;
    return ao_output(id, ptr);
}

typedef struct ao_output_param {
    int32_t handle;
    const void *ptr;
} ao_output_param;

/* Several ports are submitted together and released together: wait for the
 * latest of their deadlines once. */
static SYSV int32_t ps4_sceAudioOutOutputs(const ao_output_param *param, uint32_t num) {
    if (!atomic_load(&g_ao_init)) return AO_ERR_NOT_INIT;
    if (num == 0 || num > AO_NUM_PORTS) return AO_ERR_PORT_FULL;
    if (!param) return AO_ERR_INVALID_POINTER;
    int32_t ids[AO_NUM_PORTS];
    for (uint32_t i = 0; i < num; i++) {
        ids[i] = ao_port_id(param[i].handle);
        if (ids[i] < 0) return ids[i];
        for (uint32_t j = 0; j < i; j++)
            if (param[j].handle == param[i].handle) return AO_ERR_INVALID_PORT;
    }
    plat_mutex_lock(g_ao_lock);
    uint32_t frames = 0;
    for (uint32_t i = 0; i < num; i++) {
        ao_port *p = &g_ao_ports[ids[i]];
        if (!p->open) {
            plat_mutex_unlock(g_ao_lock);
            return AO_ERR_NOT_OPENED;
        }
        if (i == 0) frames = p->frames;
        else if (p->frames != frames) {
            plat_mutex_unlock(g_ao_lock);
            return AO_ERR_INVALID_SIZE;
        }
    }
    uint64_t wait = 0;
    for (uint32_t i = 0; i < num; i++) {
        ao_port *p = &g_ao_ports[ids[i]];
        uint64_t w = pace_reserve(&p->deadline, frames_to_ns(frames), param[i].ptr != NULL);
        if (param[i].ptr) p->last_output_ns = p->deadline;
        if (w > wait) wait = w;
    }
    plat_mutex_unlock(g_ao_lock);
    pace_sleep(wait);
    return (int32_t)frames;
}

typedef struct ao_port_state {
    uint16_t output;
    uint8_t channel;
    uint8_t reserved8_1;
    int16_t volume;
    uint16_t reroute_counter;
    uint64_t flag;
    uint64_t reserved64[2];
} ao_port_state;

static SYSV int32_t ps4_sceAudioOutGetPortState(int32_t handle, ao_port_state *state) {
    if (!atomic_load(&g_ao_init)) return AO_ERR_NOT_INIT;
    int32_t id = ao_port_id(handle);
    if (id < 0) return id;
    if (!state) return AO_ERR_INVALID_POINTER;
    plat_mutex_lock(g_ao_lock);
    ao_port p = g_ao_ports[id];
    plat_mutex_unlock(g_ao_lock);
    if (!p.open) return AO_ERR_NOT_OPENED;
    memset(state, 0, sizeof(*state));
    state->volume = -1;
    switch (p.type) {
    case AO_MAIN: case AO_BGM: case AO_AUDIO3D:
        state->output = 0x01; /* CONNECTED_PRIMARY */
        state->channel = (uint8_t)(p.channels > 2 ? 2 : p.channels);
        break;
    case AO_VOICE: case AO_PERSONAL:
        state->output = 0x40; /* CONNECTED_HEADPHONE */
        state->channel = 1;
        break;
    case AO_PADSPK:
        state->output = 0x04; /* CONNECTED_TERTIARY */
        state->channel = 1;
        state->volume = 127;
        break;
    case AO_AUX:
        state->output = 0x80; /* CONNECTED_EXTERNAL */
        break;
    default:
        break;
    }
    return SCE_OK;
}

static SYSV int32_t ps4_sceAudioOutSetMixLevelPadSpk(int32_t handle, int32_t level) {
    if (!atomic_load(&g_ao_init)) return AO_ERR_NOT_INIT;
    int32_t id = ao_port_id(handle);
    if (id < 0) return id;
    if (((handle >> 16) & 0xFF) != AO_PADSPK) return AO_ERR_INVALID_PORT_TYPE;
    if (level < 0 || level > 32768) return AO_ERR_INVALID_MIXLEVEL;
    plat_mutex_lock(g_ao_lock);
    bool open = g_ao_ports[id].open;
    if (open) g_ao_ports[id].mix_level = level;
    plat_mutex_unlock(g_ao_lock);
    return open ? SCE_OK : AO_ERR_NOT_OPENED;
}

HLE_LIBRARY(hle_audioout, "libSceAudioOut",
    HLE(sceAudioOutInit),
    HLE(sceAudioOutOpen),
    HLE(sceAudioOutClose),
    HLE(sceAudioOutOutput),
    HLE(sceAudioOutOutputs),
    HLE(sceAudioOutGetPortState),
    HLE(sceAudioOutSetMixLevelPadSpk),
)

/* ---- libSceAudio3d ----------------------------------------------------- */

/* Ports hold a count of mixed buffers (PortAdvance adds one) instead of audio.
 * ADVANCE_AND_PUSH ports drain through PortPush, paced like AudioOut;
 * ADVANCE_NO_PUSH ports drain on their own at the real-time rate. */
enum { A3D_MAX_PORTS = 4, A3D_MAX_OBJECTS = 1024 };
enum { A3D_NO_ADVANCE = 0, A3D_ADVANCE_NO_PUSH = 1, A3D_ADVANCE_AND_PUSH = 2 };
enum { A3D_BLOCKING_ASYNC = 0, A3D_BLOCKING_SYNC = 1 };

typedef struct a3d_open_params {
    uint64_t size_this;
    uint32_t granularity;
    uint32_t rate;
    uint32_t max_objects;
    uint32_t queue_depth;
    uint32_t buffer_mode;
    uint32_t pad;
    uint32_t num_beds;
} a3d_open_params;

typedef struct a3d_attribute {
    uint32_t id;
    uint32_t pad;
    const void *value;
    uint64_t value_size;
} a3d_attribute;

typedef struct a3d_port {
    bool open;
    uint32_t granularity;
    uint32_t max_objects;
    uint32_t queue_depth;
    uint32_t buffer_mode;
    uint32_t level;         /* mixed buffers waiting to be played */
    uint64_t deadline;      /* push pacing */
    uint64_t drain_ns;      /* NO_PUSH: time up to which buffers were consumed */
    uint32_t num_objects;
    uint32_t next_object;
    uint8_t objects[A3D_MAX_OBJECTS / 8]; /* reserved bitmap, indexed by id % max */
} a3d_port;

static a3d_port g_a3d_ports[A3D_MAX_PORTS];
static bool g_a3d_init;

static a3d_port *a3d_get(uint32_t port_id) {
    if (port_id >= A3D_MAX_PORTS || !g_a3d_ports[port_id].open) return NULL;
    return &g_a3d_ports[port_id];
}

/* NO_PUSH ports: drop the buffers whose playback time has passed. */
static void a3d_drain(a3d_port *p) {
    if (p->buffer_mode != A3D_ADVANCE_NO_PUSH) return;
    uint64_t now = plat_monotonic_ns(), dur = frames_to_ns(p->granularity);
    if (p->level == 0 || p->drain_ns == 0 || now > p->drain_ns + PACE_RESYNC_NS) {
        p->drain_ns = now;
        if (p->level) p->level--; /* resync: assume the head buffer just played */
        return;
    }
    while (p->level && now >= p->drain_ns + dur) {
        p->drain_ns += dur;
        p->level--;
    }
}

static SYSV int32_t ps4_sceAudio3dInitialize(int64_t reserved) {
    if (reserved != 0) return A3D_ERR_INVALID_PARAMETER;
    plat_mutex_lock(g_ao_lock);
    bool again = g_a3d_init;
    g_a3d_init = true;
    plat_mutex_unlock(g_ao_lock);
    if (again) return A3D_ERR_NOT_READY;
    int32_t r = ps4_sceAudioOutInit();
    if (r < 0 && r != AO_ERR_ALREADY_INIT) return r;
    log_info("audio3d: null initialized");
    return SCE_OK;
}

static SYSV int32_t ps4_sceAudio3dTerminate(void) {
    plat_mutex_lock(g_ao_lock);
    bool was = g_a3d_init;
    g_a3d_init = false;
    memset(g_a3d_ports, 0, sizeof(g_a3d_ports));
    plat_mutex_unlock(g_ao_lock);
    return was ? SCE_OK : A3D_ERR_NOT_READY;
}

static SYSV int32_t ps4_sceAudio3dPortOpen(int32_t user_id, const a3d_open_params *params,
                                           uint32_t *port_id) {
    if (!params || !port_id) {
        if (port_id) *port_id = 0xFFFFFFFFu;
        return A3D_ERR_INVALID_PARAMETER;
    }
    *port_id = 0xFFFFFFFFu;
    /* Defaults by structure size, as in shadPS4. */
    uint32_t max_objects = 512, depth = 2, mode = A3D_NO_ADVANCE;
    switch (params->size_this & ~7ull) {
    case 0x10: break;
    case 0x18:
        max_objects = params->max_objects; depth = params->queue_depth;
        mode = A3D_ADVANCE_NO_PUSH;
        break;
    case 0x20: case 0x28:
        max_objects = params->max_objects; depth = params->queue_depth;
        mode = params->buffer_mode;
        break;
    default:
        return A3D_ERR_INVALID_PARAMETER;
    }
    uint32_t gran = params->granularity;
    if (params->rate != 0 || gran < 0x100 || (gran & 0xFF) || gran > 2048 || max_objects == 0 ||
        depth == 0 || mode > A3D_ADVANCE_AND_PUSH)
        return A3D_ERR_INVALID_PARAMETER;

    plat_mutex_lock(g_ao_lock);
    if (!g_a3d_init) {
        plat_mutex_unlock(g_ao_lock);
        return A3D_ERR_NOT_READY;
    }
    int id = -1;
    for (int i = 0; i < A3D_MAX_PORTS; i++) {
        if (!g_a3d_ports[i].open) { id = i; break; }
    }
    if (id < 0) {
        plat_mutex_unlock(g_ao_lock);
        return A3D_ERR_OUT_OF_RESOURCES;
    }
    a3d_port *p = &g_a3d_ports[id];
    memset(p, 0, sizeof(*p));
    p->open = true;
    p->granularity = gran;
    p->max_objects = max_objects < A3D_MAX_OBJECTS ? max_objects : A3D_MAX_OBJECTS;
    p->queue_depth = depth;
    p->buffer_mode = mode;
    plat_mutex_unlock(g_ao_lock);
    *port_id = (uint32_t)id;
    log_info("audio3d: port %d open user=%d granularity=%u objects=%u depth=%u mode=%u", id,
             user_id, gran, max_objects, depth, mode);
    return SCE_OK;
}

static SYSV int32_t ps4_sceAudio3dPortClose(uint32_t port_id) {
    plat_mutex_lock(g_ao_lock);
    a3d_port *p = a3d_get(port_id);
    if (p) p->open = false;
    plat_mutex_unlock(g_ao_lock);
    return p ? SCE_OK : A3D_ERR_INVALID_PORT;
}

static SYSV int32_t ps4_sceAudio3dObjectReserve(uint32_t port_id, uint32_t *object_id) {
    if (!object_id) return A3D_ERR_INVALID_PARAMETER;
    *object_id = 0xFFFFFFFFu;
    plat_mutex_lock(g_ao_lock);
    a3d_port *p = a3d_get(port_id);
    int32_t r = SCE_OK;
    if (!p) r = A3D_ERR_INVALID_PORT;
    else if (p->num_objects >= p->max_objects) r = A3D_ERR_OUT_OF_RESOURCES;
    else {
        uint32_t id = p->next_object;
        while (p->objects[id / 8] & (1u << (id % 8))) id = (id + 1) % p->max_objects;
        p->objects[id / 8] |= (uint8_t)(1u << (id % 8));
        p->next_object = (id + 1) % p->max_objects;
        p->num_objects++;
        *object_id = id;
    }
    plat_mutex_unlock(g_ao_lock);
    return r;
}

static bool a3d_object_valid(const a3d_port *p, uint32_t id) {
    return id < p->max_objects && (p->objects[id / 8] & (1u << (id % 8)));
}

static SYSV int32_t ps4_sceAudio3dObjectUnreserve(uint32_t port_id, uint32_t object_id) {
    plat_mutex_lock(g_ao_lock);
    a3d_port *p = a3d_get(port_id);
    int32_t r = SCE_OK;
    if (!p) r = A3D_ERR_INVALID_PORT;
    else if (!a3d_object_valid(p, object_id)) r = A3D_ERR_INVALID_OBJECT;
    else {
        p->objects[object_id / 8] &= (uint8_t)~(1u << (object_id % 8));
        p->num_objects--;
    }
    plat_mutex_unlock(g_ao_lock);
    return r;
}

/* Attributes (PCM, position, gain...) are accepted and discarded. */
static SYSV int32_t ps4_sceAudio3dObjectSetAttributes(uint32_t port_id, uint32_t object_id,
                                                      uint64_t num, const a3d_attribute *attrs) {
    if (!num || !attrs) return A3D_ERR_INVALID_PARAMETER;
    plat_mutex_lock(g_ao_lock);
    a3d_port *p = a3d_get(port_id);
    int32_t r = SCE_OK;
    if (!p) r = A3D_ERR_INVALID_PORT;
    else if (object_id < p->max_objects && !a3d_object_valid(p, object_id))
        r = A3D_ERR_INVALID_OBJECT;
    /* Unknown ids beyond max are ignored, like shadPS4 does for unreserved ones. */
    plat_mutex_unlock(g_ao_lock);
    return r;
}

static SYSV int32_t ps4_sceAudio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id,
                                                   const void *attribute, uint64_t size) {
    (void)attribute_id; (void)size;
    plat_mutex_lock(g_ao_lock);
    a3d_port *p = a3d_get(port_id);
    plat_mutex_unlock(g_ao_lock);
    if (!p) return A3D_ERR_INVALID_PORT;
    if (!attribute) return A3D_ERR_INVALID_PARAMETER;
    return SCE_OK;
}

static SYSV int32_t ps4_sceAudio3dPortAdvance(uint32_t port_id) {
    plat_mutex_lock(g_ao_lock);
    a3d_port *p = a3d_get(port_id);
    int32_t r = SCE_OK;
    if (!p) r = A3D_ERR_INVALID_PORT;
    else if (p->buffer_mode == A3D_NO_ADVANCE) r = A3D_ERR_NOT_SUPPORTED;
    else {
        a3d_drain(p);
        if (p->level >= p->queue_depth) r = A3D_ERR_NOT_READY;
        else {
            if (p->level == 0 && p->buffer_mode == A3D_ADVANCE_NO_PUSH)
                p->drain_ns = plat_monotonic_ns();
            p->level++;
        }
    }
    plat_mutex_unlock(g_ao_lock);
    return r;
}

/* Like shadPS4: nothing is sent until the queue is full; then one buffer goes
 * to the (null) output, blocking for its duration. SYNC keeps sending until
 * the queue is below its depth again. */
static SYSV int32_t ps4_sceAudio3dPortPush(uint32_t port_id, uint32_t blocking) {
    for (;;) {
        plat_mutex_lock(g_ao_lock);
        a3d_port *p = a3d_get(port_id);
        if (!p) {
            plat_mutex_unlock(g_ao_lock);
            return A3D_ERR_INVALID_PORT;
        }
        if (p->buffer_mode != A3D_ADVANCE_AND_PUSH) {
            plat_mutex_unlock(g_ao_lock);
            return A3D_ERR_NOT_SUPPORTED;
        }
        if (p->level < p->queue_depth || p->level == 0) {
            plat_mutex_unlock(g_ao_lock);
            return SCE_OK;
        }
        p->level--;
        uint64_t wait = pace_reserve(&p->deadline, frames_to_ns(p->granularity), true);
        plat_mutex_unlock(g_ao_lock);
        pace_sleep(wait);
        if (blocking == A3D_BLOCKING_ASYNC) return SCE_OK;
    }
}

static SYSV int32_t ps4_sceAudio3dPortGetQueueLevel(uint32_t port_id, uint32_t *level,
                                                    uint32_t *available) {
    if (!level && !available) return A3D_ERR_INVALID_PARAMETER;
    plat_mutex_lock(g_ao_lock);
    a3d_port *p = a3d_get(port_id);
    if (!p) {
        plat_mutex_unlock(g_ao_lock);
        return A3D_ERR_INVALID_PORT;
    }
    a3d_drain(p);
    uint32_t l = p->level, d = p->queue_depth;
    plat_mutex_unlock(g_ao_lock);
    if (level) *level = l;
    if (available) *available = l < d ? d - l : 0;
    return SCE_OK;
}

HLE_LIBRARY(hle_audio3d, "libSceAudio3d",
    HLE(sceAudio3dInitialize),
    HLE(sceAudio3dTerminate),
    HLE(sceAudio3dPortOpen),
    HLE(sceAudio3dPortClose),
    HLE(sceAudio3dObjectReserve),
    HLE(sceAudio3dObjectUnreserve),
    HLE(sceAudio3dObjectSetAttributes),
    HLE(sceAudio3dPortSetAttribute),
    HLE(sceAudio3dPortAdvance),
    HLE(sceAudio3dPortPush),
    HLE(sceAudio3dPortGetQueueLevel),
)
