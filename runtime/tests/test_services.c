/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Workstream C: UserService, SystemService, Np, Pad and the small services. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/hle.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

static void *fn(const char *lib, const char *name) {
    void *p = hle_lookup(lib, name);
    if (!p) {
        fprintf(stderr, "FAIL missing %s:%s\n", lib, name);
        g_failures++;
        exit(1);
    }
    return p;
}
#define CALL(lib, name, type) ((type)fn(lib, name))

/* Every import of the in-scope libraries, exactly as in build/game/boot.txt. */
static const char *const k_imports[][2] = {
    {"libSceAppContent", "sceAppContentGetAddcontInfoList"},
    {"libSceAppContent", "sceAppContentInitialize"},
    {"libSceCommonDialog", "sceCommonDialogInitialize"},
    {"libSceCommonDialog", "sceCommonDialogIsUsed"},
    {"libSceDiscMap", "Func_7C980FFB0AA27E7A"},
    {"libSceDiscMap", "sceDiscMapIsRequestOnHDD"},
    {"libSceGameLiveStreaming", "sceGameLiveStreamingEnableLiveStreaming"},
    {"libSceIme", "sceImeKeyboardClose"},
    {"libSceIme", "sceImeKeyboardOpen"},
    {"libSceIme", "sceImeUpdate"},
    {"libSceNpManager", "sceNpSetContentRestriction"},
    {"libSceNpManager", "sceNpSetNpTitleId"},
    {"libSceNpTrophy", "sceNpTrophyCreateContext"},
    {"libSceNpTrophy", "sceNpTrophyCreateHandle"},
    {"libSceNpTrophy", "sceNpTrophyDestroyContext"},
    {"libSceNpTrophy", "sceNpTrophyDestroyHandle"},
    {"libSceNpTrophy", "sceNpTrophyRegisterContext"},
    {"libSceNpTrophy", "sceNpTrophyUnlockTrophy"},
    {"libScePad", "scePadClose"},
    {"libScePad", "scePadGetControllerInformation"},
    {"libScePad", "scePadInit"},
    {"libScePad", "scePadOpen"},
    {"libScePad", "scePadRead"},
    {"libScePad", "scePadReadState"},
    {"libScePad", "scePadSetLightBar"},
    {"libScePad", "scePadSetMotionSensorState"},
    {"libScePad", "scePadSetVibration"},
    {"libScePngEnc", "scePngEncCreate"},
    {"libScePngEnc", "scePngEncDelete"},
    {"libScePngEnc", "scePngEncEncode"},
    {"libScePngEnc", "scePngEncQueryMemorySize"},
    {"libSceRtc", "sceRtcGetCurrentClock"},
    {"libSceScreenShot", "sceScreenShotDisable"},
    {"libSceScreenShot", "sceScreenShotEnable"},
    {"libSceShareUtility", "sceShareUtilityInitializeEx2"},
    {"libSceShareUtility", "sceShareUtilityOpenShareMenuDefault"},
    {"libSceSystemGesture", "sceSystemGestureClose"},
    {"libSceSystemGesture", "sceSystemGestureCreateTouchRecognizer"},
    {"libSceSystemGesture", "sceSystemGestureFinalizePrimitiveTouchRecognizer"},
    {"libSceSystemGesture", "sceSystemGestureGetTouchEventByIndex"},
    {"libSceSystemGesture", "sceSystemGestureGetTouchEventsCount"},
    {"libSceSystemGesture", "sceSystemGestureInitializePrimitiveTouchRecognizer"},
    {"libSceSystemGesture", "sceSystemGestureOpen"},
    {"libSceSystemGesture", "sceSystemGestureUpdatePrimitiveTouchRecognizer"},
    {"libSceSystemGesture", "sceSystemGestureUpdateTouchRecognizer"},
    {"libSceSystemService", "sceSystemServiceGetDisplaySafeAreaInfo"},
    {"libSceSystemService", "sceSystemServiceGetStatus"},
    {"libSceSystemService", "sceSystemServiceHideSplashScreen"},
    {"libSceSystemService", "sceSystemServiceLoadExec"},
    {"libSceSystemService", "sceSystemServiceParamGetInt"},
    {"libSceSystemService", "sceSystemServiceReceiveEvent"},
    {"libSceUserService", "sceUserServiceGetEvent"},
    {"libSceUserService", "sceUserServiceGetInitialUser"},
    {"libSceUserService", "sceUserServiceGetLoginUserIdList"},
    {"libSceUserService", "sceUserServiceGetUserName"},
    {"libSceUserService", "sceUserServiceInitialize"},
    {"libSceVideoRecording", "sceVideoRecordingSetInfo"},
};

