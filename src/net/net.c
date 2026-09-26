#include "oops/net.h"
#include "oops/system.h"

/*
 * Two worlds, one interface.
 *
 * The endian helpers and the pton/ntop pair below are pure and identical
 * everywhere. The socket calls are not: which symbols a process can reach
 * depends on how it was loaded.
 *
 *   - A title/eboot/pkg links libSceNet, and the sceNet* entry points resolve.
 *   - An elfldr payload is handed an export table that carries NONE of
 * libSceNet, but DOES carry the platform's FreeBSD-derived POSIX socket
 * exports. obSCEne measured this on firmware 12.40 (its inbox, sweep
 * 20260909-083918): bind/listen/accept/connect/recv/
 *     _sendto/_setsockopt/close/__error are all callable there, `socket` is
 * exported under no spelling, and `__sys_socketex(name, domain, type,
 * protocol)` or the raw system call SYS_socket = 97 is what makes a descriptor.
 * Errors are POSIX here - the call returns -1 and sets errno, read through
 * `__error()` - not the 0x8041xxxx encoding sceNet uses.
 *
 * So the platform path is built on the POSIX exports first, resolved by the
 * loader as weak references to their exported names, with sceNet as a fallback
 * for a context that has it. Confirmed on hardware: in an unsigned -shared
 * --unresolved-symbols=ignore-all payload that names no library, all ten of
 * these weak references bind at load (obSCEne sweep 20260909-144348), and a
 * payload opened, bound, listened, accepted, received and echoed on a real
 * socket end to end - so no runtime kexport-table lookup is needed for these.
 * On the host build (make test) none of that exists; the socket calls compile
 * to honest failures, and the loopback integration test exercises the host's
 * own libc sockets directly, so nothing here is on its path.
 */

/* ---- pure helpers: identical on every target --------------------------------
 */

static int s_net_initialized = 0;

int oops_net_inet_pton(const char *src, uint32_t *dst) {
    if (!src || !dst)
        return -1;

    uint32_t octets[4] = {0, 0, 0, 0};
    int cur = 0;
    int digits = 0; /* in the current octet: an empty or over-long one is malformed */
    const char *p = src;

    while (*p != '\0') {
        if (*p >= '0' && *p <= '9') {
            octets[cur] = octets[cur] * 10 + (uint32_t)(*p - '0');
            digits++;
            if (octets[cur] > 255 || digits > 3)
                return -1;
        } else if (*p == '.') {
            if (digits == 0)
                return -1;
            cur++;
            if (cur > 3)
                return -1;
            digits = 0;
        } else {
            return -1;
        }
        p++;
    }

    if (cur != 3 || digits == 0)
        return -1;

    /* Network byte order (big-endian wire format) */
    *dst = (octets[0]) | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return 0;
}

int oops_net_inet_ntop(uint32_t src, char *dst, size_t dst_len) {
    if (!dst || dst_len < 16)
        return -1;

    uint8_t bytes[4] = {
        (uint8_t)(src & 0xFF),
        (uint8_t)((src >> 8) & 0xFF),
        (uint8_t)((src >> 16) & 0xFF),
        (uint8_t)((src >> 24) & 0xFF),
    };
    size_t pos = 0;

    for (int i = 0; i < 4; i++) {
        uint8_t val = bytes[i];
        if (val >= 100) {
            dst[pos++] = (char)('0' + (val / 100));
            dst[pos++] = (char)('0' + ((val / 10) % 10));
            dst[pos++] = (char)('0' + (val % 10));
        } else if (val >= 10) {
            dst[pos++] = (char)('0' + (val / 10));
            dst[pos++] = (char)('0' + (val % 10));
        } else {
            dst[pos++] = (char)('0' + val);
        }
        if (i < 3) {
            dst[pos++] = '.';
        }
    }
    dst[pos] = '\0';
    return 0;
}

#if (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1) || defined(OOPS_HOST_BUILD) ||  \
    defined(OBSCENE_HOST_BUILD)

/* ---- host build: sockets are not wired here ---------------------------------
 * The host suite reaches the OS through its own libc in the loopback test, so
 * these compile to honest failures and no unit test depends on them. */

int oops_net_init(void) {
    s_net_initialized = 1;
    return 0;
}
void oops_net_term(void) {
    s_net_initialized = 0;
}

