/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Runtime side of the sotcgpu contract (gpu/include/sotcgpu.h).
 *
 * - Backend choice: SOTC_GPU=vulkan|null. Vulkan falls back to the null PM4
 *   executor when sotcgpu_init fails.
 * - The sotcgpu_host callbacks: page protection for the GPU's page tracking
 *   (shadPS4 PageManager), backing writes and sparse reads (Memory::
 *   TryWriteBacking / CopySparseMemory), range queries, and the EOP / flip
 *   notifications.
 * - Page tracking state: one byte per 4 KiB page with the guest's protection
 *   and the GPU's restrictions; the host protection is always guest & GPU,
 *   whichever side changes it.
 * - An event thread: sotcgpu's callbacks (EOP interrupts, flips, "buffers
 *   consumed") run there, so sotcgpu's threads never wait on runtime locks
 *   (equeue, VideoOut, GnmDriver) that a faulting guest thread may hold.
 * - SOTC_SHOTS=N: a screenshot every N seconds (fractions allowed) to build/game/shots/NNNN.png.
 *
 * Semantics follow shadPS4 (commit 14ada48, GPL-2.0): src/core/memory.cpp,
 * src/video_core/page_manager.cpp, src/core/libraries/videoout/driver.cpp. */
#include "gpu_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#include "hle/pm4.h"
#include "log.h"
#include "platform.h"
#include "runtime.h"
#include "sotcgpu.h"
#include "window.h"

#define TP_SHIFT 12 /* PageManager tracks 4 KiB pages */
#define TP_PAGE (1ull << TP_SHIFT)
#define TP_CHUNK_BITS 16
#define TP_CHUNK (1u << TP_CHUNK_BITS)
#define TP_CHUNKS ((uint32_t)(PLAT_GUEST_LIMIT >> (TP_SHIFT + TP_CHUNK_BITS)))

/* Page byte: guest protection (PLAT_PROT_*), "known", GPU restrictions. */
enum { PT_PROT = 7, PT_KNOWN = 8, PT_DENY_R = 0x10, PT_DENY_W = 0x20, PT_DENY = 0x30 };

enum { ST_UNINIT = 0, ST_NULL = 1, ST_GPU = 2 };

static volatile int g_state;
static plat_mutex *g_init_lock;
static plat_mutex *g_track; /* page table and protection changes */
static uint8_t *volatile g_pt[TP_CHUNKS];
static void *(*g_window_provider)(void);

/* Event thread. */
typedef enum { EV_DONE, EV_EOP, EV_FLIP } ev_kind;
typedef struct bridge_ev {
    struct bridge_ev *next;
    ev_kind kind;
    gpu_bridge_done_fn fn;
    void *arg;
    int32_t buf;
    int64_t flip_arg;
} bridge_ev;
static plat_mutex *g_ev_lock;
static plat_cond *g_ev_cond;
static bridge_ev *g_ev_head, *g_ev_tail;

__attribute__((constructor)) static void gpu_bridge_ctor(void) {
    g_init_lock = plat_mutex_new();
    g_track = plat_mutex_new();
    g_ev_lock = plat_mutex_new();
    g_ev_cond = plat_cond_new();
}

bool gpu_bridge_active(void) { return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == ST_GPU; }

void gpu_bridge_set_window_provider(void *(*fn)(void)) { g_window_provider = fn; }

/* ---- page table ---------------------------------------------------------- */

static inline uint8_t pt_get(uint64_t page) {
    const uint8_t *c = g_pt[page >> TP_CHUNK_BITS];
    return c ? c[page & (TP_CHUNK - 1)] : 0;
}

/* Caller holds g_track. Chunks are never freed, so pt_get needs no lock. */
static inline uint8_t *pt_slot(uint64_t page) {
    uint8_t *c = g_pt[page >> TP_CHUNK_BITS];
    if (!c) {
        c = calloc(TP_CHUNK, 1);
        if (!c) log_fatal("gpu: out of memory for the page table");
        __atomic_store_n(&g_pt[page >> TP_CHUNK_BITS], c, __ATOMIC_RELEASE);
    }
    return &c[page & (TP_CHUNK - 1)];
}

