/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Null PM4 executor: walks the command buffers the game submits and applies
 * only what the CPU can observe (label/fence writes, memory copies and fills,
 * semaphores) plus end-of-pipe interrupts. Draws, dispatches and register
 * state are ignored; M3 replaces this with shadPS4's video_core.
 *
 * Packet layouts and semantics follow shadPS4 (commit 14ada48, GPL-2.0):
 * src/video_core/amdgpu/pm4_cmds.h, pm4_opcodes.h and liverpool.cpp
 * (ProcessGraphics / ProcessCompute / ProcessCeUpdate), and the GnmDriver EOP
 * interrupt plumbing in src/core/libraries/gnmdriver/gnmdriver.cpp. */
#include "pm4.h"

#include <stdlib.h>
#include <string.h>

#include "hle.h"

/* ---- packet constants (shadPS4 pm4_opcodes.h / pm4_cmds.h) -------------- */

enum {
    IT_NOP = 0x10,
    IT_INDIRECT_BUFFER_CONST = 0x33,
    IT_WRITE_DATA = 0x37,
    IT_MEM_SEMAPHORE = 0x39,
    IT_WAIT_REG_MEM = 0x3C,
    IT_INDIRECT_BUFFER = 0x3F,
    IT_EVENT_WRITE_EOP = 0x47,
    IT_EVENT_WRITE_EOS = 0x48,
    IT_RELEASE_MEM = 0x49,
    IT_DMA_DATA = 0x50,
};

/* DataSelect for EOP / RELEASE_MEM. */
enum { DATA_NONE = 0, DATA_32LOW = 1, DATA_64 = 2, DATA_GPU_CLOCK = 3, DATA_PERF_COUNTER = 4,
       DATA_GDS = 5 };

/* PM4CmdNop payloads GnmDriver emits for flips (shadPS4 pm4_cmds.h). */
enum {
    NOP_PATCHED_FLIP = 0x68750776u,
    NOP_PREPARE_FLIP = 0x68750777u,
    NOP_PREPARE_FLIP_LABEL = 0x68750778u,
    NOP_PREPARE_FLIP_INTERRUPT = 0x68750780u,
    NOP_PREPARE_FLIP_INTERRUPT_LABEL = 0x68750781u,
};

/* DMA_DATA selects. */
enum { DMA_DST_MEMORY = 0, DMA_DST_GDS = 1, DMA_DST_MEMORY_L2 = 3 };
enum { DMA_SRC_MEMORY = 0, DMA_SRC_GDS = 1, DMA_SRC_DATA = 2, DMA_SRC_MEMORY_L2 = 3 };

#define PM4_MAX_IB_DEPTH 4
#define PM4_WAIT_TIMEOUT_NS 50000000ull /* 50 ms, then give up on a WAIT_REG_MEM */
#define PM4_GPU_CLOCK_HZ 800000000ull   /* sceGnmGetGpuCoreClockFrequency, base PS4 */

/* ---- EOP equeue registry ------------------------------------------------ */

#define PM4_MAX_EOP 32

typedef struct eop_reg {
    equeue *eq;
    uint64_t ident;
} eop_reg;

static plat_mutex *g_lock;
static eop_reg g_eop[PM4_MAX_EOP];
static int g_eop_count;
static volatile uint64_t g_irq_count; /* interrupts raised; read by tests */

__attribute__((constructor)) static void pm4_init(void) { g_lock = plat_mutex_new(); }

bool pm4_add_eop_equeue(equeue *eq, uint64_t ident, void *udata) {
    if (!eq) return false;
    /* The kernel adds EV_CLEAR to non-user events (shadPS4 EqueueInternal::AddEvent). */
    if (!equeue_add_event(eq, ident, EVFILT_GRAPHICS_CORE, EV_ADD | EV_CLEAR, udata))
        return false;
    bool ok = true;
    plat_mutex_lock(g_lock);
    int i;
    for (i = 0; i < g_eop_count; i++)
        if (g_eop[i].eq == eq && g_eop[i].ident == ident) break;
    if (i == g_eop_count) {
        if (g_eop_count < PM4_MAX_EOP)
            g_eop[g_eop_count++] = (eop_reg){eq, ident};
        else
            ok = false;
    }
    plat_mutex_unlock(g_lock);
    if (!ok) log_error("pm4: too many EOP equeue registrations");
    log_debug("pm4: EOP event %#llx registered on eq %p", (unsigned long long)ident, (void *)eq);
    return ok;
}

