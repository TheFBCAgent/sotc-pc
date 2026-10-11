/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host window (SDL3) on the process main thread: creates it, pumps its
 * events, and turns an SDL gamepad and/or the keyboard into libScePad input.
 *
 * Button and axis mapping and the default keyboard bindings follow shadPS4
 * (src/input/input_handler.cpp, src/input/controller*.cpp, src/sdl_window.cpp;
 * GPL-2.0, commit 14ada48). */
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_host.h"
#include "gpu_bridge.h"
#include "log.h"
#include "window.h"

#define WINDOW_TITLE "Shadow of the Colossus"
#define DEFAULT_W 1920
#define DEFAULT_H 1080

/* ---- window handle hand-off ------------------------------------------------ */

enum { WIN_PENDING, WIN_READY, WIN_NONE };
static atomic_int g_win_state = WIN_PENDING;
static SDL_Window *g_window;          /* published by g_win_state (release) */

static void publish_window(SDL_Window *w) {
    g_window = w;
    atomic_store_explicit(&g_win_state, w ? WIN_READY : WIN_NONE, memory_order_release);
}

void *window_sdl_handle(void) {
    int s = atomic_load_explicit(&g_win_state, memory_order_acquire);
    if (s == WIN_PENDING) {
        uint64_t deadline = plat_monotonic_ns() + (uint64_t)WINDOW_HANDLE_TIMEOUT_MS * 1000000;
        while ((s = atomic_load_explicit(&g_win_state, memory_order_acquire)) == WIN_PENDING) {
            if (plat_monotonic_ns() >= deadline) {
                log_warn("window: no window after %d ms, running without one",
                         WINDOW_HANDLE_TIMEOUT_MS);
                return NULL;
            }
            plat_sleep_us(1000);
        }
    }
    return s == WIN_READY ? g_window : NULL;
}

/* ---- pad output (guest threads -> main thread) ------------------------------ */

static atomic_uint g_rumble;          /* large << 8 | small */
static atomic_uint g_light = 0x0000FF; /* r << 16 | g << 8 | b; the default blue */
static atomic_bool g_output_dirty;

void window_pad_rumble(uint8_t large_motor, uint8_t small_motor) {
    unsigned v = (unsigned)large_motor << 8 | small_motor;
    if (atomic_exchange(&g_rumble, v) != v) atomic_store(&g_output_dirty, true);
}

void window_pad_light(uint8_t r, uint8_t g, uint8_t b) {
    unsigned v = (unsigned)r << 16 | (unsigned)g << 8 | b;
    if (atomic_exchange(&g_light, v) != v) atomic_store(&g_output_dirty, true);
}

/* ---- frame rate in the title (guest threads -> main thread) ---------------- */

static atomic_ullong g_flips;
static uint64_t g_title_flips, g_title_ns; /* main thread only */

void window_note_flip(void) { atomic_fetch_add_explicit(&g_flips, 1, memory_order_relaxed); }

/* About once per second: "<title> — 30 fps (Vulkan)". */
static void update_title(SDL_Window *win) {
    uint64_t now = plat_monotonic_ns();
    if (now - g_title_ns < 1000000000ull) return;
    uint64_t flips = atomic_load_explicit(&g_flips, memory_order_relaxed);
    double fps = (double)(flips - g_title_flips) * 1e9 / (double)(now - g_title_ns);
    g_title_flips = flips;
    g_title_ns = now;
    char title[96];
    snprintf(title, sizeof title, WINDOW_TITLE " \xE2\x80\x94 %.0f fps (%s)", fps, /* em dash */
             gpu_bridge_active() ? "Vulkan" : "null GPU");
    SDL_SetWindowTitle(win, title);
}

/* ---- input -------------------------------------------------------------- */

static SDL_Gamepad *g_gamepad;        /* main thread only */
static uint64_t g_rumble_applied_ns;

