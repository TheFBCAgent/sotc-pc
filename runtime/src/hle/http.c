/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceHttp and libSceSsl for an offline console.
 *
 * Contexts, templates, connections, requests and epolls are real objects with
 * the usual id validation, so setup code runs normally. Sending a request
 * fails the way it does with no network: a host name gets RESOLVER_ENODNS, a
 * numeric host gets NETWORK. A non-blocking request bound to an epoll follows
 * the asynchronous protocol instead: SendRequest returns EAGAIN, the epoll
 * gets a RESOLVER_ERR / SOCK_ERR event, and the next SendRequest returns the
 * error. Responses never exist (GetStatusCode/ReadData fail).
 *
 * libSceSsl: Init hands out a context id, Term succeeds.
 *
 * Semantics and error codes follow shadPS4 (src/core/libraries/network/
 * http.cpp, http.h, http_error.h, ssl.cpp; GPL-2.0). */
#include <string.h>

#include "../log.h"
#include "hle.h"

#define HTTP_ERROR_BEFORE_INIT ((int32_t)0x80431001)
#define HTTP_ERROR_OUT_OF_MEMORY ((int32_t)0x80431022)
#define HTTP_ERROR_INVALID_ID ((int32_t)0x80431100)
#define HTTP_ERROR_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_ERROR_INVALID_URL ((int32_t)0x80433060)
#define HTTP_ERROR_UNKNOWN_SCHEME ((int32_t)0x80431061)
#define HTTP_ERROR_NETWORK ((int32_t)0x80431063)
#define HTTP_ERROR_BEFORE_SEND ((int32_t)0x80431065)
#define HTTP_ERROR_AFTER_SEND ((int32_t)0x80431066)
#define HTTP_ERROR_UNKNOWN_METHOD ((int32_t)0x8043106b)
#define HTTP_ERROR_ABORTED ((int32_t)0x80431080)
#define HTTP_ERROR_EAGAIN ((int32_t)0x80431082)
#define HTTP_ERROR_RESOLVER_ENODNS ((int32_t)0x80436002)

enum {
    HTTP_NB_EVENT_SOCK_ERR = 0x8,
    HTTP_NB_EVENT_RESOLVER_ERR = 0x20000,
};

typedef struct http_nb_event {
    uint32_t events;
    uint32_t event_detail;
    int32_t id;
    void *user_arg;
} http_nb_event;
_Static_assert(sizeof(http_nb_event) == 24, "OrbisHttpNBEvent");

enum { HO_FREE, HO_CTX, HO_TMPL, HO_CONN, HO_REQ, HO_EPOLL };
enum { REQ_CREATED, REQ_FAILED };

#define HTTP_MAX_OBJS 128
#define HTTP_MAX_EVENTS 16

typedef struct http_obj {
    int kind;
    int32_t parent;     /* ctx for tmpl/epoll, tmpl for conn, conn for req */
    bool nonblock;
    int32_t epoll;      /* bound epoll id, 0 = none */
    void *user_arg;
    int32_t send_error; /* conn/req: what sending fails with */
    int state;          /* req */
    /* epoll */
    http_nb_event events[HTTP_MAX_EVENTS];
    int nevents;
    int32_t waiters;
    bool destroyed;
} http_obj;

static plat_mutex *g_http_lock;
static plat_cond *g_http_cond;
static http_obj g_objs[HTTP_MAX_OBJS]; /* id = index + 1 */
static int g_ssl_next;

__attribute__((constructor)) static void http_state_init(void) {
    g_http_lock = plat_mutex_new();
    g_http_cond = plat_cond_new();
}

/* All helpers below run with g_http_lock held. */
static http_obj *obj_get(int32_t id, int kind) {
    if (id < 1 || id > HTTP_MAX_OBJS || g_objs[id - 1].kind == HO_FREE) return NULL;
    http_obj *o = &g_objs[id - 1];
    return (kind == HO_FREE || o->kind == kind) ? o : NULL;
}

/* Template, connection or request. */
static http_obj *obj_get_trc(int32_t id) {
    http_obj *o = obj_get(id, HO_FREE);
    return (o && (o->kind == HO_TMPL || o->kind == HO_CONN || o->kind == HO_REQ)) ? o : NULL;
}

static int32_t obj_alloc(int kind, int32_t parent) {
    for (int i = 0; i < HTTP_MAX_OBJS; i++)
        if (g_objs[i].kind == HO_FREE) {
            memset(&g_objs[i], 0, sizeof g_objs[i]);
            g_objs[i].kind = kind;
            g_objs[i].parent = parent;
            return i + 1;
        }
    return HTTP_ERROR_OUT_OF_MEMORY;
}