bool pm4_remove_eop_equeue(equeue *eq, uint64_t ident) {
    bool found = false;
    plat_mutex_lock(g_lock);
    for (int i = 0; i < g_eop_count; i++)
        if (g_eop[i].eq == eq && g_eop[i].ident == ident) {
            g_eop[i] = g_eop[--g_eop_count];
            found = true;
            break;
        }
    plat_mutex_unlock(g_lock);
    if (eq) equeue_remove_event(eq, ident, EVFILT_GRAPHICS_CORE);
    return found;
}

/* Raise an interrupt: trigger every registered (eq, ident). The event data is
 * the ident, as sceGnmGetEqEventType expects (shadPS4 sceGnmAddEqEvent).
 * Triggers happen outside the lock. */
static void pm4_signal_irq(void) {
    eop_reg snap[PM4_MAX_EOP];
    plat_mutex_lock(g_lock);
    int n = g_eop_count;
    memcpy(snap, g_eop, (size_t)n * sizeof(snap[0]));
    g_irq_count++;
    plat_mutex_unlock(g_lock);
    for (int i = 0; i < n; i++)
        equeue_trigger(snap[i].eq, snap[i].ident, EVFILT_GRAPHICS_CORE, snap[i].ident);
}

/* Test hook (not in pm4.h, which is a contract shared with G1). */
uint64_t pm4_debug_irq_count(void) { return g_irq_count; }

/* ---- guest memory access ------------------------------------------------ */

/* A GPU address the null executor will touch: non-null, below the guest
 * limit and aligned. Anything else is logged and skipped. */
static bool gpu_addr_ok(uint64_t addr, uint64_t size, uint64_t align) {
    if (addr < 0x10000 || addr >= PLAT_GUEST_LIMIT || size > PLAT_GUEST_LIMIT - addr ||
        (addr & (align - 1))) {
        log_warn("pm4: bad GPU address %#llx (size %llu), skipped", (unsigned long long)addr,
                 (unsigned long long)size);
        return false;
    }
    return true;
}

static void mem_write(uint64_t addr, uint64_t value, uint32_t bytes) {
    if (!gpu_addr_ok(addr, bytes, 4)) return;
    memcpy((void *)(uintptr_t)addr, &value, bytes); /* little-endian: low bytes first */
}

static uint64_t gpu_clock64(void) { return plat_monotonic_ns(); }

static uint64_t gpu_perf_counter(void) {
    uint64_t f = plat_tsc_frequency();
    if (!f) return 0;
    uint64_t t = plat_tsc(); /* t * gpu / f without 128-bit division */
    return t / f * PM4_GPU_CLOCK_HZ + t % f * PM4_GPU_CLOCK_HZ / f;
}

/* The data/interrupt part of EVENT_WRITE_EOP and RELEASE_MEM. */
static void signal_fence(uint64_t addr, uint32_t data_sel, uint32_t int_sel, uint32_t lo,
                         uint32_t hi) {
    switch (data_sel) {
    case DATA_NONE: break;
    case DATA_32LOW: mem_write(addr, lo, 4); break;
    case DATA_64: mem_write(addr, (uint64_t)hi << 32 | lo, 8); break;
    case DATA_GPU_CLOCK: mem_write(addr, gpu_clock64(), 8); break;
    case DATA_PERF_COUNTER: mem_write(addr, gpu_perf_counter(), 8); break;
    case DATA_GDS: log_debug("pm4: fence GDS store ignored"); break;
    default: log_warn("pm4: unknown fence data_sel %u", data_sel); break;
    }
    if (int_sel != 0) pm4_signal_irq();
}

