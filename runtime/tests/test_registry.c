/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HLE libraries must self-register, including files nothing else references. */
#include <stdio.h>

#include "hle/hle.h"
#include "hle/kobj.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

int main(void) {
    log_init();
    hle_init();
    /* sysmodule.c is referenced only through its HLE_LIBRARY constructor. */
    CHECK(hle_lookup("libSceSysmodule", "sceSysmoduleLoadModule") != NULL);
    CHECK(hle_lookup("libkernel", "sceKernelUsleep") != NULL);
    /* posix names resolve through libkernel tables and vice versa. */
    CHECK(hle_lookup("libScePosix", "clock_gettime") != NULL);
    CHECK(hle_lookup("libkernel", "pthread_mutex_lock") != NULL);
    CHECK(hle_lookup("libkernel", "no_such_function") == NULL);
    CHECK(hle_lookup("libkernel", "") == NULL);

    int dummy;
    int32_t h = kobj_alloc(KOBJ_PAD, &dummy);
    CHECK(h > 0);
    CHECK(kobj_get(h, KOBJ_PAD) == &dummy);
    CHECK(kobj_get(h, KOBJ_VIDEOOUT) == NULL);
    CHECK(kobj_free(h) == &dummy);
    CHECK(kobj_get(h, KOBJ_PAD) == NULL);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("registry tests passed\n");
    return 0;
}