static const struct { SDL_GamepadButton sdl; uint32_t ps4; } k_gamepad_buttons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BTN_CROSS},
    {SDL_GAMEPAD_BUTTON_EAST, PAD_BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST, PAD_BTN_SQUARE},
    {SDL_GAMEPAD_BUTTON_NORTH, PAD_BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_BTN_L1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_BTN_L3},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_BTN_R3},
    {SDL_GAMEPAD_BUTTON_START, PAD_BTN_OPTIONS},
    {SDL_GAMEPAD_BUTTON_BACK, PAD_BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, PAD_BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_BTN_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_BTN_LEFT},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_BTN_RIGHT},
};

/* Keyboard: shadPS4's default bindings (scancodes, so layout-independent),
 * plus a few free keys: Backspace / Esc = Circle, Tab = Touchpad, F / G =
 * L3 / R3, Left Shift / Left Ctrl = L2 / R2 (full press). */
static const struct { SDL_Scancode key; uint32_t ps4; } k_key_buttons[] = {
    {SDL_SCANCODE_KP_8, PAD_BTN_TRIANGLE}, {SDL_SCANCODE_C, PAD_BTN_TRIANGLE},
    {SDL_SCANCODE_KP_6, PAD_BTN_CIRCLE},   {SDL_SCANCODE_B, PAD_BTN_CIRCLE},
    {SDL_SCANCODE_KP_2, PAD_BTN_CROSS},    {SDL_SCANCODE_N, PAD_BTN_CROSS},
    {SDL_SCANCODE_KP_4, PAD_BTN_SQUARE},   {SDL_SCANCODE_V, PAD_BTN_SQUARE},
    {SDL_SCANCODE_Q, PAD_BTN_L1},          {SDL_SCANCODE_U, PAD_BTN_R1},
    {SDL_SCANCODE_X, PAD_BTN_L3},          {SDL_SCANCODE_M, PAD_BTN_R3},
    {SDL_SCANCODE_RETURN, PAD_BTN_OPTIONS}, {SDL_SCANCODE_SPACE, PAD_BTN_TOUCHPAD},
    {SDL_SCANCODE_UP, PAD_BTN_UP},         {SDL_SCANCODE_DOWN, PAD_BTN_DOWN},
    {SDL_SCANCODE_LEFT, PAD_BTN_LEFT},     {SDL_SCANCODE_RIGHT, PAD_BTN_RIGHT},
    {SDL_SCANCODE_BACKSPACE, PAD_BTN_CIRCLE}, {SDL_SCANCODE_ESCAPE, PAD_BTN_CIRCLE},
    {SDL_SCANCODE_TAB, PAD_BTN_TOUCHPAD},  {SDL_SCANCODE_F, PAD_BTN_L3},
    {SDL_SCANCODE_G, PAD_BTN_R3},
};
static const struct { SDL_Scancode minus, plus; int axis; } k_key_axes[] = {
    {SDL_SCANCODE_A, SDL_SCANCODE_D, PAD_AXIS_LEFT_X},
    {SDL_SCANCODE_W, SDL_SCANCODE_S, PAD_AXIS_LEFT_Y},
    {SDL_SCANCODE_J, SDL_SCANCODE_L, PAD_AXIS_RIGHT_X},
    {SDL_SCANCODE_I, SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y},
};
static const struct { SDL_Scancode key; int axis; } k_key_triggers[] = {
    {SDL_SCANCODE_E, PAD_AXIS_L2},
    {SDL_SCANCODE_O, PAD_AXIS_R2},
    {SDL_SCANCODE_LSHIFT, PAD_AXIS_L2},
    {SDL_SCANCODE_LCTRL, PAD_AXIS_R2},
};

/* SDL stick axis (-32768..32767) to libScePad (0..255, 128 centered), with
 * shadPS4's default dead zone (5 of 127). */
static uint8_t stick_value(Sint16 v) {
    if (v > -5 * 258 && v < 5 * 258) return 128;
    return (uint8_t)((v + 32768) >> 8);
}

static uint8_t trigger_value(Sint16 v) { return v <= 0 ? 0 : (uint8_t)(v * 255 / 32767); }

