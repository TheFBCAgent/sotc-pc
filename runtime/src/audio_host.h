/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host audio output (SDL3) behind libSceAudioOut (hle/audio.c).
 *
 * One playback device; one SDL audio stream per AudioOut port, bound to it.
 * SDL mixes the streams and converts each port's format (S16/F32, 1/2/8
 * channels, 48 kHz) to whatever the device wants. The guest's audio thread is
 * paced on the stream's queue (audio.c): its Output call returns once the
 * queue is back under audio_host_target_frames().
 *
 * Environment:
 *   SOTC_AUDIO=0             no host audio (AudioOut falls back to clock pacing)
 *   SOTC_AUDIO_VOLUME=N      0..200 percent, default 100
 *   SOTC_AUDIO_LATENCY_MS=N  queued audio per port before Output blocks
 *                            (default 60, 10..500)
 */
#ifndef SOTC_AUDIO_HOST_H
#define SOTC_AUDIO_HOST_H

#include <stdbool.h>
#include <stdint.h>

/* Open the default playback device. Call on the process main thread after
 * SDL_Init (window.c); does SDL_InitSubSystem(SDL_INIT_AUDIO) itself. Returns
 * whether audio is on. Safe to call once only. */
bool audio_host_init(void);

/* True once audio_host_init succeeded. Any thread. */
bool audio_host_ready(void);

/* Frames a port may have queued before its Output blocks. */
uint32_t audio_host_target_frames(void);

typedef struct audio_host_stream audio_host_stream;

/* A new stream bound to the device, or NULL when audio is off. Never freed:
 * AudioOut keeps one per port slot and reconfigures it on reopen. */
audio_host_stream *audio_host_stream_new(void);

/* Set the input format and drop anything queued. `std_order` only matters for
 * 8 channels: PS4's non-standard 8-channel order is L R C LFE Ls Rs Lb Rb,
 * the "STD" one (and SDL's) L R C LFE Lb Rb Ls Rs. */
bool audio_host_stream_config(audio_host_stream *s, uint32_t channels, bool is_float,
                              bool std_order);

void audio_host_stream_put(audio_host_stream *s, const void *data, uint32_t bytes);
/* Queued input frames (per the last config). */
uint32_t audio_host_stream_queued(audio_host_stream *s);
void audio_host_stream_clear(audio_host_stream *s);

/* Tests: a stream bound to no device, read back converted to F32 with
 * `out_channels` channels. */
audio_host_stream *audio_host_stream_new_unbound(uint32_t out_channels);
int audio_host_stream_get(audio_host_stream *s, float *out, uint32_t bytes);

/* Peak absolute sample value of a guest buffer, for the stats log
 * (1.0 = full scale). */
float audio_host_peak(const void *data, uint32_t samples, bool is_float);

#endif
