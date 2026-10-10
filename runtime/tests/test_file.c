/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel file I/O: guest paths match host files regardless of case, as on
 * the PS4 (the game opens /app0/pakn.psarc for pakN.psarc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hle/hle.h"
#include "log.h"

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#define mkdir_p(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

typedef SYSV int32_t (*open_fn)(const char *, int32_t, int32_t);
typedef SYSV int32_t (*close_fn)(int32_t);
typedef SYSV int64_t (*read_fn)(int32_t, void *, uint64_t);
typedef SYSV int64_t (*write_fn)(int32_t, const void *, uint64_t);

static char g_game[256];

static void write_file(const char *rel, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_game, rel);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

int main(void) {
    log_init();
    hle_init();
    char user[256], path[512];
    snprintf(g_game, sizeof(g_game), "test_file_game_%d", (int)getpid());
    snprintf(user, sizeof(user), "test_file_user_%d", (int)getpid());
#ifdef _WIN32
    _putenv_s("SOTC_USER_DIR", user);
#else
    setenv("SOTC_USER_DIR", user, 1);
#endif
    mkdir_p(g_game);
    snprintf(path, sizeof(path), "%s/CoreData", g_game);
    mkdir_p(path);
    write_file("pakN.psarc", "pak");
    write_file("CoreData/DefaultArgs.txt", "args");
    hle_file_init(g_game);

    open_fn op = (open_fn)hle_lookup("libkernel", "sceKernelOpen");
    close_fn cl = (close_fn)hle_lookup("libkernel", "sceKernelClose");
    read_fn rd = (read_fn)hle_lookup("libkernel", "sceKernelRead");
    write_fn wr = (write_fn)hle_lookup("libkernel", "sceKernelWrite");
    CHECK(op && cl && rd && wr);
    if (op && cl && rd && wr) {
        char buf[16] = {0};
        int32_t fd = op("/app0/pakn.psarc", 0, 0); /* leaf differs in case */
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(rd(fd, buf, sizeof(buf)) == 3 && memcmp(buf, "pak", 3) == 0);
            cl(fd);
        }
        memset(buf, 0, sizeof(buf));
        fd = op("/app0/coredata/defaultargs.txt", 0, 0); /* directory and leaf */
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(rd(fd, buf, sizeof(buf)) == 4 && memcmp(buf, "args", 4) == 0);
            cl(fd);
        }
        /* Sequential reads continue where the last one stopped (each guest
         * read must be one host read). */
        memset(buf, 0, sizeof(buf));
        fd = op("/app0/CoreData/DefaultArgs.txt", 0, 0);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(rd(fd, buf, 2) == 2 && memcmp(buf, "ar", 2) == 0);
            CHECK(rd(fd, buf, 2) == 2 && memcmp(buf, "gs", 2) == 0);
            CHECK(rd(fd, buf, 2) == 0);
            cl(fd);
        }
        CHECK(op("/app0/coredata/missing.txt", 0, 0) < 0);
        /* A new file in a directory named with the other case lands in it. */
        fd = op("/app0/COREDATA/New.txt", 0x200 | 1, 0644); /* O_CREAT | O_WRONLY */
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(wr(fd, "abc", 3) == 3);
            cl(fd);
        }
        struct stat st;
        snprintf(path, sizeof(path), "%s/CoreData/New.txt", g_game);
        CHECK(stat(path, &st) == 0 && st.st_size == 3); /* written once */
        remove(path);
    }

    snprintf(path, sizeof(path), "%s/pakN.psarc", g_game);
    remove(path);
    snprintf(path, sizeof(path), "%s/CoreData/DefaultArgs.txt", g_game);
    remove(path);
    snprintf(path, sizeof(path), "%s/CoreData", g_game);
    rmdir(path);
    rmdir(g_game);
    static const char *const mounts[] = {"data", "temp0", "temp", "download0"};
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", user, mounts[i]);
        rmdir(path);
    }
    rmdir(user);

    if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
    else printf("test_file: all passed\n");
    return g_failures ? 1 : 0;
}
