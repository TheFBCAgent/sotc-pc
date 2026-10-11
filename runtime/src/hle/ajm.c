/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAjm: Sony's audio decoder service. SotC's eboot doesn't import it,
 * but the firmware libSceNgs2.sprx does (linked from SOTC_FW_DIR, see
 * docs/next/audio.md): Ngs2 decodes its ATRAC9 sampler voices through Ajm
 * batches.
 *
 * Batches use the SDK chunk format. The builder functions are imports, but a
 * caller may also write chunks itself, so BatchStartBuffer parses the buffer.
 * Batches run synchronously at submission (no worker thread): BatchWait
 * always finds them done.
 *
 * ATRAC9 (the only codec Ngs2 asks for) decodes through LibAtrac9
 * (third_party/LibAtrac9, MIT, Alex Barney; shadPS4's ext-LibAtrac9 fork with
 * S32/F32 and planar output): one handle per instance, frame by frame within
 * fixed-size superframes, into S16, S32 or F32 PCM, interleaved unless the
 * instance asks for planar output, with gapless skip/total and every sideband
 * (result, stream, format, gapless, multi-frame, codec info) filled in.
 * Audio step 3 (docs/next/audio.md); steps 1-2 wrote silence here.
 *
 * Layouts, flags and semantics follow shadPS4's src/core/libraries/ajm/
 * (ajm.h, ajm_batch.cpp, ajm_context.cpp, ajm_instance.cpp, ajm_at9.cpp;
 * GPL-2.0-or-later, shadPS4 Emulator Project) and bbport's compact C version
 * (bloodborne_pc, runtime_ajm.c, GPL). */
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "libatrac9.h"

#define AJM_ERR_INVALID_CONTEXT       ((int32_t)0x80930002)
#define AJM_ERR_INVALID_INSTANCE      ((int32_t)0x80930003)
#define AJM_ERR_INVALID_BATCH         ((int32_t)0x80930004)
#define AJM_ERR_INVALID_PARAMETER     ((int32_t)0x80930005)
#define AJM_ERR_OUT_OF_RESOURCES      ((int32_t)0x80930007)
#define AJM_ERR_CODEC_NOT_SUPPORTED   ((int32_t)0x80930008)
#define AJM_ERR_ALREADY_REGISTERED    ((int32_t)0x80930009)
#define AJM_ERR_NOT_REGISTERED        ((int32_t)0x8093000A)
#define AJM_ERR_WRONG_REVISION        ((int32_t)0x8093000B)
#define AJM_ERR_MALFORMED_BATCH       ((int32_t)0x80930011)

#define AJM_RESULT_NOT_INITIALIZED    0x00000001u
#define AJM_RESULT_INVALID_DATA       0x00000002u
#define AJM_RESULT_INVALID_PARAMETER  0x00000004u
#define AJM_RESULT_PARTIAL_INPUT      0x00000008u
#define AJM_RESULT_NOT_ENOUGH_ROOM    0x00000010u
#define AJM_RESULT_CODEC_ERROR        0x40000000u
#define AJM_RESULT_FATAL              0x80000000u

#define AJM_CODEC_AT9 1u
#define AJM_NUM_CODECS 23u
#define AJM_INSTANCE_STATISTICS 0x80000u
#define AJM_INSTANCE_MASK 0x3FFFu
#define AJM_MAX_CONTEXTS 8u
#define AJM_MAX_BATCHES 256u

enum {
    IDENT_JOB = 0, IDENT_INPUT_RUN = 1, IDENT_INPUT_CONTROL = 2, IDENT_CONTROL_FLAGS = 3,
    IDENT_RUN_FLAGS = 4, IDENT_RETURN_ADDRESS = 6, IDENT_INLINE = 7, IDENT_OUTPUT_RUN = 17,
    IDENT_OUTPUT_CONTROL = 18,
};
enum { FORMAT_S16 = 0, FORMAT_S32 = 1, FORMAT_FLOAT = 2 };

/* Job flags: version:3 codec:8 run:2 control:3 reserved:29 sideband:3. */
#define RUN_CODEC_INFO(f)     (((f) >> 11) & 1)
#define RUN_MULTIPLE_FRAMES(f) (((f) >> 12) & 1)
#define CONTROL_RESET(f)      (((f) >> 13) & 1)
#define CONTROL_INITIALIZE(f) (((f) >> 14) & 1)
#define CONTROL_RESAMPLE(f)   (((f) >> 15) & 1)
#define SIDEBAND_GAPLESS(f)   (((f) >> 45) & 1)
#define SIDEBAND_FORMAT(f)    (((f) >> 46) & 1)
#define SIDEBAND_STREAM(f)    (((f) >> 47) & 1)

/* ATRAC9 instance codec flags (instance flags bits 32+). */
#define AT9_PARSE_RIFF 0x1u
#define AT9_NON_INTERLEAVED 0x100u

/* A chunk header word is ident:6 payload:20 reserved:6. */
typedef struct ajm_chunk { uint32_t word, size; } ajm_chunk;
typedef struct ajm_chunk_buffer { uint32_t word, size; void *address; } ajm_chunk_buffer;
typedef struct ajm_buffer { void *address; uint64_t size; } ajm_buffer;
typedef struct ajm_batch_error {
    int32_t error_code;
    const void *job_address;
    uint32_t command_offset;
    const void *job_return_address;
} ajm_batch_error;