static inline int effective(uint8_t e) {
    if (e & PT_DENY_R) return PLAT_PROT_NONE; /* x86 has no write-only pages */
    int p = e & PT_PROT;
    if (e & PT_DENY_W) p &= ~PLAT_PROT_W;
    return p;
}

/* Page range [*first, *end) for [addr, addr+size), clipped to the GPU range. */
static bool page_range(uint64_t addr, uint64_t size, uint64_t *first, uint64_t *end) {
    if (size == 0 || addr >= PLAT_GUEST_LIMIT) return false;
    uint64_t hi = size > PLAT_GUEST_LIMIT - addr ? PLAT_GUEST_LIMIT : addr + size;
    *first = addr >> TP_SHIFT;
    *end = (hi + TP_PAGE - 1) >> TP_SHIFT;
    return true;
}

/* Fill in unknown pages from the host's current protection (memory mapped
 * before the GPU started, or outside memory.c such as the loaded image). No
 * GPU restriction exists on such pages, so the host protection is the
 * guest's. Caller holds g_track. */
static void ensure_known(uint64_t first, uint64_t end) {
    for (uint64_t pg = first; pg < end;) {
        if (pt_get(pg) & PT_KNOWN) { pg++; continue; }
        uint64_t run_end = 0;
        int prot = plat_query_prot((void *)(uintptr_t)(pg << TP_SHIFT), &run_end);
        uint64_t stop = run_end ? (run_end + TP_PAGE - 1) >> TP_SHIFT : pg + 1;
        if (stop > end) stop = end;
        if (stop <= pg) stop = pg + 1;
        for (; pg < stop; pg++) {
            if (prot < 0) continue; /* unmapped: stays unknown */
            uint8_t *s = pt_slot(pg);
            if (!(*s & PT_KNOWN)) *s = (uint8_t)(PT_KNOWN | prot);
        }
    }
}

/* Apply the effective protection of known pages in [first, end). With
 * `old_eff` (per page, from before a change), only pages whose effective
 * protection changed are touched. Runs of equal protection go in one call.
 * Caller holds g_track. Returns false if a host call failed. */
static bool apply_range(uint64_t first, uint64_t end, const int8_t *old_eff) {
    bool ok = true;
    uint64_t run = 0, run_len = 0;
    int run_prot = -1;
    for (uint64_t pg = first; pg <= end; pg++) {
        int prot = -1;
        if (pg < end) {
            uint8_t e = pt_get(pg);
            if (e & PT_KNOWN) {
                prot = effective(e);
                if (old_eff && old_eff[pg - first] == prot) prot = -1;
            }
        }
        if (prot != run_prot || pg == end) {
            if (run_prot >= 0 && run_len)
                ok &= plat_protect((void *)(uintptr_t)(run << TP_SHIFT), run_len << TP_SHIFT,
                                   run_prot);
            run = pg;
            run_len = 0;
            run_prot = prot;
        }
        run_len++;
    }
    return ok;
}

void gpu_bridge_track_lock(void) { plat_mutex_lock(g_track); }
void gpu_bridge_track_unlock(void) { plat_mutex_unlock(g_track); }

void gpu_bridge_guest_map(uint64_t addr, uint64_t size, int prot) {
    uint64_t first, end;
    if (!gpu_bridge_active() || !page_range(addr, size, &first, &end)) return;
    plat_mutex_lock(g_track);
    for (uint64_t pg = first; pg < end; pg++) *pt_slot(pg) = (uint8_t)(PT_KNOWN | (prot & PT_PROT));
    plat_mutex_unlock(g_track);
}

