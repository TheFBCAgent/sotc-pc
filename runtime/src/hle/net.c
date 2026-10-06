/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceNet, libSceNetCtl and the libScePosix socket calls, for a console that
 * is offline: the network stack works, but there is no link, no route and no
 * DNS server.
 *
 *  - sceNetInit, pools and resolvers succeed. Resolvers only translate numeric
 *    IPv4 strings; any real lookup fails with RESOLVER_ENODNS.
 *  - sceNet sockets are created, bound and listened on like the real thing.
 *    connect/sendto fail with ENETUNREACH (ECONNREFUSED for loopback), send on
 *    an unconnected socket fails with ENOTCONN/EDESTADDRREQ, and recv/accept
 *    never get data: EWOULDBLOCK when non-blocking or on timeout, otherwise
 *    they block until the socket is closed.
 *  - NetCtl reports DISCONNECTED; GetInfo fails with NOT_CONNECTED.
 *  - posix socket() fails with ENETDOWN. Socket fds would share the fd space
 *    with file.c (close() lives there), so the posix layer refuses to create
 *    them; the other posix socket calls fail with EBADF. select() treats any
 *    fd it is given as ready (only regular files can exist) and otherwise
 *    sleeps for the timeout.
 *
 * Semantics, constants and error codes follow shadPS4
 * (src/core/libraries/net/net.cpp, net_types.h, net_error.h and
 * src/core/libraries/network/netctl.cpp, net_ctl_codes.h; GPL-2.0). */
#include <stdio.h>
#include <string.h>

#include "../log.h"
#include "hle.h"

/* ---- constants (shadPS4 net_types.h / net_error.h) ---------------------- */

enum {
    NET_AF_INET = 2,
    NET_AF_INET6 = 28,
    NET_SOCK_STREAM = 1,
    NET_SOCK_DGRAM = 2,
    NET_SOCK_RAW = 3,
    NET_SOCK_DGRAM_P2P = 6,
    NET_SOCK_STREAM_P2P = 10,
    NET_SOL_SOCKET = 0xffff,
    NET_SO_SNDBUF = 0x1001,
    NET_SO_RCVBUF = 0x1002,
    NET_SO_ERROR = 0x1007,
    NET_SO_TYPE = 0x1008,
    NET_SO_SNDTIMEO = 0x1105,
    NET_SO_RCVTIMEO = 0x1106,
    NET_SO_ERROR_EX = 0x1107,
    NET_SO_ACCEPTTIMEO = 0x1108,
    NET_SO_CONNECTTIMEO = 0x1109,
    NET_SO_NBIO = 0x1200,
    NET_MSG_DONTWAIT = 0x80,
    NET_DEBUG_NAME_LEN_MAX = 31,
    NET_RESOLVER_NTOA_DISABLE_IPADDRESS = 0x10000,
    NET_SOCKINFO_STATE_OPENED = 2,
    NET_SOCKINFO_F_SELF = 0x1,
    NET_SOCKINFO_F_NONBLOCK = 0x200000,
    NET_MIN_POOL_SIZE = 4 * 1024,
};

/* errno values (FreeBSD numbering, shared by libSceNet and libkernel). */
enum {
    NET_EBADF = 9, NET_ENOMEM = 12, NET_EFAULT = 14, NET_EINVAL = 22, NET_ENOSPC = 28,
    NET_EWOULDBLOCK = 35, NET_ENOTSOCK = 38, NET_EDESTADDRREQ = 39, NET_EPROTOTYPE = 41,
    NET_ENOPROTOOPT = 42, NET_EPROTONOSUPPORT = 43, NET_EOPNOTSUPP = 45,
    NET_EAFNOSUPPORT = 47, NET_ENETDOWN = 50, NET_ENETUNREACH = 51, NET_EISCONN = 56,
    NET_ENOTCONN = 57, NET_ECONNREFUSED = 61, NET_ENAMETOOLONG = 63,
    NET_RESOLVER_ENODNS = 225,
};

#define NET_ERROR(e) ((int32_t)(0x80410100u | (uint32_t)(e)))