typedef struct { int32_t result, internal_result; } sb_result;
typedef struct { int32_t input_consumed, output_written; uint64_t total_decoded_samples; } sb_stream;
typedef struct {
    uint32_t channels, channel_mask, sample_rate, encoding, bitrate, reserved;
} sb_format;
typedef struct { uint32_t total_samples; uint16_t skip_samples, skipped_samples; } sb_gapless;
typedef struct {
    uint32_t super_frame_size, frames_in_super_frame, next_frame_size, frame_samples;
} sb_at9_info;
_Static_assert(sizeof(ajm_chunk_buffer) == 16, "Ajm buffer chunk");
_Static_assert(sizeof(sb_stream) == 16 && sizeof(sb_format) == 24 && sizeof(sb_gapless) == 8,
               "Ajm sideband layout");

/* What LibAtrac9 derives from the 4-byte config (Atrac9GetCodecInfo). */
typedef struct at9_info {
    uint32_t channels, sample_rate, frame_samples, frames_in_superframe, superframe_bytes;
} at9_info;

typedef struct ajm_instance {
    uint32_t codec, format, codec_flags;
    bool gapless_loop;
    bool initialized;
    uint8_t config[4];
    at9_info info;
    void *decoder;           /* LibAtrac9 handle; NULL until a valid Initialize */
    uint8_t *pcm;            /* one decoded frame: frame_samples x channels x 4 bytes */
    uint32_t superframe_remain, frames;  /* bytes left in the superframe, frames done in it */
    sb_gapless gapless_init, gapless;
    uint64_t total_samples;
} ajm_instance;

typedef struct ajm_context {
    bool registered[AJM_NUM_CODECS];
    ajm_instance *instances[AJM_INSTANCE_MASK + 1];   /* index 0 unused */
    bool batch_used[AJM_MAX_BATCHES];
    uint32_t next_batch;
} ajm_context;

static plat_mutex *g_lock;
static ajm_context *g_contexts[AJM_MAX_CONTEXTS + 1];
static uint64_t g_batches, g_jobs, g_frames, g_decode_errors;

__attribute__((constructor)) static void ajm_lock_init(void) { g_lock = plat_mutex_new(); }

static uint32_t ident(uint32_t word) { return word & 0x3F; }
static uint32_t payload(uint32_t word) { return (word >> 6) & 0xFFFFF; }
static uint32_t header(uint32_t id, uint32_t value) { return (id & 0x3F) | ((value & 0xFFFFF) << 6); }
static uint32_t pcm_size(uint32_t format) { return format == FORMAT_S16 ? 2 : 4; }

static uint32_t channel_mask(uint32_t channels) {
    switch (channels) {
    case 1: return 0x4;
    case 2: return 0x3;
    case 4: return 0x33;
    case 6: return 0x60F;
    case 8: return 0x63F;
    default: return 0;
    }
}

/* ---- ATRAC9: LibAtrac9 (MIT), driven like shadPS4's ajm_at9.cpp ---------- */

/* The config is 0xFE, then big-endian bit fields: sample rate index:4,
 * channel config index:3, validation bit:1, frame bytes - 1:11, superframe
 * index:2. LibAtrac9's ReadConfigData checks the header and the validation
 * bit, but indexes its 6-entry channel config table with the 3-bit index
 * unchecked: check that here first. */
static bool at9_config_ok(const uint8_t config[4]) {
    uint32_t v = (uint32_t)config[1] << 16 | (uint32_t)config[2] << 8 | config[3];
    return config[0] == 0xFE && ((v >> 17) & 7) < 6 && !((v >> 16) & 1);
}

static void at9_close(ajm_instance *in) {
    if (in->decoder) Atrac9ReleaseHandle(in->decoder);
    in->decoder = NULL;
}

/* A fresh decoder for the instance's config (shadPS4 AjmAt9Decoder::Reset):
 * LibAtrac9 keeps the IMDCT overlap and the frame index in the superframe in
 * its handle, so a reset starts a new handle. */
static bool at9_open(ajm_instance *in) {
    at9_close(in);
    if (!at9_config_ok(in->config)) return false;
    in->decoder = Atrac9GetHandle();
    if (!in->decoder) return false;
    Atrac9CodecInfo ci;
    if (Atrac9InitDecoder(in->decoder, in->config) != 0 || Atrac9GetCodecInfo(in->decoder, &ci) != 0) {
        at9_close(in);
        return false;
    }
    in->info.channels = (uint32_t)ci.channels;
    in->info.sample_rate = (uint32_t)ci.samplingRate;
    in->info.frame_samples = (uint32_t)ci.frameSamples;
    in->info.frames_in_superframe = (uint32_t)ci.framesInSuperframe;
    in->info.superframe_bytes = (uint32_t)ci.superframeSize;
    free(in->pcm);
    in->pcm = malloc((size_t)in->info.frame_samples * in->info.channels * 4);
    if (!in->pcm) {
        at9_close(in);
        return false;
    }
    return true;
}

static void at9_reset(ajm_instance *in) {
    if (in->initialized && !at9_open(in)) in->initialized = false;
    if (!in->initialized) {
        at9_close(in);
        memset(&in->info, 0, sizeof(in->info));
    }
    in->frames = 0;
    in->superframe_remain = in->info.superframe_bytes;
}

static void at9_free(ajm_instance *in) {
    if (!in) return;
    at9_close(in);
    free(in->pcm);
    free(in);
}

