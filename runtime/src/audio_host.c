/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host audio output through SDL3 (see audio_host.h).
 *
 * The port-to-stream model, the 8-channel STD/non-STD orders and draining by
 * queue level follow shadPS4's src/core/libraries/audio/sdl_audio_out.cpp
 * (GPL-2.0-or-later, shadPS4 Emulator Project). */
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "audio_host.h"
#include "log.h"

#define AUDIO_RATE 48000

struct audio_host_stream {
    SDL_AudioStream *sdl;
    _Atomic uint32_t frame_bytes;
};

static SDL_AudioDeviceID g_device;
static atomic_bool g_ready;
static uint32_t g_target_frames = AUDIO_RATE * 60 / 1000;

static long env_long(const char *name, long def, long lo, long hi) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end;
    long n = strtol(v, &end, 10);
    if (*end || n < lo || n > hi) {
        log_warn("audio: ignoring %s=%s (want %ld..%ld)", name, v, lo, hi);
        return def;
    }
    return n;
}

bool audio_host_init(void) {
    const char *off = getenv("SOTC_AUDIO");
    if (off && *off == '0') {
        log_info("audio: off (SOTC_AUDIO=0)");
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log_warn("audio: no audio subsystem (%s), silent", SDL_GetError());
        return false;
    }
    g_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!g_device) {
        log_warn("audio: cannot open the default playback device (%s), silent", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = {0};
    int device_frames = 0;
    SDL_GetAudioDeviceFormat(g_device, &spec, &device_frames);
    long volume = env_long("SOTC_AUDIO_VOLUME", 100, 0, 200);
    SDL_SetAudioDeviceGain(g_device, (float)volume / 100.0f);
    long latency_ms = env_long("SOTC_AUDIO_LATENCY_MS", 60, 10, 500);
    uint32_t target = (uint32_t)(AUDIO_RATE * latency_ms / 1000);
    /* The device pulls whole buffers: keep at least two of them queued. */
    if (device_frames > 0 && target < 2u * (uint32_t)device_frames)
        target = 2u * (uint32_t)device_frames;
    g_target_frames = target;
    atomic_store(&g_ready, true);
    log_info("audio: %s, device %d Hz %d ch, %d frames per buffer; queue %u frames, volume %ld%%",
             SDL_GetCurrentAudioDriver(), spec.freq, spec.channels, device_frames, target, volume);
    return true;
}

bool audio_host_ready(void) { return atomic_load(&g_ready); }

uint32_t audio_host_target_frames(void) { return g_target_frames; }

static audio_host_stream *stream_new(const SDL_AudioSpec *out) {
    SDL_AudioSpec in = {SDL_AUDIO_F32, 2, AUDIO_RATE};
    SDL_AudioStream *sdl = SDL_CreateAudioStream(&in, out);
    if (!sdl) {
        log_warn("audio: cannot create a stream (%s)", SDL_GetError());
        return NULL;
    }
    audio_host_stream *s = calloc(1, sizeof(*s));
    s->sdl = sdl;
    atomic_store(&s->frame_bytes, 8);
    return s;
}

audio_host_stream *audio_host_stream_new(void) {
    if (!audio_host_ready()) return NULL;
    audio_host_stream *s = stream_new(NULL);
    if (!s) return NULL;
    if (!SDL_BindAudioStream(g_device, s->sdl)) {
        log_warn("audio: cannot bind a stream (%s)", SDL_GetError());
        SDL_DestroyAudioStream(s->sdl);
        free(s);
        return NULL;
    }
    return s;
}

audio_host_stream *audio_host_stream_new_unbound(uint32_t out_channels) {
    SDL_AudioSpec out = {SDL_AUDIO_F32, (int)out_channels, AUDIO_RATE};
    return stream_new(&out);
}

bool audio_host_stream_config(audio_host_stream *s, uint32_t channels, bool is_float,
                              bool std_order) {
    SDL_AudioSpec in = {is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, (int)channels, AUDIO_RATE};
    SDL_ClearAudioStream(s->sdl);
    if (!SDL_SetAudioStreamFormat(s->sdl, &in, NULL)) {
        log_warn("audio: stream format %u ch %s: %s", channels, is_float ? "f32" : "s16",
                 SDL_GetError());
        return false;
    }
    /* Ls Rs <-> Lb Rb: the map is its own inverse. */
    static const int swap_sides[8] = {0, 1, 2, 3, 6, 7, 4, 5};
    SDL_SetAudioStreamInputChannelMap(s->sdl, channels == 8 && !std_order ? swap_sides : NULL,
                                      channels == 8 && !std_order ? 8 : 0);
    atomic_store(&s->frame_bytes, channels * (is_float ? 4u : 2u));
    return true;
}

void audio_host_stream_put(audio_host_stream *s, const void *data, uint32_t bytes) {
    SDL_PutAudioStreamData(s->sdl, data, (int)bytes);
}

uint32_t audio_host_stream_queued(audio_host_stream *s) {
    int bytes = SDL_GetAudioStreamQueued(s->sdl);
    return bytes > 0 ? (uint32_t)bytes / atomic_load(&s->frame_bytes) : 0;
}

void audio_host_stream_clear(audio_host_stream *s) { SDL_ClearAudioStream(s->sdl); }

int audio_host_stream_get(audio_host_stream *s, float *out, uint32_t bytes) {
    SDL_FlushAudioStream(s->sdl);
    return SDL_GetAudioStreamData(s->sdl, out, (int)bytes);
}

float audio_host_peak(const void *data, uint32_t samples, bool is_float) {
    float peak = 0.0f;
    if (is_float) {
        const float *f = data;
        for (uint32_t i = 0; i < samples; i++) {
            float a = f[i] < 0 ? -f[i] : f[i];
            if (a > peak) peak = a;
        }
    } else {
        const int16_t *p = data;
        int m = 0;
        for (uint32_t i = 0; i < samples; i++) {
            int a = p[i] < 0 ? -p[i] : p[i];
            if (a > m) m = a;
        }
        peak = (float)m / 32768.0f;
    }
    return peak;
}
