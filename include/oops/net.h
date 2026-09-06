#ifndef OOPS_NET_H
#define OOPS_NET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Socket domain / address family */
#define OOPS_AF_INET     2

/* Socket types */
#define OOPS_SOCK_STREAM 1
#define OOPS_SOCK_DGRAM  2

/* IP protocols */
#define OOPS_IPPROTO_IP   0
#define OOPS_IPPROTO_TCP  6
#define OOPS_IPPROTO_UDP  17

/* Socket options */
#define OOPS_SOL_SOCKET    0xFFFF
#define OOPS_SO_REUSEADDR  0x0004
#define OOPS_SO_KEEPALIVE  0x0008
#define OOPS_SO_BROADCAST  0x0020
#define OOPS_SO_RCVTIMEO   0x1006
#define OOPS_SO_SNDTIMEO   0x1005

/* Endian utilities */
static inline uint16_t oops_htons(uint16_t val) {
    return (uint16_t)((val << 8) | (val >> 8));
}
static inline uint16_t oops_ntohs(uint16_t val) {
    return oops_htons(val);
}
static inline uint32_t oops_htonl(uint32_t val) {
    return ((val >> 24) & 0x000000FF) |
           ((val >> 8)  & 0x0000FF00) |
           ((val << 8)  & 0x00FF0000) |
           ((val << 24) & 0xFF000000);
}
static inline uint32_t oops_ntohl(uint32_t val) {
    return oops_htonl(val);
}

/* Network stack lifecycle */
int  oops_net_init(void);
void oops_net_term(void);

/* IP address formatting */
int  oops_net_inet_pton(const char *src, uint32_t *dst);
int  oops_net_inet_ntop(uint32_t src, char *dst, size_t dst_len);

/* Socket API */
int  oops_socket(int domain, int type, int protocol);
int  oops_bind(int sock, const char *ip, uint16_t port);
int  oops_listen(int sock, int backlog);
int  oops_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port);
int  oops_connect(int sock, const char *server_ip, uint16_t port);
long oops_send(int sock, const void *buf, size_t len, int flags);
long oops_recv(int sock, void *buf, size_t len, int flags);
long oops_sendto(int sock, const void *buf, size_t len, int flags, const char *to_ip, uint16_t to_port);
long oops_recvfrom(int sock, void *buf, size_t len, int flags, char *from_ip, size_t ip_len, uint16_t *from_port);
int  oops_setsockopt(int sock, int level, int optname, const void *optval, size_t optlen);
int  oops_set_nonblocking(int sock, int nonblocking);
void oops_close(int sock);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_NET_H */