static void at9_initialize(ajm_instance *in, const uint8_t config[4]) {
    memcpy(in->config, config, 4);
    in->initialized = true;
    at9_reset(in);
    if (!in->initialized) {
        log_warn("ajm: bad ATRAC9 config %02x %02x %02x %02x", config[0], config[1], config[2],
                 config[3]);
        return;
    }
    static int logged;
    if (logged < 16) {
        logged++;
        log_info("ajm: ATRAC9 %u ch %u Hz, %u samples x %u frames per %u-byte superframe, "
                 "format %u, flags %#x", in->info.channels, in->info.sample_rate,
                 in->info.frame_samples, in->info.frames_in_superframe,
                 in->info.superframe_bytes, in->format, in->codec_flags);
    }
}

/* One frame from at most `size` bytes into in->pcm, in the instance's format
 * (S16 when `s16`: draining). Returns LibAtrac9's status; *used is the frame's
 * byte count (frames are byte aligned and vary in size, the superframe has
 * the fixed size). */
static int at9_decode(ajm_instance *in, const uint8_t *data, uint32_t size, bool s16, uint32_t *used) {
    int bytes = 0, r;
    int planar = (in->codec_flags & AT9_NON_INTERLEAVED) != 0;
    uint32_t format = s16 ? FORMAT_S16 : in->format;
    if (format == FORMAT_S32)
        r = Atrac9DecodeS32(in->decoder, data, (int)size, (int *)in->pcm, &bytes, planar);
    else if (format == FORMAT_FLOAT)
        r = Atrac9DecodeF32(in->decoder, data, (int)size, (float *)in->pcm, &bytes, planar);
    else
        r = Atrac9Decode(in->decoder, data, (int)size, (short *)in->pcm, &bytes, planar);
    /* The bit reader reads zeros past the end, so a damaged frame can claim
     * more bytes than it was given. */
    *used = bytes < 0 ? 0 : (uint32_t)bytes > size ? size : (uint32_t)bytes;
    if (r) {
        g_decode_errors++;
        if (g_decode_errors <= 16)
            log_warn("ajm: Atrac9Decode failed: %#x (frame %u of the superframe, %u bytes left)",
                     (unsigned)r, in->frames, in->superframe_remain);
    }
    return r;
}

static void gapless_reset(ajm_instance *in) {
    in->gapless.total_samples = in->gapless_init.total_samples;
    in->gapless.skip_samples = in->gapless_init.skip_samples;
    in->gapless.skipped_samples = 0;
}

static bool gapless_end(const ajm_instance *in) {
    return in->gapless_init.total_samples && !in->gapless.total_samples;
}

/* PCM bytes the next frame writes (shadPS4 GetNextFrameSize). */
static uint32_t next_frame_bytes(const ajm_instance *in) {
    uint32_t frame = in->info.frame_samples;
    uint32_t skip = in->gapless.skip_samples < frame ? in->gapless.skip_samples : frame;
    uint32_t samples = frame - skip;
    if (in->gapless_init.total_samples && in->gapless.total_samples < samples)
        samples = in->gapless.total_samples;
    return samples * in->info.channels * pcm_size(in->format);
}

/* Output run buffers, written in order. */
typedef struct ajm_output { ajm_chunk_buffer *chunks; int count, current; uint32_t offset; } ajm_output;

static uint64_t output_room(const ajm_output *o) {
    uint64_t room = 0;
    for (int i = o->current; i < o->count; i++)
        room += o->chunks[i].size - (i == o->current ? o->offset : 0);
    return room;
}

static void output_write(ajm_output *o, const uint8_t *data, uint64_t size) {
    while (size && o->current < o->count) {
        ajm_chunk_buffer *c = &o->chunks[o->current];
        uint64_t n = c->size - o->offset;
        if (n > size) n = size;
        memcpy((uint8_t *)c->address + o->offset, data, n);
        data += n;
        size -= n;
        o->offset += (uint32_t)n;
        if (o->offset == c->size) { o->current++; o->offset = 0; }
    }
}

static void input_skip(const uint8_t **input, uint64_t *size, uint64_t n) {
    if (n > *size) n = *size;
    *input += n;
    *size -= n;
}

/* The end of a superframe, or of the stream: skip the rest of the superframe
 * (padding) so the next one is aligned. */
static void at9_next_superframe(ajm_instance *in, const uint8_t **input, uint64_t *input_size) {
    input_skip(input, input_size, in->superframe_remain);
    in->superframe_remain = in->info.superframe_bytes;
    in->frames = 0;
}

static uint32_t at9_avail(const ajm_instance *in, uint64_t input_size) {
    return input_size < in->superframe_remain ? (uint32_t)input_size : in->superframe_remain;
}

/* One frame (shadPS4 AjmAt9Decoder::ProcessData): decode it, consume its
 * bytes, write its samples past the gapless skip and up to the gapless total.
 * Returns LibAtrac9's status: on an error nothing is consumed or written.
 * Adds to *frames_out; *samples_out is the samples written. The caller checked
 * that the rest of the superframe is in the input. */