static void test_all_imports(void) {
    for (size_t i = 0; i < sizeof(k_imports) / sizeof(k_imports[0]); i++) {
        if (!hle_lookup(k_imports[i][0], k_imports[i][1])) {
            fprintf(stderr, "FAIL unresolved %s:%s\n", k_imports[i][0], k_imports[i][1]);
            g_failures++;
        }
    }
}

static void test_userservice(void) {
    const char *L = "libSceUserService";
    int32_t prio = 0x2BC;
    CHECK(CALL(L, "sceUserServiceInitialize", SYSV int32_t (*)(void *))(&prio) == 0);
    CHECK(CALL(L, "sceUserServiceInitialize", SYSV int32_t (*)(void *))(&prio) == 0);

    int32_t user = 0;
    CHECK(CALL(L, "sceUserServiceGetInitialUser", SYSV int32_t (*)(int32_t *))(&user) == 0);
    CHECK(user == 1);
    CHECK(CALL(L, "sceUserServiceGetInitialUser", SYSV int32_t (*)(int32_t *))(NULL) ==
          (int32_t)0x80960005);

    int32_t list[4] = {0};
    CHECK(CALL(L, "sceUserServiceGetLoginUserIdList", SYSV int32_t (*)(int32_t *))(list) == 0);
    CHECK(list[0] == 1 && list[1] == -1 && list[2] == -1 && list[3] == -1);

    SYSV int32_t (*get_name)(int32_t, char *, uint64_t) =
        CALL(L, "sceUserServiceGetUserName", SYSV int32_t (*)(int32_t, char *, uint64_t));
    char name[17] = {0};
    CHECK(get_name(1, name, sizeof(name)) == 0);
    const char *env = getenv("SOTC_USER_NAME");
    if (!env || !*env) CHECK(strcmp(name, "Wander") == 0);
    CHECK(get_name(1, name, 3) == (int32_t)0x8096000A);
    CHECK(get_name(-1, name, sizeof(name)) == (int32_t)0x80960005);

    int32_t ev[2];
    CHECK(CALL(L, "sceUserServiceGetEvent", SYSV int32_t (*)(int32_t *))(ev) ==
          (int32_t)0x80960007);
}

static void test_systemservice(void) {
    const char *L = "libSceSystemService";
    SYSV int32_t (*get_int)(int32_t, int32_t *) =
        CALL(L, "sceSystemServiceParamGetInt", SYSV int32_t (*)(int32_t, int32_t *));
    int32_t v = -5;
    CHECK(get_int(1, &v) == 0 && v >= 0 && v <= 30); /* language */
    CHECK(get_int(1000, &v) == 0 && (v == 0 || v == 1)); /* enter button */
    CHECK(get_int(4, &v) == 0 && v >= -12 * 60 && v <= 14 * 60); /* time zone */
    CHECK(get_int(5, &v) == 0 && (v == 0 || v == 1));
    CHECK(get_int(7, &v) == 0 && v == 0);
    CHECK(get_int(1, NULL) == (int32_t)0x80A10003);

    uint8_t status[160];
    memset(status, 0xCC, sizeof(status));
    CHECK(CALL(L, "sceSystemServiceGetStatus", SYSV int32_t (*)(void *))(status) == 0);
    int32_t events;
    memcpy(&events, status, 4);
    CHECK(events == 0);
    CHECK(status[4] == 0 && status[5] == 0 && status[6] == 1 && status[7] == 0 && status[8] == 0);

    struct { float ratio; uint8_t reserved[128]; } safe;
    CHECK(CALL(L, "sceSystemServiceGetDisplaySafeAreaInfo", SYSV int32_t (*)(void *))(&safe) == 0);
    CHECK(safe.ratio == 1.0f);

    static uint8_t event[0x2008];
    CHECK(CALL(L, "sceSystemServiceReceiveEvent", SYSV int32_t (*)(void *))(event) ==
          (int32_t)0x80A10004);
    CHECK(CALL(L, "sceSystemServiceHideSplashScreen", SYSV int32_t (*)(void))() == 0);
}

