/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small system services, one HLE_LIBRARY each:
 *
 *   libSceAppContent        no add-on content; boot attributes 0
 *   libSceRtc               current clock from the host
 *   libSceDiscMap           everything is installed on the HDD
 *   libSceScreenShot        enable/disable accepted
 *   libSceGameLiveStreaming accepted, never streams
 *   libSceVideoRecording    accepted, never records
 *   libSceShareUtility      accepted; the share menu never opens
 *   libSceIme               USB keyboard: opens, but no keyboard sends events
 *   libSceSystemGesture     touch recognizers that never see a touch
 *   libScePngEnc            minimal encoder (stored deflate, no compression)
 *   libSceCommonDialog      initialization state; no dialog in use
 *
 * Semantics and error codes follow shadPS4 (src/core/libraries/{app_content,
 * rtc, disc_map, screenshot, game_live_streaming, video_recording, ime,
 * system_gesture, libpng/pngenc, system/commondialog}; GPL-2.0, commit
 * 14ada48). */
#include <string.h>

#include "../log.h"
#include "../platform.h"
#include "hle.h"

/* ---- libSceAppContent --------------------------------------------------- */

#define APP_CONTENT_ERROR_PARAMETER ((int32_t)0x80D90002)

typedef struct app_content_boot_param {
    char reserved1[4];
    uint32_t attr;
    char reserved2[32];
} app_content_boot_param;

static SYSV int32_t ps4_sceAppContentInitialize(const void *init_param,
                                                app_content_boot_param *boot_param) {
    (void)init_param;
    if (boot_param) memset(boot_param, 0, sizeof(*boot_param));
    return SCE_OK;
}

static SYSV int32_t ps4_sceAppContentGetAddcontInfoList(uint32_t service_label, void *list,
                                                        uint32_t list_num, uint32_t *hit_num) {
    (void)service_label;
    if ((!list || list_num == 0) && !hit_num) return APP_CONTENT_ERROR_PARAMETER;
    if (hit_num) *hit_num = 0; /* no DLC */
    return SCE_OK;
}

HLE_LIBRARY(hle_appcontent, "libSceAppContent",
    HLE(sceAppContentInitialize),
    HLE(sceAppContentGetAddcontInfoList),
)

/* ---- libSceRtc ---------------------------------------------------------- */

#define RTC_ERROR_DATETIME_UNINITIALIZED ((int32_t)0x7FFEF9FE)

typedef struct rtc_date_time {
    uint16_t year, month, day, hour, minute, second;
    uint32_t microsecond;
} rtc_date_time;

/* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

/* Current UTC time shifted by `time_zone` minutes. */
static SYSV int32_t ps4_sceRtcGetCurrentClock(rtc_date_time *t, int32_t time_zone) {
    if (!t) return RTC_ERROR_DATETIME_UNINITIALIZED;
    int64_t us = (int64_t)(plat_realtime_ns() / 1000) + (int64_t)time_zone * 60 * 1000000;
    int64_t secs = us / 1000000;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    t->year = (uint16_t)y;
    t->month = (uint16_t)m;
    t->day = (uint16_t)d;
    t->hour = (uint16_t)(rem / 3600);
    t->minute = (uint16_t)(rem / 60 % 60);
    t->second = (uint16_t)(rem % 60);
    t->microsecond = (uint32_t)(((us % 1000000) + 1000000) % 1000000);
    return SCE_OK;
}

HLE_LIBRARY(hle_rtc, "libSceRtc",
    HLE(sceRtcGetCurrentClock),
)

/* ---- libSceDiscMap ------------------------------------------------------ */

#define DISC_MAP_ERROR_NO_BITMAP_INFO ((int32_t)0x81100004)

/* Where a byte range of a file lives (disc or HDD). Everything is local: no
 * flags, nothing to wait for. */
static SYSV int32_t ps4_Func_7C980FFB0AA27E7A(const char *path, int64_t offset, int64_t nbytes,
                                              int32_t *flags, int32_t *ret1, int32_t *ret2) {
    (void)path; (void)offset; (void)nbytes;
    if (flags) *flags = 0;
    if (ret1) *ret1 = 0;
    if (ret2) *ret2 = 0;
    return SCE_OK;
}

/* A digital install has no disc bitmap; shadPS4 reports that and games cope. */
static SYSV int32_t ps4_sceDiscMapIsRequestOnHDD(const char *path, int64_t offset, int64_t nbytes,
                                                 int32_t *ret) {
    (void)path; (void)offset; (void)nbytes; (void)ret;
    return DISC_MAP_ERROR_NO_BITMAP_INFO;
}

HLE_LIBRARY(hle_discmap, "libSceDiscMap",
    HLE(Func_7C980FFB0AA27E7A),
    HLE(sceDiscMapIsRequestOnHDD),
)