static int at9_frame(ajm_instance *in, const uint8_t **input, uint64_t *input_size,
                     ajm_output *out, uint32_t *samples_out, uint32_t *frames_out) {
    uint32_t frame = in->info.frame_samples, channels = in->info.channels;
    uint32_t sample_bytes = pcm_size(in->format), used = 0;
    *samples_out = 0;
    int r = at9_decode(in, *input, at9_avail(in, *input_size), false, &used);
    if (r) return r;
    input_skip(input, input_size, used);
    in->superframe_remain -= used;
    g_frames++;
    (*frames_out)++;
    if (g_frames == 1 || g_frames % 100000 == 0)
        log_info("ajm: %llu ATRAC9 frames decoded (%llu decode errors)",
                 (unsigned long long)g_frames, (unsigned long long)g_decode_errors);
    uint32_t skip = 0;
    if (in->gapless.skip_samples) {
        skip = in->gapless.skip_samples < frame ? in->gapless.skip_samples : frame;
        in->gapless.skip_samples -= (uint16_t)skip;
    }
    uint32_t samples = frame - skip;
    if (in->gapless_init.total_samples && samples > in->gapless.total_samples)
        samples = in->gapless.total_samples;
    uint64_t room = output_room(out) / ((uint64_t)channels * sample_bytes);
    if (samples > room) samples = (uint32_t)room;
    if (in->codec_flags & AT9_NON_INTERLEAVED) {
        /* One plane per channel, each cut to the same sample range. */
        for (uint32_t c = 0; c < channels; c++)
            output_write(out, in->pcm + ((uint64_t)c * frame + skip) * sample_bytes,
                         (uint64_t)samples * sample_bytes);
    } else {
        output_write(out, in->pcm + (uint64_t)skip * channels * sample_bytes,
                     (uint64_t)samples * channels * sample_bytes);
    }
    in->gapless.skipped_samples += (uint16_t)(frame - samples);
    if (in->gapless_init.total_samples) in->gapless.total_samples -= samples;
    *samples_out = samples;
    if (++in->frames == in->info.frames_in_superframe) {
        at9_next_superframe(in, input, input_size);
    } else if (gapless_end(in)) {
        /* The stream ends inside a superframe: decode (and drop) its other
         * frames, as shadPS4 does, so the decoder's frame index stays in step
         * with the next superframe (whose first block says "first"). */
        while (in->frames < in->info.frames_in_superframe) {
            if (at9_decode(in, *input, at9_avail(in, *input_size), true, &used)) {
                at9_open(in);   /* out of step: start over at the next superframe */
                break;
            }
            input_skip(input, input_size, used);
            in->superframe_remain -= used;
            in->frames++;
            g_frames++;
            (*frames_out)++;
        }
        at9_next_superframe(in, input, input_size);
    }
    return 0;
}

/* A RIFF/WAVE header in front of the data (codec flag ParseRiffHeader). */
static void parse_riff(ajm_instance *in, const uint8_t **input, uint64_t *size) {
    const uint8_t *p = *input, *end = p + *size;
    p += 12;
    while (p + 8 <= end) {
        uint32_t tag, length;
        memcpy(&tag, p, 4);
        memcpy(&length, p + 4, 4);
        p += 8;
        if (tag == 0x61746164) break;                                      /* "data" */
        if (tag == 0x20746D66 && length >= 52 && p + 52 <= end) {          /* "fmt " */
            at9_initialize(in, p + 44);
        } else if (tag == 0x74636166 && length >= 12 && p + 12 <= end) {   /* "fact" */
            uint32_t total, delay;
            memcpy(&total, p, 4);
            memcpy(&delay, p + 4, 4);
            in->gapless_init.total_samples = total;
            in->gapless_init.skip_samples = (uint16_t)delay;
            gapless_reset(in);
        }
        p += length;
    }
    if (p > end) p = end;
    *size -= (uint64_t)(p - *input);
    *input = p;
}

/* ---- jobs --------------------------------------------------------------- */

#define AJM_MAX_JOB_BUFFERS 16

typedef struct ajm_job {
    uint32_t instance;
    uint64_t flags;
    bool have_flags;
    ajm_chunk_buffer inputs[AJM_MAX_JOB_BUFFERS];
    int input_count;
    ajm_chunk_buffer outputs[AJM_MAX_JOB_BUFFERS];
    int output_count;
    ajm_chunk_buffer input_control, output_control;
} ajm_job;

static bool parse_job(const uint8_t *p, uint32_t size, ajm_job *job) {
    const uint8_t *end = p + size;
    while (p < end) {
        uint32_t word;
        if (p + 8 > end) return false;
        memcpy(&word, p, 4);
        uint32_t id = ident(word);
        if (id == IDENT_CONTROL_FLAGS || id == IDENT_RUN_FLAGS) {
            if (job->have_flags) return false;
            ajm_chunk c;
            memcpy(&c, p, 8);
            job->flags = (uint64_t)payload(c.word) << 32 | c.size;
            job->have_flags = true;
            p += 8;
            continue;
        }
        if (p + 16 > end) return false;
        ajm_chunk_buffer b;
        memcpy(&b, p, 16);
        p += 16;
        switch (id) {
        case IDENT_INPUT_RUN:
            if (job->input_count == AJM_MAX_JOB_BUFFERS) return false;
            job->inputs[job->input_count++] = b;
            break;
        case IDENT_OUTPUT_RUN:
            if (!b.address || !b.size) break;
            if (job->output_count == AJM_MAX_JOB_BUFFERS) return false;
            job->outputs[job->output_count++] = b;
            break;
        case IDENT_INPUT_CONTROL: job->input_control = b; break;
        case IDENT_OUTPUT_CONTROL: job->output_control = b; break;
        case IDENT_RETURN_ADDRESS: break;
        default: return false;
        }
    }
    return job->have_flags;
}