static void test_np(void) {
    const char *M = "libSceNpManager", *T = "libSceNpTrophy";
    struct { char id[13]; uint8_t pad[3]; } title = {"CUSA08809_00", {0}};
    uint8_t secret[128] = {0};
    CHECK(CALL(M, "sceNpSetNpTitleId", SYSV int32_t (*)(void *, void *))(&title, secret) == 0);
    CHECK(CALL(M, "sceNpSetNpTitleId", SYSV int32_t (*)(void *, void *))(NULL, secret) ==
          (int32_t)0x80550003);
    struct { uint64_t size; int8_t def; uint8_t pad[3]; int32_t count; void *ages; } r = {
        sizeof(r), 0, {0}, 0, NULL};
    SYSV int32_t (*restrict_fn)(void *) = CALL(M, "sceNpSetContentRestriction", SYSV int32_t (*)(void *));
    CHECK(restrict_fn(&r) == 0);
    r.size = 8;
    CHECK(restrict_fn(&r) == (int32_t)0x80550011);

    SYSV int32_t (*create_ctx)(int32_t *, int32_t, uint32_t, uint64_t) =
        CALL(T, "sceNpTrophyCreateContext", SYSV int32_t (*)(int32_t *, int32_t, uint32_t, uint64_t));
    SYSV int32_t (*create_h)(int32_t *) = CALL(T, "sceNpTrophyCreateHandle", SYSV int32_t (*)(int32_t *));
    SYSV int32_t (*reg)(int32_t, int32_t, uint64_t) =
        CALL(T, "sceNpTrophyRegisterContext", SYSV int32_t (*)(int32_t, int32_t, uint64_t));
    SYSV int32_t (*unlock)(int32_t, int32_t, int32_t, int32_t *) =
        CALL(T, "sceNpTrophyUnlockTrophy", SYSV int32_t (*)(int32_t, int32_t, int32_t, int32_t *));
    SYSV int32_t (*destroy_ctx)(int32_t) = CALL(T, "sceNpTrophyDestroyContext", SYSV int32_t (*)(int32_t));
    SYSV int32_t (*destroy_h)(int32_t) = CALL(T, "sceNpTrophyDestroyHandle", SYSV int32_t (*)(int32_t));

    int32_t ctx = -1, h = -1, plat = 0;
    CHECK(create_ctx(&ctx, 1, 0, 0) == 0 && ctx > 0);
    int32_t ctx2;
    CHECK(create_ctx(&ctx2, 1, 0, 0) == (int32_t)0x80551613); /* same user + label */
    CHECK(create_ctx(&ctx2, 1, 0, 1) == (int32_t)0x80551604); /* options must be 0 */
    CHECK(create_h(&h) == 0 && h > 0);
    CHECK(unlock(ctx, h, 3, &plat) == (int32_t)0x8055160F);    /* not registered */
    CHECK(reg(ctx, h, 0) == 0);
    CHECK(reg(ctx, h, 0) == (int32_t)0x80551610);
    CHECK(unlock(ctx, h, 3, &plat) == 0 && plat == -1);
    CHECK(unlock(ctx, h, 3, &plat) == 0);
    CHECK(unlock(ctx, h, 128, &plat) == (int32_t)0x8055160A);
    CHECK(unlock(ctx, h, 3, NULL) == (int32_t)0x80551604);
    CHECK(unlock(ctx + 1000, h, 3, &plat) == (int32_t)0x80551609);
    CHECK(unlock(ctx, h + 1000, 3, &plat) == (int32_t)0x80551608);
    CHECK(reg(h, ctx, 0) != 0); /* handle and context ids are not interchangeable */
    CHECK(destroy_h(h) == 0);
    CHECK(destroy_h(h) == (int32_t)0x80551608);
    CHECK(destroy_ctx(ctx) == 0);
    CHECK(destroy_ctx(ctx) == (int32_t)0x80551609);
    CHECK(create_ctx(&ctx, 1, 0, 0) == 0); /* the slot is free again */
    CHECK(destroy_ctx(ctx) == 0);

    int32_t hs[5];
    for (int i = 0; i < 4; i++) CHECK(create_h(&hs[i]) == 0);
    CHECK(create_h(&hs[4]) == (int32_t)0x80551624);
    for (int i = 0; i < 4; i++) CHECK(destroy_h(hs[i]) == 0);
}