void gpu_bridge_guest_remap(uint64_t addr, uint64_t size, int prot) {
    uint64_t first, end;
    if (!gpu_bridge_active() || !page_range(addr, size, &first, &end)) return;
    plat_mutex_lock(g_track);
    bool restricted = false;
    for (uint64_t pg = first; pg < end; pg++) {
        uint8_t *s = pt_slot(pg);
        uint8_t deny = (*s & PT_KNOWN) ? (*s & PT_DENY) : 0;
        restricted |= deny != 0;
        *s = (uint8_t)(PT_KNOWN | deny | (prot & PT_PROT));
    }
    /* The new mapping got the guest protection; put the GPU's back. */
    if (restricted) apply_range(first, end, NULL);
    plat_mutex_unlock(g_track);
}

void gpu_bridge_guest_unmap(uint64_t addr, uint64_t size) {
    uint64_t first, end;
    if (!gpu_bridge_active() || !page_range(addr, size, &first, &end)) return;
    plat_mutex_lock(g_track);
    for (uint64_t pg = first; pg < end; pg++)
        if (g_pt[pg >> TP_CHUNK_BITS]) *pt_slot(pg) = 0;
    plat_mutex_unlock(g_track);
}

bool gpu_bridge_guest_protect(uint64_t addr, uint64_t size, int prot) {
    uint64_t first, end;
    if (!gpu_bridge_active() || !page_range(addr, size, &first, &end))
        return plat_protect((void *)(uintptr_t)addr, size, prot);
    plat_mutex_lock(g_track);
    for (uint64_t pg = first; pg < end; pg++) {
        uint8_t *s = pt_slot(pg);
        uint8_t deny = (*s & PT_KNOWN) ? (*s & PT_DENY) : 0;
        *s = (uint8_t)(PT_KNOWN | deny | (prot & PT_PROT));
    }
    bool ok = apply_range(first, end, NULL);
    plat_mutex_unlock(g_track);
    return ok;
}

/* ---- host callbacks: protection ------------------------------------------ */

static bool host_protect(uint64_t addr, uint64_t size, bool allow_read, bool allow_write) {
    uint64_t first, end;
    if (!page_range(addr, size, &first, &end)) return false;
    uint8_t deny = (uint8_t)((allow_read ? 0 : PT_DENY_R) | (allow_write ? 0 : PT_DENY_W));
    plat_mutex_lock(g_track);
    ensure_known(first, end);
    /* Remember the old effective protection so unchanged pages are skipped:
     * the page manager re-protects ranges it already protected. Chunked to
     * keep the stack small (host code runs on guest stacks). */
    enum { BATCH = 256 };
    int8_t old_eff[BATCH];
    bool ok = true;
    for (uint64_t b = first; b < end; b += BATCH) {
        uint64_t be = end - b > BATCH ? b + BATCH : end;
        for (uint64_t pg = b; pg < be; pg++) {
            uint8_t e = pt_get(pg);
            old_eff[pg - b] = (int8_t)((e & PT_KNOWN) ? effective(e) : -1);
            if (e & PT_KNOWN) *pt_slot(pg) = (uint8_t)((e & ~PT_DENY) | deny);
        }
        ok &= apply_range(b, be, old_eff);
    }
    plat_mutex_unlock(g_track);
    return ok;
}