int oops_socket(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    return -1;
}
int oops_bind(int sock, const char *ip, uint16_t port) {
    (void)sock;
    (void)ip;
    (void)port;
    return -1;
}
int oops_listen(int sock, int backlog) {
    (void)sock;
    (void)backlog;
    return -1;
}
int oops_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port) {
    (void)sock;
    if (client_ip && ip_len)
        client_ip[0] = '\0';
    if (client_port)
        *client_port = 0;
    return -1;
}
int oops_connect(int sock, const char *server_ip, uint16_t port) {
    (void)sock;
    (void)server_ip;
    (void)port;
    return -1;
}
long oops_send(int sock, const void *buf, size_t len, int flags) {
    (void)sock;
    (void)buf;
    (void)len;
    (void)flags;
    return -1;
}
long oops_recv(int sock, void *buf, size_t len, int flags) {
    (void)sock;
    (void)buf;
    (void)len;
    (void)flags;
    return -1;
}
long oops_sendto(int sock, const void *buf, size_t len, int flags, const char *to_ip,
                 uint16_t to_port) {
    (void)sock;
    (void)buf;
    (void)len;
    (void)flags;
    (void)to_ip;
    (void)to_port;
    return -1;
}
long oops_recvfrom(int sock, void *buf, size_t len, int flags, char *from_ip,
                   size_t ip_len, uint16_t *from_port) {
    (void)sock;
    (void)buf;
    (void)len;
    (void)flags;
    if (from_ip && ip_len)
        from_ip[0] = '\0';
    if (from_port)
        *from_port = 0;
    return -1;
}
int oops_setsockopt(int sock, int level, int optname, const void *optval,
                    size_t optlen) {
    (void)sock;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return -1;
}
int oops_set_nonblocking(int sock, int nonblocking) {
    (void)sock;
    (void)nonblocking;
    return -1;
}
void oops_close(int sock) {
    (void)sock;
}
int oops_net_would_block(long rc) {
    (void)rc;
    return 0;
}

#else

/* ---- freestanding target: the POSIX exports, resolved by the loader
 * ----------
 *
 * Weak references to the platform's exported names, primary spelling and the
 * leading-underscore alternate where the platform carries one. The loader binds
 * whichever the export table holds; a name that resolves to nothing stays null
 * and is skipped. All arities and the sockaddr shape are hardware-confirmed
 * (see the file header); none is guessed.
 */

typedef long ssize_t_;
typedef unsigned int socklen_t_;

/* Descriptor creation. `socket` is not exported; __sys_socketex takes a name
 * first, like the vendor's own creator, and a name or NULL both return a
 * descriptor. */
__attribute__((weak)) int __sys_socketex(const char *name, int domain, int type,
                                         int protocol);

/* The raw system call behind it. SYS_socket = 97 is hardware-confirmed (a bare
 * sys_call(97, AF_INET, SOCK_STREAM, IPPROTO_TCP) returned a descriptor).
 * Declared locally rather than via syscall.h so this file stays clear of the
 * kernel-primitive headers. */
__attribute__((weak)) long sys_call(long num, long a1, long a2, long a3, long a4,
                                    long a5, long a6);
#define OOPS_SYS_SOCKET 97
#define OOPS_SYS_CLOSE 6
#define OOPS_SYS_RECVFROM 29
#define OOPS_SYS_ACCEPT 30
#define OOPS_SYS_CONNECT 98
#define OOPS_SYS_BIND 104
#define OOPS_SYS_SETSOCKOPT 105
#define OOPS_SYS_LISTEN 106
#define OOPS_SYS_SENDTO 133

__attribute__((weak)) int bind(int s, const void *addr, socklen_t_ addrlen);
__attribute__((weak)) int _bind(int s, const void *addr, socklen_t_ addrlen);
__attribute__((weak)) int listen(int s, int backlog);
__attribute__((weak)) int _listen(int s, int backlog);
__attribute__((weak)) int accept(int s, void *addr, socklen_t_ *addrlen);
__attribute__((weak)) int _accept(int s, void *addr, socklen_t_ *addrlen);
__attribute__((weak)) int connect(int s, const void *addr, socklen_t_ addrlen);
__attribute__((weak)) int _connect(int s, const void *addr, socklen_t_ addrlen);
__attribute__((weak)) ssize_t_ recv(int s, void *buf, size_t len, int flags);
__attribute__((weak)) ssize_t_ _recv(int s, void *buf, size_t len, int flags);
/* The peer-reporting receive. Declared the same way as everything else here and
 * for the same reason: a weak reference the loader either binds or leaves null,
 * so asking costs nothing. It was previously absent because it did not appear in
 * the export census - but `sendto`, `bind` and `accept` all did, and `accept`
 * already takes an address out-parameter of exactly this shape, so the census
 * missing one name is a thinner reason than it looked. `p_recvfrom` below falls
 * back to `p_recv` if neither spelling binds, which is precisely the behaviour
 * this file had before. */