/* SOTC_PAD_SCRIPT="25:cross,31.5:down,32:cross": hold each button for 150 ms
 * at that many seconds after the window loop started (for unattended runs). */
#define PAD_SCRIPT_MAX 32
#define PAD_SCRIPT_HOLD_NS 150000000ull
typedef struct { uint64_t at_ns; uint32_t button; } script_press;
static script_press g_script[PAD_SCRIPT_MAX];
static int g_script_len;
static uint64_t g_script_t0;

static void load_pad_script(void) {
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"cross", PAD_BTN_CROSS},   {"circle", PAD_BTN_CIRCLE}, {"triangle", PAD_BTN_TRIANGLE},
        {"square", PAD_BTN_SQUARE}, {"options", PAD_BTN_OPTIONS}, {"up", PAD_BTN_UP},
        {"down", PAD_BTN_DOWN},     {"left", PAD_BTN_LEFT},     {"right", PAD_BTN_RIGHT},
        {"l1", PAD_BTN_L1},         {"r1", PAD_BTN_R1},         {"touchpad", PAD_BTN_TOUCHPAD},
    };
    g_script_t0 = plat_monotonic_ns();
    const char *p = getenv("SOTC_PAD_SCRIPT");
    while (p && *p && g_script_len < PAD_SCRIPT_MAX) {
        char *end;
        double t = strtod(p, &end);
        if (end == p || *end != ':') break;
        p = end + 1;
        size_t n = strcspn(p, ",");
        uint32_t bit = 0;
        for (size_t i = 0; i < SDL_arraysize(names); i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) bit = names[i].bit;
        if (!bit) log_warn("window: SOTC_PAD_SCRIPT: unknown button '%.*s'", (int)n, p);
        else g_script[g_script_len++] = (script_press){(uint64_t)(t * 1e9), bit};
        p += n;
        if (*p == ',') p++;
    }
    if (g_script_len) log_info("window: pad script with %d presses", g_script_len);
}

static uint32_t script_buttons(void) {
    uint64_t t = plat_monotonic_ns() - g_script_t0;
    uint32_t bits = 0;
    for (int i = 0; i < g_script_len; i++)
        if (t >= g_script[i].at_ns && t < g_script[i].at_ns + PAD_SCRIPT_HOLD_NS) bits |= g_script[i].button;
    return bits;
}

