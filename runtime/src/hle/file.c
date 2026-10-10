/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel file I/O over host files.
 *   /app0/...      -> SOTC_GAME_DIR (read-only game files)
 *   /data, /temp0, /temp, /download0, /savedata0 -> SOTC_USER_DIR/<mount>/ */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <io.h>
#include <windows.h>
#define h_open(p, f, m) _open(p, (f) | _O_BINARY, m)
#define h_close _close
#define h_read(fd, b, n) _read(fd, b, (unsigned)(n))
#define h_write(fd, b, n) _write(fd, b, (unsigned)(n))
#define h_lseek _lseeki64
#define h_ftruncate(fd, n) _chsize_s(fd, n)
#define h_mkdir(p) _mkdir(p)
#define h_rmdir _rmdir
#define h_unlink _unlink
typedef struct _stat64 h_stat_t;
#define h_stat _stat64
#define h_fstat _fstat64
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#define h_open open
#define h_close close
#define h_read read
#define h_write write
#define h_lseek lseek
#define h_ftruncate ftruncate
#define h_mkdir(p) mkdir(p, 0755)
#define h_rmdir rmdir
#define h_unlink unlink
typedef struct stat h_stat_t;
#define h_stat stat
#define h_fstat fstat
#endif

#include "../log.h"
#include "hle.h"

#define MAX_FDS 1024
#define MAX_PATH_LEN 1024

/* PS4 open flags. */
enum {
    O_ACC = 3, PO_WRONLY = 1, PO_RDWR = 2, PO_APPEND = 0x8, PO_CREAT = 0x200, PO_TRUNC = 0x400,
    PO_EXCL = 0x800, PO_DIRECTORY = 0x20000,
};

typedef struct dir_listing {
    char **names;
    uint8_t *is_dir;
    uint32_t count, next;
} dir_listing;

typedef struct guest_fd {
    bool used;
    int host;               /* -1 for directories */
    plat_mutex *lock;       /* serializes seek+read for pread */
    dir_listing *dir;
    char path[MAX_PATH_LEN];
} guest_fd;

static guest_fd g_fds[MAX_FDS];
static plat_mutex *g_lock;

/* Guest path prefix -> host directory. */
#define MAX_MOUNTS 32
typedef struct mount {
    char guest[64];
    char host[MAX_PATH_LEN];
} mount;
static mount g_mounts[MAX_MOUNTS];
static plat_mutex *g_mount_lock;

static int ps4_errno_from_host(int e) {
    switch (e) {
    case ENOENT: return PS4_ENOENT;
    case EACCES: return PS4_EACCES;
    case EEXIST: return PS4_EEXIST;
    case ENOTDIR: return PS4_ENOTDIR;
    case EISDIR: return PS4_EISDIR;
    case EMFILE: return PS4_EMFILE;
    case ENOSPC: return PS4_ENOSPC;
    case ENOTEMPTY: return PS4_ENOTEMPTY;
    case EBADF: return PS4_EBADF;
    default: return PS4_EIO;
    }
}

static void make_dirs(const char *path) {
    char buf[MAX_PATH_LEN];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char *p = buf + 1; *p; p++)
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            h_mkdir(buf);
            *p = c;
        }
    h_mkdir(buf);
}

bool hle_file_mount(const char *guest_prefix, const char *host_dir) {
    if (!g_mount_lock) g_mount_lock = plat_mutex_new();
    plat_mutex_lock(g_mount_lock);
    mount *free_slot = NULL, *slot = NULL;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].guest[0] && strcmp(g_mounts[i].guest, guest_prefix) == 0) slot = &g_mounts[i];
        if (!g_mounts[i].guest[0] && !free_slot) free_slot = &g_mounts[i];
    }
    if (!slot) slot = free_slot;
    if (slot) {
        strncpy(slot->guest, guest_prefix, sizeof(slot->guest) - 1);
        strncpy(slot->host, host_dir, sizeof(slot->host) - 1);
    }
    plat_mutex_unlock(g_mount_lock);
    if (slot) log_debug("files: mounted %s -> %s", guest_prefix, host_dir);
    return slot != NULL;
}

