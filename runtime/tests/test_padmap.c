/* SPDX-License-Identifier: GPL-2.0-or-later */
/* window_pad_mapping: the mapping synthesized for a joystick SDL does not
 * know, and that SDL accepts it (no device or display needed). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "window.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#define GUID "03000000790000000600000000000000"

int main(void) {
    char m[1024];

    /* The common 4-axis, 12-button, 1-hat pad: everything mapped. */
    CHECK(window_pad_mapping(m, sizeof m, GUID, "Generic, USB Joystick", 4, 12, 1, NULL));
    CHECK(!strcmp(m, GUID ",Generic  USB Joystick,y:b0,b:b1,a:b2,x:b3,leftshoulder:b4,"
                     "rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,"
                     "leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,"
                     "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"));

    /* A 2-axis, 10-button pad without a hat: no right stick, no clicks, no d-pad. */
    CHECK(window_pad_mapping(m, sizeof m, GUID, NULL, 2, 10, 0, NULL));
    CHECK(strstr(m, ",Generic USB pad,") != NULL);
    CHECK(strstr(m, "start:b9,leftx:a0,lefty:a1,") != NULL);
    CHECK(!strstr(m, "rightx") && !strstr(m, "leftstick") && !strstr(m, "dpup"));

    /* No hat but 16 buttons: d-pad on buttons 12..15. */
    CHECK(window_pad_mapping(m, sizeof m, GUID, "x", 4, 16, 0, NULL));
    CHECK(strstr(m, "dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,") != NULL);

    /* A template body is used as-is. */
    CHECK(window_pad_mapping(m, sizeof m, GUID, "pad", 4, 12, 1, "a:b1,b:b2"));
    CHECK(!strcmp(m, GUID ",pad,a:b1,b:b2"));

    /* Too small a buffer, or nothing to map. */
    CHECK(!window_pad_mapping(m, 40, GUID, "pad", 4, 12, 1, NULL));
    CHECK(!window_pad_mapping(m, sizeof m, GUID, "pad", 0, 0, 0, NULL));

    /* SDL parses the synthesized mapping. */
    if (SDL_Init(SDL_INIT_GAMEPAD)) {
        CHECK(window_pad_mapping(m, sizeof m, "0300000012340000abcd000000000000", "Test pad", 4,
                                 12, 1, NULL));
        CHECK(SDL_AddGamepadMapping(m) >= 0);
        char *back = SDL_GetGamepadMappingForGUID(SDL_StringToGUID("0300000012340000abcd000000000000"));
        CHECK(back && strstr(back, "rightx:a2") && strstr(back, "dpleft:h0.8"));
        SDL_free(back);
        SDL_Quit();
    } else {
        printf("SKIP SDL check: %s\n", SDL_GetError());
    }

    if (g_failures) {
        printf("padmap: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("padmap: passed\n");
    return 0;
}