/* NetCtl (shadPS4 net_ctl_codes.h). */
#define NET_CTL_ERROR_INVALID_ADDR ((int32_t)0x80412107)
#define NET_CTL_ERROR_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_ERROR_NOT_INITIALIZED ((int32_t)0x80412101)
enum { NET_CTL_STATE_DISCONNECTED = 0 };

/* ---- guest structures --------------------------------------------------- */

typedef struct net_sockaddr_in {
    uint8_t len;
    uint8_t family;
    uint16_t port;     /* network byte order */
    uint32_t addr;     /* network byte order */
    uint16_t vport;
    char zero[6];
} net_sockaddr_in;
_Static_assert(sizeof(net_sockaddr_in) == 16, "OrbisNetSockaddrIn");

typedef struct net_sock_info {
    char name[NET_DEBUG_NAME_LEN_MAX + 1];
    int32_t pid;
    int32_t s;
    int8_t socket_type;
    int8_t policy;
    int8_t priority;
    int8_t reserved8;
    int32_t recv_queue_length;
    int32_t send_queue_length;
    uint32_t local_adr;
    uint32_t remote_adr;
    uint16_t local_port;
    uint16_t remote_port;
    uint16_t local_vport;
    uint16_t remote_vport;
    int32_t state;
    int32_t flags;
    int32_t tx_bps, rx_bps, max_tx_bps, max_rx_bps, tx_vbps, rx_vbps;
    int32_t recv_buffer_size;
    int32_t send_buffer_size;
    int32_t reserved6[8];
    int32_t tx_drops, rx_drops, tx_wait;
    int32_t reserved[2];
} net_sock_info;
_Static_assert(sizeof(net_sock_info) == 160, "OrbisNetSockInfo");

/* ---- state -------------------------------------------------------------- */

#define NET_MAX_SOCKETS 64
#define NET_SOCK_BASE 0x40 /* ids start here so they stand out in logs */
#define NET_MAX_POOLS 16
#define NET_MAX_RESOLVERS 32
#define NET_DEFAULT_BUF 65536

typedef struct net_socket {
    bool used;
    bool closing;      /* closed while a thread waits in it */
    int32_t waiters;
    int32_t family, type, protocol;
    char name[NET_DEBUG_NAME_LEN_MAX + 1];
    bool nbio, bound, listening;
    uint32_t local_addr; /* network order */
    uint16_t local_port; /* network order */
    int32_t rcvtimeo_us, sndtimeo_us, accepttimeo_us;
    int32_t rcvbuf, sndbuf;
} net_socket;

static plat_mutex *g_net_lock;
static plat_cond *g_net_cond;
static net_socket g_sockets[NET_MAX_SOCKETS];
static bool g_pools[NET_MAX_POOLS];
static bool g_resolvers[NET_MAX_RESOLVERS];
static uint16_t g_next_port = 49152;
static int g_netctl_inited;

__attribute__((constructor)) static void net_state_init(void) {
    g_net_lock = plat_mutex_new();
    g_net_cond = plat_cond_new();
}

static int32_t net_fail(int e) {
    log_debug("net: error %d", e);
    return NET_ERROR(e);
}

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* Caller holds g_net_lock. */
static net_socket *sock_get(int32_t s) {
    int32_t i = s - NET_SOCK_BASE;
    if (i < 0 || i >= NET_MAX_SOCKETS || !g_sockets[i].used || g_sockets[i].closing) return NULL;
    return &g_sockets[i];
}

static bool is_loopback(uint32_t addr_be) { return (addr_be & 0xff) == 127; }

/* ---- libSceNet: init, pools, byte order ---------------------------------- */

static SYSV int32_t ps4_sceNetInit(void) {
    log_info("net: sceNetInit (offline)");
    return SCE_OK;
}