void hle_file_unmount(const char *guest_prefix) {
    if (!g_mount_lock) return;
    plat_mutex_lock(g_mount_lock);
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (strcmp(g_mounts[i].guest, guest_prefix) == 0) memset(&g_mounts[i], 0, sizeof(mount));
    plat_mutex_unlock(g_mount_lock);
}

void hle_file_init(const char *game_dir) {
    static const char *const user_mounts[] = {"/data", "/temp0", "/temp", "/download0"};
    g_lock = plat_mutex_new();
    const char *user = getenv("SOTC_USER_DIR");
    if (!user) user = "user";
    make_dirs(user);
    hle_file_mount("/app0", game_dir);
    for (size_t i = 0; i < sizeof(user_mounts) / sizeof(user_mounts[0]); i++) {
        char host[MAX_PATH_LEN];
        snprintf(host, sizeof(host), "%s%s", user, user_mounts[i]);
        make_dirs(host);
        hle_file_mount(user_mounts[i], host);
    }
    for (int i = 0; i < 3; i++) g_fds[i].used = true; /* stdin, stdout, stderr */
    log_info("files: /app0 -> %s, user data -> %s", game_dir, user);
}

#ifndef _WIN32
/* The PS4 file system ignores case and the game relies on it (it opens
 * /app0/pakn.psarc for pakN.psarc), like NTFS does on Windows. If `path`
 * doesn't exist, replace each missing component below `root_len` with a
 * case-insensitive match from its directory, in place (same length). A
 * component without a match stays as given: a file about to be created.
 * After shadPS4's MntPoints::GetHostPath (src/core/file_sys/fs.cpp). */
static void fix_case(char *path, size_t root_len) {
    h_stat_t st;
    if (h_stat(path, &st) == 0) return;
    for (char *part = path + root_len; *part;) {
        while (*part == '/') part++;
        char *end = strchr(part, '/');
        char saved = end ? *end : 0;
        if (end) *end = 0;
        if (*part && h_stat(path, &st) != 0) {
            part[-1] = 0; /* the directory holding this component */
            DIR *d = opendir(path[0] ? path : "/");
            part[-1] = '/';
            bool found = false;
            for (struct dirent *e; d && (e = readdir(d));)
                if (strcasecmp(e->d_name, part) == 0) {
                    memcpy(part, e->d_name, strlen(part));
                    found = true;
                    break;
                }
            if (d) closedir(d);
            if (!found) {
                if (end) *end = saved;
                return;
            }
        }
        if (!end) return;
        *end = saved;
        part = end;
    }
}
#endif

/* Map a guest path to a host path using the longest matching mount. */
static bool translate(const char *guest, char *out, size_t size) {
    if (!guest || !g_mount_lock) return false;
    plat_mutex_lock(g_mount_lock);
    const mount *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        size_t n = strlen(g_mounts[i].guest);
        if (n > best_len && strncmp(guest, g_mounts[i].guest, n) == 0 &&
            (guest[n] == '/' || guest[n] == 0)) {
            best = &g_mounts[i];
            best_len = n;
        }
    }
    size_t root_len = best ? strlen(best->host) : 0;
    if (best) snprintf(out, size, "%s%s", best->host, guest + best_len);
    plat_mutex_unlock(g_mount_lock);
    if (!best) log_warn("files: unknown mount in '%s'", guest);
#ifndef _WIN32
    else if (root_len < size) fix_case(out, root_len);
#endif
    return best != NULL;
}

static guest_fd *fd_get(int32_t fd) {
    return fd >= 0 && fd < MAX_FDS && g_fds[fd].used ? &g_fds[fd] : NULL;
}

static int32_t fd_alloc(void) {
    plat_mutex_lock(g_lock);
    for (int32_t i = 3; i < MAX_FDS; i++)
        if (!g_fds[i].used) {
            memset(&g_fds[i], 0, sizeof(g_fds[i]));
            g_fds[i].used = true;
            plat_mutex_unlock(g_lock);
            return i;
        }
    plat_mutex_unlock(g_lock);
    return -1;
}