bool gpu_bridge_force_copy(uint64_t guest, void *host, uint64_t size, bool to_guest) {
    uint64_t first, end;
    if (!size) return true;
    if (!page_range(guest, size, &first, &end) || guest + size > PLAT_GUEST_LIMIT) return false;
    if (!gpu_bridge_active()) { /* no GPU restrictions; don't fill the table */
        if (!plat_is_readable((const void *)(uintptr_t)guest, size)) return false;
        if (to_guest) memcpy((void *)(uintptr_t)guest, host, size);
        else memcpy(host, (const void *)(uintptr_t)guest, size);
        return true;
    }
    int need = to_guest ? PLAT_PROT_W : PLAT_PROT_R;
    plat_mutex_lock(g_track);
    ensure_known(first, end);
    bool blocked = false;
    for (uint64_t pg = first; pg < end; pg++) {
        uint8_t e = pt_get(pg);
        if (!(e & PT_KNOWN)) { /* unmapped */
            plat_mutex_unlock(g_track);
            return false;
        }
        if (!(effective(e) & need)) blocked = true;
    }
    if (blocked)
        plat_protect((void *)(uintptr_t)(first << TP_SHIFT), (end - first) << TP_SHIFT,
                     PLAT_PROT_R | PLAT_PROT_W);
    if (to_guest) memcpy((void *)(uintptr_t)guest, host, size);
    else memcpy(host, (const void *)(uintptr_t)guest, size);
    if (blocked) apply_range(first, end, NULL);
    plat_mutex_unlock(g_track);
    return true;
}

/* Fault handler hook. Lock-free unless the GPU caused the fault. */
bool gpu_bridge_on_fault(uint64_t addr, bool is_write, uint64_t rip) {
    if (!gpu_bridge_active()) return false;
    static _Thread_local uint64_t last_addr;
    static _Thread_local uint32_t repeats;
    repeats = addr == last_addr ? repeats + 1 : 0;
    last_addr = addr;
    if (repeats > 10000) { /* sotcgpu "handled" it but nothing changed */
        log_error("gpu: fault at %#llx keeps repeating; giving up", (unsigned long long)addr);
        return false;
    }
    uint8_t e = addr < PLAT_GUEST_LIMIT ? pt_get(addr >> TP_SHIFT) : 0;
    /* A fault in host code that isn't on a GPU-watched page: only sotcgpu's
     * generated code (the SRT walker) can claim it, by its rip. */
    if (!(e & PT_KNOWN))
        return !loader_module_at((void *)(uintptr_t)rip) && sotcgpu_handle_fault(addr, is_write, rip);
    bool gpu_blocked = (e & PT_DENY_R) || (is_write && (e & PT_DENY_W));
    if (gpu_blocked) return sotcgpu_handle_fault(addr, is_write, rip);
    /* The GPU lifted its restriction between the fault and now: retry, a few
     * times at most (the host protection may disagree for another reason). */
    int prot = e & PT_PROT;
    bool allowed = is_write ? (prot & PLAT_PROT_W) : (prot & (PLAT_PROT_R | PLAT_PROT_X));
    return allowed && repeats < 8;
}

void gpu_bridge_describe(uint64_t addr, char *buf, size_t len) {
    uint8_t e = addr < PLAT_GUEST_LIMIT ? pt_get(addr >> TP_SHIFT) : 0;
    int host = plat_query_prot((const void *)(uintptr_t)addr, NULL);
    snprintf(buf, len, "gpu %s, tracking %s guest prot %d%s%s, host prot %d",
             gpu_bridge_active() ? "active" : "off", (e & PT_KNOWN) ? "known," : "unknown,",
             e & PT_PROT, (e & PT_DENY_R) ? " deny-r" : "", (e & PT_DENY_W) ? " deny-w" : "", host);
}

/* ---- host callbacks: memory access --------------------------------------- */

static bool host_write_backing(uint64_t addr, const void *src, uint64_t size) {
    return hle_memory_write_backing(addr, src, size);
}

static void host_read_sparse(void *dst, uint64_t addr, uint64_t size) {
    hle_memory_read_sparse(dst, addr, size);
}

static uint64_t host_clamp_range(uint64_t addr, uint64_t size) {
    return hle_memory_clamp_range(addr, size);
}

static bool host_is_gpu_mapping(uint64_t addr, uint64_t size) {
    return hle_memory_is_gpu_mapping(addr, size);
}

/* ---- host callbacks: events ------------------------------------------------ */

