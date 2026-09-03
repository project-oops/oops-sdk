#include "oops/net.h"

__attribute__((weak)) int sceNetInit(void);
__attribute__((weak)) int sceNetTerm(void);
__attribute__((weak)) int sceNetSocket(const char *name, int domain, int type, int protocol);
__attribute__((weak)) int sceNetSocketClose(int s);
__attribute__((weak)) int sceNetBind(int s, const void *addr, unsigned int addrlen);
__attribute__((weak)) int sceNetListen(int s, int backlog);
__attribute__((weak)) int sceNetAccept(int s, void *addr, unsigned int *addrlen);
__attribute__((weak)) int sceNetSend(int s, const void *buf, size_t len, int flags);
__attribute__((weak)) int sceNetRecv(int s, void *buf, size_t len, int flags);

int oops_net_init(void) {
    if (sceNetInit) return sceNetInit();
    return 0;
}

int oops_net_get_ip(char *buf, size_t len) {
    (void)buf; (void)len;
    return -1;
}

int oops_socket_create(int type) {
    if (!sceNetSocket) return -1;
    int proto = (type == OOPS_SOCK_STREAM) ? 6 : 17;
    return sceNetSocket("oops_sock", 2 /* AF_INET */, type, proto);
}

int oops_socket_bind(int sock, uint16_t port) {
    if (!sceNetBind || sock < 0) return -1;
    struct {
        uint8_t  len;
        uint8_t  family;
        uint16_t port;
        uint32_t addr;
        uint16_t vport;
        char     zero[6];
    } sin = { 16, 2, (uint16_t)((port >> 8) | (port << 8)), 0, 0, {0} };
    return sceNetBind(sock, &sin, 16);
}

int oops_socket_listen(int sock, int backlog) {
    if (!sceNetListen || sock < 0) return -1;
    return sceNetListen(sock, backlog);
}

int oops_socket_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port) {
    (void)client_ip; (void)ip_len; (void)client_port;
    if (!sceNetAccept || sock < 0) return -1;
    return sceNetAccept(sock, NULL, NULL);
}

int oops_socket_send(int sock, const void *buf, size_t len) {
    if (!sceNetSend || sock < 0) return -1;
    return sceNetSend(sock, buf, len, 0);
}

int oops_socket_recv(int sock, void *buf, size_t len) {
    if (!sceNetRecv || sock < 0) return -1;
    return sceNetRecv(sock, buf, len, 0);
}

void oops_socket_close(int sock) {
    if (sceNetSocketClose && sock >= 0) {
        (void)sceNetSocketClose(sock);
    }
}

void oops_net_term(void) {
    if (sceNetTerm) sceNetTerm();
}