static void obj_free(int32_t id) {
    http_obj *o = &g_objs[id - 1];
    if (o->kind == HO_EPOLL && o->waiters > 0) {
        o->destroyed = true; /* the last waiter frees it */
        plat_cond_broadcast(g_http_cond);
        return;
    }
    o->kind = HO_FREE;
}

static bool any_ctx(void) {
    for (int i = 0; i < HTTP_MAX_OBJS; i++)
        if (g_objs[i].kind == HO_CTX) return true;
    return false;
}

/* Does `id` descend from `ancestor` (through the parent chain)? */
static bool descends(int32_t id, int32_t ancestor) {
    for (int depth = 0; id > 0 && depth < 4; depth++) {
        if (id == ancestor) return true;
        http_obj *o = obj_get(id, HO_FREE);
        if (!o) return false;
        id = o->parent;
    }
    return false;
}

/* ---- URL ---------------------------------------------------------------- */

/* Classify the URL: returns what sending to it fails with offline, or a
 * parse error. */
static int32_t url_send_error(const char *url) {
    if (!url) return HTTP_ERROR_INVALID_URL;
    const char *sep = strstr(url, "://");
    if (!sep || sep == url) return HTTP_ERROR_INVALID_URL;
    size_t slen = (size_t)(sep - url);
    if (!((slen == 4 && strncmp(url, "http", 4) == 0) ||
          (slen == 5 && strncmp(url, "https", 5) == 0)))
        return HTTP_ERROR_UNKNOWN_SCHEME;
    const char *host = sep + 3;
    const char *at = host;
    while (*at && *at != '/' && *at != '?' && *at != '#') at++;
    /* skip userinfo */
    for (const char *p = host; p < at; p++)
        if (*p == '@') host = p + 1;
    size_t hlen = 0;
    bool numeric = true;
    while (host + hlen < at && host[hlen] != ':') {
        char c = host[hlen];
        if (!((c >= '0' && c <= '9') || c == '.')) numeric = false;
        hlen++;
    }
    if (hlen == 0) return HTTP_ERROR_INVALID_URL;
    /* A literal address needs no DNS but has no route either. */
    return numeric || host[0] == '[' ? HTTP_ERROR_NETWORK : HTTP_ERROR_RESOLVER_ENODNS;
}

/* ---- init / objects ------------------------------------------------------ */

static SYSV int32_t ps4_sceHttpInit(int32_t libnet_mem_id, int32_t libssl_ctx_id,
                                   uint64_t pool_size) {
    if (pool_size == 0) return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_mutex_lock(g_http_lock);
    int32_t id = obj_alloc(HO_CTX, 0);
    plat_mutex_unlock(g_http_lock);
    log_info("http: init mem %d ssl %d pool %llu -> %d (offline)", libnet_mem_id, libssl_ctx_id,
             (unsigned long long)pool_size, id);
    return id;
}

static SYSV int32_t ps4_sceHttpTerm(int32_t ctx) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!obj_get(ctx, HO_CTX)) ret = HTTP_ERROR_INVALID_ID;
    else {
        /* Free everything that belongs to this context, children first. */
        for (int kind = HO_REQ; kind >= HO_TMPL; kind--)
            for (int i = 0; i < HTTP_MAX_OBJS; i++)
                if (g_objs[i].kind == kind && descends(i + 1, ctx)) obj_free(i + 1);
        for (int i = 0; i < HTTP_MAX_OBJS; i++)
            if (g_objs[i].kind == HO_EPOLL && g_objs[i].parent == ctx) obj_free(i + 1);
        obj_free(ctx);
    }
    plat_mutex_unlock(g_http_lock);
    return ret;
}

static SYSV int32_t ps4_sceHttpCreateTemplate(int32_t ctx, const char *user_agent,
                                             int32_t http_ver, int32_t auto_proxy) {
    (void)http_ver; (void)auto_proxy;
    int32_t ret;
    plat_mutex_lock(g_http_lock);
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!obj_get(ctx, HO_CTX)) ret = HTTP_ERROR_INVALID_ID;
    else ret = obj_alloc(HO_TMPL, ctx);
    plat_mutex_unlock(g_http_lock);
    log_debug("http: template '%s' -> %d", user_agent ? user_agent : "", ret);
    return ret;
}