static SYSV int32_t ps4_sceNetPoolCreate(const char *name, int32_t size, int32_t flags) {
    if (name && strnlen(name, NET_DEBUG_NAME_LEN_MAX + 1) > NET_DEBUG_NAME_LEN_MAX)
        return net_fail(NET_ENAMETOOLONG);
    if (flags != 0 || size < NET_MIN_POOL_SIZE) return net_fail(NET_EINVAL);
    plat_mutex_lock(g_net_lock);
    int32_t id = -1;
    for (int i = 0; i < NET_MAX_POOLS; i++)
        if (!g_pools[i]) {
            g_pools[i] = true;
            id = i + 1;
            break;
        }
    plat_mutex_unlock(g_net_lock);
    log_debug("net: pool '%s' size %d -> %d", name ? name : "", size, id);
    return id > 0 ? id : net_fail(NET_ENOMEM);
}

static SYSV int32_t ps4_sceNetPoolDestroy(int32_t id) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_net_lock);
    if (id < 1 || id > NET_MAX_POOLS || !g_pools[id - 1]) ret = net_fail(NET_EBADF);
    else g_pools[id - 1] = false;
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV uint32_t ps4_sceNetHtonl(uint32_t v) { return __builtin_bswap32(v); }
static SYSV uint32_t ps4_sceNetNtohl(uint32_t v) { return __builtin_bswap32(v); }
static SYSV uint16_t ps4_sceNetHtons(uint16_t v) { return bswap16(v); }
static SYSV uint16_t ps4_sceNetNtohs(uint16_t v) { return bswap16(v); }

/* ---- address text (IPv4 only) -------------------------------------------- */

/* FreeBSD inet_pton4: four decimal octets, no leading zeros. 1 = ok. */
static int parse_ipv4(const char *src, uint8_t out[4]) {
    int parts = 0;
    while (parts < 4) {
        if (*src < '0' || *src > '9') return 0;
        unsigned v = 0;
        const char *start = src;
        while (*src >= '0' && *src <= '9') {
            if (src != start && v == 0) return 0; /* leading zero */
            v = v * 10 + (unsigned)(*src++ - '0');
            if (v > 255) return 0;
        }
        out[parts++] = (uint8_t)v;
        if (parts < 4 && *src++ != '.') return 0;
    }
    return *src == '\0';
}

static SYSV int32_t ps4_sceNetInetPton(int32_t af, const char *src, void *dst) {
    if (!src || !dst) return net_fail(NET_EFAULT);
    if (af != NET_AF_INET) return net_fail(NET_EAFNOSUPPORT); /* IPv6: not needed offline */
    uint8_t b[4];
    if (!parse_ipv4(src, b)) return 0;
    memcpy(dst, b, 4);
    return 1;
}

static SYSV const char *ps4_sceNetInetNtop(int32_t af, const void *src, char *dst, uint32_t size) {
    if (!src || !dst) return NULL;
    if (af != NET_AF_INET) return NULL;
    const uint8_t *b = src;
    char tmp[16];
    int n = snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if ((uint32_t)n + 1 > size) return NULL;
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
}

/* ---- resolver ----------------------------------------------------------- */

static SYSV int32_t ps4_sceNetResolverCreate(const char *name, int32_t poolid, int32_t flags) {
    if (flags != 0) return net_fail(NET_EINVAL);
    if (name && strnlen(name, NET_DEBUG_NAME_LEN_MAX + 1) > NET_DEBUG_NAME_LEN_MAX)
        return net_fail(NET_ENAMETOOLONG);
    int32_t id = -1;
    plat_mutex_lock(g_net_lock);
    if (poolid >= 1 && poolid <= NET_MAX_POOLS && g_pools[poolid - 1])
        for (int i = 0; i < NET_MAX_RESOLVERS; i++)
            if (!g_resolvers[i]) {
                g_resolvers[i] = true;
                id = i + 1;
                break;
            }
    plat_mutex_unlock(g_net_lock);
    if (id < 0) return net_fail(poolid >= 1 && poolid <= NET_MAX_POOLS ? NET_ENOMEM : NET_EBADF);
    return id;
}