typedef struct pad_data {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2, pad[2];
    float orientation[4];
    float accel[3], gyro[3];
    uint8_t touch[24];
    uint8_t connected;
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t connected_count;
    uint8_t rest[15];
} pad_data;

static void test_pad(void) {
    const char *L = "libScePad";
    SYSV int32_t (*open)(int32_t, int32_t, int32_t, void *) =
        CALL(L, "scePadOpen", SYSV int32_t (*)(int32_t, int32_t, int32_t, void *));
    CHECK(open(1, 0, 0, NULL) == (int32_t)0x80920005); /* before scePadInit */
    CHECK(CALL(L, "scePadInit", SYSV int32_t (*)(void))() == 0);
    CHECK(open(2, 0, 0, NULL) == (int32_t)0x809B0081);
    int32_t h = open(1, 0, 0, NULL);
    CHECK(h > 0);
    CHECK(open(1, 0, 0, NULL) == (int32_t)0x80920004);

    CHECK(sizeof(pad_data) == 120);
    pad_data d[4];
    memset(d, 0xAB, sizeof(d));
    SYSV int32_t (*read)(int32_t, void *, int32_t) =
        CALL(L, "scePadRead", SYSV int32_t (*)(int32_t, void *, int32_t));
    CHECK(read(h, d, 4) == 1);
    CHECK(d[0].buttons == 0 && d[0].lx == 128 && d[0].ly == 128 && d[0].rx == 128 && d[0].ry == 128);
    CHECK(d[0].l2 == 0 && d[0].r2 == 0 && d[0].orientation[3] == 1.0f);
    CHECK(d[0].connected == 1 && d[0].connected_count == 1 && d[0].timestamp > 0);
    CHECK(read(h, d, 0) == (int32_t)0x80920001);
    CHECK(read(h + 100, d, 1) == (int32_t)0x80920003);

    memset(d, 0xAB, sizeof(d));
    CHECK(CALL(L, "scePadReadState", SYSV int32_t (*)(int32_t, void *))(h, d) == 0);
    CHECK(d[0].connected == 1 && d[0].lx == 128);

    uint8_t info[28];
    CHECK(CALL(L, "scePadGetControllerInformation", SYSV int32_t (*)(int32_t, void *))(h, info) == 0);
    CHECK(info[10] == 0 /* local */ && info[11] == 1 && info[12] == 1);

    SYSV int32_t (*light)(int32_t, const uint8_t *) =
        CALL(L, "scePadSetLightBar", SYSV int32_t (*)(int32_t, const uint8_t *));
    const uint8_t blue[4] = {0, 0, 255, 0}, dark[4] = {1, 2, 3, 0};
    CHECK(light(h, blue) == 0);
    CHECK(light(h, dark) == (int32_t)0x80920006);
    const uint8_t vib[2] = {10, 20};
    CHECK(CALL(L, "scePadSetVibration", SYSV int32_t (*)(int32_t, const uint8_t *))(h, vib) == 0);
    CHECK(CALL(L, "scePadSetMotionSensorState", SYSV int32_t (*)(int32_t, int32_t))(h, 1) == 0);

    SYSV int32_t (*close)(int32_t) = CALL(L, "scePadClose", SYSV int32_t (*)(int32_t));
    CHECK(close(h) == 0);
    CHECK(close(h) == (int32_t)0x80920003);
    h = open(1, 0, 0, NULL);
    CHECK(h > 0);
    CHECK(close(h) == 0);
}