static SYSV int32_t ps4_sceHttpCreateConnectionWithURL(int32_t tmpl_id, const char *url,
                                                      int32_t keepalive) {
    (void)keepalive;
    int32_t ret;
    plat_mutex_lock(g_http_lock);
    http_obj *t;
    int32_t err = url_send_error(url);
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!(t = obj_get(tmpl_id, HO_TMPL))) ret = HTTP_ERROR_INVALID_ID;
    else if (err == HTTP_ERROR_INVALID_URL || err == HTTP_ERROR_UNKNOWN_SCHEME) ret = err;
    else if ((ret = obj_alloc(HO_CONN, tmpl_id)) > 0) {
        http_obj *c = &g_objs[ret - 1];
        t = &g_objs[tmpl_id - 1];
        c->nonblock = t->nonblock;
        c->epoll = t->epoll;
        c->user_arg = t->user_arg;
        c->send_error = err;
    }
    plat_mutex_unlock(g_http_lock);
    log_info("http: connection '%s' -> %#x", url ? url : "(null)", (unsigned)ret);
    return ret;
}

static SYSV int32_t ps4_sceHttpCreateRequestWithURL(int32_t conn_id, int32_t method,
                                                   const char *url, uint64_t content_length) {
    (void)content_length;
    int32_t ret;
    plat_mutex_lock(g_http_lock);
    http_obj *c;
    int32_t err = url_send_error(url);
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!(c = obj_get(conn_id, HO_CONN))) ret = HTTP_ERROR_INVALID_ID;
    else if (err == HTTP_ERROR_INVALID_URL || err == HTTP_ERROR_UNKNOWN_SCHEME) ret = err;
    else if (method < 0 || method > 8 || ((0x177u >> method) & 1u) == 0)
        ret = HTTP_ERROR_UNKNOWN_METHOD;
    else if ((ret = obj_alloc(HO_REQ, conn_id)) > 0) {
        http_obj *r = &g_objs[ret - 1];
        c = &g_objs[conn_id - 1];
        r->nonblock = c->nonblock;
        r->epoll = c->epoll;
        r->user_arg = c->user_arg;
        r->send_error = err;
        r->state = REQ_CREATED;
    }
    plat_mutex_unlock(g_http_lock);
    log_debug("http: request method %d '%s' -> %#x", method, url ? url : "(null)",
              (unsigned)ret);
    return ret;
}

static int32_t delete_kind(int32_t id, int kind) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!obj_get(id, kind)) ret = HTTP_ERROR_INVALID_ID;
    else obj_free(id);
    plat_mutex_unlock(g_http_lock);
    return ret;
}

static SYSV int32_t ps4_sceHttpDeleteTemplate(int32_t id) { return delete_kind(id, HO_TMPL); }
static SYSV int32_t ps4_sceHttpDeleteConnection(int32_t id) { return delete_kind(id, HO_CONN); }
static SYSV int32_t ps4_sceHttpDeleteRequest(int32_t id) { return delete_kind(id, HO_REQ); }

static SYSV int32_t ps4_sceHttpAddRequestHeader(int32_t id, const char *name, const char *value,
                                               int32_t mode) {
    (void)mode;
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!obj_get_trc(id)) ret = HTTP_ERROR_INVALID_ID;
    else if (!name || !value) ret = HTTP_ERROR_INVALID_VALUE;
    plat_mutex_unlock(g_http_lock);
    return ret;
}

static SYSV int32_t ps4_sceHttpSetNonblock(int32_t id, int32_t enable) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    http_obj *o;
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!(o = obj_get_trc(id))) ret = HTTP_ERROR_INVALID_ID;
    else o->nonblock = enable != 0;
    plat_mutex_unlock(g_http_lock);
    return ret;
}

/* ---- sending and responses ----------------------------------------------- */

