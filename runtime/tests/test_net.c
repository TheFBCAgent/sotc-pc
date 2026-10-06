/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Offline network: libSceNet, NetCtl, Http, Ssl and the posix socket calls. */
#include <stdio.h>
#include <string.h>

#include "hle/hle.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#define FN(lib, name, type) ((type)hle_lookup(lib, name))

typedef SYSV int32_t (*fn_v)(void);
typedef SYSV int32_t (*fn_i)(int32_t);
typedef SYSV int32_t (*fn_pi)(int32_t *);
typedef SYSV int32_t (*fn_ip)(int32_t, void *);
typedef SYSV int32_t (*fn_pool)(const char *, int32_t, int32_t);
typedef SYSV uint32_t (*fn_u32)(uint32_t);
typedef SYSV uint16_t (*fn_u16)(uint16_t);
typedef SYSV int32_t (*fn_pton)(int32_t, const char *, void *);
typedef SYSV const char *(*fn_ntop)(int32_t, const void *, char *, uint32_t);
typedef SYSV int32_t (*fn_ntoa)(int32_t, const char *, uint32_t *, int32_t, int32_t, int32_t);
typedef SYSV int32_t (*fn_sock)(const char *, int32_t, int32_t, int32_t);
typedef SYSV int32_t (*fn_addr)(int32_t, const void *, uint32_t);
typedef SYSV int32_t (*fn_addrp)(int32_t, void *, uint32_t *);
typedef SYSV int32_t (*fn_ii)(int32_t, int32_t);
typedef SYSV int32_t (*fn_recv)(int32_t, void *, uint64_t, int32_t);
typedef SYSV int32_t (*fn_setopt)(int32_t, int32_t, int32_t, const void *, uint32_t);
typedef SYSV int32_t (*fn_getopt)(int32_t, int32_t, int32_t, void *, uint32_t *);
typedef SYSV int32_t (*fn_sockinfo)(int32_t, void *, int32_t, int32_t);
typedef SYSV int32_t (*fn_psock)(int32_t, int32_t, int32_t);
typedef SYSV int32_t (*fn_select)(int32_t, void *, void *, void *, void *);
typedef SYSV int32_t (*fn_hinit)(int32_t, int32_t, uint64_t);
typedef SYSV int32_t (*fn_htmpl)(int32_t, const char *, int32_t, int32_t);
typedef SYSV int32_t (*fn_hconn)(int32_t, const char *, int32_t);
typedef SYSV int32_t (*fn_hreq)(int32_t, int32_t, const char *, uint64_t);
typedef SYSV int32_t (*fn_hhdr)(int32_t, const char *, const char *, int32_t);
typedef SYSV int32_t (*fn_hsend)(int32_t, const void *, uint64_t);
typedef SYSV int32_t (*fn_hcep)(int32_t, void **);
typedef SYSV int32_t (*fn_hdep)(int32_t, void *);
typedef SYSV int32_t (*fn_hsep)(int32_t, void *, void *);
typedef SYSV int32_t (*fn_hwait)(void *, void *, int32_t, int32_t);
typedef SYSV int32_t (*fn_hesc)(char *, uint64_t *, uint64_t, const char *);
typedef SYSV int32_t (*fn_ssl)(uint64_t);

#define NET_ERR(e) ((int32_t)(0x80410100u | (e)))

typedef struct {
    uint8_t len, family;
    uint16_t port;
    uint32_t addr;
    uint16_t vport;
    char zero[6];
} sockaddr_in_t;

typedef struct {
    uint32_t events, detail;
    int32_t id;
    void *user_arg;
} nb_event_t;