static void run_job(ajm_context *ctx, ajm_job *job) {
    g_jobs++;
    uint8_t *side = job->output_control.address && job->output_control.size
                        ? job->output_control.address : NULL;
    uint8_t *side_end = side ? side + job->output_control.size : NULL;
    sb_result *result = NULL;
    if (side && side + sizeof(sb_result) <= side_end) {
        result = (sb_result *)side;
        side += sizeof(*result);
        memset(result, 0, sizeof(*result));
    }
    if (job->instance == AJM_INSTANCE_STATISTICS) {
        if (side) memset(side, 0, (size_t)(side_end - side));
        return;
    }
    uint32_t index = job->instance & AJM_INSTANCE_MASK;
    ajm_instance *in = index ? ctx->instances[index] : NULL;
    if (!in) {
        if (result) result->result = (int32_t)(AJM_RESULT_FATAL | AJM_RESULT_INVALID_PARAMETER);
        return;
    }
    uint64_t f = job->flags;
    uint32_t flags_result = 0;

    /* Control: reset, then the input sideband (format, gapless, resample, init). */
    if (CONTROL_RESET(f)) {
        in->total_samples = 0;
        gapless_reset(in);
        at9_reset(in);
    }
    const uint8_t *control = job->input_control.address && job->input_control.size
                                 ? job->input_control.address : NULL;
    const uint8_t *control_end = control ? control + job->input_control.size : NULL;
    if (control && SIDEBAND_FORMAT(f) && control + 24 <= control_end) control += 24;
    if (control && SIDEBAND_GAPLESS(f) && control + 8 <= control_end) {
        sb_gapless g;
        memcpy(&g, control, 8);
        control += 8;
        uint32_t processed = in->gapless_init.total_samples - in->gapless.total_samples;
        if (g.total_samples || !g.skip_samples) {
            if (g.total_samples >= processed) {
                int64_t diff = (int64_t)in->gapless_init.total_samples - g.total_samples;
                in->gapless_init.total_samples = g.total_samples;
                in->gapless.total_samples = (uint32_t)((int64_t)in->gapless.total_samples - diff);
            } else {
                flags_result |= AJM_RESULT_INVALID_PARAMETER;
            }
        }
        uint32_t skipped = (uint32_t)in->gapless_init.skip_samples - in->gapless.skip_samples;
        if (g.skip_samples || !g.total_samples) {
            if (g.skip_samples >= skipped) {
                int32_t diff = (int32_t)in->gapless_init.skip_samples - g.skip_samples;
                in->gapless_init.skip_samples = g.skip_samples;
                in->gapless.skip_samples = (uint16_t)((int32_t)in->gapless.skip_samples - diff);
            } else {
                flags_result |= AJM_RESULT_INVALID_PARAMETER;
            }
        }
    }
    if (control && CONTROL_RESAMPLE(f) && control + 8 <= control_end) control += 8;
    if (control && CONTROL_INITIALIZE(f) && control + 8 <= control_end) at9_initialize(in, control);

    /* Run: the input buffers are one stream. */
    uint64_t in_size = 0;
    for (int i = 0; i < job->input_count; i++) in_size += job->inputs[i].size;
    uint8_t *joined = NULL;
    const uint8_t *input = NULL;
    if (job->input_count == 1) {
        input = job->inputs[0].address;
    } else if (job->input_count > 1) {
        joined = malloc(in_size ? in_size : 1);
        uint64_t at = 0;
        for (int i = 0; i < job->input_count; i++) {
            if (job->inputs[i].size) memcpy(joined + at, job->inputs[i].address, job->inputs[i].size);
            at += job->inputs[i].size;
        }
        input = joined;
    }
    ajm_output out = {job->outputs, job->output_count, 0, 0};
    uint64_t in_start = in_size, out_start = output_room(&out);
    uint32_t frames = 0;
    int32_t internal_result = 0;
    if (in_size && !flags_result) {
        for (;;) {
            if (in->gapless_loop && gapless_end(in)) {
                gapless_reset(in);
                in->total_samples = 0;
            }
            if ((in->codec_flags & AT9_PARSE_RIFF) && in_size >= 4 && !memcmp(input, "RIFF", 4)) {
                parse_riff(in, &input, &in_size);
                in->total_samples = 0;
            }
            if (!in->initialized) { flags_result |= AJM_RESULT_NOT_INITIALIZED; break; }
            if (!gapless_end(in) && output_room(&out) < next_frame_bytes(in))
                flags_result |= AJM_RESULT_NOT_ENOUGH_ROOM;
            if (in_size < in->superframe_remain) flags_result |= AJM_RESULT_PARTIAL_INPUT;
            if (flags_result) break;
            uint32_t samples = 0;
            int r = at9_frame(in, &input, &in_size, &out, &samples, &frames);
            if (r) {
                /* shadPS4: a codec error is fatal, the frame stays unconsumed. */
                flags_result |= AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
                internal_result = r;
                break;
            }
            in->total_samples += samples;
            if (!RUN_MULTIPLE_FRAMES(f)) break;   /* else until partial input or no room */
        }
    }
    free(joined);
    if (result) {
        result->result = (int32_t)flags_result;
        result->internal_result = internal_result;
    }

    /* Output sideband order: stream, format, gapless, multi-frame, codec info. */
    if (side && SIDEBAND_STREAM(f) && side + 16 <= side_end) {
        sb_stream s = {(int32_t)(in_start - in_size), (int32_t)(out_start - output_room(&out)),
                       in->total_samples};
        memcpy(side, &s, 16);
        side += 16;
    }
    if (side && SIDEBAND_FORMAT(f) && side + 24 <= side_end) {
        sb_format s = {in->info.channels, channel_mask(in->info.channels), in->info.sample_rate,
                       in->format, 0, 0};
        if (in->info.frames_in_superframe && in->info.frame_samples)
            s.bitrate = (uint32_t)((uint64_t)in->info.sample_rate * in->info.superframe_bytes * 8 /
                                   ((uint64_t)in->info.frames_in_superframe * in->info.frame_samples));
        memcpy(side, &s, 24);
        side += 24;
    }
    if (side && SIDEBAND_GAPLESS(f) && side + 8 <= side_end) {
        memcpy(side, &in->gapless, 8);
        side += 8;
    }
    if (side && RUN_MULTIPLE_FRAMES(f) && side + 8 <= side_end) {
        uint32_t m[2] = {frames, 0};
        memcpy(side, m, 8);
        side += 8;
    }
    if (side && RUN_CODEC_INFO(f) && side + 16 <= side_end) {
        sb_at9_info info = {in->info.superframe_bytes, in->info.frames_in_superframe,
                            in->superframe_remain, in->info.frame_samples};
        memcpy(side, &info, 16);
    }
}