static void ev_push(bridge_ev *ev) {
    ev->next = NULL;
    plat_mutex_lock(g_ev_lock);
    if (g_ev_tail) g_ev_tail->next = ev;
    else g_ev_head = ev;
    g_ev_tail = ev;
    plat_cond_signal(g_ev_cond);
    plat_mutex_unlock(g_ev_lock);
}

static bridge_ev *ev_new(ev_kind kind) {
    bridge_ev *ev = calloc(1, sizeof(*ev));
    if (!ev) log_fatal("gpu: out of memory");
    ev->kind = kind;
    return ev;
}

static void host_eop_interrupt(void) { ev_push(ev_new(EV_EOP)); }

static void host_flip_done(int32_t buf_index, int64_t flip_arg) {
    bridge_ev *ev = ev_new(EV_FLIP);
    ev->buf = buf_index;
    ev->flip_arg = flip_arg;
    ev_push(ev);
}

/* sotcgpu's `done` for a submission: the event itself is the argument. */
static void bridge_done(void *arg) { ev_push(arg); }

static void host_log(int level, const char *msg) {
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    if (level >= 3 && log_level() < level) return;
    log_write(level, "gpu: %s", msg);
}

static void event_thread(void *unused) {
    (void)unused;
    plat_thread_set_name("sotc:gpu-events");
    for (;;) {
        plat_mutex_lock(g_ev_lock);
        while (!g_ev_head) plat_cond_wait(g_ev_cond, g_ev_lock, UINT64_MAX);
        bridge_ev *list = g_ev_head;
        g_ev_head = g_ev_tail = NULL;
        plat_mutex_unlock(g_ev_lock);
        while (list) {
            bridge_ev *ev = list;
            list = ev->next;
            switch (ev->kind) {
            case EV_DONE: ev->fn(ev->arg); break;
            case EV_EOP: pm4_raise_eop_interrupt(); break;
            case EV_FLIP: videoout_flip_done(ev->buf, ev->flip_arg); break;
            }
            free(ev);
        }
    }
}

void gpu_bridge_submit(uint32_t queue, const uint32_t *dcb, uint32_t dcb_dwords,
                       const uint32_t *ccb, uint32_t ccb_dwords, gpu_bridge_done_fn done,
                       void *arg) {
    bridge_ev *ev = NULL;
    if (done) {
        ev = ev_new(EV_DONE);
        ev->fn = done;
        ev->arg = arg;
    }
    if (queue == PM4_QUEUE_GFX)
        sotcgpu_submit_gfx(dcb, dcb_dwords, ccb, ccb_dwords, ev ? bridge_done : NULL, ev);
    else
        sotcgpu_submit_compute(queue, dcb, dcb_dwords, ev ? bridge_done : NULL, ev);
}

/* ---- map notifications ------------------------------------------------------ */

void gpu_bridge_notify_map(uint64_t addr, uint64_t size) {
    if (gpu_bridge_active() && size && addr + size <= PLAT_GUEST_LIMIT)
        sotcgpu_map_memory(addr, size);
}

void gpu_bridge_notify_unmap(uint64_t addr, uint64_t size) {
    if (gpu_bridge_active() && size && addr + size <= PLAT_GUEST_LIMIT)
        sotcgpu_unmap_memory(addr, size);
}

static void replay_map(uint64_t addr, uint64_t size, void *ctx) {
    (void)ctx;
    gpu_bridge_notify_map(addr, size);
}

/* ---- screenshots --------------------------------------------------------------- */

static void shots_thread(void *arg) {
    uint64_t every_us = (uint64_t)(uintptr_t)arg;
    plat_thread_set_name("sotc:gpu-shots");
    make_dir("build");
    make_dir("build/game");
    make_dir("build/game/shots");
    plat_mutex *m = plat_mutex_new();
    plat_cond *c = plat_cond_new(); /* never signalled: a sleep that P's timer work also improves */
    for (unsigned n = 0;; n++) {
        plat_mutex_lock(m);
        plat_cond_wait(c, m, every_us);
        plat_mutex_unlock(m);
        char path[64];
        snprintf(path, sizeof path, "build/game/shots/%04u.png", n);
        if (sotcgpu_screenshot(path)) log_info("gpu: screenshot %s", path);
        else log_warn("gpu: screenshot %s failed (nothing presented yet?)", path);
    }
}

