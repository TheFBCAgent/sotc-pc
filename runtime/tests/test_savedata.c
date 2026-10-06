/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Workstream D: SaveData on the host and the self-answering dialogs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hle/hle.h"

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

/* Guest structures, as the game lays them out. */
typedef struct { char data[32]; } dir_name;
typedef struct { char data[16]; } mount_point;
typedef struct {
    char title[128], sub_title[128], detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} sd_param;
typedef struct {
    int32_t user_id, pad;
    const dir_name *dir;
    uint64_t blocks;
    uint32_t mode;
    uint8_t reserved[36];
} mount2;
typedef struct {
    mount_point mp;
    uint64_t required_blocks;
    uint32_t unused, status;
    uint8_t reserved[32];
} mount_result;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } mount_info;
typedef struct {
    int32_t user_id, pad;
    const void *title_id;
    const dir_name *dir;
    uint32_t unused;
    uint8_t reserved[36];
} del_param;
typedef struct { void *buf; uint64_t buf_size, data_size; uint8_t reserved[32]; } icon;
typedef struct {
    int32_t user_id, pad;
    const void *title_id;
    const dir_name *dir;
    sd_param *param;
    icon *icon;
    uint8_t reserved[32];
} check_backup;
typedef struct {
    int32_t user_id, pad;
    const void *title_id;
    const dir_name *dir;
    const void *fingerprint;
    uint32_t unused;
    uint8_t reserved[36];
} restore_backup;
typedef struct {
    int32_t user_id, pad;
    const void *title_id;
    const dir_name *dir;
    uint32_t key, order;
    uint8_t reserved[32];
} search_cond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } search_info;
typedef struct {
    uint32_t hit_num;
    int32_t pad;
    dir_name *dirs;
    uint32_t dirs_num, set_num;
    sd_param *params;
    search_info *infos;
    uint8_t reserved[16];
} search_result;

typedef int32_t SYSV (*fn0)(void);
typedef int32_t SYSV (*fn1)(const void *);
typedef int32_t SYSV (*fn2)(const void *, void *);
typedef int32_t SYSV (*fn_setparam)(const void *, uint32_t, const void *, uint64_t);

#define SD(name) hle_lookup("libSceSaveData", name)
#define E_NOT_FOUND ((int32_t)0x809F0008)
#define E_EXISTS ((int32_t)0x809F0007)
#define E_BUSY ((int32_t)0x809F0003)
#define E_NOT_MOUNTED ((int32_t)0x809F0004)
#define E_NOT_INIT ((int32_t)0x809F0001)

static char g_user[512];

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int read_text(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
    return (int)n;
}