static void update_pad(void) {
    pad_input in = {.axes = {128, 128, 128, 128, 0, 0}};
    in.buttons = script_buttons();
    if (g_gamepad) {
        for (size_t i = 0; i < SDL_arraysize(k_gamepad_buttons); i++)
            if (SDL_GetGamepadButton(g_gamepad, k_gamepad_buttons[i].sdl))
                in.buttons |= k_gamepad_buttons[i].ps4;
        in.axes[PAD_AXIS_LEFT_X] = stick_value(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        in.axes[PAD_AXIS_LEFT_Y] = stick_value(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY));
        in.axes[PAD_AXIS_RIGHT_X] = stick_value(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
        in.axes[PAD_AXIS_RIGHT_Y] = stick_value(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
        in.axes[PAD_AXIS_L2] = trigger_value(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        in.axes[PAD_AXIS_R2] = trigger_value(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    }

    /* The keyboard adds to the gamepad; a held direction key overrides its
     * stick axis. SDL clears the key state when the window loses focus. */
    const bool *keys = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < SDL_arraysize(k_key_buttons); i++)
        if (keys[k_key_buttons[i].key]) in.buttons |= k_key_buttons[i].ps4;
    for (size_t i = 0; i < SDL_arraysize(k_key_axes); i++) {
        bool minus = keys[k_key_axes[i].minus], plus = keys[k_key_axes[i].plus];
        if (minus != plus) in.axes[k_key_axes[i].axis] = minus ? 0 : 255;
    }
    for (size_t i = 0; i < SDL_arraysize(k_key_triggers); i++)
        if (keys[k_key_triggers[i].key]) in.axes[k_key_triggers[i].axis] = 255;

    /* The digital L2/R2 bits follow the analog triggers (shadPS4: > 0x20). */
    if (in.axes[PAD_AXIS_L2] > 0x20) in.buttons |= PAD_BTN_L2;
    if (in.axes[PAD_AXIS_R2] > 0x20) in.buttons |= PAD_BTN_R2;
    pad_input_update(&in);
}

static void apply_pad_output(bool force) {
    if (!g_gamepad) return;
    uint64_t now = plat_monotonic_ns();
    unsigned rumble = atomic_load(&g_rumble);
    bool dirty = atomic_exchange(&g_output_dirty, false);
    /* PS4 vibration lasts until changed; SDL rumble needs a duration, so
     * renew a running one every 30 s. */
    bool renew = rumble && now - g_rumble_applied_ns > 30ull * 1000000000;
    if (!dirty && !force && !renew) return;
    SDL_RumbleGamepad(g_gamepad, (Uint16)((rumble >> 8) * 257), (Uint16)((rumble & 0xFF) * 257),
                      rumble ? 60000 : 0);
    g_rumble_applied_ns = now;
    unsigned light = atomic_load(&g_light);
    SDL_SetGamepadLED(g_gamepad, light >> 16 & 0xFF, light >> 8 & 0xFF, light & 0xFF);
}

/* ---- mappings for joysticks SDL does not know ------------------------------ */

/* Cheap USB pads (DirectInput HID, e.g. the DragonRise "Generic USB
 * Joystick" family) mostly share this layout: the face buttons from the top
 * going clockwise, then shoulders, triggers as buttons, select / start, stick
 * clicks; X/Y left stick, Z/Rz right stick; hat 0 the d-pad. SDL names are
 * positional (a = south = Cross, b = east = Circle, x = west = Square, y =
 * north = Triangle); back is Select (-> Touchpad), start is Options. */
static const struct { const char *sdl; char kind; int index; } k_generic_layout[] = {
    {"y", 'b', 0},             {"b", 'b', 1},             {"a", 'b', 2},
    {"x", 'b', 3},             {"leftshoulder", 'b', 4},  {"rightshoulder", 'b', 5},
    {"lefttrigger", 'b', 6},   {"righttrigger", 'b', 7},  {"back", 'b', 8},
    {"start", 'b', 9},         {"leftstick", 'b', 10},    {"rightstick", 'b', 11},
    {"leftx", 'a', 0},         {"lefty", 'a', 1},         {"rightx", 'a', 2},
    {"righty", 'a', 3},        {"dpup", 'h', 1},          {"dpright", 'h', 2},
    {"dpdown", 'h', 4},        {"dpleft", 'h', 8},
    /* No hat: some pads report the d-pad as buttons 12..15. */
    {"dpup", 'B', 12},         {"dpdown", 'B', 13},       {"dpleft", 'B', 14},
    {"dpright", 'B', 15},
};

bool window_pad_mapping(char *out, size_t size, const char *guid, const char *name, int axes,
                        int buttons, int hats, const char *layout) {
    char clean[128];
    size_t n = 0;
    /* A comma would end the name field. */
    for (const char *p = name && *name ? name : "Generic USB pad"; *p && n + 1 < sizeof clean; p++)
        clean[n++] = *p == ',' ? ' ' : *p;
    clean[n] = 0;
    int len = snprintf(out, size, "%s,%s,", guid, clean);
    if (len < 0 || (size_t)len >= size) return false;
    if (layout) {
        int m = snprintf(out + len, size - len, "%s", layout);
        return m > 0 && (size_t)m < size - len;
    }
    int mapped = 0;
    for (size_t i = 0; i < SDL_arraysize(k_generic_layout); i++) {
        char kind = k_generic_layout[i].kind;
        int idx = k_generic_layout[i].index;
        char item[48];
        if (kind == 'b' && idx < buttons) snprintf(item, sizeof item, "%s:b%d,", k_generic_layout[i].sdl, idx);
        else if (kind == 'B' && hats == 0 && buttons >= 16)
            snprintf(item, sizeof item, "%s:b%d,", k_generic_layout[i].sdl, idx);
        else if (kind == 'a' && idx < axes) snprintf(item, sizeof item, "%s:a%d,", k_generic_layout[i].sdl, idx);
        else if (kind == 'h' && hats > 0) snprintf(item, sizeof item, "%s:h0.%d,", k_generic_layout[i].sdl, idx);
        else continue;
        size_t il = strlen(item);
        if ((size_t)len + il >= size) return false;
        memcpy(out + len, item, il + 1);
        len += (int)il;
        mapped++;
    }
    return mapped > 0;
}

static const char *g_pad_template; /* SOTC_PAD_MAPPING's '*' line body, or NULL */
static char g_pad_template_buf[1024];
static bool g_automap = true;
#define SYNTH_MAX 8
static SDL_GUID g_synth[SYNTH_MAX];   /* GUIDs given a synthesized mapping */
static int g_synth_len;

static bool synthesized(SDL_GUID guid) {
    for (int i = 0; i < g_synth_len; i++)
        if (!memcmp(&g_synth[i], &guid, sizeof guid)) return true;
    return false;
}

/* SOTC_PAD_MAPPINGS / gamecontrollerdb.txt, then SOTC_PAD_MAPPING. */
static void load_pad_mappings(void) {
    const char *path = getenv("SOTC_PAD_MAPPINGS");
    if (path && *path) {
        int n = SDL_AddGamepadMappingsFromFile(path);
        if (n < 0) log_warn("window: SOTC_PAD_MAPPINGS=%s: %s", path, SDL_GetError());
        else log_info("window: %d pad mapping(s) from %s", n, path);
    } else {
        char exe_db[1024];
        const char *base = SDL_GetBasePath();
        snprintf(exe_db, sizeof exe_db, "%sgamecontrollerdb.txt", base ? base : "");
        const char *tries[] = {exe_db, "gamecontrollerdb.txt"};
        for (size_t i = 0; i < SDL_arraysize(tries); i++) {
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(tries[i], &info) || info.type != SDL_PATHTYPE_FILE) continue;
            int n = SDL_AddGamepadMappingsFromFile(tries[i]);
            if (n < 0) log_warn("window: %s: %s", tries[i], SDL_GetError());
            else log_info("window: %d pad mapping(s) from %s", n, tries[i]);
            break;
        }
    }

    const char *env = getenv("SOTC_PAD_MAPPING");
    for (const char *p = env; p && *p;) {
        size_t n = strcspn(p, ";");
        char line[1024];
        if (n > 0 && n < sizeof line) {
            memcpy(line, p, n);
            line[n] = 0;
            if (line[0] == '*' && line[1] == ',') {
                /* Template: keep the body after "*,name,". */
                const char *body = strchr(line + 2, ',');
                if (body) {
                    snprintf(g_pad_template_buf, sizeof g_pad_template_buf, "%s", body + 1);
                    g_pad_template = g_pad_template_buf;
                    log_info("window: SOTC_PAD_MAPPING: layout template for unknown pads");
                } else {
                    log_warn("window: SOTC_PAD_MAPPING: template needs '*,name,layout'");
                }
            } else if (SDL_AddGamepadMapping(line) < 0) {
                log_warn("window: SOTC_PAD_MAPPING: bad mapping '%s': %s", line, SDL_GetError());
            } else {
                log_info("window: SOTC_PAD_MAPPING: mapping added");
            }
        } else if (n > 0) {
            log_warn("window: SOTC_PAD_MAPPING: mapping too long");
        }
        p += n;
        if (*p == ';') p++;
    }
    const char *am = getenv("SOTC_PAD_AUTOMAP");
    g_automap = !(am && *am == '0');
}

/* A joystick SDL has no gamepad mapping for: give it one, so that it then
 * shows up as a gamepad (SDL sends SDL_EVENT_GAMEPAD_ADDED). */
static void map_joystick(SDL_JoystickID id) {
    if (!g_automap || SDL_IsGamepad(id)) return;
    SDL_Joystick *j = SDL_OpenJoystick(id);
    if (!j) {
        log_warn("window: cannot open joystick %u: %s", (unsigned)id, SDL_GetError());
        return;
    }
    SDL_GUID guid = SDL_GetJoystickGUID(j);
    char guid_str[64], mapping[1024];
    SDL_GUIDToString(guid, guid_str, sizeof guid_str);
    const char *name = SDL_GetJoystickName(j);
    int axes = SDL_GetNumJoystickAxes(j), buttons = SDL_GetNumJoystickButtons(j);
    int hats = SDL_GetNumJoystickHats(j);
    log_info("window: joystick \"%s\" (%04x:%04x, guid %s, %d axes, %d buttons, %d hats) has no "
             "gamepad mapping", name ? name : "?", SDL_GetJoystickVendor(j),
             SDL_GetJoystickProduct(j), guid_str, axes, buttons, hats);
    SDL_CloseJoystick(j);
    if (!window_pad_mapping(mapping, sizeof mapping, guid_str, name, axes, buttons, hats,
                            g_pad_template)) {
        log_warn("window: cannot build a mapping for it; keyboard only");
        return;
    }
    if (SDL_AddGamepadMapping(mapping) < 0) {
        log_warn("window: synthesized mapping rejected (%s): %s", SDL_GetError(), mapping);
        return;
    }
    log_info("window: synthesized mapping (%s): %s",
             g_pad_template ? "SOTC_PAD_MAPPING template" : "generic DirectInput layout", mapping);
    if (g_synth_len < SYNTH_MAX && !synthesized(guid)) g_synth[g_synth_len++] = guid;
}

static void map_all_joysticks(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    if (ids) {
        for (int i = 0; i < n; i++) map_joystick(ids[i]);
        SDL_free(ids);
    }
}

static void open_gamepad(SDL_JoystickID id) {
    if (g_gamepad) return;
    g_gamepad = SDL_OpenGamepad(id);
    if (!g_gamepad) {
        log_warn("window: cannot open gamepad %u: %s", (unsigned)id, SDL_GetError());
        return;
    }
    const char *name = SDL_GetGamepadName(g_gamepad);
    log_info("window: gamepad \"%s\" (%04x:%04x) connected%s", name ? name : "?",
             SDL_GetGamepadVendor(g_gamepad), SDL_GetGamepadProduct(g_gamepad),
             synthesized(SDL_GetJoystickGUIDForID(id)) ? " (synthesized mapping)" : "");
    apply_pad_output(true);
}

static void open_any_gamepad(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids) {
        for (int i = 0; i < n && !g_gamepad; i++) open_gamepad(ids[i]);
        SDL_free(ids);
    }
}

/* ---- window and loop ------------------------------------------------------ */

static atomic_bool g_guest_done;
static atomic_bool g_events_ready;
static Uint32 g_wake_event;

static void guest_watch(void *arg) {
    plat_thread_set_name("guest-watch");
    plat_thread_join((plat_thread *)arg);
    atomic_store(&g_guest_done, true);
    if (atomic_load(&g_events_ready)) {
        SDL_Event e;
        SDL_zero(e);
        e.type = g_wake_event;
        SDL_PushEvent(&e);
    }
}

static int wait_headless(plat_thread *guest_main) {
    publish_window(NULL);
    plat_thread_join(guest_main);
    log_info("guest main thread returned");
    return 0;
}

static bool env_flag(const char *name) {
    const char *v = getenv(name);
    return v && *v && *v != '0';
}

static void window_size(int *w, int *h) {
    *w = DEFAULT_W;
    *h = DEFAULT_H;
    const char *env = getenv("SOTC_WINDOW");
    int ew, eh;
    if (env && *env) {
        if (sscanf(env, "%dx%d", &ew, &eh) == 2 && ew >= 64 && eh >= 64 && ew <= 16384 &&
            eh <= 16384) {
            *w = ew;
            *h = eh;
            return;
        }
        log_warn("window: ignoring SOTC_WINDOW=%s (want WxH)", env);
    }
    /* Shrink the default to the usable desktop (taskbar and title bar off). */
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r) && r.w > 0 && r.h > 64) {
        int avail_h = r.h - 48;
        if (*w > r.w || *h > avail_h) {
            int nh = avail_h, nw = nh * 16 / 9;
            if (nw > r.w) {
                nw = r.w;
                nh = nw * 9 / 16;
            }
            *w = nw;
            *h = nh;
        }
    }
}