static void test_net(void) {
    CHECK(FN("libSceNet", "sceNetInit", fn_v)() == 0);
    int32_t pool = FN("libSceNet", "sceNetPoolCreate", fn_pool)("pool", 64 * 1024, 0);
    CHECK(pool > 0);
    CHECK(FN("libSceNet", "sceNetPoolCreate", fn_pool)("pool", 16, 0) == NET_ERR(22));

    CHECK(FN("libSceNet", "sceNetHtonl", fn_u32)(0x11223344) == 0x44332211);
    CHECK(FN("libSceNet", "sceNetNtohs", fn_u16)(0x1122) == 0x2211);

    uint8_t a[4] = {0};
    fn_pton pton = FN("libSceNet", "sceNetInetPton", fn_pton);
    CHECK(pton(2, "192.168.1.20", a) == 1 && a[0] == 192 && a[3] == 20);
    CHECK(pton(2, "1.2.3", a) == 0);
    CHECK(pton(2, "1.2.3.256", a) == 0);
    CHECK(pton(2, "01.2.3.4", a) == 0);
    CHECK(pton(28, "::1", a) == NET_ERR(47));
    char txt[16];
    uint8_t b[4] = {10, 0, 0, 1};
    fn_ntop ntop = FN("libSceNet", "sceNetInetNtop", fn_ntop);
    CHECK(ntop(2, b, txt, sizeof txt) == txt && strcmp(txt, "10.0.0.1") == 0);
    CHECK(ntop(2, b, txt, 4) == NULL);

    /* Resolver: numeric works, names need DNS. */
    int32_t rid = FN("libSceNet", "sceNetResolverCreate", fn_pool)("res", pool, 0);
    CHECK(rid > 0);
    fn_ntoa ntoa = FN("libSceNet", "sceNetResolverStartNtoa", fn_ntoa);
    uint32_t ip = 0;
    CHECK(ntoa(rid, "127.0.0.1", &ip, 0, 0, 0) == 0 && ip == 0x0100007f);
    CHECK(ntoa(rid, "example.com", &ip, 0, 0, 0) == NET_ERR(225));
    CHECK(ntoa(999, "example.com", &ip, 0, 0, 0) == NET_ERR(9));
    CHECK(FN("libSceNet", "sceNetResolverDestroy", fn_i)(rid) == 0);
    CHECK(FN("libSceNet", "sceNetResolverDestroy", fn_i)(rid) == NET_ERR(9));
    CHECK(FN("libSceNet", "sceNetPoolDestroy", fn_i)(pool) == 0);

    /* Sockets. */
    fn_sock sock = FN("libSceNet", "sceNetSocket", fn_sock);
    int32_t tcp = sock("tcp", 2, 1, 0);
    int32_t udp = sock("udp", 2, 2, 0);
    CHECK(tcp > 0 && udp > 0 && tcp != udp);
    CHECK(sock("bad", 99, 1, 0) == NET_ERR(47));

    sockaddr_in_t sa = {16, 2, 0x5000 /* port 80 */, 0x0101a8c0 /* 192.168.1.1 */, 0, {0}};
    fn_addr connect = FN("libSceNet", "sceNetConnect", fn_addr);
    CHECK(connect(tcp, &sa, sizeof sa) == NET_ERR(51));
    sockaddr_in_t lo = sa;
    lo.addr = 0x0100007f;
    CHECK(connect(tcp, &lo, sizeof lo) == NET_ERR(61));
    CHECK(connect(12345, &sa, sizeof sa) == NET_ERR(9));

    fn_recv send = FN("libSceNet", "sceNetSend", fn_recv);
    fn_recv recv = FN("libSceNet", "sceNetRecv", fn_recv);
    char buf[8];
    CHECK(send(tcp, "x", 1, 0) == NET_ERR(57));
    CHECK(recv(tcp, buf, sizeof buf, 0) == NET_ERR(57));
    CHECK(send(udp, "x", 1, 0) == NET_ERR(39));
    CHECK(recv(udp, buf, sizeof buf, 0x80 /* DONTWAIT */) == NET_ERR(35));

    /* Blocking recv honours SO_RCVTIMEO. */
    int32_t tmo = 20000;
    fn_setopt setopt = FN("libSceNet", "sceNetSetsockopt", fn_setopt);
    fn_getopt getopt = FN("libSceNet", "sceNetGetsockopt", fn_getopt);
    CHECK(setopt(udp, 0xffff, 0x1106, &tmo, 4) == 0);
    uint64_t t0 = plat_monotonic_ns();
    CHECK(recv(udp, buf, sizeof buf, 0) == NET_ERR(35));
    CHECK(plat_monotonic_ns() - t0 >= 15000000ull);
    int32_t v = 0;
    uint32_t vl = 4;
    CHECK(getopt(udp, 0xffff, 0x1106, &v, &vl) == 0 && v == 20000);
    CHECK(getopt(udp, 0xffff, 0x1008, &v, &vl) == 0 && v == 2);

    /* Bind, getsockname, listen, non-blocking accept. */
    sockaddr_in_t any = {16, 2, 0, 0, 0, {0}};
    CHECK(FN("libSceNet", "sceNetBind", fn_addr)(tcp, &any, sizeof any) == 0);
    CHECK(FN("libSceNet", "sceNetBind", fn_addr)(tcp, &any, sizeof any) == NET_ERR(22));
    sockaddr_in_t got;
    uint32_t gl = sizeof got;
    CHECK(FN("libSceNet", "sceNetGetsockname", fn_addrp)(tcp, &got, &gl) == 0);
    CHECK(got.family == 2 && got.port != 0);
    CHECK(FN("libSceNet", "sceNetListen", fn_ii)(tcp, 4) == 0);
    CHECK(FN("libSceNet", "sceNetListen", fn_ii)(udp, 4) == NET_ERR(45));
    int32_t one = 1;
    CHECK(setopt(tcp, 0xffff, 0x1200, &one, 4) == 0);
    CHECK(FN("libSceNet", "sceNetAccept", fn_addrp)(tcp, NULL, NULL) == NET_ERR(35));

    uint8_t info[160 * 4];
    fn_sockinfo sockinfo = FN("libSceNet", "sceNetGetSockInfo", fn_sockinfo);
    CHECK(sockinfo(-1, NULL, 0, 0) == 2);
    CHECK(sockinfo(tcp, info, 1, 0) == 1);
    CHECK(sockinfo(-1, info, 4, 0) == 2);

    fn_i sclose = FN("libSceNet", "sceNetSocketClose", fn_i);
    CHECK(sclose(tcp) == 0 && sclose(udp) == 0);
    CHECK(sclose(tcp) == NET_ERR(9));
    CHECK(sockinfo(-1, NULL, 0, 0) == 0);
}

