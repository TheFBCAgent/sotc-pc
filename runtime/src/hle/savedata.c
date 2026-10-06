/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceSaveData: save directories on the host.
 *
 * Semantics ported from shadPS4 (src/core/libraries/save_data/savedata.cpp,
 * save_instance.cpp, save_backup.cpp; GPL-2.0, commit 14ada48).
 *
 * Layout: SOTC_USER_DIR/savedata/<dirName>/
 *     <game files>          what the game writes through /savedataN
 *     sce_sys/param.bin     our metadata: magic, max blocks, SceSaveDataParam
 *     sce_sys/icon0.png     sceSaveDataSaveIcon / COPY_ICON
 *     sce_sys/corrupted     present while mounted read-write (crash marker)
 *     sce_backup/           copy made by sceSaveDataUmountWithBackup
 * A directory counts as a save when it has a sce_sys subdirectory.
 * Mounted slot N is exposed to the guest file API as /savedataN via
 * hle_file_mount(). Only one user and one title id are supported: titleId
 * and userId in requests are ignored apart from parameter checks.
 * Backups run synchronously (SotC saves are small). The behaviour follows
 * firmware >= 4.50 where shadPS4 distinguishes SDK versions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <windows.h>
#define h_mkdir(p) _mkdir(p)
#define h_rmdir _rmdir
#define h_unlink _unlink
typedef struct _stat64 h_stat_t;
#define h_stat _stat64
#else
#include <dirent.h>
#include <unistd.h>
#define h_mkdir(p) mkdir(p, 0755)
#define h_rmdir rmdir
#define h_unlink unlink
typedef struct stat h_stat_t;
#define h_stat stat
#endif

#include "../log.h"
#include "hle.h"

/* Error codes (shadPS4 savedata_error.h). */
#define SD_ERR_PARAMETER ((int32_t)0x809F0000)
#define SD_ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define SD_ERR_BUSY ((int32_t)0x809F0003)
#define SD_ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define SD_ERR_EXISTS ((int32_t)0x809F0007)
#define SD_ERR_NOT_FOUND ((int32_t)0x809F0008)
#define SD_ERR_INTERNAL ((int32_t)0x809F000B)
#define SD_ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define SD_ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define SD_ERR_INVALID_LOGIN_USER ((int32_t)0x809F0011)

enum {
    SD_MOUNT_RDONLY = 1 << 0, SD_MOUNT_RDWR = 1 << 1, SD_MOUNT_CREATE = 1 << 2,
    SD_MOUNT_DESTRUCT_OFF = 1 << 3, SD_MOUNT_COPY_ICON = 1 << 4, SD_MOUNT_CREATE2 = 1 << 5,
};
enum { SD_STATUS_NOTHING = 0, SD_STATUS_CREATED = 1 };
enum {
    SD_PARAM_ALL = 0, SD_PARAM_TITLE = 1, SD_PARAM_SUB_TITLE = 2, SD_PARAM_DETAIL = 3,
    SD_PARAM_USER_PARAM = 4,
};
enum { SD_SORT_DIRNAME = 0, SD_SORT_USER_PARAM = 1, SD_SORT_BLOCKS = 2, SD_SORT_MTIME = 3,
       SD_SORT_FREE_BLOCKS = 5 };

#define SD_BLOCK_SIZE 32768ull
#define SD_BLOCKS_MIN 96
#define SD_BLOCKS_MAX 32768
#define SD_SLOTS 16
#define SD_DIRNAME_MAX 32
#define SD_MOUNTPOINT_MAX 16
#define SD_PATH_MAX 1024
#define SD_SEARCH_MAX 1024

/* Guest structures (layouts from shadPS4 savedata.h / savedata.cpp). */
typedef struct { char data[10]; char pad[6]; } sd_title_id;
typedef struct { char data[SD_DIRNAME_MAX]; } sd_dir_name;
typedef struct { char data[SD_MOUNTPOINT_MAX]; } sd_mount_point;

typedef struct {
    char title[128];
    char sub_title[128];
    char detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} sd_param;
_Static_assert(sizeof(sd_param) == 1328, "SceSaveDataParam");

typedef struct {
    int32_t user_id;
    int32_t pad;
    const sd_dir_name *dir_name;
    uint64_t blocks;
    uint32_t mount_mode;
    uint8_t reserved[32];
    int32_t pad2;
} sd_mount2;
_Static_assert(sizeof(sd_mount2) == 64, "SceSaveDataMount2");