/* ---- init ------------------------------------------------------------------------ */

static const sotcgpu_host g_host = {
    .protect = host_protect,
    .write_backing = host_write_backing,
    .read_sparse = host_read_sparse,
    .clamp_range = host_clamp_range,
    .is_gpu_mapping = host_is_gpu_mapping,
    .eop_interrupt = host_eop_interrupt,
    .flip_done = host_flip_done,
    .log = host_log,
};

static bool env_on(const char *name) {
    const char *v = getenv(name);
    return v && *v == '1';
}

static bool init_gpu(void) {
    void *window = g_window_provider ? g_window_provider() : window_sdl_handle();
    if (!window) log_warn("gpu: no window; sotcgpu will probably refuse to start");
    plat_phys_backing(); /* write_backing for direct memory; falls back to unprotecting */

    static char cache_dir[512];
    const char *user = getenv("SOTC_USER_DIR");
    snprintf(cache_dir, sizeof cache_dir, "%s/gpu_cache", user && *user ? user : "user");
    sotcgpu_config cfg = {
        .sdl_window = window,
        .width = 1920,
        .height = 1080,
        .neo = env_on("SOTC_NEO"),
        .validation = env_on("SOTC_VK_VALIDATION"),
        .cache_dir = cache_dir,
    };
    int rc = sotcgpu_init(&g_host, &cfg);
    if (rc != 0) {
        log_info("gpu: sotcgpu_init returned %d; using the null PM4 executor", rc);
        return false;
    }
    plat_thread *t = plat_thread_create(event_thread, NULL, 256 * 1024);
    if (!t) log_fatal("gpu: cannot start the event thread");
    plat_thread_detach(t);
    return true;
}

void gpu_bridge_init(void) {
    if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != ST_UNINIT) return;
    plat_mutex_lock(g_init_lock);
    if (g_state == ST_UNINIT) {
        const char *env = getenv("SOTC_GPU");
        bool want = g_rt.module_count > 0;
        if (env && *env) {
            want = strcmp(env, "vulkan") == 0;
            if (!want && strcmp(env, "null") != 0)
                log_warn("gpu: SOTC_GPU=%s is not vulkan or null; using null", env);
        }
        bool gpu = want && init_gpu();
        if (!want) log_info("gpu: null PM4 executor");
        __atomic_store_n(&g_state, gpu ? ST_GPU : ST_NULL, __ATOMIC_RELEASE);
        if (gpu) {
            log_info("gpu: sotcgpu active (%ux%u)", 1920u, 1080u);
            /* Mappings made before the GPU started (set active first: a map
             * racing with this replay is then reported, at worst twice). */
            hle_memory_for_each_mapping(replay_map, NULL);
            if (g_rt.image && (uint64_t)(uintptr_t)g_rt.image + g_rt.image_size <= PLAT_GUEST_LIMIT)
                gpu_bridge_notify_map((uint64_t)(uintptr_t)g_rt.image, g_rt.image_size);
            const char *shots = getenv("SOTC_SHOTS");
            double every = shots ? strtod(shots, NULL) : 0; /* seconds; 0.1 for bursts */
            if (every > 0) {
                uint64_t every_us = (uint64_t)(every * 1e6);
                plat_thread *t = plat_thread_create(shots_thread, (void *)(uintptr_t)every_us,
                                                    256 * 1024);
                if (t) plat_thread_detach(t);
                log_info("gpu: screenshot every %g s to build/game/shots/", every);
            }
        }
    }
    plat_mutex_unlock(g_init_lock);
}