static void test_savedata(void) {
    fn1 init = (fn1)SD("sceSaveDataInitialize3");
    fn0 term = (fn0)SD("sceSaveDataTerminate");
    fn2 mount = (fn2)SD("sceSaveDataMount2");
    fn1 umount_backup = (fn1)SD("sceSaveDataUmountWithBackup");
    fn2 info = (fn2)SD("sceSaveDataGetMountInfo");
    fn_setparam setparam = (fn_setparam)SD("sceSaveDataSetParam");
    fn2 saveicon = (fn2)SD("sceSaveDataSaveIcon");
    fn1 del = (fn1)SD("sceSaveDataDelete");
    fn2 search = (fn2)SD("sceSaveDataDirNameSearch");
    fn1 check = (fn1)SD("sceSaveDataCheckBackupData");
    fn1 restore = (fn1)SD("sceSaveDataRestoreBackupData");
    CHECK(init && term && mount && umount_backup && info && setparam && saveicon && del &&
          search && check && restore);
    if (g_failures) return;

    dir_name dn = {"SOTC0001"};
    mount2 m = {.user_id = 1, .dir = &dn, .blocks = 1000, .mode = 2 /* RDWR */};
    mount_result mr;
    CHECK(mount(&m, &mr) == E_NOT_INIT);
    CHECK(init(NULL) == 0);

    /* Not there yet, and no create flag. */
    CHECK(mount(&m, &mr) == E_NOT_FOUND);
    search_cond sc = {.user_id = 1};
    dir_name names[4];
    sd_param params[4];
    search_info infos[4];
    search_result sr = {.dirs = names, .dirs_num = 4, .params = params, .infos = infos};
    CHECK(search(&sc, &sr) == 0 && sr.hit_num == 0 && sr.set_num == 0);

    /* Create it (CREATE2 reports CREATED). */
    m.mode = 2 | 32;
    memset(&mr, 0xcc, sizeof(mr));
    CHECK(mount(&m, &mr) == 0);
    CHECK(strcmp(mr.mp.data, "/savedata0") == 0);
    CHECK(mr.status == 1);
    CHECK(mount(&m, &mr) == E_BUSY); /* already mounted */

    char path[1024];
    snprintf(path, sizeof(path), "%s/savedata/SOTC0001/sce_sys/corrupted", g_user);
    struct stat st;
    CHECK(stat(path, &st) == 0); /* crash marker while mounted read-write */

    /* The guest writes through /savedata0, which maps onto the directory. */
    snprintf(path, sizeof(path), "%s/savedata/SOTC0001/data.bin", g_user);
    write_text(path, "first");

    mount_point mp = {"/savedata0"};
    mount_info mi;
    CHECK(info(&mp, &mi) == 0 && mi.blocks == 1000 && mi.free_blocks <= 1000);
    mount_point bad = {"/savedata9"};
    CHECK(info(&bad, &mi) == E_NOT_MOUNTED);

    sd_param p = {0};
    snprintf(p.title, sizeof(p.title), "Shadow of the Colossus");
    snprintf(p.sub_title, sizeof(p.sub_title), "Colossus 3");
    p.user_param = 7;
    CHECK(setparam(&mp, 0, &p, sizeof(p)) == 0);
    CHECK(setparam(&mp, 3, "Detail text", 12) == 0);
    unsigned char png[4] = {0x89, 'P', 'N', 'G'};
    icon ic = {.buf = png, .buf_size = 4, .data_size = 4};
    CHECK(saveicon(&mp, &ic) == 0);

    /* Delete refuses while mounted; Terminate too. */
    del_param dp = {.user_id = 1, .dir = &dn};
    CHECK(del(&dp) == E_BUSY);
    CHECK(term() == E_BUSY);

    CHECK(umount_backup(&mp) == 0);
    CHECK(umount_backup(&mp) == E_NOT_FOUND);
    snprintf(path, sizeof(path), "%s/savedata/SOTC0001/sce_sys/corrupted", g_user);
    CHECK(stat(path, &st) != 0);
    snprintf(path, sizeof(path), "%s/savedata/SOTC0001/sce_backup/data.bin", g_user);
    char text[64];
    CHECK(read_text(path, text, sizeof(text)) == 5 && strcmp(text, "first") == 0);

    /* CREATE on an existing save fails; plain mount works and keeps params. */
    m.mode = 2 | 4;
    CHECK(mount(&m, &mr) == E_EXISTS);
    m.mode = 1; /* RDONLY */
    CHECK(mount(&m, &mr) == 0 && mr.status == 0);
    snprintf(path, sizeof(path), "%s/savedata/SOTC0001/sce_sys/corrupted", g_user);
    CHECK(stat(path, &st) != 0); /* no marker for read-only mounts */
    CHECK(saveicon(&mr.mp, &ic) == (int32_t)0x809F000D); /* BAD_MOUNTED */
    CHECK(umount_backup(&mr.mp) == 0);

    /* Search: finds it with params; wildcard filter. */
    memset(&sr, 0, sizeof(sr));
    sr.dirs = names;
    sr.dirs_num = 4;
    sr.params = params;
    sr.infos = infos;
    CHECK(search(&sc, &sr) == 0 && sr.hit_num == 1 && sr.set_num == 1);
    CHECK(strcmp(names[0].data, "SOTC0001") == 0);
    CHECK(strcmp(params[0].title, "Shadow of the Colossus") == 0);
    CHECK(strcmp(params[0].detail, "Detail text") == 0);
    CHECK(params[0].user_param == 7 && params[0].mtime != 0);
    CHECK(infos[0].blocks == 1000);
    dir_name pat = {"sotc%"};
    sc.dir = &pat;
    CHECK(search(&sc, &sr) == 0 && sr.hit_num == 1);
    dir_name pat2 = {"sotc_"};
    sc.dir = &pat2;
    CHECK(search(&sc, &sr) == 0 && sr.hit_num == 0);
    sc.dir = NULL;

    /* A second save to check sorting by user param, descending. */
    dir_name dn2 = {"SOTC0002"};
    mount2 m2 = {.user_id = 1, .dir = &dn2, .blocks = 96, .mode = 2 | 4};
    CHECK(mount(&m2, &mr) == 0);
    uint32_t up = 9;
    CHECK(setparam(&mr.mp, 4, &up, 4) == 0);
    CHECK(umount_backup(&mr.mp) == 0);
    sc.key = 1;   /* USER_PARAM */
    sc.order = 1; /* DESCENT */
    CHECK(search(&sc, &sr) == 0 && sr.hit_num == 2);
    CHECK(strcmp(names[0].data, "SOTC0002") == 0 && strcmp(names[1].data, "SOTC0001") == 0);
    sr.dirs_num = 1;
    CHECK(search(&sc, &sr) == 0 && sr.hit_num == 2 && sr.set_num == 1);

    /* Backup check and restore. */
    snprintf(path, sizeof(path), "%s/savedata/SOTC0001/data.bin", g_user);
    write_text(path, "second");
    sd_param bp;
    unsigned char ibuf[16];
    icon bic = {.buf = ibuf, .buf_size = sizeof(ibuf)};
    check_backup cb = {.user_id = 1, .dir = &dn, .param = &bp, .icon = &bic};
    CHECK(check(&cb) == 0);
    CHECK(strcmp(bp.title, "Shadow of the Colossus") == 0);
    CHECK(bic.data_size == 4 && memcmp(ibuf, png, 4) == 0);
    restore_backup rb = {.user_id = 1, .dir = &dn};
    CHECK(restore(&rb) == 0);
    CHECK(read_text(path, text, sizeof(text)) == 5 && strcmp(text, "first") == 0);
    dir_name none = {"NOPE"};
    rb.dir = &none;
    CHECK(restore(&rb) == E_NOT_FOUND);
    cb.dir = &none;
    CHECK(check(&cb) == E_NOT_FOUND);

    /* A crash leaves the marker: the next mount restores the backup. */
    fn1 umount_plain = (fn1)SD("sceSaveDataUmount");
    CHECK(umount_plain != NULL);
    m.mode = 2;
    CHECK(mount(&m, &mr) == 0);
    write_text(path, "torn"); /* data.bin */
    if (umount_plain) CHECK(umount_plain(&mr.mp) == 0); /* no new backup */
    char marker[1024];
    snprintf(marker, sizeof(marker), "%s/savedata/SOTC0001/sce_sys/corrupted", g_user);
    write_text(marker, ""); /* as if the runtime had crashed while mounted */
    CHECK(mount(&m, &mr) == 0);
    CHECK(read_text(path, text, sizeof(text)) == 5 && strcmp(text, "first") == 0);
    CHECK(umount_backup(&mr.mp) == 0);

    /* Delete. */
    dp.dir = &dn2;
    CHECK(del(&dp) == 0);
    snprintf(path, sizeof(path), "%s/savedata/SOTC0002", g_user);
    CHECK(stat(path, &st) != 0);
    CHECK(term() == 0);
}