static bool handle_event(SDL_Window *win, const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return true;
    case SDL_EVENT_JOYSTICK_ADDED:
        map_joystick(e->jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        open_gamepad(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (g_gamepad && SDL_GetGamepadID(g_gamepad) == e->gdevice.which) {
            log_info("window: gamepad disconnected");
            SDL_CloseGamepad(g_gamepad);
            g_gamepad = NULL;
            open_any_gamepad();
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_F11 && !e->key.repeat && win)
            SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
        break;
    default:
        break;
    }
    return false;
}

int host_main_loop(plat_thread *guest_main) {
    if (env_flag("SOTC_HEADLESS")) {
        log_info("window: headless (SOTC_HEADLESS)");
        return wait_headless(guest_main);
    }

    SDL_SetHint(SDL_HINT_APP_NAME, WINDOW_TITLE);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* SDL would raise the Windows timer resolution to 1 ms, which changes
     * the guest's frame pacing; that is workstream P's call (platform
     * timing), so leave the system timer alone. */
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_warn("window: no video (%s), running headless", SDL_GetError());
        return wait_headless(guest_main);
    }
    audio_host_init(); /* main thread: SDL wants its subsystems started here */
    bool pads = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!pads) log_warn("window: no gamepad support (%s), keyboard only", SDL_GetError());
    else load_pad_mappings();

    int w, h;
    window_size(&w, &h);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    SDL_Window *win = SDL_CreateWindow(WINDOW_TITLE, w, h, flags | SDL_WINDOW_VULKAN);
    bool vulkan = win != NULL;
    if (!win) {
        log_warn("window: no Vulkan window (%s); trying a plain one for input", SDL_GetError());
        win = SDL_CreateWindow(WINDOW_TITLE, w, h, flags);
    }
    if (!win) {
        log_warn("window: cannot create a window (%s), running headless", SDL_GetError());
        SDL_Quit();
        return wait_headless(guest_main);
    }
    log_info("window: %dx%d%s", w, h, vulkan ? " (vulkan)" : " (no vulkan, not presentable)");
    publish_window(vulkan ? win : NULL);

    g_wake_event = SDL_RegisterEvents(1);
    atomic_store(&g_events_ready, g_wake_event != 0);
    plat_thread *watch = plat_thread_create(guest_watch, guest_main, 64 * 1024);
    if (!watch) {
        log_warn("window: cannot start the guest watcher; the window ignores the guest's end");
    } else {
        plat_thread_detach(watch);
    }
    if (pads) {
        map_all_joysticks();
        open_any_gamepad();
        if (!g_gamepad) log_info("window: no gamepad yet, keyboard only (docs/m3/pad.md)");
    }
    load_pad_script();
    g_title_ns = plat_monotonic_ns();
    g_title_flips = atomic_load(&g_flips);

    int code = 0;
    bool closed = false;
    while (!atomic_load(&g_guest_done)) {
        SDL_Event e;
        /* Wake at least every 8 ms to sample the keyboard and gamepad (the
         * guest reads the pad once per frame). */
        if (SDL_WaitEventTimeout(&e, 8)) {
            do {
                if (handle_event(win, &e)) closed = true;
            } while (SDL_PollEvent(&e));
        }
        if (closed) break;
        apply_pad_output(false);
        update_pad();
        update_title(win);
    }
    if (closed) log_info("window: closed, exiting");
    else log_info("guest main thread returned");
    /* Leave the window and SDL alone: the GPU may still present into the
     * window from another thread until the process exits. */
    return code;
}