/* ---- batch builders ------------------------------------------------------ */

static uint8_t *put_buffer(uint8_t *p, uint32_t id, const void *address, uint64_t size) {
    ajm_chunk_buffer c = {header(id, 0), (uint32_t)size, (void *)address};
    memcpy(p, &c, sizeof(c));
    return p + sizeof(c);
}

static uint8_t *put_flags(uint8_t *p, uint32_t id, uint64_t flags) {
    ajm_chunk c = {header(id, (uint32_t)(flags >> 32)), (uint32_t)flags};
    memcpy(p, &c, sizeof(c));
    return p + sizeof(c);
}

static void *finish_job(uint8_t *start, uint8_t *end, uint32_t instance) {
    ajm_chunk job = {header(IDENT_JOB, instance), (uint32_t)(end - start - sizeof(ajm_chunk))};
    memcpy(start, &job, sizeof(job));
    return end;
}

static SYSV void *ps4_sceAjmBatchJobControlBufferRa(void *buffer, uint32_t instance, uint64_t flags,
                                                    void *in, uint64_t in_size, void *out,
                                                    uint64_t out_size, void *return_address) {
    uint8_t *p = (uint8_t *)buffer + sizeof(ajm_chunk);
    if (return_address) p = put_buffer(p, IDENT_RETURN_ADDRESS, return_address, 0);
    p = put_buffer(p, IDENT_INPUT_CONTROL, in, in_size);
    flags &= instance == AJM_INSTANCE_STATISTICS ? 0xC0018007ull : 0x60000000E7FFull;
    p = put_flags(p, IDENT_CONTROL_FLAGS, flags);
    p = put_buffer(p, IDENT_OUTPUT_CONTROL, out, out_size);
    return finish_job(buffer, p, instance);
}

static SYSV void *ps4_sceAjmBatchJobRunBufferRa(void *buffer, uint32_t instance, uint64_t flags,
                                                void *in, uint64_t in_size, void *out,
                                                uint64_t out_size, void *sideband,
                                                uint64_t sideband_size, void *return_address) {
    uint8_t *p = (uint8_t *)buffer + sizeof(ajm_chunk);
    if (return_address) p = put_buffer(p, IDENT_RETURN_ADDRESS, return_address, 0);
    p = put_buffer(p, IDENT_INPUT_RUN, in, in_size);
    p = put_flags(p, IDENT_RUN_FLAGS, flags & 0xE00000001FFFull);
    p = put_buffer(p, IDENT_OUTPUT_RUN, out, out_size);
    p = put_buffer(p, IDENT_OUTPUT_CONTROL, sideband, sideband_size);
    return finish_job(buffer, p, instance);
}

static SYSV void *ps4_sceAjmBatchJobRunSplitBufferRa(void *buffer, uint32_t instance, uint64_t flags,
                                                     const ajm_buffer *in, uint64_t in_count,
                                                     const ajm_buffer *out, uint64_t out_count,
                                                     void *sideband, uint64_t sideband_size,
                                                     void *return_address) {
    uint8_t *p = (uint8_t *)buffer + sizeof(ajm_chunk);
    if (return_address) p = put_buffer(p, IDENT_RETURN_ADDRESS, return_address, 0);
    for (uint64_t i = 0; i < in_count; i++) p = put_buffer(p, IDENT_INPUT_RUN, in[i].address, in[i].size);
    p = put_flags(p, IDENT_RUN_FLAGS, flags & 0xE00000001FFFull);
    for (uint64_t i = 0; i < out_count; i++)
        p = put_buffer(p, IDENT_OUTPUT_RUN, out[i].address, out[i].size);
    p = put_buffer(p, IDENT_OUTPUT_CONTROL, sideband, sideband_size);
    return finish_job(buffer, p, instance);
}

static SYSV void *ps4_sceAjmBatchJobInlineBuffer(void *buffer, const void *data, uint64_t size,
                                                 const void **address) {
    ajm_chunk c = {header(IDENT_INLINE, 0), (uint32_t)((size + 7) & ~7ull)};
    memcpy(buffer, &c, sizeof(c));
    uint8_t *p = (uint8_t *)buffer + sizeof(c);
    memcpy(p, data, size);
    if (address) *address = p;
    return p + c.size;
}