static SYSV int32_t ps4_sceHttpSendRequest(int32_t req_id, const void *post, uint64_t size) {
    (void)post; (void)size;
    int32_t ret;
    plat_mutex_lock(g_http_lock);
    http_obj *r, *ep;
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!(r = obj_get(req_id, HO_REQ))) ret = HTTP_ERROR_INVALID_ID;
    else if (r->state == REQ_FAILED) ret = r->send_error;
    else if (r->nonblock && (ep = obj_get(r->epoll, HO_EPOLL)) && !ep->destroyed) {
        /* Asynchronous: report EAGAIN now, the failure through the epoll. */
        if (ep->nevents < HTTP_MAX_EVENTS) {
            http_nb_event *e = &ep->events[ep->nevents++];
            e->events = r->send_error == HTTP_ERROR_RESOLVER_ENODNS ? HTTP_NB_EVENT_RESOLVER_ERR
                                                                     : HTTP_NB_EVENT_SOCK_ERR;
            e->event_detail = 0;
            e->id = req_id;
            e->user_arg = r->user_arg;
        }
        r->state = REQ_FAILED; /* the next SendRequest reports the error */
        plat_cond_broadcast(g_http_cond);
        ret = HTTP_ERROR_EAGAIN;
    } else {
        r->state = REQ_FAILED;
        ret = r->send_error;
    }
    plat_mutex_unlock(g_http_lock);
    log_info("http: send request %#x -> %#x (offline)", (unsigned)req_id, (unsigned)ret);
    return ret;
}

/* No request ever gets a response. */
static int32_t response_error(int32_t req_id) {
    int32_t ret;
    plat_mutex_lock(g_http_lock);
    http_obj *r;
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!(r = obj_get(req_id, HO_REQ))) ret = HTTP_ERROR_INVALID_ID;
    else if (r->state == REQ_CREATED) ret = HTTP_ERROR_BEFORE_SEND;
    else ret = r->send_error;
    plat_mutex_unlock(g_http_lock);
    return ret;
}

static SYSV int32_t ps4_sceHttpGetStatusCode(int32_t req_id, int32_t *status) {
    if (!status) return HTTP_ERROR_INVALID_VALUE;
    return response_error(req_id);
}

static SYSV int32_t ps4_sceHttpReadData(int32_t req_id, void *data, uint64_t size) {
    (void)size;
    if (!data) return HTTP_ERROR_INVALID_VALUE;
    return response_error(req_id);
}

static SYSV int32_t ps4_sceHttpGetAllResponseHeaders(int32_t req_id, char **header,
                                                    uint64_t *size) {
    if (!header || !size) return HTTP_ERROR_INVALID_VALUE;
    return response_error(req_id);
}

/* ---- epoll -------------------------------------------------------------- */

/* Handles are opaque pointers to the guest; encode the object id. */
static void *epoll_handle(int32_t id) { return (void *)(uintptr_t)(0x48000000u + (uint32_t)id); }
static int32_t epoll_id(const void *h) { return (int32_t)((uintptr_t)h - 0x48000000u); }

static SYSV int32_t ps4_sceHttpCreateEpoll(int32_t ctx, void **eh) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!obj_get(ctx, HO_CTX)) ret = HTTP_ERROR_INVALID_ID;
    else if (!eh) ret = HTTP_ERROR_INVALID_VALUE;
    else {
        int32_t id = obj_alloc(HO_EPOLL, ctx);
        if (id < 0) ret = id;
        else *eh = epoll_handle(id);
    }
    plat_mutex_unlock(g_http_lock);
    return ret;
}

static SYSV int32_t ps4_sceHttpDestroyEpoll(int32_t ctx, void *eh) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    int32_t id = epoll_id(eh);
    http_obj *ep;
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!obj_get(ctx, HO_CTX)) ret = HTTP_ERROR_INVALID_ID;
    else if (!eh) ret = HTTP_ERROR_INVALID_VALUE;
    else if (!(ep = obj_get(id, HO_EPOLL)) || ep->destroyed) ret = HTTP_ERROR_INVALID_ID;
    else obj_free(id);
    plat_mutex_unlock(g_http_lock);
    return ret;
}

static SYSV int32_t ps4_sceHttpSetEpoll(int32_t id, void *eh, void *user_arg) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    http_obj *o, *ep;
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!(ep = obj_get(epoll_id(eh), HO_EPOLL)) || ep->destroyed) ret = HTTP_ERROR_INVALID_ID;
    else if (!(o = obj_get_trc(id))) ret = HTTP_ERROR_INVALID_ID;
    else {
        o->epoll = epoll_id(eh);
        o->user_arg = user_arg;
    }
    plat_mutex_unlock(g_http_lock);
    return ret;
}

static SYSV int32_t ps4_sceHttpUnsetEpoll(int32_t id) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_http_lock);
    http_obj *o;
    if (!any_ctx()) ret = HTTP_ERROR_BEFORE_INIT;
    else if (!(o = obj_get_trc(id))) ret = HTTP_ERROR_INVALID_ID;
    else {
        o->epoll = 0;
        o->user_arg = NULL;
    }
    plat_mutex_unlock(g_http_lock);
    return ret;
}