static bool resolver_ok(int32_t rid) {
    plat_mutex_lock(g_net_lock);
    bool ok = rid >= 1 && rid <= NET_MAX_RESOLVERS && g_resolvers[rid - 1];
    plat_mutex_unlock(g_net_lock);
    return ok;
}

static SYSV int32_t ps4_sceNetResolverDestroy(int32_t rid) {
    if (!resolver_ok(rid)) return net_fail(NET_EBADF);
    plat_mutex_lock(g_net_lock);
    g_resolvers[rid - 1] = false;
    plat_mutex_unlock(g_net_lock);
    return SCE_OK;
}

/* Name -> address. Numeric strings work without a network; names need DNS. */
static SYSV int32_t ps4_sceNetResolverStartNtoa(int32_t rid, const char *hostname, uint32_t *addr,
                                               int32_t timeout, int32_t retry, int32_t flags) {
    (void)timeout; (void)retry;
    if (!resolver_ok(rid)) return net_fail(NET_EBADF);
    if (!hostname || !addr) return net_fail(NET_EINVAL);
    uint8_t b[4];
    if (!(flags & NET_RESOLVER_NTOA_DISABLE_IPADDRESS) && parse_ipv4(hostname, b)) {
        memcpy(addr, b, 4);
        return SCE_OK;
    }
    log_info("net: resolve '%s' -> no DNS (offline)", hostname);
    return net_fail(NET_RESOLVER_ENODNS);
}

/* Address -> name: always a DNS query. */
static SYSV int32_t ps4_sceNetResolverStartAton(int32_t rid, const uint32_t *addr, char *hostname,
                                               int32_t len, int32_t timeout, int32_t retry,
                                               int32_t flags) {
    (void)timeout; (void)retry; (void)flags;
    if (!resolver_ok(rid)) return net_fail(NET_EBADF);
    if (!addr || !hostname || len <= 0) return net_fail(NET_EINVAL);
    return net_fail(NET_RESOLVER_ENODNS);
}

/* ---- sockets ------------------------------------------------------------ */

static SYSV int32_t ps4_sceNetSocket(const char *name, int32_t family, int32_t type,
                                    int32_t protocol) {
    if (family != NET_AF_INET && family != NET_AF_INET6) return net_fail(NET_EAFNOSUPPORT);
    if (type != NET_SOCK_STREAM && type != NET_SOCK_DGRAM && type != NET_SOCK_RAW &&
        type != NET_SOCK_DGRAM_P2P && type != NET_SOCK_STREAM_P2P)
        return net_fail(NET_EPROTONOSUPPORT);
    int32_t id = -1;
    plat_mutex_lock(g_net_lock);
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket *k = &g_sockets[i];
        if (k->used) continue;
        memset(k, 0, sizeof *k);
        k->used = true;
        k->family = family;
        k->type = type;
        k->protocol = protocol;
        k->rcvbuf = k->sndbuf = NET_DEFAULT_BUF;
        if (name) strncpy(k->name, name, NET_DEBUG_NAME_LEN_MAX);
        id = NET_SOCK_BASE + i;
        break;
    }
    plat_mutex_unlock(g_net_lock);
    log_info("net: socket '%s' family %d type %d proto %d -> %#x", name ? name : "", family,
             type, protocol, (unsigned)id);
    return id >= 0 ? id : net_fail(NET_ENOMEM);
}