__attribute__((weak)) ssize_t_ recvfrom(int s, void *buf, size_t len, int flags,
                                        void *from, socklen_t_ *fromlen);
__attribute__((weak)) ssize_t_ _recvfrom(int s, void *buf, size_t len, int flags,
                                         void *from, socklen_t_ *fromlen);
__attribute__((weak)) ssize_t_ _sendto(int s, const void *buf, size_t len, int flags,
                                       const void *to, socklen_t_ tolen);
__attribute__((weak)) ssize_t_ sendto(int s, const void *buf, size_t len, int flags,
                                      const void *to, socklen_t_ tolen);
__attribute__((weak)) int _setsockopt(int s, int level, int name, const void *val,
                                      socklen_t_ len);
__attribute__((weak)) int setsockopt(int s, int level, int name, const void *val,
                                     socklen_t_ len);
__attribute__((weak)) int close(int fd);
__attribute__((weak)) int _close(int fd);
/* errno lives behind __error(); try-again is 35 (EAGAIN) on this console,
 * POSIX-plain. */
__attribute__((weak)) int *__error(void);
__attribute__((weak)) int *__errno(void);

#define OOPS_EAGAIN 35

/* Plain FreeBSD sockaddr_in - 16 bytes, sin_len then sin_family, no sin_vport.
 */
struct fbsd_sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};

/*
 * # The bare names in this link may not be the platform's
 *
 * **Every plain spelling above - `bind`, `connect`, `recv`, `close` - is also the name
 * a POSIX shim defines.** `oops-apps/common/posix/posix.c` defines all four, because a
 * port calls `bind` and something has to answer. A weak *reference* like the ones above
 * is satisfied by any strong definition in the same link, and the static linker gets
 * there long before the loader looks at the platform's export table. So in a title that
 * links both this file and that shim:
 *
 *     port calls bind()  ->  shim's bind()  ->  oops_bind()  ->  p_bind()  ->  shim's
 * bind()
 *
 * which is an unbounded recursion, and on this target that is a stack overflow on the
 * first packet rather than anything that names itself. `close` is the quieter version
 * of the same thing: it would reach the shim's descriptor `close`, which routes to the
 * *filesystem* layer, and a socket closed through `oops_fs_close` is a leak that looks
 * like a working call.
 *
 * `_sendto`, `_setsockopt` and `_recvfrom` already preferred the underscore spelling
 * for this reason, one function at a time. The rule is now uniform: **the underscore
 * spelling first, always, and the bare one only when nothing in this link has taken
 * it.**
 *
 * A title tells us which ones it has taken through `oops_net_bare_names_are_shimmed`,
 * whose bits `oops/net.h` defines. The answer is per name rather than all-or-nothing,
 * because the shim defines `bind` and `connect` and does *not* define `listen` or
 * `accept`, and a blanket refusal would take a working path away from the latter two
 * for the sake of the former.
 *
 * The default is defined here, weak, rather than left as a weak reference: a weak
 * reference leaves the symbol undefined in the payload, and oops-apps' link guard
 * reports every undefined name whether it is weak or not. 0 means nothing is shimmed and
 * every bare name is available; a title linking the POSIX shim overrides this, a strong
 * definition beating a weak one.
 *
 * The null test below costs one compare and covers a link that has neither definition.
 */
__attribute__((weak)) unsigned oops_net_bare_names_are_shimmed(void) {
    return 0u;
}
static int bare_ok(unsigned which) {
    if (!oops_net_bare_names_are_shimmed)
        return 1;
    return (oops_net_bare_names_are_shimmed() & which) ? 0 : 1;
}