/* ---- libSceScreenShot, libSceGameLiveStreaming, libSceVideoRecording,
 *      libSceShareUtility: accepted and ignored ----------------------------- */

HLE_STUB_OK(sceScreenShotEnable)
HLE_STUB_OK(sceScreenShotDisable)
HLE_LIBRARY(hle_screenshot, "libSceScreenShot",
    HLE(sceScreenShotEnable),
    HLE(sceScreenShotDisable),
)

HLE_STUB_OK(sceGameLiveStreamingEnableLiveStreaming)
HLE_LIBRARY(hle_gamelivestreaming, "libSceGameLiveStreaming",
    HLE(sceGameLiveStreamingEnableLiveStreaming),
)

HLE_STUB_OK(sceVideoRecordingSetInfo)
HLE_LIBRARY(hle_videorecording, "libSceVideoRecording",
    HLE(sceVideoRecordingSetInfo),
)

HLE_STUB_OK(sceShareUtilityInitializeEx2)
HLE_STUB_OK(sceShareUtilityOpenShareMenuDefault)
HLE_LIBRARY(hle_shareutility, "libSceShareUtility",
    HLE(sceShareUtilityInitializeEx2),
    HLE(sceShareUtilityOpenShareMenuDefault),
)

/* ---- libSceIme (USB keyboard) ------------------------------------------- */

#define IME_ERROR_NOT_OPENED      ((int32_t)0x80BC0002)
#define IME_ERROR_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_ERROR_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_ERROR_INVALID_ADDRESS ((int32_t)0x80BC0031)

typedef struct ime_keyboard_param {
    uint32_t option;
    int8_t reserved1[4];
    void *arg;
    void *handler; /* guest callback, never called: no keyboard is attached */
    int8_t reserved2[8];
} ime_keyboard_param;

static volatile int g_keyboard_open;

static SYSV int32_t ps4_sceImeKeyboardOpen(int32_t user_id, const ime_keyboard_param *param) {
    if (!param) return IME_ERROR_INVALID_ADDRESS;
    if (!param->handler) return IME_ERROR_INVALID_HANDLER;
    if (user_id == -1) return IME_ERROR_INVALID_USER_ID;
    g_keyboard_open = 1;
    log_debug("ime: keyboard opened for user %d", user_id);
    return SCE_OK;
}

static SYSV int32_t ps4_sceImeKeyboardClose(int32_t user_id) {
    if (!g_keyboard_open) return IME_ERROR_NOT_OPENED;
    if (user_id == -1) return IME_ERROR_INVALID_USER_ID;
    g_keyboard_open = 0;
    return SCE_OK;
}

static SYSV int32_t ps4_sceImeUpdate(void *handler) {
    (void)handler;
    return g_keyboard_open ? SCE_OK : IME_ERROR_NOT_OPENED;
}

HLE_LIBRARY(hle_ime, "libSceIme",
    HLE(sceImeKeyboardOpen),
    HLE(sceImeKeyboardClose),
    HLE(sceImeUpdate),
)

/* ---- libSceSystemGesture ------------------------------------------------ */
/* Touch-pad gestures. No touches ever arrive, so every recognizer stays idle
 * and GetTouchEventsCount returns 0 (the stubs return 0). */

HLE_STUB_OK(sceSystemGestureOpen)
HLE_STUB_OK(sceSystemGestureClose)
HLE_STUB_OK(sceSystemGestureInitializePrimitiveTouchRecognizer)
HLE_STUB_OK(sceSystemGestureFinalizePrimitiveTouchRecognizer)
HLE_STUB_OK(sceSystemGestureUpdatePrimitiveTouchRecognizer)
HLE_STUB_OK(sceSystemGestureCreateTouchRecognizer)
HLE_STUB_OK(sceSystemGestureUpdateTouchRecognizer)
HLE_STUB_OK(sceSystemGestureGetTouchEventsCount)
HLE_STUB_OK(sceSystemGestureGetTouchEventByIndex)
HLE_LIBRARY(hle_systemgesture, "libSceSystemGesture",
    HLE(sceSystemGestureOpen),
    HLE(sceSystemGestureClose),
    HLE(sceSystemGestureInitializePrimitiveTouchRecognizer),
    HLE(sceSystemGestureFinalizePrimitiveTouchRecognizer),
    HLE(sceSystemGestureUpdatePrimitiveTouchRecognizer),
    HLE(sceSystemGestureCreateTouchRecognizer),
    HLE(sceSystemGestureUpdateTouchRecognizer),
    HLE(sceSystemGestureGetTouchEventsCount),
    HLE(sceSystemGestureGetTouchEventByIndex),
)

/* ---- libScePngEnc ------------------------------------------------------- */
/* Writes valid PNGs with uncompressed (stored) deflate blocks: large, but no
 * zlib dependency. Output that doesn't fit fails with DATA_OVERFLOW. */