static bool wait_test(uint32_t function, uint32_t value, uint32_t ref, uint32_t mask) {
    value &= mask;
    switch (function) {
    case 0: return true;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

/* ---- packet walker ------------------------------------------------------ */

static bool walk(const uint32_t *buf, uint32_t dwords, int depth, bool ccb);

/* Workers wait like the GPU (forever, warning now and then); the synchronous
 * pm4_execute gives up after PM4_WAIT_TIMEOUT_NS so it can't hang a caller. */
static _Thread_local bool t_is_worker;

/* Poll until `ready` or the timeout; true if satisfied. */
#define PM4_WAIT_WARN_NS 2000000000ull
static bool wait_until(bool (*ready)(const void *), const void *ctx, const char *what,
                       uint64_t addr) {
    uint64_t t0 = plat_monotonic_ns(), warn_at = t0 + PM4_WAIT_WARN_NS;
    uint32_t spins = 0;
    while (!ready(ctx)) {
        uint64_t now = plat_monotonic_ns();
        if (!t_is_worker && now - t0 > PM4_WAIT_TIMEOUT_NS) {
            log_warn("pm4: %s at %#llx timed out; continuing", what, (unsigned long long)addr);
            return false;
        }
        if (t_is_worker && now > warn_at) {
            log_warn("pm4: queue still waiting on %s at %#llx (%.1f s)", what,
                     (unsigned long long)addr, (double)(now - t0) / 1e9);
            warn_at = now + PM4_WAIT_WARN_NS * 5;
        }
        if (++spins < 64) plat_yield();
        else plat_sleep_us(50);
    }
    return true;
}

typedef struct reg_wait { volatile uint32_t *v; uint32_t fn, ref, mask; } reg_wait;
static bool reg_wait_ready(const void *p) {
    const reg_wait *w = p;
    return wait_test(w->fn, *w->v, w->ref, w->mask);
}
static bool sem_ready(const void *p) {
    return __atomic_load_n((volatile uint64_t *)p, __ATOMIC_ACQUIRE) != 0;
}

static void exec_type3(uint32_t op, const uint32_t *p, uint32_t body, int depth, bool ccb) {
    /* p[0] is the header, p[1..body] the body. Each case checks `body`. */
    switch (op) {
    case IT_NOP: {
        if (body < 1) break;
        uint32_t kind = p[1];
        if (kind == NOP_PREPARE_FLIP_LABEL && body >= 4) {
            /* Unpatched flip marker: [type, addr_lo, addr_hi, label]. */
            mem_write((uint64_t)p[3] << 32 | (p[2] & ~3u), p[4], 4);
        } else if (kind == NOP_PREPARE_FLIP_INTERRUPT_LABEL && body >= 4) {
            mem_write((uint64_t)(p[3] & 0xffff) << 32 | (p[2] & ~3u), p[4], 4);
            pm4_signal_irq();
        } else if (kind == NOP_PREPARE_FLIP_INTERRUPT) {
            pm4_signal_irq();
        }
        /* NOP_PATCHED_FLIP / NOP_PREPARE_FLIP / markers: no memory effect; the
         * flip itself arrives through videoout_submit_flip. */
        break;
    }
    case IT_WRITE_DATA: {
        if (body < 3) goto short_packet;
        uint32_t ctl = p[1];
        uint32_t dst_sel = (ctl >> 8) & 0xf;
        bool one_addr = (ctl >> 16) & 1;
        uint64_t addr = (uint64_t)p[3] << 32 | p[2];
        uint32_t n = body - 3;
        if (dst_sel != 1 && dst_sel != 2 && dst_sel != 5) {
            log_debug("pm4: WRITE_DATA dst_sel %u ignored", dst_sel);
            break;
        }
        if (n == 0) break;
        if (one_addr) {
            mem_write(addr, p[3 + n], 4); /* every dword to one address: the last wins */
        } else if (gpu_addr_ok(addr, (uint64_t)n * 4, 4)) {
            memcpy((void *)(uintptr_t)addr, p + 4, (size_t)n * 4);
        }
        break;
    }
    case IT_EVENT_WRITE_EOP: {
        if (body < 5) goto short_packet;
        uint32_t dc = p[3];
        uint64_t addr = (uint64_t)(dc & 0xffff) << 32 | p[2];
        signal_fence(addr, dc >> 29, (dc >> 24) & 3, p[4], p[5]);
        break;
    }
    case IT_RELEASE_MEM: {
        if (body < 6) goto short_packet;
        uint32_t dc = p[2];
        uint64_t addr = (uint64_t)p[4] << 32 | p[3];
        signal_fence(addr, dc >> 29, (dc >> 24) & 7, p[5], p[6]);
        break;
    }
    case IT_EVENT_WRITE_EOS: {
        if (body < 4) goto short_packet;
        uint32_t cmd = p[3] >> 29;
        uint64_t addr = (uint64_t)(p[3] & 0xffff) << 32 | p[2];
        if (cmd == 2) mem_write(addr, p[4], 4); /* SignalFence */
        else if (cmd != 1) log_warn("pm4: EVENT_WRITE_EOS command %u", cmd);
        /* cmd 1 = GDS store: no GDS in the null executor. */
        break;
    }
    case IT_DMA_DATA: {
        if (body < 6) goto short_packet;
        uint32_t ctl = p[1];
        uint32_t dst_sel = (ctl >> 20) & 3, src_sel = (ctl >> 29) & 3;
        uint32_t cmd = p[6];
        uint32_t bytes = cmd & 0x1fffff;
        bool sas = (cmd >> 26) & 1, das = (cmd >> 27) & 1, saic = (cmd >> 28) & 1;
        uint64_t src = (uint64_t)p[3] << 32 | p[2];
        uint64_t dst = (uint64_t)p[5] << 32 | p[4];
        if (p[4] == 0x3022C) break; /* register write used by Gnm; shadPS4 skips it too */
        bool dst_mem = dst_sel == DMA_DST_MEMORY || dst_sel == DMA_DST_MEMORY_L2;
        bool src_mem = src_sel == DMA_SRC_MEMORY || src_sel == DMA_SRC_MEMORY_L2;
        if (!dst_mem || das) {
            log_debug("pm4: DMA_DATA to GDS/register ignored");
            break;
        }
        if (bytes == 0) break;
        if (src_sel == DMA_SRC_DATA) {
            if (!gpu_addr_ok(dst, bytes, 4)) break;
            uint32_t v = p[2];
            uint8_t *d = (uint8_t *)(uintptr_t)dst;
            for (uint32_t i = 0; i + 4 <= bytes; i += 4) memcpy(d + i, &v, 4);
            if (bytes & 3) memcpy(d + (bytes & ~3u), &v, bytes & 3);
        } else if (src_mem && !sas) {
            if (!gpu_addr_ok(dst, bytes, 1) || !gpu_addr_ok(src, saic ? 4 : bytes, 1)) break;
            if (saic) {
                uint32_t v;
                memcpy(&v, (const void *)(uintptr_t)src, 4);
                uint8_t *d = (uint8_t *)(uintptr_t)dst;
                for (uint32_t i = 0; i + 4 <= bytes; i += 4) memcpy(d + i, &v, 4);
            } else {
                memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, bytes);
            }
        } else {
            log_debug("pm4: DMA_DATA src_sel %u ignored", src_sel);
        }
        break;
    }
    case IT_MEM_SEMAPHORE: {
        if (body < 2) goto short_packet;
        uint64_t addr = (uint64_t)(p[2] & 0xff) << 32 | (p[1] & ~7u);
        uint32_t sel = p[2] >> 29;
        if (!gpu_addr_ok(addr, 8, 8)) break;
        volatile uint64_t *sem = (volatile uint64_t *)(uintptr_t)addr;
        if (sel == 6) { /* signal */
            if ((p[2] >> 20) & 1) *sem = 1;
            else __atomic_fetch_add(sem, 1, __ATOMIC_SEQ_CST);
        } else if (sel == 7) { /* wait, then decrement */
            if (wait_until(sem_ready, (const void *)sem, "MEM_SEMAPHORE", addr))
                __atomic_fetch_sub(sem, 1, __ATOMIC_SEQ_CST);
        }
        break;
    }
    case IT_WAIT_REG_MEM: {
        if (body < 6) goto short_packet;
        uint32_t ctl = p[1];
        if (((ctl >> 4) & 1) == 0) break; /* register space: nothing to poll */
        uint64_t addr = (uint64_t)p[3] << 32 | (p[2] & ~3u);
        uint32_t fn = ctl & 7, ref = p[4], mask = p[5];
        if (!gpu_addr_ok(addr, 4, 4)) break;
        reg_wait w = {(volatile uint32_t *)(uintptr_t)addr, fn, ref, mask};
        wait_until(reg_wait_ready, &w, "WAIT_REG_MEM", addr);
        break;
    }
    case IT_INDIRECT_BUFFER:
    case IT_INDIRECT_BUFFER_CONST: {
        if (body < 3) goto short_packet;
        uint64_t addr = (uint64_t)(p[2] & 0xffff) << 32 | (p[1] & ~3u);
        uint32_t size = p[3] & 0xfffff;
        if (depth >= PM4_MAX_IB_DEPTH) {
            log_warn("pm4: indirect buffer nesting too deep, skipped");
            break;
        }
        if (size == 0 || !gpu_addr_ok(addr, (uint64_t)size * 4, 4)) break;
        walk((const uint32_t *)(uintptr_t)addr, size, depth + 1,
             ccb || op == IT_INDIRECT_BUFFER_CONST);
        break;
    }
    default:
        /* State, draws, dispatches, cache flushes: nothing the CPU sees. */
        break;
    }
    return;
short_packet:
    log_warn("pm4: opcode %#x with short body (%u dwords) ignored", op, body);
}

/* Walk one buffer. Returns false if it stopped on a malformed packet. */
static bool walk(const uint32_t *buf, uint32_t dwords, int depth, bool ccb) {
    if (!buf || dwords == 0) return true;
    if (!gpu_addr_ok((uint64_t)(uintptr_t)buf, (uint64_t)dwords * 4, 4)) return false;
    uint32_t i = 0, prev[3] = {0, 0, 0}, prev_at[3] = {0, 0, 0};
    while (i < dwords) {
        uint32_t h = buf[i];
        uint32_t type = h >> 30;
        uint32_t len; /* packet length in dwords, header included */
        if (type == 2) {
            len = 1; /* filler */
        } else if (type == 3) {
            uint32_t count = (h >> 16) & 0x3fff;
            len = ((count + 1) & 0x3fff) + 1; /* count 0x3fff: one-dword NOP */
        } else if (type == 0) {
            len = ((h >> 16) & 0x3fff) + 2; /* register writes: skip */
        } else {
            log_error("pm4: bad packet header %#010x at dword %u of %u (%p), buffer dropped; "
                      "previous packets %#010x@%u %#010x@%u %#010x@%u", h, i, dwords,
                      (const void *)buf, prev[0], prev_at[0], prev[1], prev_at[1], prev[2],
                      prev_at[2]);
            for (uint32_t k = dwords > 16 ? dwords - 16 : 0; k < dwords; k += 4)
                log_error("pm4:   [%3u] %08x %08x %08x %08x", k, buf[k], k + 1 < dwords ? buf[k + 1] : 0,
                          k + 2 < dwords ? buf[k + 2] : 0, k + 3 < dwords ? buf[k + 3] : 0);
            return false;
        }
        if (len > dwords - i) {
            log_error("pm4: packet %#010x at dword %u overruns buffer (%u > %u), buffer dropped",
                      h, i, len, dwords - i);
            return false;
        }
        prev[0] = prev[1], prev_at[0] = prev_at[1];
        prev[1] = prev[2], prev_at[1] = prev_at[2];
        prev[2] = h, prev_at[2] = i;
        if (type == 3 && !(ccb && ((h >> 8) & 0xff) != IT_INDIRECT_BUFFER_CONST))
            exec_type3((h >> 8) & 0xff, buf + i, len - 1, depth, ccb);
        i += len;
    }
    return true;
}

void pm4_execute(const uint32_t *dcb, uint32_t dcb_dwords, const uint32_t *ccb,
                 uint32_t ccb_dwords) {
    log_trace("pm4: execute dcb %p (%u dw) ccb %p (%u dw)", (const void *)dcb, dcb_dwords,
              (const void *)ccb, ccb_dwords);
    /* The constant engine only feeds the shader constant RAM, which nothing
     * reads without a GPU; walk it for validation and chained buffers. */
    walk(ccb, ccb_dwords, 0, true);
    walk(dcb, dcb_dwords, 0, false);
}

/* ---- asynchronous queues ------------------------------------------------ */

typedef struct pm4_job {
    struct pm4_job *next;
    const uint32_t *dcb, *ccb;
    uint32_t dcb_dw, ccb_dw;
    pm4_done_fn done;
    void *arg;
} pm4_job;

typedef struct pm4_queue {
    plat_mutex *lock;
    plat_cond *cond;
    pm4_job *head, *tail;
    bool busy;
    plat_thread *thread;
    uint32_t id;
} pm4_queue;

static pm4_queue *g_queues[PM4_MAX_QUEUES];

static void queue_main(void *p) {
    pm4_queue *q = p;
    t_is_worker = true;
    char name[24] = "GPU:gfx";
    if (q->id) {
        static const char digits[] = "0123456789";
        memcpy(name, "GPU:compute", 11);
        name[11] = digits[q->id / 10 % 10];
        name[12] = digits[q->id % 10];
        name[13] = 0;
    }
    plat_thread_set_name(name);
    log_debug("pm4: queue %u running on %s", q->id, name);
    plat_mutex_lock(q->lock);
    for (;;) {
        while (!q->head) {
            q->busy = false;
            plat_cond_broadcast(q->cond);
            plat_cond_wait(q->cond, q->lock, UINT64_MAX);
        }
        pm4_job *j = q->head;
        q->head = j->next;
        if (!q->head) q->tail = NULL;
        q->busy = true;
        plat_mutex_unlock(q->lock);
        pm4_execute(j->dcb, j->dcb_dw, j->ccb, j->ccb_dw);
        if (j->done) j->done(j->arg);
        free(j);
        plat_mutex_lock(q->lock);
    }
}

static pm4_queue *get_queue(uint32_t id) {
    plat_mutex_lock(g_lock);
    pm4_queue *q = g_queues[id];
    if (!q) {
        q = calloc(1, sizeof(*q));
        q->lock = plat_mutex_new();
        q->cond = plat_cond_new();
        q->id = id;
        q->thread = plat_thread_create(queue_main, q, 1u << 20);
        if (!q->thread) log_fatal("pm4: cannot start GPU queue thread %u", id);
        plat_thread_detach(q->thread);
        g_queues[id] = q;
    }
    plat_mutex_unlock(g_lock);
    return q;
}

void pm4_submit(uint32_t queue, const uint32_t *dcb, uint32_t dcb_dwords, const uint32_t *ccb,
                uint32_t ccb_dwords, pm4_done_fn done, void *arg) {
    if (queue >= PM4_MAX_QUEUES) {
        log_error("pm4: bad queue %u", queue);
        return;
    }
    pm4_queue *q = get_queue(queue);
    pm4_job *j = malloc(sizeof(*j));
    *j = (pm4_job){NULL, dcb, ccb, dcb_dwords, ccb_dwords, done, arg};
    plat_mutex_lock(q->lock);
    if (q->tail) q->tail->next = j;
    else q->head = j;
    q->tail = j;
    q->busy = true;
    plat_cond_broadcast(q->cond);
    plat_mutex_unlock(q->lock);
}

void pm4_wait_idle(void) {
    for (uint32_t i = 0; i < PM4_MAX_QUEUES; i++) {
        plat_mutex_lock(g_lock);
        pm4_queue *q = g_queues[i];
        plat_mutex_unlock(g_lock);
        if (!q) continue;
        plat_mutex_lock(q->lock);
        while (q->head || q->busy) plat_cond_wait(q->cond, q->lock, UINT64_MAX);
        plat_mutex_unlock(q->lock);
    }
}
