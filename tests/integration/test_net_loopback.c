#include "tests/test_common.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_loopback_socket_communication(void) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_TRUE(server_fd >= 0);

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0; /* Let OS pick port */

  int rc = bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
  ASSERT_EQ(rc, 0);

  socklen_t addr_len = sizeof(addr);
  rc = getsockname(server_fd, (struct sockaddr *)&addr, &addr_len);
  ASSERT_EQ(rc, 0);
  int port = ntohs(addr.sin_port);

  rc = listen(server_fd, 1);
  ASSERT_EQ(rc, 0);

  /* Client socket connect */
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_TRUE(client_fd >= 0);

  struct sockaddr_in client_target;
  memset(&client_target, 0, sizeof(client_target));
  client_target.sin_family = AF_INET;
  client_target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  client_target.sin_port = htons(port);

  rc = connect(client_fd, (struct sockaddr *)&client_target,
               sizeof(client_target));
  ASSERT_EQ(rc, 0);

  /* Accept on server */
  int conn_fd = accept(server_fd, NULL, NULL);
  ASSERT_TRUE(conn_fd >= 0);

  /* Send / Recv */
  const char *msg = "OOPS-SDK Loopback Test Payload 2026";
  size_t msg_len = strlen(msg) + 1;
  ssize_t sent = send(client_fd, msg, msg_len, 0);
  ASSERT_EQ(sent, (ssize_t)msg_len);

  char recv_buf[64];
  memset(recv_buf, 0, sizeof(recv_buf));
  ssize_t recvd = recv(conn_fd, recv_buf, sizeof(recv_buf), 0);
  ASSERT_EQ(recvd, (ssize_t)msg_len);
  ASSERT_STR_EQ(recv_buf, msg);

  close(conn_fd);
  close(client_fd);
  close(server_fd);
}

void run_integration_tests_net_loopback(void) {
  TEST_SUITE_BEGIN("Integration: BSD Sockets Loopback Communication");
  RUN_TEST(test_loopback_socket_communication);
}