#define PNG_ENC_ERROR_INVALID_ADDR   ((int32_t)0x80690101)
#define PNG_ENC_ERROR_INVALID_SIZE   ((int32_t)0x80690102)
#define PNG_ENC_ERROR_INVALID_PARAM  ((int32_t)0x80690103)
#define PNG_ENC_ERROR_INVALID_HANDLE ((int32_t)0x80690104)
#define PNG_ENC_ERROR_DATA_OVERFLOW  ((int32_t)0x80690110)

enum { PNG_PIXEL_R8G8B8A8 = 0, PNG_PIXEL_B8G8R8A8 = 1 };
enum { PNG_COLOR_SPACE_RGB = 3, PNG_COLOR_SPACE_RGBA = 19 };

typedef struct png_enc_create_param {
    uint32_t this_size;
    uint32_t attribute;
    uint32_t max_image_width;
    uint32_t max_filter_number;
} png_enc_create_param;

typedef struct png_enc_encode_param {
    const uint8_t *image_mem_addr;
    uint8_t *png_mem_addr;
    uint32_t image_mem_size;
    uint32_t png_mem_size;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t image_pitch;
    uint16_t pixel_format;
    uint16_t color_space;
    uint16_t bit_depth;
    uint16_t clut_number;
    uint16_t filter_type;
    uint16_t compression_level;
} png_enc_encode_param;

typedef struct png_enc_output_info {
    uint32_t data_size;
    uint32_t processed_height;
} png_enc_output_info;

/* The guest-provided handle memory holds only this. */
typedef struct png_encoder {
    uint32_t magic;
    uint32_t max_width;
} png_encoder;
#define PNG_ENC_MAGIC 0x50454E43u /* "PENC" */

static SYSV int32_t ps4_scePngEncQueryMemorySize(const png_enc_create_param *param) {
    if (!param) return PNG_ENC_ERROR_INVALID_ADDR;
    if (param->attribute != 0 || param->max_filter_number > 5) return PNG_ENC_ERROR_INVALID_PARAM;
    if (param->max_image_width - 1 > 1000000) return PNG_ENC_ERROR_INVALID_SIZE;
    return (int32_t)sizeof(png_encoder);
}

static SYSV int32_t ps4_scePngEncCreate(const png_enc_create_param *param, void *memory,
                                        uint32_t memory_size, void **handle) {
    if (!param || param->attribute != 0 || !memory || !handle) return PNG_ENC_ERROR_INVALID_ADDR;
    if (param->max_image_width - 1 > 1000000) return PNG_ENC_ERROR_INVALID_SIZE;
    if (memory_size < sizeof(png_encoder)) return PNG_ENC_ERROR_INVALID_SIZE;
    png_encoder *enc = memory;
    enc->magic = PNG_ENC_MAGIC;
    enc->max_width = param->max_image_width;
    *handle = enc;
    return SCE_OK;
}