static int p_bind(int s, const void *a, socklen_t_ l) {
    if (_bind)
        return _bind(s, a, l);
    if (bind && bare_ok(OOPS_NET_SHIMMED_BIND))
        return bind(s, a, l);
    if (sys_call)
        return (int)sys_call(OOPS_SYS_BIND, (long)s, (long)a, (long)l, 0, 0, 0);
    return -1;
}
static int p_listen(int s, int b) {
    if (_listen)
        return _listen(s, b);
    if (listen && bare_ok(OOPS_NET_SHIMMED_LISTEN))
        return listen(s, b);
    if (sys_call)
        return (int)sys_call(OOPS_SYS_LISTEN, (long)s, (long)b, 0, 0, 0, 0);
    return -1;
}
static int p_accept(int s, void *a, socklen_t_ *l) {
    if (_accept)
        return _accept(s, a, l);
    if (accept && bare_ok(OOPS_NET_SHIMMED_ACCEPT))
        return accept(s, a, l);
    if (sys_call)
        return (int)sys_call(OOPS_SYS_ACCEPT, (long)s, (long)a, (long)l, 0, 0, 0);
    return -1;
}
static int p_connect(int s, const void *a, socklen_t_ l) {
    if (_connect)
        return _connect(s, a, l);
    if (connect && bare_ok(OOPS_NET_SHIMMED_CONNECT))
        return connect(s, a, l);
    if (sys_call)
        return (int)sys_call(OOPS_SYS_CONNECT, (long)s, (long)a, (long)l, 0, 0, 0);
    return -1;
}
static ssize_t_ p_recv(int s, void *b, size_t n, int f) {
    if (_recv)
        return _recv(s, b, n, f);
    if (recv && bare_ok(OOPS_NET_SHIMMED_RECV))
        return recv(s, b, n, f);
    if (sys_call)
        return (ssize_t_)sys_call(OOPS_SYS_RECVFROM, (long)s, (long)b, (long)n, (long)f, 0, 0);
    return -1;
}
/* Whether the platform can report a datagram's sender at all. `oops_recvfrom`
 * below leaves the peer empty when this is 0, and its callers have to be able to
 * tell that apart from "the peer really is 0.0.0.0" - a POSIX `recvfrom` shim
 * reporting the wrong sender for every packet is the kind of wrong that looks
 * like a protocol bug in the game. */
static int p_have_recvfrom(void) {
    return (_recvfrom || (recvfrom && bare_ok(OOPS_NET_SHIMMED_RECVFROM)) || sys_call) ? 1 : 0;
}

static ssize_t_ p_recvfrom(int s, void *b, size_t n, int f, void *from,
                           socklen_t_ *fromlen) {
    if (_recvfrom)
        return _recvfrom(s, b, n, f, from, fromlen);
    if (recvfrom && bare_ok(OOPS_NET_SHIMMED_RECVFROM))
        return recvfrom(s, b, n, f, from, fromlen);
    if (sys_call)
        return (ssize_t_)sys_call(OOPS_SYS_RECVFROM, (long)s, (long)b, (long)n, (long)f, (long)from, (long)fromlen);
    return -1;
}
static ssize_t_ p_sendto(int s, const void *b, size_t n, int f, const void *to,
                         socklen_t_ tl) {
    if (_sendto)
        return _sendto(s, b, n, f, to, tl);
    if (sendto && bare_ok(OOPS_NET_SHIMMED_SENDTO))
        return sendto(s, b, n, f, to, tl);
    if (sys_call)
        return (ssize_t_)sys_call(OOPS_SYS_SENDTO, (long)s, (long)b, (long)n, (long)f, (long)to, (long)tl);
    return -1;
}
static int p_setsockopt(int s, int lv, int nm, const void *v, socklen_t_ l) {
    if (_setsockopt)
        return _setsockopt(s, lv, nm, v, l);
    if (setsockopt && bare_ok(OOPS_NET_SHIMMED_SETSOCKOPT))
        return setsockopt(s, lv, nm, v, l);
    if (sys_call)
        return (int)sys_call(OOPS_SYS_SETSOCKOPT, (long)s, (long)lv, (long)nm, (long)v, (long)l, 0);
    return -1;
}
static void p_close(int fd) {
    if (_close) {
        _close(fd);
        return;
    }
    if (close && bare_ok(OOPS_NET_SHIMMED_CLOSE)) {
        close(fd);
        return;
    }
    if (sys_call) {
        (void)sys_call(OOPS_SYS_CLOSE, (long)fd, 0, 0, 0, 0, 0);
    }
}
static int *p_errno(void) {
    if (__error)
        return __error();
    if (__errno)
        return __errno();
    return (int *)0;
}

static int socket_reachable(void) {
    return (__sys_socketex || sys_call) ? 1 : 0;
}