/* ---- dialogs ---- */

#define CD_RUNNING 2
#define CD_FINISHED 3

typedef struct { uint64_t size; uint8_t reserved[36]; uint32_t magic; } base_param;
typedef struct {
    uint32_t button_type;
    int32_t pad;
    const char *msg;
    const void *buttons;
    char reserved[24];
} msg_user;
typedef struct {
    base_param base;
    uint64_t size;
    uint32_t mode;
    int32_t pad;
    const msg_user *user;
    const void *prog, *sys;
    int32_t user_id;
    char reserved[44];
} msg_param;
typedef struct { uint32_t mode; int32_t result; uint32_t button; char reserved[32]; } msg_result;

static void test_msgdialog(void) {
#define MD(n) hle_lookup("libSceMsgDialog", n)
    fn0 init = (fn0)MD("sceMsgDialogInitialize");
    fn1 open = (fn1)MD("sceMsgDialogOpen");
    fn0 update = (fn0)MD("sceMsgDialogUpdateStatus");
    fn1 result = (fn1)MD("sceMsgDialogGetResult");
    fn0 close = (fn0)MD("sceMsgDialogClose");
    fn0 term = (fn0)MD("sceMsgDialogTerminate");
    CHECK(init && open && update && result && close && term);
    if (!init || !open || !update || !result || !close || !term) return;

    msg_user u = {.button_type = 1 /* YESNO */, .msg = "Overwrite?"};
    msg_param p = {.base = {.size = sizeof(base_param)}, .size = sizeof(p), .mode = 1, .user = &u};
    CHECK(open(&p) == (int32_t)0x80B80006); /* not initialized */
    CHECK(init() == 0);
    CHECK(init() == (int32_t)0x80B80004);
    CHECK(open(&p) == 0);
    msg_result r = {0};
    CHECK(result(&r) == (int32_t)0x80B80005); /* not finished */
    CHECK(update() == CD_RUNNING);
    CHECK(update() == CD_FINISHED);
    CHECK(result(&r) == 0 && r.result == 0 && r.button == 1 && r.mode == 1);
    CHECK(close() == (int32_t)0x80B8000B);

    /* A "please wait" dialog stays up until the game closes it. */
    u.button_type = 5; /* WAIT */
    CHECK(open(&p) == 0);
    CHECK(update() == CD_RUNNING && update() == CD_RUNNING);
    CHECK(close() == 0);
    CHECK(update() == CD_FINISHED);
    CHECK(result(&r) == 0 && r.button == 0);
    CHECK(term() == 0);
    CHECK(term() == (int32_t)0x80B80003);
#undef MD
}