typedef struct {
    sd_mount_point mount_point;
    uint64_t required_blocks;
    uint32_t unused;
    uint32_t mount_status;
    uint8_t reserved[28];
    int32_t pad;
} sd_mount_result;
_Static_assert(sizeof(sd_mount_result) == 64, "SceSaveDataMountResult");

typedef struct {
    uint64_t blocks;
    uint64_t free_blocks;
    uint8_t reserved[32];
} sd_mount_info;

typedef struct {
    int32_t user_id;
    int32_t pad;
    const sd_title_id *title_id;
    const sd_dir_name *dir_name;
    uint32_t unused;
    uint8_t reserved[32];
    int32_t pad2;
} sd_delete;

typedef struct {
    void *buf;
    uint64_t buf_size;
    uint64_t data_size;
    uint8_t reserved[32];
} sd_icon;

typedef struct {
    int32_t user_id;
    int32_t pad;
    const sd_title_id *title_id;
    const sd_dir_name *dir_name;
    sd_param *param;
    sd_icon *icon;
    uint8_t reserved[32];
} sd_check_backup;

typedef struct {
    int32_t user_id;
    int32_t pad;
    const sd_title_id *title_id;
    const sd_dir_name *dir_name;
    const void *fingerprint;
    uint32_t unused;
    uint8_t reserved[32];
    int32_t pad2;
} sd_restore_backup;

typedef struct {
    int32_t user_id;
    int32_t pad;
    const sd_title_id *title_id;
    const sd_dir_name *dir_name;
    uint32_t key;
    uint32_t order;
    uint8_t reserved[32];
} sd_search_cond;

typedef struct {
    uint64_t blocks;
    uint64_t free_blocks;
    uint8_t reserved[32];
} sd_search_info;

typedef struct {
    uint32_t hit_num;
    int32_t pad;
    sd_dir_name *dir_names;
    uint32_t dir_names_num;
    uint32_t set_num;
    sd_param *params;
    sd_search_info *infos;
    uint8_t reserved[12];
    int32_t pad2;
} sd_search_result;
_Static_assert(sizeof(sd_search_result) == 56, "SceSaveDataDirNameSearchResult");

/* sce_sys/param.bin */
#define SD_PARAM_MAGIC "SOTCSDP1"
typedef struct {
    char magic[8];
    uint64_t blocks;
    sd_param param;
} sd_param_file;

typedef struct {
    bool used;
    bool read_only;
    char dir_name[SD_DIRNAME_MAX];
    char mount_point[SD_MOUNTPOINT_MAX];
    char path[SD_PATH_MAX];
    uint64_t blocks;
    sd_param param;
} sd_slot;

static plat_mutex *g_lock;
static bool g_initialized;
static char g_root[SD_PATH_MAX];
static sd_slot g_slots[SD_SLOTS];

__attribute__((constructor)) static void savedata_lock_init(void) { g_lock = plat_mutex_new(); }

/* ---- host filesystem helpers ---- */

static bool path_exists(const char *p) {
    h_stat_t st;
    return h_stat(p, &st) == 0;
}