/* ---- API ---------------------------------------------------------------- */

static ajm_context *context(uint32_t id) {
    return id && id <= AJM_MAX_CONTEXTS ? g_contexts[id] : NULL;
}

static SYSV int32_t ps4_sceAjmInitialize(int64_t reserved, uint32_t *out) {
    if (!out || reserved) return AJM_ERR_INVALID_PARAMETER;
    plat_mutex_lock(g_lock);
    for (uint32_t i = 1; i <= AJM_MAX_CONTEXTS; i++) {
        if (g_contexts[i]) continue;
        g_contexts[i] = calloc(1, sizeof(ajm_context));
        plat_mutex_unlock(g_lock);
        if (!g_contexts[i]) return AJM_ERR_OUT_OF_RESOURCES;
        *out = i;
        log_info("ajm: context %u (ATRAC9 through LibAtrac9)", i);
        return SCE_OK;
    }
    plat_mutex_unlock(g_lock);
    return AJM_ERR_OUT_OF_RESOURCES;
}

static SYSV int32_t ps4_sceAjmFinalize(uint32_t id) {
    plat_mutex_lock(g_lock);
    ajm_context *c = context(id);
    if (c) {
        for (uint32_t i = 0; i <= AJM_INSTANCE_MASK; i++) at9_free(c->instances[i]);
        free(c);
        g_contexts[id] = NULL;
        log_info("ajm: context %u finalized (%llu batches, %llu jobs, %llu ATRAC9 frames, "
                 "%llu decode errors)", id, (unsigned long long)g_batches,
                 (unsigned long long)g_jobs, (unsigned long long)g_frames,
                 (unsigned long long)g_decode_errors);
    }
    plat_mutex_unlock(g_lock);
    return c ? SCE_OK : AJM_ERR_INVALID_CONTEXT;
}

static SYSV int32_t ps4_sceAjmModuleRegister(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec >= AJM_NUM_CODECS) return AJM_ERR_INVALID_PARAMETER;
    plat_mutex_lock(g_lock);
    ajm_context *c = context(id);
    int32_t r = !c ? AJM_ERR_INVALID_CONTEXT : c->registered[codec] ? AJM_ERR_ALREADY_REGISTERED : 0;
    if (!r) c->registered[codec] = true;
    plat_mutex_unlock(g_lock);
    if (!r) log_info("ajm: context %u registers codec %u", id, codec);
    return r;
}

static SYSV int32_t ps4_sceAjmModuleUnregister(uint32_t id, uint32_t codec) {
    plat_mutex_lock(g_lock);
    ajm_context *c = context(id);
    int32_t r = !c ? AJM_ERR_INVALID_CONTEXT
              : codec >= AJM_NUM_CODECS ? AJM_ERR_INVALID_PARAMETER
              : !c->registered[codec] ? AJM_ERR_NOT_REGISTERED : 0;
    if (!r) c->registered[codec] = false;
    plat_mutex_unlock(g_lock);
    return r;
}

/* Instance flags: version:3 channels:4 format:3 gapless_loop:1 :21 codec:32. */
static SYSV int32_t ps4_sceAjmInstanceCreate(uint32_t id, uint32_t codec, uint64_t flags,
                                             uint32_t *out) {
    if (!out || codec >= AJM_NUM_CODECS) return AJM_ERR_INVALID_PARAMETER;
    if (!(flags & 7)) return AJM_ERR_WRONG_REVISION;
    uint32_t format = (uint32_t)(flags >> 7) & 7;
    if (format > FORMAT_FLOAT) return AJM_ERR_INVALID_PARAMETER;
    plat_mutex_lock(g_lock);
    ajm_context *c = context(id);
    int32_t r = !c ? AJM_ERR_INVALID_CONTEXT
              : !c->registered[codec] ? AJM_ERR_NOT_REGISTERED
              : codec != AJM_CODEC_AT9 ? AJM_ERR_CODEC_NOT_SUPPORTED : AJM_ERR_OUT_OF_RESOURCES;
    if (r == AJM_ERR_OUT_OF_RESOURCES) {
        for (uint32_t i = 1; i <= AJM_INSTANCE_MASK; i++) {
            if (c->instances[i]) continue;
            ajm_instance *in = calloc(1, sizeof(*in));
            if (!in) break;
            in->codec = codec;
            in->format = format;
            in->gapless_loop = (flags >> 10) & 1;
            in->codec_flags = (uint32_t)(flags >> 32);
            c->instances[i] = in;
            *out = i | codec << 14;
            r = SCE_OK;
            break;
        }
    }
    plat_mutex_unlock(g_lock);
    if (r == AJM_ERR_CODEC_NOT_SUPPORTED) log_warn("ajm: codec %u instances are not implemented", codec);
    return r;
}

static SYSV int32_t ps4_sceAjmInstanceDestroy(uint32_t id, uint32_t instance) {
    plat_mutex_lock(g_lock);
    ajm_context *c = context(id);
    uint32_t index = instance & AJM_INSTANCE_MASK;
    int32_t r = !c ? AJM_ERR_INVALID_CONTEXT
              : !index || !c->instances[index] ? AJM_ERR_INVALID_INSTANCE : SCE_OK;
    if (!r) {
        at9_free(c->instances[index]);
        c->instances[index] = NULL;
    }
    plat_mutex_unlock(g_lock);
    return r;
}