static void test_netctl(void) {
    CHECK(FN("libSceNetCtl", "sceNetCtlInit", fn_v)() == 0);
    int32_t state = -1;
    CHECK(FN("libSceNetCtl", "sceNetCtlGetState", fn_pi)(&state) == 0 && state == 0);
    char info[256];
    CHECK(FN("libSceNetCtl", "sceNetCtlGetInfo", fn_ip)(14, info) == (int32_t)0x80412108);
    CHECK(hle_lookup("libSceNetCtl", "sceNetCtlTerm") != NULL);
}

static void test_posix(void) {
    *hle_errno() = 0;
    CHECK(FN("libScePosix", "socket", fn_psock)(2, 1, 0) == -1 && *hle_errno() == 50);
    CHECK(FN("libScePosix", "connect", fn_addr)(-1, NULL, 0) == -1 && *hle_errno() == 9);
    const char *names[] = {"bind", "listen", "accept", "send", "recv", "setsockopt", "shutdown"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        CHECK(hle_lookup("libScePosix", names[i]) != NULL);
    /* select: nothing to watch, so it sleeps for the timeout. */
    int64_t tv[2] = {0, 10000};
    uint64_t t0 = plat_monotonic_ns();
    CHECK(FN("libScePosix", "select", fn_select)(0, NULL, NULL, NULL, tv) == 0);
    CHECK(plat_monotonic_ns() - t0 >= 5000000ull);
    uint64_t rd[16] = {0};
    rd[0] = 1u << 3;
    CHECK(FN("libScePosix", "select", fn_select)(4, rd, NULL, NULL, tv) == 1);
}

static void test_http(void) {
    fn_hinit init = FN("libSceHttp", "sceHttpInit", fn_hinit);
    fn_htmpl tmpl = FN("libSceHttp", "sceHttpCreateTemplate", fn_htmpl);
    fn_hconn conn = FN("libSceHttp", "sceHttpCreateConnectionWithURL", fn_hconn);
    fn_hreq req = FN("libSceHttp", "sceHttpCreateRequestWithURL", fn_hreq);
    fn_hsend sendreq = FN("libSceHttp", "sceHttpSendRequest", fn_hsend);

    CHECK(tmpl(1, "ua", 1, 0) == (int32_t)0x80431001); /* before init */
    int32_t ssl = FN("libSceSsl", "sceSslInit", fn_ssl)(0x10000);
    CHECK(ssl > 0);
    int32_t ctx = init(1, ssl, 0x10000);
    CHECK(ctx > 0);
    int32_t t = tmpl(ctx, "ua", 1, 0);
    CHECK(t > 0);
    CHECK(tmpl(t, "ua", 1, 0) == (int32_t)0x80431100); /* wrong kind of id */
    CHECK(conn(t, "ftp://x/", 1) == (int32_t)0x80431061);
    CHECK(conn(t, "nonsense", 1) == (int32_t)0x80433060);

    /* Blocking: a name fails with ENODNS, a literal address with NETWORK. */
    int32_t c = conn(t, "https://example.com/api", 1);
    CHECK(c > 0);
    int32_t r = req(c, 0, "https://example.com/api?x=1", 0);
    CHECK(r > 0);
    CHECK(req(c, 3, "https://example.com/", 0) == (int32_t)0x8043106b);
    CHECK(FN("libSceHttp", "sceHttpAddRequestHeader", fn_hhdr)(r, NULL, "v", 0) ==
          (int32_t)0x804311fe);
    int32_t st;
    CHECK(FN("libSceHttp", "sceHttpGetStatusCode", fn_ip)(r, &st) == (int32_t)0x80431065);
    CHECK(sendreq(r, NULL, 0) == (int32_t)0x80436002);
    CHECK(FN("libSceHttp", "sceHttpGetStatusCode", fn_ip)(r, &st) == (int32_t)0x80436002);
    int32_t c2 = conn(t, "http://10.0.0.1:8080/", 0);
    int32_t r2 = req(c2, 1, "http://10.0.0.1:8080/", 0);
    CHECK(sendreq(r2, NULL, 0) == (int32_t)0x80431063);

    /* Non-blocking with an epoll: EAGAIN, an error event, then the error. */
    void *eh = NULL;
    CHECK(FN("libSceHttp", "sceHttpCreateEpoll", fn_hcep)(ctx, &eh) == 0 && eh);
    int32_t r3 = req(c, 0, "https://example.com/", 0);
    CHECK(FN("libSceHttp", "sceHttpSetNonblock", fn_ii)(r3, 1) == 0);
    int marker;
    CHECK(FN("libSceHttp", "sceHttpSetEpoll", fn_hsep)(r3, eh, &marker) == 0);
    fn_hwait wait = FN("libSceHttp", "sceHttpWaitRequest", fn_hwait);
    nb_event_t ev[4];
    CHECK(wait(eh, ev, 4, 0) == 0);
    CHECK(wait(eh, ev, 4, 1000) == 0);
    CHECK(sendreq(r3, NULL, 0) == (int32_t)0x80431082);
    CHECK(wait(eh, ev, 4, -1) == 1 && ev[0].id == r3 && ev[0].user_arg == &marker &&
          ev[0].events == 0x20000);
    CHECK(sendreq(r3, NULL, 0) == (int32_t)0x80436002);
    CHECK(FN("libSceHttp", "sceHttpUnsetEpoll", fn_i)(r3) == 0);
    CHECK(FN("libSceHttp", "sceHttpDestroyEpoll", fn_hdep)(ctx, eh) == 0);
    CHECK(wait(eh, ev, 4, 0) == (int32_t)0x80431100);

    /* UriEscape. */
    fn_hesc esc = FN("libSceHttp", "sceHttpUriEscape", fn_hesc);
    uint64_t need = 0;
    CHECK(esc(NULL, &need, 0, "a b/~") == 0 && need == 10);
    char out[16];
    CHECK(esc(out, NULL, 4, "a b/~") == (int32_t)0x80431022);
    CHECK(esc(out, NULL, sizeof out, "a b/~") == 0 && strcmp(out, "a%20b%2F~") == 0);

    CHECK(FN("libSceHttp", "sceHttpDeleteRequest", fn_i)(r) == 0);
    CHECK(FN("libSceHttp", "sceHttpDeleteRequest", fn_i)(r) == (int32_t)0x80431100);
    CHECK(FN("libSceHttp", "sceHttpDeleteConnection", fn_i)(c) == 0);
    CHECK(FN("libSceHttp", "sceHttpDeleteTemplate", fn_i)(t) == 0);
    CHECK(FN("libSceHttp", "sceHttpTerm", fn_i)(ctx) == 0);
    CHECK(FN("libSceHttp", "sceHttpTerm", fn_i)(ctx) == (int32_t)0x80431001);
    CHECK(FN("libSceSsl", "sceSslTerm", fn_i)(ssl) == 0);
}

int main(void) {
    log_init();
    hle_init();
    test_net();
    test_netctl();
    test_posix();
    test_http();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("net tests passed\n");
    return 0;
}
