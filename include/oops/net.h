#ifndef OOPS_NET_H
#define OOPS_NET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OOPS_SOCK_STREAM 1
#define OOPS_SOCK_DGRAM  2

int oops_net_init(void);
int oops_net_get_ip(char *buf, size_t len);
int oops_socket_create(int type);
int oops_socket_bind(int sock, uint16_t port);
int oops_socket_listen(int sock, int backlog);
int oops_socket_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port);
int oops_socket_send(int sock, const void *buf, size_t len);
int oops_socket_recv(int sock, void *buf, size_t len);
void oops_socket_close(int sock);
void oops_net_term(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_NET_H */
