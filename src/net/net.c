#include "oops/net.h"

/* Platform symbols from libSceNet */
__attribute__((weak)) int sceNetInit(void);
__attribute__((weak)) int sceNetTerm(void);
__attribute__((weak)) int sceNetSocket(const char *name, int domain, int type, int protocol);
__attribute__((weak)) int sceNetSocketClose(int s);
__attribute__((weak)) int sceNetBind(int s, const void *addr, unsigned int addrlen);
__attribute__((weak)) int sceNetListen(int s, int backlog);
__attribute__((weak)) int sceNetAccept(int s, void *addr, unsigned int *addrlen);
__attribute__((weak)) int sceNetConnect(int s, const void *addr, unsigned int addrlen);
__attribute__((weak)) int sceNetSend(int s, const void *buf, size_t len, int flags);
__attribute__((weak)) int sceNetRecv(int s, void *buf, size_t len, int flags);
__attribute__((weak)) int sceNetSendto(int s, const void *buf, size_t len, int flags, const void *to, unsigned int tolen);
__attribute__((weak)) int sceNetRecvfrom(int s, void *buf, size_t len, int flags, void *from, unsigned int *fromlen);
__attribute__((weak)) int sceNetSetsockopt(int s, int level, int optname, const void *optval, unsigned int optlen);

struct sce_net_sockaddr_in {
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint16_t sin_vport;
    char     sin_zero[6];
};

static int s_net_initialized = 0;

int oops_net_init(void) {
    if (!s_net_initialized) {
        if (sceNetInit) {
            int rc = sceNetInit();
            if (rc < 0 && rc != (int)0x80410101) { /* SCE_NET_ERROR_EALREADY */
                return rc;
            }
        }
        s_net_initialized = 1;
    }
    return 0;
}

void oops_net_term(void) {
    if (s_net_initialized && sceNetTerm) {
        sceNetTerm();
        s_net_initialized = 0;
    }
}

int oops_net_inet_pton(const char *src, uint32_t *dst) {
    if (!src || !dst) return -1;

    uint32_t octets[4] = {0, 0, 0, 0};
    int cur = 0;
    const char *p = src;

    while (*p != '\0') {
        if (*p >= '0' && *p <= '9') {
            octets[cur] = octets[cur] * 10 + (uint32_t)(*p - '0');
            if (octets[cur] > 255) return -1;
        } else if (*p == '.') {
            cur++;
            if (cur > 3) return -1;
        } else {
            return -1;
        }
        p++;
    }

    if (cur != 3) return -1;

    /* Network byte order (big-endian wire format) */
    *dst = (octets[0]) | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return 0;
}