/* timeout in microseconds; < 0 waits forever, 0 polls. */
static SYSV int32_t ps4_sceHttpWaitRequest(void *eh, http_nb_event *nbev, int32_t maxevents,
                                          int32_t timeout) {
    if (maxevents <= 0 || !eh || !nbev) return HTTP_ERROR_INVALID_VALUE;
    plat_mutex_lock(g_http_lock);
    int32_t id = epoll_id(eh);
    http_obj *ep = obj_get(id, HO_EPOLL);
    if (!any_ctx()) {
        plat_mutex_unlock(g_http_lock);
        return HTTP_ERROR_BEFORE_INIT;
    }
    if (!ep || ep->destroyed) {
        plat_mutex_unlock(g_http_lock);
        return HTTP_ERROR_INVALID_ID;
    }
    uint64_t deadline = timeout > 0 ? plat_monotonic_ns() + (uint64_t)timeout * 1000 : 0;
    ep->waiters++;
    while (ep->nevents == 0 && !ep->destroyed && timeout != 0) {
        uint64_t wait_us = UINT64_MAX;
        if (deadline) {
            uint64_t now = plat_monotonic_ns();
            if (now >= deadline) break;
            wait_us = (deadline - now + 999) / 1000;
        }
        plat_cond_wait(g_http_cond, g_http_lock, wait_us);
    }
    int32_t n = ep->nevents < maxevents ? ep->nevents : maxevents;
    memcpy(nbev, ep->events, (size_t)n * sizeof *nbev);
    memmove(ep->events, ep->events + n, (size_t)(ep->nevents - n) * sizeof *nbev);
    ep->nevents -= n;
    bool aborted = ep->destroyed && n == 0;
    if (--ep->waiters == 0 && ep->destroyed) ep->kind = HO_FREE;
    plat_mutex_unlock(g_http_lock);
    return aborted ? HTTP_ERROR_ABORTED : n;
}

/* ---- URI helpers --------------------------------------------------------- */

/* Percent-encode everything but RFC 3986 unreserved characters. */
static SYSV int32_t ps4_sceHttpUriEscape(char *out, uint64_t *require, uint64_t prepare,
                                        const char *in) {
    if (!in) return HTTP_ERROR_INVALID_VALUE;
    uint64_t need = 1;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        need += plain ? 1 : 3;
    }
    if (require) *require = need;
    if (!out) return SCE_OK;
    if (prepare < need) return HTTP_ERROR_OUT_OF_MEMORY;
    static const char hex[] = "0123456789ABCDEF";
    char *o = out;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) *o++ = (char)c;
        else {
            *o++ = '%';
            *o++ = hex[c >> 4];
            *o++ = hex[c & 15];
        }
    }
    *o = '\0';
    return SCE_OK;
}

HLE_LIBRARY(hle_http, "libSceHttp",
    HLE(sceHttpInit),
    HLE(sceHttpTerm),
    HLE(sceHttpCreateTemplate),
    HLE(sceHttpDeleteTemplate),
    HLE(sceHttpCreateConnectionWithURL),
    HLE(sceHttpDeleteConnection),
    HLE(sceHttpCreateRequestWithURL),
    HLE(sceHttpDeleteRequest),
    HLE(sceHttpAddRequestHeader),
    HLE(sceHttpSetNonblock),
    HLE(sceHttpSendRequest),
    HLE(sceHttpGetStatusCode),
    HLE(sceHttpReadData),
    HLE(sceHttpGetAllResponseHeaders),
    HLE(sceHttpCreateEpoll),
    HLE(sceHttpDestroyEpoll),
    HLE(sceHttpSetEpoll),
    HLE(sceHttpUnsetEpoll),
    HLE(sceHttpWaitRequest),
    HLE(sceHttpUriEscape),
)

/* ---- libSceSsl ----------------------------------------------------------- */

static SYSV int32_t ps4_sceSslInit(uint64_t pool_size) {
    plat_mutex_lock(g_http_lock);
    int32_t id = ++g_ssl_next;
    plat_mutex_unlock(g_http_lock);
    log_info("ssl: init pool %llu -> %d", (unsigned long long)pool_size, id);
    return id;
}

static SYSV int32_t ps4_sceSslTerm(int32_t ctx) {
    log_debug("ssl: term %d", ctx);
    return SCE_OK;
}

HLE_LIBRARY(hle_ssl, "libSceSsl",
    HLE(sceSslInit),
    HLE(sceSslTerm),
)