int oops_net_init(void) {
    /* POSIX sockets need no library bring-up; the marker keeps the interface
     * honest. */
    s_net_initialized = socket_reachable() ? 1 : 0;
    oops_log_info("NET", "network subsystem init: reachable=%d", s_net_initialized);
    return s_net_initialized ? 0 : -1;
}

void oops_net_term(void) {
    oops_log_info("NET", "network subsystem terminated");
    s_net_initialized = 0;
}

int oops_socket(int domain, int type, int protocol) {
    int proto = protocol;
    if (proto == 0) {
        proto = (type == OOPS_SOCK_STREAM) ? OOPS_IPPROTO_TCP : OOPS_IPPROTO_UDP;
    }
    int fd = -1;
    if (__sys_socketex) {
        fd = __sys_socketex("oops_sock", domain, type, proto);
    } else if (sys_call) {
        long r =
            sys_call(OOPS_SYS_SOCKET, (long)domain, (long)type, (long)proto, 0, 0, 0);
        if (r >= 0)
            fd = (int)r;
    }
    oops_log_debug("NET", "socket(domain=%d, type=%d, proto=%d) -> fd=%d", domain, type,
                   proto, fd);
    return fd;
}

static void fill_addr(struct fbsd_sockaddr_in *a, uint32_t ip_net, uint16_t port) {
    for (size_t i = 0; i < sizeof(*a); i++)
        ((uint8_t *)a)[i] = 0;
    a->sin_len = (uint8_t)sizeof(*a);
    a->sin_family = OOPS_AF_INET;
    a->sin_port = oops_htons(port);
    a->sin_addr = ip_net;
}

int oops_bind(int sock, const char *ip, uint16_t port) {
    if (sock < 0)
        return -1;
    uint32_t ip_net = 0; /* INADDR_ANY */
    if (ip && ip[0] != '\0') {
        if (oops_net_inet_pton(ip, &ip_net) != 0)
            return -1;
    }
    struct fbsd_sockaddr_in addr;
    fill_addr(&addr, ip_net, port);
    int rc = p_bind(sock, &addr, (socklen_t_)sizeof(addr));
    oops_log_debug("NET", "bind(sock=%d, ip=%s, port=%u) -> rc=%d", sock,
                   ip ? ip : "0.0.0.0", port, rc);
    return rc;
}

int oops_listen(int sock, int backlog) {
    if (sock < 0)
        return -1;
    int rc = p_listen(sock, (backlog <= 0) ? 5 : backlog);
    oops_log_debug("NET", "listen(sock=%d, backlog=%d) -> rc=%d", sock, backlog, rc);
    return rc;
}

int oops_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port) {
    if (sock < 0)
        return -1;
    if (client_ip && ip_len > 0)
        client_ip[0] = '\0';

    struct fbsd_sockaddr_in addr;
    for (size_t i = 0; i < sizeof(addr); i++)
        ((uint8_t *)&addr)[i] = 0;
    socklen_t_ addrlen = (socklen_t_)sizeof(addr);

    int client_sock = p_accept(sock, &addr, &addrlen);
    if (client_sock >= 0) {
        if (client_ip && ip_len > 0)
            oops_net_inet_ntop(addr.sin_addr, client_ip, ip_len);
        if (client_port)
            *client_port = oops_ntohs(addr.sin_port);
        oops_log_info("NET", "accept(sock=%d) -> client_sock=%d from %s:%u", sock,
                      client_sock, client_ip ? client_ip : "?",
                      client_port ? *client_port : 0);
    }
    return client_sock;
}

int oops_connect(int sock, const char *server_ip, uint16_t port) {
    if (sock < 0 || !server_ip)
        return -1;
    uint32_t ip_net = 0;
    if (oops_net_inet_pton(server_ip, &ip_net) != 0) {
        char resolved[32];
        if (oops_net_resolve(server_ip, resolved, sizeof(resolved)) != 0) {
            oops_log_warn("NET", "connect: failed to resolve host '%s'", server_ip);
            return -1;
        }
        if (oops_net_inet_pton(resolved, &ip_net) != 0)
            return -1;
    }
    struct fbsd_sockaddr_in addr;
    fill_addr(&addr, ip_net, port);
    int rc = p_connect(sock, &addr, (socklen_t_)sizeof(addr));
    oops_log_info("NET", "connect(sock=%d, server=%s, port=%u) -> rc=%d", sock,
                  server_ip, port, rc);
    return rc;
}