static void test_errordialog(void) {
#define ED(n) hle_lookup("libSceErrorDialog", n)
    fn0 init = (fn0)ED("sceErrorDialogInitialize");
    fn1 open = (fn1)ED("sceErrorDialogOpen");
    fn0 update = (fn0)ED("sceErrorDialogUpdateStatus");
    fn0 close = (fn0)ED("sceErrorDialogClose");
    fn0 term = (fn0)ED("sceErrorDialogTerminate");
    CHECK(init && open && update && close && term);
    if (!init || !open || !update || !close || !term) return;
    int32_t ep[4] = {16, (int32_t)0x80020002, 1, 0};
    CHECK(init() == 0);
    CHECK(open(ep) == 0);
    CHECK(update() == CD_RUNNING);
    CHECK(update() == CD_FINISHED);
    CHECK(close() == (int32_t)0x80B8000B);
    CHECK(term() == 0);
#undef ED
}

typedef struct {
    int32_t user_id, pad;
    const void *title_id;
    const dir_name *dirs;
    uint32_t dirs_num;
    int32_t pad2;
    const void *new_item;
    uint32_t focus_pos;
    int32_t pad3;
    const dir_name *focus_dir;
    uint32_t style;
    uint8_t reserved[36];
} sdd_items;
typedef struct { uint32_t msg_type; int32_t pad; uint64_t value; uint8_t reserved[32]; } sdd_sys;
typedef struct {
    base_param base;
    int32_t size;
    uint32_t mode, type;
    int32_t pad;
    const void *anim;
    const sdd_items *items;
    const void *user;
    const sdd_sys *sys;
    const void *err, *bar;
    void *user_data;
    const void *option;
    uint8_t reserved[24];
} sdd_param;
typedef struct {
    uint32_t mode;
    int32_t result;
    uint32_t button;
    int32_t pad;
    dir_name *dir;
    sd_param *param;
    void *user_data;
    uint8_t reserved[32];
} sdd_result;