int oops_net_inet_ntop(uint32_t src, char *dst, size_t dst_len) {
    if (!dst || dst_len < 16) return -1;

    uint8_t b0 = (uint8_t)(src & 0xFF);
    uint8_t b1 = (uint8_t)((src >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)((src >> 16) & 0xFF);
    uint8_t b3 = (uint8_t)((src >> 24) & 0xFF);

    uint8_t bytes[4] = { b0, b1, b2, b3 };
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

int oops_socket(int domain, int type, int protocol) {
    (void)oops_net_init();
    if (!sceNetSocket) return -1;

    int proto = protocol;
    if (proto == 0) {
        proto = (type == OOPS_SOCK_STREAM) ? OOPS_IPPROTO_TCP : OOPS_IPPROTO_UDP;
    }
    return sceNetSocket("oops_sock", domain, type, proto);
}

int oops_bind(int sock, const char *ip, uint16_t port) {
    if (!sceNetBind || sock < 0) return -1;

    struct sce_net_sockaddr_in addr;
    for (size_t i = 0; i < sizeof(addr); i++) ((uint8_t *)&addr)[i] = 0;

    addr.sin_len = (uint8_t)sizeof(addr);
    addr.sin_family = OOPS_AF_INET;
    addr.sin_port = oops_htons(port);

    if (ip && ip[0] != '\0') {
        if (oops_net_inet_pton(ip, &addr.sin_addr) != 0) return -1;
    } else {
        addr.sin_addr = 0; /* INADDR_ANY */
    }

    return sceNetBind(sock, &addr, (unsigned int)sizeof(addr));
}

int oops_listen(int sock, int backlog) {
    if (!sceNetListen || sock < 0) return -1;
    return sceNetListen(sock, (backlog <= 0) ? 5 : backlog);
}

int oops_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port) {
    if (!sceNetAccept || sock < 0) return -1;

    struct sce_net_sockaddr_in addr;
    unsigned int addrlen = (unsigned int)sizeof(addr);
    for (size_t i = 0; i < sizeof(addr); i++) ((uint8_t *)&addr)[i] = 0;

    int client_sock = sceNetAccept(sock, &addr, &addrlen);
    if (client_sock >= 0) {
        if (client_ip && ip_len > 0) {
            oops_net_inet_ntop(addr.sin_addr, client_ip, ip_len);
        }
        if (client_port) {
            *client_port = oops_ntohs(addr.sin_port);
        }
    }
    return client_sock;
}

int oops_connect(int sock, const char *server_ip, uint16_t port) {
    if (!sceNetConnect || sock < 0 || !server_ip) return -1;

    struct sce_net_sockaddr_in addr;
    for (size_t i = 0; i < sizeof(addr); i++) ((uint8_t *)&addr)[i] = 0;

    addr.sin_len = (uint8_t)sizeof(addr);
    addr.sin_family = OOPS_AF_INET;
    addr.sin_port = oops_htons(port);

    if (oops_net_inet_pton(server_ip, &addr.sin_addr) != 0) return -1;

    return sceNetConnect(sock, &addr, (unsigned int)sizeof(addr));
}

long oops_send(int sock, const void *buf, size_t len, int flags) {
    if (!sceNetSend || sock < 0 || !buf) return -1;
    return (long)sceNetSend(sock, buf, len, flags);
}

long oops_recv(int sock, void *buf, size_t len, int flags) {
    if (!sceNetRecv || sock < 0 || !buf) return -1;
    return (long)sceNetRecv(sock, buf, len, flags);
}

long oops_sendto(int sock, const void *buf, size_t len, int flags, const char *to_ip, uint16_t to_port) {
    if (!sceNetSendto || sock < 0 || !buf || !to_ip) return -1;

    struct sce_net_sockaddr_in addr;
    for (size_t i = 0; i < sizeof(addr); i++) ((uint8_t *)&addr)[i] = 0;

    addr.sin_len = (uint8_t)sizeof(addr);
    addr.sin_family = OOPS_AF_INET;
    addr.sin_port = oops_htons(to_port);
    if (oops_net_inet_pton(to_ip, &addr.sin_addr) != 0) return -1;

    return (long)sceNetSendto(sock, buf, len, flags, &addr, (unsigned int)sizeof(addr));
}

long oops_recvfrom(int sock, void *buf, size_t len, int flags, char *from_ip, size_t ip_len, uint16_t *from_port) {
    if (!sceNetRecvfrom || sock < 0 || !buf) return -1;

    struct sce_net_sockaddr_in addr;
    unsigned int addrlen = (unsigned int)sizeof(addr);
    for (size_t i = 0; i < sizeof(addr); i++) ((uint8_t *)&addr)[i] = 0;

    long rc = (long)sceNetRecvfrom(sock, buf, len, flags, &addr, &addrlen);
    if (rc >= 0) {
        if (from_ip && ip_len > 0) {
            oops_net_inet_ntop(addr.sin_addr, from_ip, ip_len);
        }
        if (from_port) {
            *from_port = oops_ntohs(addr.sin_port);
        }
    }
    return rc;
}

int oops_setsockopt(int sock, int level, int optname, const void *optval, size_t optlen) {
    if (!sceNetSetsockopt || sock < 0) return -1;
    return sceNetSetsockopt(sock, level, optname, optval, (unsigned int)optlen);
}

int oops_set_nonblocking(int sock, int nonblocking) {
    int val = nonblocking ? 1 : 0;
    /* SO_NBIO option in libSceNet is 0x1200 */
    return oops_setsockopt(sock, OOPS_SOL_SOCKET, 0x1200, &val, sizeof(val));
}

void oops_close(int sock) {
    if (sock >= 0 && sceNetSocketClose) {
        sceNetSocketClose(sock);
    }
}