static SYSV int32_t ps4_sceNetSocketClose(int32_t s) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    if (!k) ret = net_fail(NET_EBADF);
    else if (k->waiters > 0) {
        k->closing = true; /* the last waiter frees it */
        plat_cond_broadcast(g_net_cond);
    } else {
        k->used = false;
    }
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV int32_t ps4_sceNetBind(int32_t s, const net_sockaddr_in *addr, uint32_t addrlen) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    if (!k) ret = net_fail(NET_EBADF);
    else if (!addr) ret = net_fail(NET_EFAULT);
    else if (addrlen < sizeof(net_sockaddr_in)) ret = net_fail(NET_EINVAL);
    else if (k->bound) ret = net_fail(NET_EINVAL);
    else {
        k->bound = true;
        k->local_addr = addr->addr;
        k->local_port = addr->port ? addr->port : bswap16(g_next_port++);
    }
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV int32_t ps4_sceNetListen(int32_t s, int32_t backlog) {
    (void)backlog;
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    if (!k) ret = net_fail(NET_EBADF);
    else if (k->type != NET_SOCK_STREAM && k->type != NET_SOCK_STREAM_P2P)
        ret = net_fail(NET_EOPNOTSUPP);
    else {
        if (!k->bound) {
            k->bound = true;
            k->local_port = bswap16(g_next_port++);
        }
        k->listening = true;
    }
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV int32_t ps4_sceNetConnect(int32_t s, const net_sockaddr_in *addr, uint32_t addrlen) {
    int32_t ret;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    if (!k) ret = net_fail(NET_EBADF);
    else if (!addr) ret = net_fail(NET_EFAULT);
    else if (addrlen < sizeof(net_sockaddr_in)) ret = net_fail(NET_EINVAL);
    else if (k->listening) ret = net_fail(NET_EISCONN);
    else if (is_loopback(addr->addr)) ret = net_fail(NET_ECONNREFUSED);
    else ret = net_fail(NET_ENETUNREACH);
    plat_mutex_unlock(g_net_lock);
    if (addr)
        log_info("net: connect %#x to %u.%u.%u.%u:%u -> offline", (unsigned)s,
                 addr->addr & 0xff, (addr->addr >> 8) & 0xff, (addr->addr >> 16) & 0xff,
                 addr->addr >> 24, bswap16(addr->port));
    return ret;
}

/* Wait until the socket is closed or `timeout_us` passes (0 = forever). Nothing
 * ever arrives, so the result is EWOULDBLOCK (timeout) or EBADF (closed).
 * Called with g_net_lock held; returns with it released. */
static int32_t sock_wait_nothing(net_socket *k, int32_t timeout_us) {
    k->waiters++;
    uint64_t deadline = timeout_us > 0 ? plat_monotonic_ns() + (uint64_t)timeout_us * 1000 : 0;
    int32_t ret;
    for (;;) {
        if (k->closing) {
            ret = net_fail(NET_EBADF);
            break;
        }
        uint64_t wait_us = UINT64_MAX;
        if (deadline) {
            uint64_t now = plat_monotonic_ns();
            if (now >= deadline) {
                ret = net_fail(NET_EWOULDBLOCK);
                break;
            }
            wait_us = (deadline - now + 999) / 1000;
        }
        plat_cond_wait(g_net_cond, g_net_lock, wait_us);
    }
    if (--k->waiters == 0 && k->closing) k->used = false;
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV int32_t ps4_sceNetAccept(int32_t s, net_sockaddr_in *addr, uint32_t *addrlen) {
    (void)addr; (void)addrlen;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    int32_t ret;
    if (!k) ret = net_fail(NET_EBADF);
    else if (!k->listening) ret = net_fail(NET_EINVAL);
    else if (k->nbio) ret = net_fail(NET_EWOULDBLOCK);
    else return sock_wait_nothing(k, k->accepttimeo_us);
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV int32_t ps4_sceNetRecvfrom(int32_t s, void *buf, uint64_t len, int32_t flags,
                                      net_sockaddr_in *from, uint32_t *fromlen) {
    (void)buf; (void)len; (void)from; (void)fromlen;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    int32_t ret;
    if (!k) ret = net_fail(NET_EBADF);
    else if (k->type == NET_SOCK_STREAM || k->type == NET_SOCK_STREAM_P2P)
        ret = net_fail(NET_ENOTCONN); /* a stream socket can never connect offline */
    else if (k->nbio || (flags & NET_MSG_DONTWAIT)) ret = net_fail(NET_EWOULDBLOCK);
    else return sock_wait_nothing(k, k->rcvtimeo_us);
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV int32_t ps4_sceNetRecv(int32_t s, void *buf, uint64_t len, int32_t flags) {
    return ps4_sceNetRecvfrom(s, buf, len, flags, NULL, NULL);
}

static SYSV int32_t ps4_sceNetSendto(int32_t s, const void *buf, uint64_t len, int32_t flags,
                                    const net_sockaddr_in *to, uint32_t tolen) {
    (void)buf; (void)flags;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    int32_t ret;
    if (!k) ret = net_fail(NET_EBADF);
    else if (k->type == NET_SOCK_STREAM || k->type == NET_SOCK_STREAM_P2P)
        ret = net_fail(NET_ENOTCONN);
    else if (!to) ret = net_fail(NET_EDESTADDRREQ);
    else if (tolen < sizeof(net_sockaddr_in)) ret = net_fail(NET_EINVAL);
    else if (is_loopback(to->addr)) ret = (int32_t)len; /* nobody listens: dropped */
    else ret = net_fail(NET_ENETUNREACH);
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static SYSV int32_t ps4_sceNetSend(int32_t s, const void *buf, uint64_t len, int32_t flags) {
    return ps4_sceNetSendto(s, buf, len, flags, NULL, 0);
}

static SYSV int32_t ps4_sceNetGetsockname(int32_t s, net_sockaddr_in *addr, uint32_t *addrlen) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    if (!k) ret = net_fail(NET_EBADF);
    else if (!addr || !addrlen) ret = net_fail(NET_EFAULT);
    else if (*addrlen < sizeof(net_sockaddr_in)) ret = net_fail(NET_EINVAL);
    else {
        memset(addr, 0, sizeof *addr);
        addr->len = sizeof *addr;
        addr->family = NET_AF_INET;
        addr->addr = k->local_addr;
        addr->port = k->local_port;
        *addrlen = sizeof *addr;
    }
    plat_mutex_unlock(g_net_lock);
    return ret;
}

/* Integer socket options this layer keeps; NULL for the rest (accepted, read as 0). */
static int32_t *sockopt_slot(net_socket *k, int32_t name) {
    switch (name) {
    case NET_SO_RCVTIMEO: return &k->rcvtimeo_us;
    case NET_SO_SNDTIMEO: return &k->sndtimeo_us;
    case NET_SO_ACCEPTTIMEO: return &k->accepttimeo_us;
    case NET_SO_RCVBUF: return &k->rcvbuf;
    case NET_SO_SNDBUF: return &k->sndbuf;
    default: return NULL;
    }
}

static SYSV int32_t ps4_sceNetSetsockopt(int32_t s, int32_t level, int32_t optname,
                                        const void *optval, uint32_t optlen) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    if (!k) ret = net_fail(NET_EBADF);
    else if (optlen && !optval) ret = net_fail(NET_EFAULT);
    else if (level == NET_SOL_SOCKET && optlen >= 4) {
        int32_t v;
        memcpy(&v, optval, 4);
        int32_t *slot = sockopt_slot(k, optname);
        if (optname == NET_SO_NBIO) k->nbio = v != 0;
        else if (slot) *slot = v;
    }
    plat_mutex_unlock(g_net_lock);
    log_debug("net: setsockopt %#x level %#x opt %#x", (unsigned)s, (unsigned)level,
              (unsigned)optname);
    return ret;
}

static SYSV int32_t ps4_sceNetGetsockopt(int32_t s, int32_t level, int32_t optname, void *optval,
                                        uint32_t *optlen) {
    int32_t ret = SCE_OK;
    plat_mutex_lock(g_net_lock);
    net_socket *k = sock_get(s);
    if (!k) ret = net_fail(NET_EBADF);
    else if (!optval || !optlen) ret = net_fail(NET_EFAULT);
    else if (*optlen < 4) ret = net_fail(NET_EINVAL);
    else {
        int32_t v = 0;
        if (level == NET_SOL_SOCKET) {
            int32_t *slot = sockopt_slot(k, optname);
            if (optname == NET_SO_NBIO) v = k->nbio;
            else if (optname == NET_SO_TYPE) v = k->type;
            else if (slot) v = *slot;
            /* SO_ERROR / SO_ERROR_EX: no pending error */
        }
        memcpy(optval, &v, 4);
        *optlen = 4;
    }
    plat_mutex_unlock(g_net_lock);
    return ret;
}

static void fill_sock_info(int32_t id, const net_socket *k, net_sock_info *o) {
    memset(o, 0, sizeof *o);
    memcpy(o->name, k->name, sizeof o->name);
    o->s = id;
    o->socket_type = (int8_t)k->type;
    o->policy = -1;
    o->state = NET_SOCKINFO_STATE_OPENED;
    o->flags = NET_SOCKINFO_F_SELF | (k->nbio ? NET_SOCKINFO_F_NONBLOCK : 0);
    o->local_adr = k->local_addr;
    o->local_port = k->local_port;
    o->recv_buffer_size = k->rcvbuf;
    o->send_buffer_size = k->sndbuf;
}

/* s >= 0: that socket. s < 0: all sockets, up to n (or the count if info is NULL). */
static SYSV int32_t ps4_sceNetGetSockInfo(int32_t s, net_sock_info *info, int32_t n,
                                         int32_t flags) {
    (void)flags;
    int32_t ret = 0;
    plat_mutex_lock(g_net_lock);
    if (!info) {
        if (s >= 0) ret = net_fail(NET_EINVAL);
        else
            for (int i = 0; i < NET_MAX_SOCKETS; i++) ret += g_sockets[i].used && !g_sockets[i].closing;
    } else if (n <= 0) {
        ret = net_fail(NET_EINVAL);
    } else if (s >= 0) {
        net_socket *k = sock_get(s);
        if (!k) ret = net_fail(NET_EBADF);
        else {
            fill_sock_info(s, k, info);
            ret = 1;
        }
    } else {
        for (int i = 0; i < NET_MAX_SOCKETS && ret < n; i++)
            if (g_sockets[i].used && !g_sockets[i].closing)
                fill_sock_info(NET_SOCK_BASE + i, &g_sockets[i], &info[ret++]);
    }
    plat_mutex_unlock(g_net_lock);
    return ret;
}

HLE_LIBRARY(hle_net, "libSceNet",
    HLE(sceNetInit),
    HLE(sceNetPoolCreate),
    HLE(sceNetPoolDestroy),
    HLE(sceNetHtonl), HLE(sceNetHtons), HLE(sceNetNtohl), HLE(sceNetNtohs),
    HLE(sceNetInetPton),
    HLE(sceNetInetNtop),
    HLE(sceNetResolverCreate),
    HLE(sceNetResolverDestroy),
    HLE(sceNetResolverStartNtoa),
    HLE(sceNetResolverStartAton),
    HLE(sceNetSocket),
    HLE(sceNetSocketClose),
    HLE(sceNetBind),
    HLE(sceNetListen),
    HLE(sceNetAccept),
    HLE(sceNetConnect),
    HLE(sceNetRecv),
    HLE(sceNetRecvfrom),
    HLE(sceNetSend),
    HLE(sceNetSendto),
    HLE(sceNetGetsockname),
    HLE(sceNetSetsockopt),
    HLE(sceNetGetsockopt),
    HLE(sceNetGetSockInfo),
)

/* ---- libSceNetCtl ------------------------------------------------------- */

static SYSV int32_t ps4_sceNetCtlInit(void) {
    g_netctl_inited = 1;
    log_info("net: sceNetCtlInit (disconnected)");
    return SCE_OK;
}

static SYSV void ps4_sceNetCtlTerm(void) { g_netctl_inited = 0; }

static SYSV int32_t ps4_sceNetCtlGetState(int32_t *state) {
    if (!state) return NET_CTL_ERROR_INVALID_ADDR;
    *state = NET_CTL_STATE_DISCONNECTED;
    return SCE_OK;
}

static SYSV int32_t ps4_sceNetCtlGetInfo(int32_t code, void *info) {
    log_debug("net: sceNetCtlGetInfo(%d) -> not connected", code);
    if (!info) return NET_CTL_ERROR_INVALID_ADDR;
    return NET_CTL_ERROR_NOT_CONNECTED;
}

HLE_LIBRARY(hle_netctl, "libSceNetCtl",
    HLE(sceNetCtlInit),
    HLE(sceNetCtlTerm),
    HLE(sceNetCtlGetState),
    HLE(sceNetCtlGetInfo),
)

/* ---- libScePosix sockets ------------------------------------------------ */

static int32_t posix_fail(int e) {
    *hle_errno() = e;
    return -1;
}

static SYSV int32_t ps4_socket(int32_t domain, int32_t type, int32_t protocol) {
    log_info("net: posix socket(%d, %d, %d) -> ENETDOWN (offline)", domain, type, protocol);
    return posix_fail(NET_ENETDOWN);
}

/* No posix socket can exist, so every fd handed to these is not one. */
static SYSV int32_t ps4_bind(int32_t fd, const void *a, uint32_t l) {
    (void)fd; (void)a; (void)l;
    return posix_fail(NET_EBADF);
}
static SYSV int32_t ps4_connect(int32_t fd, const void *a, uint32_t l) {
    (void)fd; (void)a; (void)l;
    return posix_fail(NET_EBADF);
}
static SYSV int32_t ps4_listen(int32_t fd, int32_t backlog) {
    (void)fd; (void)backlog;
    return posix_fail(NET_EBADF);
}
static SYSV int32_t ps4_accept(int32_t fd, void *a, uint32_t *l) {
    (void)fd; (void)a; (void)l;
    return posix_fail(NET_EBADF);
}
static SYSV int64_t ps4_send(int32_t fd, const void *b, uint64_t n, int32_t f) {
    (void)fd; (void)b; (void)n; (void)f;
    return posix_fail(NET_EBADF);
}
static SYSV int64_t ps4_recv(int32_t fd, void *b, uint64_t n, int32_t f) {
    (void)fd; (void)b; (void)n; (void)f;
    return posix_fail(NET_EBADF);
}
static SYSV int32_t ps4_setsockopt(int32_t fd, int32_t lv, int32_t nm, const void *v, uint32_t l) {
    (void)fd; (void)lv; (void)nm; (void)v; (void)l;
    return posix_fail(NET_EBADF);
}
static SYSV int32_t ps4_shutdown(int32_t fd, int32_t how) {
    (void)fd; (void)how;
    return posix_fail(NET_EBADF);
}

/* FreeBSD fd_set (1024 bits) and timeval. */
typedef struct { uint64_t bits[16]; } ps4_fd_set;
typedef struct { int64_t sec, usec; } ps4_timeval;

static int fdset_count(const ps4_fd_set *s, int nfds) {
    int n = 0;
    if (s)
        for (int fd = 0; fd < nfds; fd++) n += (int)((s->bits[fd / 64] >> (fd % 64)) & 1);
    return n;
}

/* Only regular files can be in the sets (no sockets exist), and files are
 * always ready; with nothing to watch, select is a sleep. */
static SYSV int32_t ps4_select(int32_t nfds, ps4_fd_set *rd, ps4_fd_set *wr, ps4_fd_set *ex,
                              ps4_timeval *tv) {
    if (nfds < 0 || (tv && (tv->sec < 0 || tv->usec < 0 || tv->usec >= 1000000)))
        return posix_fail(NET_EINVAL);
    if (nfds > 1024) nfds = 1024;
    int ready = fdset_count(rd, nfds) + fdset_count(wr, nfds);
    if (ex) memset(ex, 0, sizeof *ex);
    if (ready > 0) return ready;
    if (tv) {
        uint64_t us = (uint64_t)tv->sec * 1000000u + (uint64_t)tv->usec;
        if (us) plat_sleep_us(us);
        return 0;
    }
    log_warn("net: select with no fds and no timeout blocks forever");
    for (;;) plat_sleep_us(1000000);
}

HLE_LIBRARY(hle_posix_net, "libScePosix",
    HLE(socket), HLE(bind), HLE(connect), HLE(listen), HLE(accept),
    HLE(send), HLE(recv), HLE(setsockopt), HLE(shutdown), HLE(select),
)