static void test_savedatadialog(void) {
#define DD(n) hle_lookup("libSceSaveDataDialog", n)
    fn0 init = (fn0)DD("sceSaveDataDialogInitialize");
    fn0 ready = (fn0)DD("sceSaveDataDialogIsReadyToDisplay");
    fn1 open = (fn1)DD("sceSaveDataDialogOpen");
    fn0 update = (fn0)DD("sceSaveDataDialogUpdateStatus");
    fn0 status = (fn0)DD("sceSaveDataDialogGetStatus");
    fn1 result = (fn1)DD("sceSaveDataDialogGetResult");
    fn0 close = (fn0)DD("sceSaveDataDialogClose");
    fn0 term = (fn0)DD("sceSaveDataDialogTerminate");
    void *setval = DD("sceSaveDataDialogProgressBarSetValue");
    CHECK(init && ready && open && update && status && result && close && term && setval);
    if (!init || !ready || !open || !update || !status || !result || !close || !term) return;

    /* SOTC0001 exists (from test_savedata); a load list picks it. */
    dir_name listed[2] = {{"MISSING"}, {"SOTC0001"}};
    sdd_items items = {.user_id = 1, .dirs = listed, .dirs_num = 2};
    sdd_param p = {.base = {.size = sizeof(base_param)}, .size = sizeof(p), .mode = 1 /* LIST */,
                   .type = 2 /* LOAD */, .items = &items, .user_data = (void *)0x1234};
    CHECK(init() == 0);
    CHECK(ready() == 1);
    CHECK(open(&p) == 0);
    CHECK(status() == CD_RUNNING);
    CHECK(update() == CD_RUNNING && update() == CD_FINISHED);
    dir_name out;
    sd_param op;
    sdd_result r = {.dir = &out, .param = &op};
    CHECK(result(&r) == 0);
    CHECK(r.result == 0 && r.mode == 1 && r.user_data == (void *)0x1234);
    CHECK(strcmp(out.data, "SOTC0001") == 0);
    CHECK(strcmp(op.title, "Shadow of the Colossus") == 0);

    /* Nothing to load: the list is cancelled. */
    items.dirs_num = 1;
    CHECK(open(&p) == 0);
    update();
    CHECK(update() == CD_FINISHED);
    CHECK(result(&r) == 0 && r.result == 1);

    /* "Saving..." system message stays until closed. */
    sdd_sys sys = {.msg_type = 5 /* PROGRESS */};
    p.mode = 3;
    p.type = 1;
    p.sys = &sys;
    CHECK(open(&p) == 0);
    CHECK(update() == CD_RUNNING && update() == CD_RUNNING);
    CHECK(close() == 0 && status() == CD_FINISHED);

    /* Confirmation: answered Yes. */
    sys.msg_type = 2; /* CONFIRM */
    CHECK(open(&p) == 0);
    update();
    CHECK(update() == CD_FINISHED);
    CHECK(result(&r) == 0 && r.result == 0 && r.button == 1);
    CHECK(term() == 0);
#undef DD
}

int main(void) {
    log_init();
    hle_init();
    snprintf(g_user, sizeof(g_user), "test_savedata_%d", (int)getpid());
#ifdef _WIN32
    _putenv_s("SOTC_USER_DIR", g_user);
#else
    setenv("SOTC_USER_DIR", g_user, 1);
#endif
    test_savedata();
    test_msgdialog();
    test_errordialog();
    test_savedatadialog();

    /* Clean up the test directory. */
    dir_name d1 = {"SOTC0001"};
    fn1 init = (fn1)SD("sceSaveDataInitialize3");
    fn1 del = (fn1)SD("sceSaveDataDelete");
    fn0 term = (fn0)SD("sceSaveDataTerminate");
    if (init && del && term) {
        del_param dp = {.user_id = 1, .dir = &d1};
        init(NULL);
        del(&dp);
        term();
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/savedata", g_user);
#ifdef _WIN32
    _rmdir(path);
    _rmdir(g_user);
#else
    rmdir(path);
    rmdir(g_user);
#endif

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("savedata tests passed\n");
    return 0;
}