static void test_rtc(void) {
    struct { uint16_t y, mo, d, h, mi, s; uint32_t us; } t, t2;
    SYSV int32_t (*clock)(void *, int32_t) =
        CALL("libSceRtc", "sceRtcGetCurrentClock", SYSV int32_t (*)(void *, int32_t));
    CHECK(clock(&t, 0) == 0);
    CHECK(t.y >= 2024 && t.y < 2200 && t.mo >= 1 && t.mo <= 12 && t.d >= 1 && t.d <= 31);
    CHECK(t.h < 24 && t.mi < 60 && t.s < 60 && t.us < 1000000);
    CHECK(clock(&t2, 60) == 0);
    CHECK(t2.h == (t.h + 1) % 24 || t2.h == (t.h + 2) % 24); /* +60 minutes */
    CHECK(clock(NULL, 0) == 0x7FFEF9FE);
}

static void test_misc(void) {
    uint8_t boot[40];
    memset(boot, 0xFF, sizeof(boot));
    CHECK(CALL("libSceAppContent", "sceAppContentInitialize", SYSV int32_t (*)(void *, void *))(
              NULL, boot) == 0);
    CHECK(boot[4] == 0);
    uint32_t hits = 99;
    SYSV int32_t (*addcont)(uint32_t, void *, uint32_t, uint32_t *) = CALL(
        "libSceAppContent", "sceAppContentGetAddcontInfoList",
        SYSV int32_t (*)(uint32_t, void *, uint32_t, uint32_t *));
    CHECK(addcont(0, NULL, 0, &hits) == 0 && hits == 0);
    CHECK(addcont(0, NULL, 0, NULL) == (int32_t)0x80D90002);

    int32_t flags = 7, r1 = 7, r2 = 7;
    CHECK(CALL("libSceDiscMap", "Func_7C980FFB0AA27E7A",
               SYSV int32_t (*)(const char *, int64_t, int64_t, int32_t *, int32_t *, int32_t *))(
              "/app0/x", 0, 4096, &flags, &r1, &r2) == 0);
    CHECK(flags == 0 && r1 == 0 && r2 == 0);

    const char *C = "libSceCommonDialog";
    CHECK(CALL(C, "sceCommonDialogInitialize", SYSV int32_t (*)(void))() == 0);
    CHECK(CALL(C, "sceCommonDialogInitialize", SYSV int32_t (*)(void))() == (int32_t)0x80B80002);
    CHECK(CALL(C, "sceCommonDialogIsUsed", SYSV int32_t (*)(void))() == 0);

    const char *I = "libSceIme";
    SYSV int32_t (*update)(void *) = CALL(I, "sceImeUpdate", SYSV int32_t (*)(void *));
    CHECK(update(NULL) == (int32_t)0x80BC0002);
    struct { uint32_t option; int8_t r1[4]; void *arg; void *handler; int8_t r2[8]; } kp = {0};
    kp.handler = (void *)test_misc;
    SYSV int32_t (*kopen)(int32_t, void *) = CALL(I, "sceImeKeyboardOpen", SYSV int32_t (*)(int32_t, void *));
    CHECK(kopen(1, NULL) == (int32_t)0x80BC0031);
    CHECK(kopen(1, &kp) == 0);
    CHECK(update(NULL) == 0);
    CHECK(CALL(I, "sceImeKeyboardClose", SYSV int32_t (*)(int32_t))(1) == 0);
    CHECK(update(NULL) == (int32_t)0x80BC0002);

    CHECK(CALL("libSceSystemGesture", "sceSystemGestureGetTouchEventsCount",
               SYSV int32_t (*)(int32_t, void *))(0, NULL) == 0);
    CHECK(CALL("libSceShareUtility", "sceShareUtilityInitializeEx2", SYSV int32_t (*)(void))() == 0);
}