static bool host_is_dir(const char *path) {
    h_stat_t st;
    return h_stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static dir_listing *list_dir(const char *path) {
    dir_listing *d = calloc(1, sizeof(*d));
    uint32_t cap = 0;
#define ADD(name, isdir)                                                          \
    do {                                                                          \
        if (d->count == cap) {                                                    \
            cap = cap ? cap * 2 : 32;                                             \
            d->names = realloc(d->names, cap * sizeof(char *));                   \
            d->is_dir = realloc(d->is_dir, cap);                                  \
        }                                                                         \
        d->names[d->count] = strdup(name);                                        \
        d->is_dir[d->count++] = (isdir);                                          \
    } while (0)
#ifdef _WIN32
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do ADD(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *dir = opendir(path);
    if (dir) {
        for (struct dirent *e; (e = readdir(dir));) ADD(e->d_name, e->d_type == DT_DIR);
        closedir(dir);
    }
#endif
#undef ADD
    return d;
}

/* ---- open/close -------------------------------------------------------- */

static int file_open(const char *path, int32_t flags, int32_t mode, int32_t *fd_out) {
    char host[MAX_PATH_LEN];
    if (!translate(path, host, sizeof(host))) return PS4_ENOENT;
    int32_t fd = fd_alloc();
    if (fd < 0) return PS4_EMFILE;
    guest_fd *g = &g_fds[fd];
    strncpy(g->path, host, sizeof(g->path) - 1);
    g->host = -1;
    if ((flags & PO_DIRECTORY) || host_is_dir(host)) {
        if (!host_is_dir(host)) { g->used = false; return PS4_ENOTDIR; }
        g->dir = list_dir(host);
    } else {
        int hf = 0;
        switch (flags & O_ACC) {
        case PO_WRONLY: hf = O_WRONLY; break;
        case PO_RDWR: hf = O_RDWR; break;
        default: hf = O_RDONLY;
        }
        if (flags & PO_APPEND) hf |= O_APPEND;
        if (flags & PO_CREAT) hf |= O_CREAT;
        if (flags & PO_TRUNC) hf |= O_TRUNC;
        if (flags & PO_EXCL) hf |= O_EXCL;
        g->host = h_open(host, hf, 0666);
        if (g->host < 0) {
            int e = ps4_errno_from_host(errno);
            g->used = false;
            log_debug("open %s -> errno %d", path, e);
            return e;
        }
    }
    g->lock = plat_mutex_new();
    log_debug("open %s (flags %#x mode %#o) -> fd %d", path, flags, mode, fd);
    *fd_out = fd;
    return 0;
}

static int file_close(int32_t fd) {
    guest_fd *g = fd_get(fd);
    if (!g || fd < 3) return PS4_EBADF;
    if (g->host >= 0) h_close(g->host);
    if (g->dir) {
        for (uint32_t i = 0; i < g->dir->count; i++) free(g->dir->names[i]);
        free(g->dir->names);
        free(g->dir->is_dir);
        free(g->dir);
    }
    if (g->lock) plat_mutex_free(g->lock);
    g->used = false;
    return 0;
}

static SYSV int32_t ps4_sceKernelOpen(const char *path, int32_t flags, int32_t mode) {
    int32_t fd;
    int e = file_open(path, flags, mode, &fd);
    return e ? SCE_KERNEL_ERROR(e) : fd;
}
static SYSV int32_t ps4_sceKernelClose(int32_t fd) {
    int e = file_close(fd);
    return e ? SCE_KERNEL_ERROR(e) : SCE_OK;
}
static SYSV int32_t ps4_close(int32_t fd) {
    int e = file_close(fd);
    if (e) { *hle_errno() = e; return -1; }
    return 0;
}

/* ---- read/write -------------------------------------------------------- */

/* Host I/O goes through a bounce buffer. Guest memory may be briefly
 * unmapped while another thread splits a mapping (see memory.c); the host
 * kernel then fails the transfer instead of faulting, but a memcpy faults and
 * the fault handler retries it once the split is done. */
#define BOUNCE_SIZE (4u << 20)

static int64_t do_read(guest_fd *g, void *buf, uint64_t n) {
    char *bounce = malloc(n < BOUNCE_SIZE ? (size_t)n : BOUNCE_SIZE);
    if (!bounce) return -1;
    int64_t total = 0;
    while ((uint64_t)total < n) {
        uint64_t chunk = n - (uint64_t)total > BOUNCE_SIZE ? BOUNCE_SIZE : n - (uint64_t)total;
        int64_t r = h_read(g->host, bounce, chunk);
        if (r < 0) {
            if (!total) total = -1;
            break;
        }
        if (r == 0) break;
        memcpy((char *)buf + total, bounce, (size_t)r);
        total += r;
        if ((uint64_t)r < chunk) break;
    }
    free(bounce);
    return total;
}

static int64_t do_write(guest_fd *g, const void *buf, uint64_t n) {
    char *bounce = malloc(n < BOUNCE_SIZE ? (size_t)n : BOUNCE_SIZE);
    if (!bounce) return -1;
    int64_t total = 0;
    while ((uint64_t)total < n) {
        uint64_t chunk = n - (uint64_t)total > BOUNCE_SIZE ? BOUNCE_SIZE : n - (uint64_t)total;
        memcpy(bounce, (const char *)buf + total, (size_t)chunk);
        int64_t r = h_write(g->host, bounce, chunk);
        if (r < 0) {
            if (!total) total = -1;
            break;
        }
        total += r;
        if ((uint64_t)r < chunk) break;
    }
    free(bounce);
    return total;
}

static int64_t console_write(int32_t fd, const void *buf, uint64_t n) {
    log_info("guest %s: %.*s", fd == 1 ? "stdout" : "stderr", (int)(n && ((const char *)buf)[n - 1] == '\n' ? n - 1 : n),
             (const char *)buf);
    return (int64_t)n;
}

static int64_t file_read(int32_t fd, void *buf, uint64_t n) {
    guest_fd *g = fd_get(fd);
    if (!g || g->host < 0) return -PS4_EBADF;
    plat_mutex_lock(g->lock);
    int64_t r = do_read(g, buf, n);
    plat_mutex_unlock(g->lock);
    return r < 0 ? -PS4_EIO : r;
}

static int64_t file_write(int32_t fd, const void *buf, uint64_t n) {
    if (fd == 1 || fd == 2) return console_write(fd, buf, n);
    guest_fd *g = fd_get(fd);
    if (!g || g->host < 0) return -PS4_EBADF;
    plat_mutex_lock(g->lock);
    int64_t r = do_write(g, buf, n);
    plat_mutex_unlock(g->lock);
    return r < 0 ? -ps4_errno_from_host(errno) : r;
}

static int64_t file_pread(int32_t fd, void *buf, uint64_t n, int64_t off) {
    guest_fd *g = fd_get(fd);
    if (!g || g->host < 0) return -PS4_EBADF;
    plat_mutex_lock(g->lock);
    int64_t saved = h_lseek(g->host, 0, SEEK_CUR);
    h_lseek(g->host, off, SEEK_SET);
    int64_t r = do_read(g, buf, n);
    h_lseek(g->host, saved, SEEK_SET);
    plat_mutex_unlock(g->lock);
    return r < 0 ? -PS4_EIO : r;
}

static int64_t file_pwrite(int32_t fd, const void *buf, uint64_t n, int64_t off) {
    guest_fd *g = fd_get(fd);
    if (!g || g->host < 0) return -PS4_EBADF;
    plat_mutex_lock(g->lock);
    int64_t saved = h_lseek(g->host, 0, SEEK_CUR);
    h_lseek(g->host, off, SEEK_SET);
    int64_t r = do_write(g, buf, n);
    h_lseek(g->host, saved, SEEK_SET);
    plat_mutex_unlock(g->lock);
    return r < 0 ? -ps4_errno_from_host(errno) : r;
}

int64_t hle_file_pread(int32_t fd, void *buf, uint64_t len, int64_t offset) {
    int64_t r = file_pread(fd, buf, len, offset);
    return r < 0 ? -1 : r;
}

/* Functions, not macros: the macros evaluated their argument twice, so every
 * read and write ran twice (a sequential read returned the second one). */
static inline int64_t SCE_RET(int64_t r) { return r < 0 ? (int64_t)SCE_KERNEL_ERROR(-r) : r; }
static inline int64_t POSIX_RET(int64_t r) {
    if (r >= 0) return r;
    *hle_errno() = (int)-r;
    return -1;
}

static SYSV int64_t ps4_sceKernelRead(int32_t fd, void *b, uint64_t n) { return SCE_RET(file_read(fd, b, n)); }
static SYSV int64_t ps4_sceKernelWrite(int32_t fd, const void *b, uint64_t n) { return SCE_RET(file_write(fd, b, n)); }
static SYSV int64_t ps4_sceKernelPread(int32_t fd, void *b, uint64_t n, int64_t o) { return SCE_RET(file_pread(fd, b, n, o)); }
static SYSV int64_t ps4_sceKernelPwrite(int32_t fd, const void *b, uint64_t n, int64_t o) { return SCE_RET(file_pwrite(fd, b, n, o)); }
static SYSV int64_t ps4__read(int32_t fd, void *b, uint64_t n) { return POSIX_RET(file_read(fd, b, n)); }
static SYSV int64_t ps4__write(int32_t fd, const void *b, uint64_t n) { return POSIX_RET(file_write(fd, b, n)); }

static SYSV int64_t ps4_sceKernelLseek(int32_t fd, int64_t off, int32_t whence) {
    guest_fd *g = fd_get(fd);
    if (!g || g->host < 0) return SCE_KERNEL_ERROR(PS4_EBADF);
    plat_mutex_lock(g->lock);
    int64_t r = h_lseek(g->host, off, whence);
    plat_mutex_unlock(g->lock);
    return r < 0 ? SCE_KERNEL_ERROR(PS4_EINVAL) : r;
}

/* ---- stat -------------------------------------------------------------- */

typedef struct ps4_stat {
    uint32_t st_dev, st_ino;
    uint16_t st_mode, st_nlink;
    uint32_t st_uid, st_gid, st_rdev;
    int64_t st_atim[2], st_mtim[2], st_ctim[2];
    int64_t st_size, st_blocks;
    uint32_t st_blksize, st_flags, st_gen;
    int32_t st_lspare;
    int64_t st_birthtim[2];
} ps4_stat;
_Static_assert(sizeof(ps4_stat) == 120, "ps4_stat layout");

static void fill_stat(const h_stat_t *st, ps4_stat *out) {
    memset(out, 0, sizeof(*out));
    bool dir = (st->st_mode & S_IFMT) == S_IFDIR;
    out->st_mode = dir ? 0040000 | 0777 : 0100000 | 0666;
    out->st_nlink = 1;
    out->st_size = dir ? 65536 : (int64_t)st->st_size;
    out->st_blksize = 512;
    out->st_blocks = (out->st_size + 511) / 512;
    out->st_atim[0] = (int64_t)st->st_atime;
    out->st_mtim[0] = (int64_t)st->st_mtime;
    out->st_ctim[0] = (int64_t)st->st_ctime;
    out->st_birthtim[0] = (int64_t)st->st_ctime;
}

static SYSV int32_t ps4_sceKernelStat(const char *path, ps4_stat *out) {
    char host[MAX_PATH_LEN];
    h_stat_t st;
    if (!out) return SCE_KERNEL_ERROR(PS4_EFAULT);
    if (!translate(path, host, sizeof(host)) || h_stat(host, &st) != 0) {
        log_debug("stat %s -> ENOENT", path ? path : "(null)");
        return SCE_KERNEL_ERROR(PS4_ENOENT);
    }
    fill_stat(&st, out);
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelFstat(int32_t fd, ps4_stat *out) {
    guest_fd *g = fd_get(fd);
    h_stat_t st;
    if (!g || !out) return SCE_KERNEL_ERROR(PS4_EBADF);
    if (g->host >= 0 ? h_fstat(g->host, &st) != 0 : h_stat(g->path, &st) != 0)
        return SCE_KERNEL_ERROR(PS4_EIO);
    fill_stat(&st, out);
    return SCE_OK;
}

/* ---- directories and metadata ------------------------------------------ */

static SYSV int32_t ps4_sceKernelGetdirentries(int32_t fd, char *buf, int32_t nbytes,
                                               int64_t *basep) {
    guest_fd *g = fd_get(fd);
    if (!g || !g->dir) return SCE_KERNEL_ERROR(PS4_EBADF);
    if (basep) *basep = g->dir->next;
    int32_t used = 0;
    while (g->dir->next < g->dir->count) {
        const char *name = g->dir->names[g->dir->next];
        size_t len = strlen(name);
        if (len > 255) len = 255;
        uint16_t reclen = (uint16_t)((8 + len + 1 + 3) & ~3u);
        if (used + reclen > nbytes) break;
        char *rec = buf + used;
        memset(rec, 0, reclen);
        uint32_t fileno = g->dir->next + 1;
        memcpy(rec, &fileno, 4);
        memcpy(rec + 4, &reclen, 2);
        rec[6] = g->dir->is_dir[g->dir->next] ? 4 : 8; /* DT_DIR : DT_REG */
        rec[7] = (char)len;
        memcpy(rec + 8, name, len);
        used += reclen;
        g->dir->next++;
    }
    return used;
}

static SYSV int32_t ps4_sceKernelMkdir(const char *path, int32_t mode) {
    (void)mode;
    char host[MAX_PATH_LEN];
    if (!translate(path, host, sizeof(host))) return SCE_KERNEL_ERROR(PS4_ENOENT);
    if (h_mkdir(host) != 0) return SCE_KERNEL_ERROR(ps4_errno_from_host(errno));
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelRmdir(const char *path) {
    char host[MAX_PATH_LEN];
    if (!translate(path, host, sizeof(host))) return SCE_KERNEL_ERROR(PS4_ENOENT);
    if (h_rmdir(host) != 0) return SCE_KERNEL_ERROR(ps4_errno_from_host(errno));
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelUnlink(const char *path) {
    char host[MAX_PATH_LEN];
    if (!translate(path, host, sizeof(host))) return SCE_KERNEL_ERROR(PS4_ENOENT);
    if (h_unlink(host) != 0) return SCE_KERNEL_ERROR(ps4_errno_from_host(errno));
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelRename(const char *from, const char *to) {
    char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
    if (!translate(from, a, sizeof(a)) || !translate(to, b, sizeof(b)))
        return SCE_KERNEL_ERROR(PS4_ENOENT);
#ifdef _WIN32
    if (!MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING)) return SCE_KERNEL_ERROR(PS4_EIO);
#else
    if (rename(a, b) != 0) return SCE_KERNEL_ERROR(ps4_errno_from_host(errno));
#endif
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelFtruncate(int32_t fd, int64_t len) {
    guest_fd *g = fd_get(fd);
    if (!g || g->host < 0) return SCE_KERNEL_ERROR(PS4_EBADF);
    return h_ftruncate(g->host, len) == 0 ? SCE_OK : SCE_KERNEL_ERROR(PS4_EIO);
}

static SYSV int32_t ps4_sceKernelTruncate(const char *path, int64_t len) {
    int32_t fd = ps4_sceKernelOpen(path, PO_WRONLY, 0);
    if (fd < 0) return fd;
    int32_t r = ps4_sceKernelFtruncate(fd, len);
    file_close(fd);
    return r;
}

static SYSV int32_t ps4_sceKernelFsync(int32_t fd) {
    return fd_get(fd) ? SCE_OK : SCE_KERNEL_ERROR(PS4_EBADF);
}

HLE_LIBRARY(hle_kernel_file, "libkernel",
    HLE(sceKernelOpen), HLE(sceKernelClose), HLE(sceKernelRead), HLE(sceKernelWrite),
    HLE(sceKernelPread), HLE(sceKernelPwrite), HLE(sceKernelLseek), HLE(sceKernelStat),
    HLE(sceKernelFstat), HLE(sceKernelGetdirentries), HLE(sceKernelMkdir), HLE(sceKernelRmdir),
    HLE(sceKernelUnlink), HLE(sceKernelRename), HLE(sceKernelFtruncate), HLE(sceKernelTruncate),
    HLE(sceKernelFsync), HLE(_read), HLE(_write), HLE(close),
)