static bool path_is_dir(const char *p) {
    h_stat_t st;
    return h_stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static void make_dirs(const char *path) {
    char buf[SD_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            h_mkdir(buf);
            *p = c;
        }
    h_mkdir(buf);
}

typedef void (*dir_cb)(const char *name, bool is_dir, void *ctx);

/* Calls cb for each entry except . and .. */
static bool list_dir(const char *path, dir_cb cb, void *ctx) {
#ifdef _WIN32
    char pattern[SD_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        cb(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[SD_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        cb(e->d_name, path_is_dir(full), ctx);
    }
    closedir(d);
    return true;
#endif
}

/* A small growable list of names, so callbacks never recurse while a
 * directory handle is open. */
typedef struct {
    char (*names)[260];
    bool *is_dir;
    size_t count, cap;
    const char *skip; /* entry name to leave out, or NULL */
} name_list;

static void name_list_add(const char *name, bool is_dir, void *ctx) {
    name_list *l = ctx;
    if (l->skip && strcmp(name, l->skip) == 0) return;
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        void *n = realloc(l->names, cap * sizeof(*l->names));
        void *d = realloc(l->is_dir, cap * sizeof(*l->is_dir));
        if (!n || !d) {
            if (n) l->names = n;
            if (d) l->is_dir = d;
            return;
        }
        l->names = n;
        l->is_dir = d;
        l->cap = cap;
    }
    snprintf(l->names[l->count], sizeof(l->names[0]), "%s", name);
    l->is_dir[l->count++] = is_dir;
}

static void name_list_free(name_list *l) {
    free(l->names);
    free(l->is_dir);
    memset(l, 0, sizeof(*l));
}

static void remove_tree(const char *path) {
    if (!path_is_dir(path)) {
        h_unlink(path);
        return;
    }
    name_list l = {0};
    list_dir(path, name_list_add, &l);
    for (size_t i = 0; i < l.count; i++) {
        char child[SD_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, l.names[i]);
        if (l.is_dir[i]) remove_tree(child);
        else h_unlink(child);
    }
    name_list_free(&l);
    h_rmdir(path);
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char *buf = malloc(65536);
    bool ok = buf != NULL;
    size_t n;
    while (ok && (n = fread(buf, 1, 65536, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    free(buf);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

/* Copy the contents of `from` into `to` (created), leaving out `skip`. */
static bool copy_tree(const char *from, const char *to, const char *skip) {
    make_dirs(to);
    name_list l = {.skip = skip};
    if (!list_dir(from, name_list_add, &l)) return false;
    bool ok = true;
    for (size_t i = 0; i < l.count && ok; i++) {
        char src[SD_PATH_MAX], dst[SD_PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", from, l.names[i]);
        snprintf(dst, sizeof(dst), "%s/%s", to, l.names[i]);
        ok = l.is_dir[i] ? copy_tree(src, dst, NULL) : copy_file(src, dst);
    }
    name_list_free(&l);
    return ok;
}

static uint64_t tree_size(const char *path) {
    h_stat_t st;
    if (h_stat(path, &st) != 0) return 0;
    if ((st.st_mode & S_IFMT) != S_IFDIR) return (uint64_t)st.st_size;
    name_list l = {0};
    list_dir(path, name_list_add, &l);
    uint64_t total = 0;
    for (size_t i = 0; i < l.count; i++) {
        char child[SD_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, l.names[i]);
        total += tree_size(child);
    }
    name_list_free(&l);
    return total;
}

static bool write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

/* ---- save directory helpers ---- */

static uint64_t clamp_blocks(uint64_t b) {
    return b < SD_BLOCKS_MIN ? SD_BLOCKS_MIN : b > SD_BLOCKS_MAX ? SD_BLOCKS_MAX : b;
}

static void ensure_root(void) {
    if (g_root[0]) return;
    const char *user = getenv("SOTC_USER_DIR");
    if (!user || !*user) user = "user";
    snprintf(g_root, sizeof(g_root), "%s/savedata", user);
    make_dirs(g_root);
}

/* Copies a guest dir name (char[32], maybe unterminated) and validates it. */
static bool get_dir_name(const sd_dir_name *dn, char out[SD_DIRNAME_MAX + 1]) {
    if (!dn) return false;
    size_t n = 0;
    while (n < SD_DIRNAME_MAX && dn->data[n]) n++;
    memcpy(out, dn->data, n);
    out[n] = 0;
    if (n == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) return false;
    for (size_t i = 0; i < n; i++)
        if (out[i] == '/' || out[i] == '\\' || out[i] == ':') return false;
    return true;
}

static void dir_path(const char *dir_name, char *out, size_t size) {
    ensure_root();
    snprintf(out, size, "%s/%s", g_root, dir_name);
}

static void default_param(sd_param *p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->title, sizeof(p->title), "Saved Data");
}

/* Reads <dir>/sce_sys/param.bin. */
static bool read_param_file(const char *dir, sd_param *param, uint64_t *blocks) {
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/sce_sys/param.bin", dir);
    sd_param_file pf;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(&pf, 1, sizeof(pf), f) == sizeof(pf) &&
              memcmp(pf.magic, SD_PARAM_MAGIC, 8) == 0;
    fclose(f);
    if (!ok) return false;
    if (param) *param = pf.param;
    if (blocks) *blocks = clamp_blocks(pf.blocks);
    return true;
}

static bool write_param_file(const char *dir, const sd_param *param, uint64_t blocks) {
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/sce_sys", dir);
    make_dirs(path);
    snprintf(path, sizeof(path), "%s/sce_sys/param.bin", dir);
    sd_param_file pf;
    memset(&pf, 0, sizeof(pf));
    memcpy(pf.magic, SD_PARAM_MAGIC, 8);
    pf.blocks = blocks;
    pf.param = *param;
    return write_file(path, &pf, sizeof(pf));
}

/* Metadata for an existing save: param.bin, or defaults if it's missing. */
static void load_meta(const char *dir, sd_param *param, uint64_t *blocks) {
    if (read_param_file(dir, param, blocks)) return;
    default_param(param);
    *blocks = SD_BLOCKS_MAX;
}

static bool save_exists(const char *dir) {
    char sys[SD_PATH_MAX];
    snprintf(sys, sizeof(sys), "%s/sce_sys", dir);
    return path_is_dir(sys);
}

/* Restore <dir>/sce_backup over <dir>. */
static bool restore_backup(const char *dir) {
    char backup[SD_PATH_MAX];
    snprintf(backup, sizeof(backup), "%s/sce_backup", dir);
    if (!path_is_dir(backup)) return false;
    log_info("savedata: restoring backup of %s", dir);
    name_list l = {.skip = "sce_backup"};
    list_dir(dir, name_list_add, &l);
    for (size_t i = 0; i < l.count; i++) {
        char child[SD_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", dir, l.names[i]);
        remove_tree(child);
    }
    name_list_free(&l);
    return copy_tree(backup, dir, NULL);
}

/* Copy <dir> into <dir>/sce_backup (through a temporary directory, so an
 * interrupted backup leaves the previous one intact). */
static bool make_backup(const char *dir) {
    char backup[SD_PATH_MAX], tmp[SD_PATH_MAX], old[SD_PATH_MAX];
    snprintf(backup, sizeof(backup), "%s/sce_backup", dir);
    snprintf(tmp, sizeof(tmp), "%s/sce_backup_tmp", dir);
    snprintf(old, sizeof(old), "%s/sce_backup_old", dir);
    remove_tree(tmp);
    remove_tree(old);
    name_list l = {.skip = "sce_backup"};
    list_dir(dir, name_list_add, &l);
    make_dirs(tmp);
    bool ok = true;
    for (size_t i = 0; i < l.count && ok; i++) {
        if (strcmp(l.names[i], "sce_backup_tmp") == 0) continue;
        char src[SD_PATH_MAX], dst[SD_PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", dir, l.names[i]);
        snprintf(dst, sizeof(dst), "%s/%s", tmp, l.names[i]);
        ok = l.is_dir[i] ? copy_tree(src, dst, NULL) : copy_file(src, dst);
    }
    name_list_free(&l);
    if (!ok) {
        remove_tree(tmp);
        return false;
    }
    bool had = path_is_dir(backup);
    if (had && rename(backup, old) != 0) {
        remove_tree(tmp);
        return false;
    }
    if (rename(tmp, backup) != 0) {
        if (had) rename(old, backup);
        remove_tree(tmp);
        return false;
    }
    if (had) remove_tree(old);
    return true;
}

static sd_slot *find_mount_point(const sd_mount_point *mp) {
    if (!mp) return NULL;
    for (int i = 0; i < SD_SLOTS; i++)
        if (g_slots[i].used && strncmp(g_slots[i].mount_point, mp->data, SD_MOUNTPOINT_MAX) == 0)
            return &g_slots[i];
    return NULL;
}

static bool dir_is_mounted(const char *dir_name) {
    for (int i = 0; i < SD_SLOTS; i++)
        if (g_slots[i].used && strcmp(g_slots[i].dir_name, dir_name) == 0) return true;
    return false;
}

/* For dialogs.c (same workstream): does a save exist, and its param. */
bool hle_savedata_dir_exists(const char *dir_name) {
    sd_dir_name dn;
    char name[SD_DIRNAME_MAX + 1], dir[SD_PATH_MAX];
    snprintf(dn.data, sizeof(dn.data), "%s", dir_name ? dir_name : "");
    if (!get_dir_name(&dn, name)) return false;
    dir_path(name, dir, sizeof(dir));
    return save_exists(dir);
}

bool hle_savedata_read_param(const char *dir_name, void *param_out) {
    if (!hle_savedata_dir_exists(dir_name)) return false;
    char dir[SD_PATH_MAX];
    uint64_t blocks;
    dir_path(dir_name, dir, sizeof(dir));
    load_meta(dir, param_out, &blocks);
    return true;
}

/* ---- API ---- */

static SYSV int32_t ps4_sceSaveDataInitialize3(const void *param) {
    (void)param;
    plat_mutex_lock(g_lock);
    ensure_root();
    g_initialized = true;
    plat_mutex_unlock(g_lock);
    log_info("savedata: initialized, root %s", g_root);
    return SCE_OK;
}

static SYSV int32_t ps4_sceSaveDataTerminate(void) {
    plat_mutex_lock(g_lock);
    int32_t ret = SCE_OK;
    if (!g_initialized) ret = SD_ERR_NOT_INITIALIZED;
    for (int i = 0; ret == SCE_OK && i < SD_SLOTS; i++)
        if (g_slots[i].used) ret = SD_ERR_BUSY;
    if (ret == SCE_OK) g_initialized = false;
    plat_mutex_unlock(g_lock);
    return ret;
}

static SYSV int32_t ps4_sceSaveDataMount2(const sd_mount2 *m, sd_mount_result *res) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    char name[SD_DIRNAME_MAX + 1], dir[SD_PATH_MAX];
    if (!m || !res || !get_dir_name(m->dir_name, name)) return SD_ERR_PARAMETER;
    if (m->user_id < 0) return SD_ERR_INVALID_LOGIN_USER;
    const uint32_t mode = m->mount_mode;
    const bool ro = (mode & SD_MOUNT_RDONLY) != 0;
    const bool create = (mode & SD_MOUNT_CREATE) != 0;
    const bool create2 = (mode & SD_MOUNT_CREATE2) != 0;
    const bool ignore_corrupt = (mode & SD_MOUNT_DESTRUCT_OFF) != 0;
    log_debug("savedata: Mount2(%s, mode %#x, blocks %llu)", name, mode,
              (unsigned long long)m->blocks);
    if (create && create2) return SD_ERR_PARAMETER;

    plat_mutex_lock(g_lock);
    int32_t ret = SCE_OK;
    sd_slot *slot = NULL;
    int slot_num = -1;
    for (int i = 0; i < SD_SLOTS; i++) {
        if (g_slots[i].used && strcmp(g_slots[i].dir_name, name) == 0) {
            ret = SD_ERR_BUSY;
            break;
        }
        if (!g_slots[i].used && slot_num < 0) slot_num = i;
    }
    if (ret == SCE_OK && slot_num < 0) ret = SD_ERR_MOUNT_FULL;
    dir_path(name, dir, sizeof(dir));
    const bool exists = save_exists(dir);
    if (ret == SCE_OK && !exists && !create && !create2) ret = SD_ERR_NOT_FOUND;
    if (ret == SCE_OK && exists && create) ret = SD_ERR_EXISTS;
    if (ret != SCE_OK) {
        plat_mutex_unlock(g_lock);
        log_debug("savedata: Mount2(%s) -> %#x", name, (uint32_t)ret);
        return ret;
    }

    slot = &g_slots[slot_num];
    memset(slot, 0, sizeof(*slot));
    char marker[SD_PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/sce_sys/corrupted", dir);
    if (!exists) {
        default_param(&slot->param);
        slot->blocks = clamp_blocks(m->blocks);
        if (!write_param_file(dir, &slot->param, slot->blocks)) {
            plat_mutex_unlock(g_lock);
            log_error("savedata: cannot create %s", dir);
            return SD_ERR_INTERNAL;
        }
        if (mode & SD_MOUNT_COPY_ICON) {
            const char *game = getenv("SOTC_GAME_DIR");
            if (game) {
                char src[SD_PATH_MAX], dst[SD_PATH_MAX];
                snprintf(src, sizeof(src), "%s/sce_sys/save_data.png", game);
                snprintf(dst, sizeof(dst), "%s/sce_sys/icon0.png", dir);
                if (path_exists(src)) copy_file(src, dst);
            }
        }
        log_info("savedata: created %s", dir);
    } else {
        /* A marker left behind means the last read-write mount never
         * unmounted (crash). shadPS4 restores the backup if there is one and
         * otherwise mounts the data as is. */
        if (!ignore_corrupt && path_exists(marker)) {
            log_warn("savedata: %s was not unmounted cleanly", name);
            restore_backup(dir);
        }
        load_meta(dir, &slot->param, &slot->blocks);
    }
    if (!ro && !ignore_corrupt) write_file(marker, "", 0);

    slot->used = true;
    slot->read_only = ro;
    snprintf(slot->dir_name, sizeof(slot->dir_name), "%s", name);
    snprintf(slot->mount_point, sizeof(slot->mount_point), "/savedata%d", slot_num);
    snprintf(slot->path, sizeof(slot->path), "%s", dir);
    hle_file_mount(slot->mount_point, dir);

    memset(res, 0, sizeof(*res));
    memcpy(res->mount_point.data, slot->mount_point, sizeof(slot->mount_point));
    res->mount_status = create2 && !exists ? SD_STATUS_CREATED : SD_STATUS_NOTHING;
    plat_mutex_unlock(g_lock);
    log_info("savedata: mounted %s at %s%s", name, slot->mount_point, ro ? " (read-only)" : "");
    return SCE_OK;
}

static int32_t sd_umount(const sd_mount_point *mp, bool backup) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    if (!mp) return SD_ERR_PARAMETER;
    plat_mutex_lock(g_lock);
    sd_slot *s = find_mount_point(mp);
    if (!s) {
        plat_mutex_unlock(g_lock);
        return SD_ERR_NOT_FOUND;
    }
    int32_t ret = SCE_OK;
    if (!s->read_only) {
        s->param.mtime = (int64_t)time(NULL);
        if (!write_param_file(s->path, &s->param, s->blocks)) ret = SD_ERR_INTERNAL;
    }
    char marker[SD_PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/sce_sys/corrupted", s->path);
    h_unlink(marker);
    hle_file_unmount(s->mount_point);
    if (backup && ret == SCE_OK && !make_backup(s->path)) {
        log_warn("savedata: backup of %s failed", s->dir_name);
    }
    log_info("savedata: unmounted %s%s", s->mount_point, backup ? " (with backup)" : "");
    memset(s, 0, sizeof(*s));
    plat_mutex_unlock(g_lock);
    return ret;
}

static SYSV int32_t ps4_sceSaveDataUmount(const sd_mount_point *mp) { return sd_umount(mp, false); }

static SYSV int32_t ps4_sceSaveDataUmountWithBackup(const sd_mount_point *mp) {
    return sd_umount(mp, true);
}

static SYSV int32_t ps4_sceSaveDataGetMountInfo(const sd_mount_point *mp, sd_mount_info *info) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    if (!mp || !info) return SD_ERR_PARAMETER;
    plat_mutex_lock(g_lock);
    sd_slot *s = find_mount_point(mp);
    if (s) {
        uint64_t used = tree_size(s->path) / SD_BLOCK_SIZE;
        info->blocks = s->blocks;
        info->free_blocks = used < s->blocks ? s->blocks - used : 0;
    }
    plat_mutex_unlock(g_lock);
    return s ? SCE_OK : SD_ERR_NOT_MOUNTED;
}

static void copy_str(char *dst, size_t size, const void *src) {
    snprintf(dst, size, "%s", (const char *)src);
}

static SYSV int32_t ps4_sceSaveDataSetParam(const sd_mount_point *mp, uint32_t type,
                                            const void *buf, uint64_t size) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    if (type > SD_PARAM_USER_PARAM || !mp || !buf) return SD_ERR_PARAMETER;
    if (type == SD_PARAM_ALL && size < sizeof(sd_param)) return SD_ERR_PARAMETER;
    plat_mutex_lock(g_lock);
    sd_slot *s = find_mount_point(mp);
    if (s) {
        switch (type) {
        case SD_PARAM_ALL: {
            const sd_param *p = buf;
            copy_str(s->param.title, sizeof(s->param.title), p->title);
            copy_str(s->param.sub_title, sizeof(s->param.sub_title), p->sub_title);
            copy_str(s->param.detail, sizeof(s->param.detail), p->detail);
            s->param.user_param = p->user_param;
            break;
        }
        case SD_PARAM_TITLE: copy_str(s->param.title, sizeof(s->param.title), buf); break;
        case SD_PARAM_SUB_TITLE:
            copy_str(s->param.sub_title, sizeof(s->param.sub_title), buf);
            break;
        case SD_PARAM_DETAIL: copy_str(s->param.detail, sizeof(s->param.detail), buf); break;
        case SD_PARAM_USER_PARAM: s->param.user_param = *(const uint32_t *)buf; break;
        }
        log_debug("savedata: SetParam(%s, %u) title '%s'", s->dir_name, type, s->param.title);
    }
    plat_mutex_unlock(g_lock);
    return s ? SCE_OK : SD_ERR_NOT_MOUNTED;
}

static SYSV int32_t ps4_sceSaveDataSaveIcon(const sd_mount_point *mp, const sd_icon *icon) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    if (!mp || !icon || !icon->buf) return SD_ERR_PARAMETER;
    plat_mutex_lock(g_lock);
    int32_t ret = SCE_OK;
    sd_slot *s = find_mount_point(mp);
    if (!s) ret = SD_ERR_NOT_MOUNTED;
    else if (s->read_only) ret = SD_ERR_BAD_MOUNTED;
    else {
        char path[SD_PATH_MAX];
        snprintf(path, sizeof(path), "%s/sce_sys/icon0.png", s->path);
        uint64_t n = icon->buf_size < icon->data_size ? icon->buf_size : icon->data_size;
        if (!write_file(path, icon->buf, (size_t)n)) ret = SD_ERR_INTERNAL;
    }
    plat_mutex_unlock(g_lock);
    return ret;
}

static SYSV int32_t ps4_sceSaveDataDelete(const sd_delete *del) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    char name[SD_DIRNAME_MAX + 1], dir[SD_PATH_MAX];
    if (!del || !get_dir_name(del->dir_name, name)) return SD_ERR_PARAMETER;
    plat_mutex_lock(g_lock);
    int32_t ret = SCE_OK;
    if (dir_is_mounted(name)) ret = SD_ERR_BUSY;
    else {
        dir_path(name, dir, sizeof(dir));
        if (path_exists(dir)) {
            remove_tree(dir);
            if (path_exists(dir)) ret = SD_ERR_INTERNAL;
            else log_info("savedata: deleted %s", name);
        }
    }
    plat_mutex_unlock(g_lock);
    return ret;
}

/* Wildcards: % = any run (including empty), _ = one character. */
static bool match(const char *s, const char *p) {
    for (; *p; p++, s++) {
        if (*p == '%') {
            for (const char *t = s;; t++) {
                if (match(t, p + 1)) return true;
                if (!*t) return false;
            }
        }
        if (!*s) return false;
        if (*p != '_' && *p != *s) return false;
    }
    return *s == 0;
}

static void lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

typedef struct {
    char name[SD_DIRNAME_MAX + 1];
    sd_param param;
    uint64_t blocks, free_blocks;
} search_entry;

static uint32_t g_sort_key;
static int search_cmp(const void *a, const void *b) {
    const search_entry *x = a, *y = b;
    uint64_t kx = 0, ky = 0;
    switch (g_sort_key) {
    case SD_SORT_USER_PARAM: kx = x->param.user_param; ky = y->param.user_param; break;
    case SD_SORT_BLOCKS: kx = x->blocks; ky = y->blocks; break;
    case SD_SORT_MTIME: kx = (uint64_t)x->param.mtime; ky = (uint64_t)y->param.mtime; break;
    case SD_SORT_FREE_BLOCKS: kx = x->free_blocks; ky = y->free_blocks; break;
    default: break;
    }
    if (kx != ky) return kx < ky ? -1 : 1;
    return strcmp(x->name, y->name); /* deterministic (qsort isn't stable) */
}

static SYSV int32_t ps4_sceSaveDataDirNameSearch(const sd_search_cond *cond,
                                                 sd_search_result *res) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    if (!cond || !res || cond->key > SD_SORT_FREE_BLOCKS || cond->key == 4 || cond->order > 1)
        return SD_ERR_PARAMETER;
    char pattern[SD_DIRNAME_MAX + 1] = "";
    if (cond->dir_name) {
        size_t n = 0;
        while (n < SD_DIRNAME_MAX && cond->dir_name->data[n]) n++;
        memcpy(pattern, cond->dir_name->data, n);
        pattern[n] = 0;
        lower(pattern);
    }

    plat_mutex_lock(g_lock);
    ensure_root();
    name_list l = {0};
    list_dir(g_root, name_list_add, &l);
    search_entry *entries = calloc(l.count ? l.count : 1, sizeof(*entries));
    size_t count = 0;
    for (size_t i = 0; entries && i < l.count && count < SD_SEARCH_MAX; i++) {
        const char *n = l.names[i];
        if (!l.is_dir[i] || strncmp(n, "sce_", 4) == 0 || strlen(n) > SD_DIRNAME_MAX) continue;
        char dir[SD_PATH_MAX], low[SD_DIRNAME_MAX + 1];
        snprintf(dir, sizeof(dir), "%s/%s", g_root, n);
        if (!save_exists(dir)) continue;
        snprintf(low, sizeof(low), "%s", n);
        lower(low);
        if (pattern[0] && !match(low, pattern)) continue;
        search_entry *e = &entries[count++];
        snprintf(e->name, sizeof(e->name), "%s", n);
        load_meta(dir, &e->param, &e->blocks);
        uint64_t used = tree_size(dir) / SD_BLOCK_SIZE;
        e->free_blocks = used < e->blocks ? e->blocks - used : 0;
    }
    name_list_free(&l);
    plat_mutex_unlock(g_lock);
    if (!entries) return SD_ERR_INTERNAL;

    g_sort_key = cond->key; /* only read inside qsort below; calls are rare */
    qsort(entries, count, sizeof(*entries), search_cmp);
    if (cond->order == 1)
        for (size_t i = 0; i < count / 2; i++) {
            search_entry t = entries[i];
            entries[i] = entries[count - 1 - i];
            entries[count - 1 - i] = t;
        }

    size_t n = count < res->dir_names_num ? count : res->dir_names_num;
    if (!res->dir_names) n = 0;
    res->hit_num = (uint32_t)count;
    res->set_num = (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        memset(&res->dir_names[i], 0, sizeof(res->dir_names[i]));
        memcpy(res->dir_names[i].data, entries[i].name, strlen(entries[i].name));
        if (res->params) res->params[i] = entries[i].param;
        if (res->infos) {
            memset(&res->infos[i], 0, sizeof(res->infos[i]));
            res->infos[i].blocks = entries[i].blocks;
            res->infos[i].free_blocks = entries[i].free_blocks;
        }
    }
    free(entries);
    log_debug("savedata: DirNameSearch('%s') -> %u hit(s)", pattern, res->hit_num);
    return SCE_OK;
}

static SYSV int32_t ps4_sceSaveDataCheckBackupData(const sd_check_backup *check) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    char name[SD_DIRNAME_MAX + 1], dir[SD_PATH_MAX], backup[SD_PATH_MAX];
    if (!check || !get_dir_name(check->dir_name, name)) return SD_ERR_PARAMETER;
    plat_mutex_lock(g_lock);
    int32_t ret = SCE_OK;
    dir_path(name, dir, sizeof(dir));
    snprintf(backup, sizeof(backup), "%s/sce_backup", dir);
    if (dir_is_mounted(name)) ret = SD_ERR_BUSY;
    else if (!path_is_dir(backup)) ret = SD_ERR_NOT_FOUND;
    else {
        if (check->param) {
            uint64_t blocks;
            if (!read_param_file(backup, check->param, &blocks)) ret = SD_ERR_INTERNAL;
        }
        if (ret == SCE_OK && check->icon) {
            char icon[SD_PATH_MAX];
            snprintf(icon, sizeof(icon), "%s/sce_sys/icon0.png", backup);
            FILE *f = fopen(icon, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                check->icon->data_size = size > 0 ? (uint64_t)size : 0;
                uint64_t n = check->icon->buf_size < check->icon->data_size
                                 ? check->icon->buf_size : check->icon->data_size;
                if (check->icon->buf && n) fread(check->icon->buf, 1, (size_t)n, f);
                fclose(f);
            }
        }
    }
    plat_mutex_unlock(g_lock);
    return ret;
}

static SYSV int32_t ps4_sceSaveDataRestoreBackupData(const sd_restore_backup *restore) {
    if (!g_initialized) return SD_ERR_NOT_INITIALIZED;
    char name[SD_DIRNAME_MAX + 1], dir[SD_PATH_MAX], backup[SD_PATH_MAX];
    if (!restore || !get_dir_name(restore->dir_name, name)) return SD_ERR_PARAMETER;
    plat_mutex_lock(g_lock);
    int32_t ret = SCE_OK;
    dir_path(name, dir, sizeof(dir));
    snprintf(backup, sizeof(backup), "%s/sce_backup", dir);
    if (dir_is_mounted(name)) ret = SD_ERR_BUSY;
    else if (!path_is_dir(backup)) ret = SD_ERR_NOT_FOUND;
    else if (!restore_backup(dir)) ret = SD_ERR_INTERNAL;
    plat_mutex_unlock(g_lock);
    return ret;
}

HLE_LIBRARY(hle_savedata, "libSceSaveData",
    HLE(sceSaveDataInitialize3),
    HLE(sceSaveDataTerminate),
    HLE(sceSaveDataMount2),
    HLE(sceSaveDataUmount),
    HLE(sceSaveDataUmountWithBackup),
    HLE(sceSaveDataGetMountInfo),
    HLE(sceSaveDataSetParam),
    HLE(sceSaveDataSaveIcon),
    HLE(sceSaveDataDelete),
    HLE(sceSaveDataDirNameSearch),
    HLE(sceSaveDataCheckBackupData),
    HLE(sceSaveDataRestoreBackupData),
)