static SYSV uint32_t ps4_sceAjmInstanceCodecType(uint32_t instance) { return (instance >> 14) & 0x1F; }

static SYSV int32_t ps4_sceAjmBatchStartBuffer(uint32_t id, uint8_t *buffer, uint32_t size,
                                               int32_t priority, ajm_batch_error *error,
                                               uint32_t *out) {
    (void)priority;
    if (!out || (size & 7)) return size & 7 ? AJM_ERR_MALFORMED_BATCH : AJM_ERR_INVALID_PARAMETER;
    plat_mutex_lock(g_lock);
    ajm_context *c = context(id);
    if (!c) {
        plat_mutex_unlock(g_lock);
        return AJM_ERR_INVALID_CONTEXT;
    }
    /* Check the whole batch first, so a malformed one runs nothing. */
    for (uint8_t *p = buffer, *end = buffer + size; p < end;) {
        ajm_chunk chunk;
        memcpy(&chunk, p, 8);
        uint8_t *body = p + 8;
        p = body + chunk.size;
        if (ident(chunk.word) == IDENT_INLINE && p <= end) continue;
        ajm_job job = {0};
        if (ident(chunk.word) != IDENT_JOB || p > end || !parse_job(body, chunk.size, &job)) {
            log_warn("ajm: malformed batch at +%#x (chunk %#x size %#x)",
                     (unsigned)(body - 8 - buffer), chunk.word, chunk.size);
            if (error) {
                error->error_code = AJM_ERR_MALFORMED_BATCH;
                error->job_address = body - 8;
                error->command_offset = (uint32_t)(body - 8 - buffer);
                error->job_return_address = NULL;
            }
            plat_mutex_unlock(g_lock);
            return AJM_ERR_MALFORMED_BATCH;
        }
    }
    for (uint8_t *p = buffer, *end = buffer + size; p < end;) {
        ajm_chunk chunk;
        memcpy(&chunk, p, 8);
        uint8_t *body = p + 8;
        p = body + chunk.size;
        if (ident(chunk.word) == IDENT_INLINE) continue;
        ajm_job job = {0};
        parse_job(body, chunk.size, &job);
        job.instance = payload(chunk.word);
        run_job(c, &job);
    }
    /* The batch is done; its id stays valid until BatchWait. A caller that
     * never waits would fill the table: then the oldest id is reused. */
    uint32_t slot = c->next_batch;
    for (uint32_t i = 0; i < AJM_MAX_BATCHES; i++) {
        uint32_t s = (c->next_batch + i) % AJM_MAX_BATCHES;
        if (!c->batch_used[s]) { slot = s; break; }
    }
    c->batch_used[slot] = true;
    c->next_batch = (slot + 1) % AJM_MAX_BATCHES;
    g_batches++;
    *out = slot + 1;
    plat_mutex_unlock(g_lock);
    return SCE_OK;
}

static SYSV int32_t ps4_sceAjmBatchWait(uint32_t id, uint32_t batch, uint32_t timeout,
                                        ajm_batch_error *error) {
    (void)timeout;
    (void)error;
    plat_mutex_lock(g_lock);
    int32_t r = !context(id) ? AJM_ERR_INVALID_CONTEXT
              : !batch || batch > AJM_MAX_BATCHES || !g_contexts[id]->batch_used[batch - 1]
                  ? AJM_ERR_INVALID_BATCH : SCE_OK;
    if (!r) g_contexts[id]->batch_used[batch - 1] = false;
    plat_mutex_unlock(g_lock);
    return r;
}

static SYSV int32_t ps4_sceAjmBatchCancel(uint32_t id, uint32_t batch) {
    plat_mutex_lock(g_lock);
    int32_t r = !context(id) ? AJM_ERR_INVALID_CONTEXT
              : !batch || batch > AJM_MAX_BATCHES || !g_contexts[id]->batch_used[batch - 1]
                  ? AJM_ERR_INVALID_BATCH : SCE_OK;
    plat_mutex_unlock(g_lock);
    return r;   /* already processed at submission: nothing to cancel */
}

static SYSV int32_t ps4_sceAjmMemoryRegister(uint32_t id, void *p, uint64_t pages) {
    (void)p;
    (void)pages;
    return context(id) ? SCE_OK : AJM_ERR_INVALID_CONTEXT;
}

static SYSV int32_t ps4_sceAjmMemoryUnregister(uint32_t id, void *p) {
    (void)p;
    return context(id) ? SCE_OK : AJM_ERR_INVALID_CONTEXT;
}

HLE_LIBRARY(hle_ajm, "libSceAjm",
    HLE(sceAjmInitialize),
    HLE(sceAjmFinalize),
    HLE(sceAjmModuleRegister),
    HLE(sceAjmModuleUnregister),
    HLE(sceAjmInstanceCreate),
    HLE(sceAjmInstanceDestroy),
    HLE(sceAjmInstanceCodecType),
    HLE(sceAjmBatchJobControlBufferRa),
    HLE(sceAjmBatchJobRunBufferRa),
    HLE(sceAjmBatchJobRunSplitBufferRa),
    HLE(sceAjmBatchJobInlineBuffer),
    HLE(sceAjmBatchStartBuffer),
    HLE(sceAjmBatchWait),
    HLE(sceAjmBatchCancel),
    HLE(sceAjmMemoryRegister),
    HLE(sceAjmMemoryUnregister),
)