static SYSV int32_t ps4_scePngEncDelete(void *handle) {
    png_encoder *enc = handle;
    if (!enc || enc->magic != PNG_ENC_MAGIC) return PNG_ENC_ERROR_INVALID_HANDLE;
    enc->magic = 0;
    return SCE_OK;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    static volatile int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

typedef struct png_writer {
    uint8_t *buf;
    size_t cap, len;
    int overflow;
    /* zlib stored-block state */
    size_t raw_left;   /* uncompressed bytes not yet emitted */
    size_t block_left; /* bytes left in the current stored block */
    uint32_t adler_a, adler_b;
} png_writer;

static void put(png_writer *w, const void *p, size_t n) {
    if (w->len + n > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void put_be32(png_writer *w, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(w, b, 4);
}

/* One byte of the zlib stream's uncompressed data. */
static void put_raw(png_writer *w, uint8_t byte) {
    if (w->block_left == 0) {
        size_t n = w->raw_left < 65535 ? w->raw_left : 65535;
        uint8_t hdr[5] = {(uint8_t)(n == w->raw_left), (uint8_t)n, (uint8_t)(n >> 8),
                          (uint8_t)~n, (uint8_t)(~n >> 8)};
        put(w, hdr, 5);
        w->block_left = n;
    }
    put(w, &byte, 1);
    w->block_left--;
    w->raw_left--;
    w->adler_a = (w->adler_a + byte) % 65521;
    w->adler_b = (w->adler_b + w->adler_a) % 65521;
}

static void put_chunk_end(png_writer *w, size_t chunk_start) {
    /* CRC covers the type and data (not the length). */
    if (w->overflow) return;
    uint32_t crc = crc32_update(0, w->buf + chunk_start + 4, w->len - chunk_start - 4);
    put_be32(w, crc);
}

static SYSV int32_t ps4_scePngEncEncode(void *handle, const png_enc_encode_param *param,
                                        png_enc_output_info *out) {
    png_encoder *enc = handle;
    if (!enc || enc->magic != PNG_ENC_MAGIC) return PNG_ENC_ERROR_INVALID_HANDLE;
    if (!param) return PNG_ENC_ERROR_INVALID_PARAM;
    if (!param->image_mem_addr || !param->png_mem_addr) return PNG_ENC_ERROR_INVALID_ADDR;
    if (!param->png_mem_size || !param->image_mem_size || !param->image_width ||
        !param->image_height)
        return PNG_ENC_ERROR_INVALID_SIZE;
    if (param->bit_depth != 8 || param->pixel_format > PNG_PIXEL_B8G8R8A8 ||
        (param->color_space != PNG_COLOR_SPACE_RGB && param->color_space != PNG_COLOR_SPACE_RGBA))
        return PNG_ENC_ERROR_INVALID_PARAM;

    const uint32_t width = param->image_width, height = param->image_height;
    const uint32_t pitch = param->image_pitch ? param->image_pitch : width * 4;
    if (pitch < width * 4 || (uint64_t)pitch * (height - 1) + width * 4 > param->image_mem_size)
        return PNG_ENC_ERROR_INVALID_SIZE;
    const int alpha = param->color_space == PNG_COLOR_SPACE_RGBA;
    const int bgr = param->pixel_format == PNG_PIXEL_B8G8R8A8;
    const uint32_t channels = alpha ? 4 : 3;

    png_writer w = {.buf = param->png_mem_addr, .cap = param->png_mem_size, .adler_a = 1};
    if (out) out->processed_height = 0;

    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    put(&w, signature, 8);

    size_t start = w.len;
    put_be32(&w, 13);
    put(&w, "IHDR", 4);
    put_be32(&w, width);
    put_be32(&w, height);
    uint8_t ihdr_tail[5] = {8, (uint8_t)(alpha ? 6 : 2), 0, 0, 0};
    put(&w, ihdr_tail, 5);
    put_chunk_end(&w, start);

    /* IDAT: zlib header, stored blocks, adler32. */
    w.raw_left = (size_t)height * (1 + (size_t)width * channels);
    size_t nblocks = (w.raw_left + 65534) / 65535;
    size_t idat_len = 2 + nblocks * 5 + w.raw_left + 4;
    if (idat_len > 0x7FFFFFFF || 8 + 25 + 12 + idat_len + 12 > w.cap) return PNG_ENC_ERROR_DATA_OVERFLOW;
    start = w.len;
    put_be32(&w, (uint32_t)idat_len);
    put(&w, "IDAT", 4);
    static const uint8_t zlib_header[2] = {0x78, 0x01};
    put(&w, zlib_header, 2);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = param->image_mem_addr + (size_t)y * pitch;
        put_raw(&w, 0); /* filter: none */
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *px = row + x * 4;
            put_raw(&w, bgr ? px[2] : px[0]);
            put_raw(&w, px[1]);
            put_raw(&w, bgr ? px[0] : px[2]);
            if (alpha) put_raw(&w, px[3]);
        }
        if (w.overflow) return PNG_ENC_ERROR_DATA_OVERFLOW;
        if (out) out->processed_height = y + 1;
    }
    put_be32(&w, (w.adler_b << 16) | w.adler_a);
    put_chunk_end(&w, start);

    start = w.len;
    put_be32(&w, 0);
    put(&w, "IEND", 4);
    put_chunk_end(&w, start);
    if (w.overflow) return PNG_ENC_ERROR_DATA_OVERFLOW;

    if (out) out->data_size = (uint32_t)w.len;
    return (int32_t)w.len;
}

HLE_LIBRARY(hle_pngenc, "libScePngEnc",
    HLE(scePngEncQueryMemorySize),
    HLE(scePngEncCreate),
    HLE(scePngEncDelete),
    HLE(scePngEncEncode),
)

/* ---- libSceCommonDialog ------------------------------------------------- */
/* Dialog state machines themselves live in dialogs.c (workstream D). */

#define COMMON_DIALOG_ERROR_ALREADY_SYSTEM_INITIALIZED ((int32_t)0x80B80002)

static volatile int g_common_dialog_initialized;

static SYSV int32_t ps4_sceCommonDialogInitialize(void) {
    if (__atomic_exchange_n(&g_common_dialog_initialized, 1, __ATOMIC_ACQ_REL))
        return COMMON_DIALOG_ERROR_ALREADY_SYSTEM_INITIALIZED;
    return SCE_OK;
}

static SYSV int32_t ps4_sceCommonDialogIsUsed(void) { return 0; }

HLE_LIBRARY(hle_commondialog, "libSceCommonDialog",
    HLE(sceCommonDialogInitialize),
    HLE(sceCommonDialogIsUsed),
)