static uint32_t crc32_ref(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void test_pngenc(void) {
    const char *L = "libScePngEnc";
    struct { uint32_t this_size, attribute, max_width, max_filter; } cp = {16, 0, 4, 0};
    int32_t size = CALL(L, "scePngEncQueryMemorySize", SYSV int32_t (*)(void *))(&cp);
    CHECK(size > 0 && size <= 256);
    static uint8_t mem[256];
    void *h = NULL;
    CHECK(CALL(L, "scePngEncCreate", SYSV int32_t (*)(void *, void *, uint32_t, void **))(
              &cp, mem, (uint32_t)size, &h) == 0);
    CHECK(h != NULL);

    /* 3x2 BGRA image, pitch 16 (one pad pixel per row). */
    uint8_t img[2 * 16];
    for (int i = 0; i < (int)sizeof(img); i++) img[i] = (uint8_t)(i * 7 + 1);
    static uint8_t png[1024];
    struct {
        const uint8_t *image; uint8_t *png; uint32_t image_size, png_size, w, h, pitch;
        uint16_t pixel_format, color_space, bit_depth, clut, filter, level;
    } ep = {img, png, sizeof(img), sizeof(png), 3, 2, 16, 1, 19, 8, 0, 0, 0};
    uint32_t out[2] = {0, 0};
    SYSV int32_t (*encode)(void *, void *, void *) =
        CALL(L, "scePngEncEncode", SYSV int32_t (*)(void *, void *, void *));
    int32_t n = encode(h, &ep, out);
    CHECK(n > 0 && (uint32_t)n == out[0] && out[1] == 2);
    /* Signature and chunks, each with a correct CRC. */
    CHECK(memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0);
    size_t off = 8;
    int chunks = 0;
    while (n > 0 && off + 12 <= (size_t)n) {
        uint32_t len = be32(png + off);
        CHECK(be32(png + off + 8 + len) == crc32_ref(png + off + 4, len + 4));
        if (memcmp(png + off + 4, "IDAT", 4) == 0) {
            /* zlib header, one final stored block of 2 * (1 + 3 * 4) bytes. */
            const uint8_t *z = png + off + 8;
            CHECK(z[0] == 0x78 && (z[0] * 256 + z[1]) % 31 == 0);
            CHECK(z[2] == 1 && z[3] == 26 && z[4] == 0 && z[5] == (uint8_t)~26);
            CHECK(z[7] == 0); /* filter byte of row 0 */
            CHECK(z[8] == img[2] && z[9] == img[1] && z[10] == img[0] && z[11] == img[3]);
            CHECK(z[7 + 13 + 1] == img[16 + 2]); /* row 1 starts at the pitch */
            uint32_t a = 1, b = 0;
            for (int i = 0; i < 26; i++) {
                a = (a + z[7 + i]) % 65521;
                b = (b + a) % 65521;
            }
            CHECK(be32(z + 7 + 26) == (b << 16 | a));
            CHECK(len == 2 + 5 + 26 + 4);
        }
        off += 12 + len;
        chunks++;
    }
    CHECK(chunks == 3 && off == (size_t)n);
    CHECK(memcmp(png + n - 8, "IEND", 4) == 0);

    ep.png_size = 40;
    CHECK(encode(h, &ep, out) == (int32_t)0x80690110);
    ep.png_size = sizeof(png);
    ep.bit_depth = 16;
    CHECK(encode(h, &ep, out) == (int32_t)0x80690103);
    SYSV int32_t (*del)(void *) = CALL(L, "scePngEncDelete", SYSV int32_t (*)(void *));
    CHECK(del(h) == 0);
    CHECK(encode(h, &ep, out) == (int32_t)0x80690104);
}

int main(void) {
    log_init();
    hle_init();
    test_all_imports();
    test_userservice();
    test_systemservice();
    test_np();
    test_pad();
    test_rtc();
    test_misc();
    test_pngenc();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("services tests passed\n");
    return 0;
}