long oops_send(int sock, const void *buf, size_t len, int flags) {
    if (sock < 0 || !buf)
        return -1;
    /* No exported `send`; it is `_sendto` with a null destination. */
    long rc = (long)p_sendto(sock, buf, len, flags, (const void *)0, 0);
    oops_log_trace("NET", "send(sock=%d, len=%zu) -> sent=%ld", sock, len, rc);
    return rc;
}

long oops_recv(int sock, void *buf, size_t len, int flags) {
    if (sock < 0 || !buf)
        return -1;
    long rc = (long)p_recv(sock, buf, len, flags);
    oops_log_trace("NET", "recv(sock=%d, max_len=%zu) -> recvd=%ld", sock, len, rc);
    return rc;
}

long oops_sendto(int sock, const void *buf, size_t len, int flags, const char *to_ip,
                 uint16_t to_port) {
    if (sock < 0 || !buf || !to_ip)
        return -1;
    uint32_t ip_net = 0;
    if (oops_net_inet_pton(to_ip, &ip_net) != 0)
        return -1;
    struct fbsd_sockaddr_in addr;
    fill_addr(&addr, ip_net, to_port);
    return (long)p_sendto(sock, buf, len, flags, &addr, (socklen_t_)sizeof(addr));
}

long oops_recvfrom(int sock, void *buf, size_t len, int flags, char *from_ip,
                   size_t ip_len, uint16_t *from_port) {
    if (sock < 0 || !buf)
        return -1;
    if (from_ip && ip_len > 0)
        from_ip[0] = '\0';

    if (from_port)
        *from_port = 0;

    /* No `recvfrom` export bound: the data still arrives, the sender does not.
     * `from_ip` stays the empty string it was set to above, which is this call's
     * documented way of saying "unknown" - a caller that needs the peer must check
     * for it rather than read 0.0.0.0 out of a zeroed address. */
    if (!p_have_recvfrom())
        return (long)p_recv(sock, buf, len, flags);

    struct fbsd_sockaddr_in addr;
    for (size_t i = 0; i < sizeof(addr); i++)
        ((uint8_t *)&addr)[i] = 0;
    socklen_t_ addrlen = (socklen_t_)sizeof(addr);

    long rc = (long)p_recvfrom(sock, buf, len, flags, &addr, &addrlen);
    if (rc < 0)
        return rc;

    /* Only an IPv4 address of the full size is transcribed. A short write means
     * the platform reported something this struct does not describe, and a partly
     * filled address is worse than none - so it is left unknown. */
    if (addrlen >= (socklen_t_)sizeof(addr) && addr.sin_family == OOPS_AF_INET) {
        if (from_port)
            *from_port = oops_ntohs(addr.sin_port);
        if (from_ip && ip_len > 0) {
            if (oops_net_inet_ntop(addr.sin_addr, from_ip, ip_len) != 0)
                from_ip[0] = '\0';
        }
    }
    return rc;
}

int oops_setsockopt(int sock, int level, int optname, const void *optval,
                    size_t optlen) {
    if (sock < 0)
        return -1;
    return p_setsockopt(sock, level, optname, optval, (socklen_t_)optlen);
}

int oops_set_nonblocking(int sock, int nonblocking) {
    /* Non-blocking goes through the socket option, not fcntl: fcntl(F_SETFL,
     * O_NONBLOCK) returns -1 on this console, while SO_NBIO does not. SO_NBIO in
     * libSceNet is 0x1200. */
    int val = nonblocking ? 1 : 0;
    return oops_setsockopt(sock, OOPS_SOL_SOCKET, 0x1200, &val, sizeof(val));
}

void oops_close(int sock) {
    if (sock >= 0) {
        oops_log_debug("NET", "close(sock=%d)", sock);
        p_close(sock);
    }
}

int oops_net_would_block(long rc) {
    /* Two error worlds meet here. The POSIX exports return -1 and set errno
     * (EAGAIN = 35), read through __error(); sceNet, where it is the path,
     * encodes errno into the return as 0x80410100 + errno, so 0x80410123 is its
     * EAGAIN. Report either as would-block. */
    if (rc >= 0)
        return 0;
    if (rc == (long)0x80410123)
        return 1;
    if (rc == -1) {
        int *e = p_errno();
        if (e && *e == OOPS_EAGAIN)
            return 1;
    }
    return 0;
}

#endif /* hosted vs freestanding */
