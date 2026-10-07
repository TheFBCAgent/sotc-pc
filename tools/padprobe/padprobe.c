/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sotc-padprobe: lists every joystick SDL3 sees (name, ids, GUID, axis /
 * button / hat counts, whether SDL knows it as a gamepad and the mapping it
 * would use), then prints raw button, axis and hat changes for a while. Use
 * it to write a mapping for a pad SDL does not know (docs/m3/pad.md).
 *
 *   sotc-padprobe [seconds]      default 15; 0 lists and exits
 *
 * Only the joystick and gamepad subsystems are started: no window, no GPU.
 * The runtime's mapping environment (SOTC_PAD_MAPPINGS, SOTC_PAD_MAPPING) is
 * not applied here, so this shows what SDL knows on its own. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static void list_joysticks(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    printf("%d joystick(s)\n", n);
    for (int i = 0; i < n; i++) {
        SDL_JoystickID id = ids[i];
        SDL_Joystick *j = SDL_OpenJoystick(id);
        if (!j) {
            printf("  [%u] cannot open: %s\n", (unsigned)id, SDL_GetError());
            continue;
        }
        char guid[64];
        SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof guid);
        const char *name = SDL_GetJoystickName(j);
        const char *path = SDL_GetJoystickPath(j);
        printf("  [%u] \"%s\"\n", (unsigned)id, name ? name : "?");
        printf("      vendor 0x%04x product 0x%04x version 0x%04x type %d\n",
               SDL_GetJoystickVendor(j), SDL_GetJoystickProduct(j),
               SDL_GetJoystickProductVersion(j), (int)SDL_GetJoystickType(j));
        printf("      guid %s\n", guid);
        printf("      path %s\n", path ? path : "-");
        printf("      axes %d buttons %d hats %d balls %d\n", SDL_GetNumJoystickAxes(j),
               SDL_GetNumJoystickButtons(j), SDL_GetNumJoystickHats(j),
               SDL_GetNumJoystickBalls(j));
        bool is_pad = SDL_IsGamepad(id);
        printf("      gamepad: %s\n", is_pad ? "yes" : "no (needs a mapping)");
        if (is_pad) {
            char *m = SDL_GetGamepadMappingForID(id);
            printf("      mapping %s\n", m ? m : "-");
            SDL_free(m);
        }
        /* Leave it open so its events are reported. */
    }
    SDL_free(ids);
    fflush(stdout);
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 15.0;
    /* No window, so the process never has focus: report events anyway. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    printf("SDL %d.%d.%d\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
           SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    list_joysticks();
    if (seconds <= 0) {
        SDL_Quit();
        return 0;
    }

    printf("reporting input for %.0f s (press buttons, move sticks, d-pad)...\n", seconds);
    fflush(stdout);
    Uint64 end = SDL_GetTicks() + (Uint64)(seconds * 1000);
    int events = 0;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 50)) continue;
        do {
            switch (e.type) {
            case SDL_EVENT_JOYSTICK_ADDED:
                printf("added joystick %u\n", (unsigned)e.jdevice.which);
                list_joysticks();
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                printf("removed joystick %u\n", (unsigned)e.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                printf("[%u] button %d %s\n", (unsigned)e.jbutton.which, e.jbutton.button,
                       e.jbutton.down ? "down" : "up");
                events++;
                break;
            case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
                /* Axes jitter: report only when one crosses into another
                 * third (low / centered / high). Triggers rest at -32768. */
                static signed char zone[64];
                int z = e.jaxis.value < -16000 ? -1 : e.jaxis.value > 16000 ? 1 : 0;
                int a = e.jaxis.axis & 63;
                if (zone[a] != z) {
                    zone[a] = (signed char)z;
                    printf("[%u] axis %d %d\n", (unsigned)e.jaxis.which, e.jaxis.axis,
                           e.jaxis.value);
                    events++;
                }
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                printf("[%u] hat %d 0x%x\n", (unsigned)e.jhat.which, e.jhat.hat, e.jhat.value);
                events++;
                break;
            default:
                break;
            }
        } while (SDL_PollEvent(&e));
        fflush(stdout);
    }
    printf("%d input event(s)\n", events);
    SDL_Quit();
    return 0;
}
