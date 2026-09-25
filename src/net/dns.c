/*
 * Freestanding RFC 1035 DNS client and hostname resolver.
 * Zero libc dependencies on target.
 */

#include "oops/net.h"
#include "oops/netctl.h"
#include "oops/freestd.h"
#include "oops/time.h"
#include "oops/system.h"

#if defined(OOPS_HOST_BUILD)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

int oops_dns_build_query(const char *hostname, uint16_t tx_id, uint8_t *out_buf,
                         size_t max_len) {
  if (!hostname || !out_buf || max_len < 18) {
    return -1;
  }
  size_t hlen = obs_strlen(hostname);
  if (hlen == 0 || hlen > 253) {
    return -1;
  }

  /* DNS Header (12 bytes) */
  out_buf[0] = (uint8_t)(tx_id >> 8);
  out_buf[1] = (uint8_t)(tx_id & 0xFF);
  out_buf[2] = 0x01; /* QR=0, Opcode=0, RD=1 */
  out_buf[3] = 0x00;
  out_buf[4] = 0x00; /* QDCOUNT = 1 */
  out_buf[5] = 0x01;
  out_buf[6] = 0x00; /* ANCOUNT = 0 */
  out_buf[7] = 0x00;
  out_buf[8] = 0x00; /* NSCOUNT = 0 */
  out_buf[9] = 0x00;
  out_buf[10] = 0x00; /* ARCOUNT = 0 */
  out_buf[11] = 0x00;

  size_t pos = 12;
  const char *p = hostname;
  while (*p != '\0') {
    const char *dot = p;
    while (*dot != '\0' && *dot != '.') {
      dot++;
    }
    size_t label_len = (size_t)(dot - p);
    if (label_len == 0 || label_len > 63) {
      return -1;
    }
    if (pos + 1 + label_len + 1 + 4 > max_len) {
      return -1;
    }

    out_buf[pos++] = (uint8_t)label_len;
    for (size_t i = 0; i < label_len; i++) {
      out_buf[pos++] = (uint8_t)p[i];
    }
    if (*dot == '.') {
      p = dot + 1;
    } else {
      p = dot;
    }
  }
  out_buf[pos++] = 0; /* Terminating zero length root label */

  /* QTYPE = 0x0001 (A - IPv4) */
  out_buf[pos++] = 0x00;
  out_buf[pos++] = 0x01;

  /* QCLASS = 0x0001 (IN - Internet) */
  out_buf[pos++] = 0x00;
  out_buf[pos++] = 0x01;

  return (int)pos;
}

int oops_dns_parse_response(const uint8_t *resp, size_t resp_len,
                            uint16_t expected_tx_id, char *out_ip,
                            size_t out_len) {
  if (!resp || resp_len < 12 || !out_ip || out_len < 16) {
    return -1;
  }

  uint16_t id = (uint16_t)((resp[0] << 8) | resp[1]);
  if (expected_tx_id != 0 && id != expected_tx_id) {
    return -1;
  }

  uint16_t flags = (uint16_t)((resp[2] << 8) | resp[3]);
  int qr = (flags >> 15) & 1;
  int rcode = flags & 0x0F;
  if (qr != 1 || rcode != 0) {
    return -1;
  }

  uint16_t qdcount = (uint16_t)((resp[4] << 8) | resp[5]);
  uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
  if (ancount == 0) {
    return -1;
  }

  size_t cursor = 12;

  /* Skip Question section */
  for (uint16_t q = 0; q < qdcount; q++) {
    while (cursor < resp_len) {
      uint8_t len = resp[cursor++];
      if (len == 0) break;
      if ((len & 0xC0) == 0xC0) {
        cursor++; /* 2-byte pointer */
        break;
      }
      cursor += len;
    }
    cursor += 4; /* QTYPE + QCLASS */
    if (cursor > resp_len) return -1;
  }

  /* Scan Answer records for an A (Type 1, Class 1) record */
  for (uint16_t a = 0; a < ancount; a++) {
    if (cursor >= resp_len) break;

    /* Skip NAME field (pointer or sequence of labels) */
    if ((resp[cursor] & 0xC0) == 0xC0) {
      cursor += 2;
    } else {
      while (cursor < resp_len) {
        uint8_t len = resp[cursor++];
        if (len == 0) break;
        if ((len & 0xC0) == 0xC0) {
          cursor++;
          break;
        }
        cursor += len;
      }
    }
    if (cursor + 10 > resp_len) break;

    uint16_t type = (uint16_t)((resp[cursor] << 8) | resp[cursor + 1]);
    uint16_t class_ = (uint16_t)((resp[cursor + 2] << 8) | resp[cursor + 3]);
    uint16_t rdlen = (uint16_t)((resp[cursor + 8] << 8) | resp[cursor + 9]);
    cursor += 10;

    if (cursor + rdlen > resp_len) break;

    if (type == 1 && class_ == 1 && rdlen == 4) {
      /* IPv4 Address in network byte order */
      uint32_t ip;
      memcpy(&ip, &resp[cursor], 4);
      return oops_net_inet_ntop(ip, out_ip, out_len);
    }
    cursor += rdlen;
  }

  return -1;
}

int oops_net_resolve(const char *hostname, char *out_ip, size_t out_len) {
  if (!hostname || !out_ip || out_len < 16) {
    return -1;
  }

  oops_log_debug("DNS", "resolving hostname '%s'", hostname);

  /* 1. Direct dotted-quad IPv4 passthrough */
  uint32_t ip_test;
  if (oops_net_inet_pton(hostname, &ip_test) == 0) {
    size_t len = obs_strlen(hostname);
    if (len >= out_len) return -1;
    obs_strncpy(out_ip, hostname, out_len);
    oops_log_trace("DNS", "'%s' is dotted-quad IPv4 passthrough", hostname);
    return 0;
  }

  /* 2. Standard localhost alias */
  if (obs_strcmp(hostname, "localhost") == 0) {
    if (out_len < 10) return -1;
    obs_strncpy(out_ip, "127.0.0.1", out_len);
    oops_log_trace("DNS", "resolved localhost -> 127.0.0.1");
    return 0;
  }

#if defined(OOPS_HOST_BUILD)
  /* Host environment resolver */
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(hostname, NULL, &hints, &res) == 0 && res) {
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    const char *ptr = inet_ntop(AF_INET, &(ipv4->sin_addr), out_ip, (socklen_t)out_len);
    freeaddrinfo(res);
    if (ptr) {
      oops_log_info("DNS", "resolved '%s' -> %s", hostname, out_ip);
      return 0;
    }
  }
  oops_log_warn("DNS", "failed to resolve host '%s'", hostname);
  return -1;
#else
  /* Target freestanding RFC 1035 UDP DNS resolution */
  uint8_t qpkt[512];
  uint16_t tx_id = 0x5053; /* 'PS' */
  int qlen = oops_dns_build_query(hostname, tx_id, qpkt, sizeof(qpkt));
  if (qlen <= 0) return -1;

  const char *servers[6];
  int num_servers = 0;

  oops_net_info_t net_info;
  if (oops_net_ctl_get_info(&net_info) == 0) {
    if (net_info.primary_dns[0] != '\0') servers[num_servers++] = net_info.primary_dns;
    if (net_info.secondary_dns[0] != '\0') servers[num_servers++] = net_info.secondary_dns;
  }
  servers[num_servers++] = "1.1.1.1";
  servers[num_servers++] = "8.8.8.8";
  servers[num_servers++] = "1.0.0.1";
  servers[num_servers++] = "8.8.4.4";

  for (int s = 0; s < num_servers; s++) {
    int sock = oops_socket(OOPS_AF_INET, OOPS_SOCK_DGRAM, OOPS_IPPROTO_UDP);
    if (sock < 0) continue;
    oops_set_nonblocking(sock, 1);

    long sent = oops_sendto(sock, qpkt, (size_t)qlen, 0, servers[s], 53);
    if (sent != qlen) {
      oops_close(sock);
      continue;
    }

    uint8_t resp[512];
    uint64_t start_ms = oops_time_get_ms();
    int success = 0;

    while (oops_time_get_ms() - start_ms < 1500) {
      long r = oops_recv(sock, resp, sizeof(resp), 0);
      if (r > 0) {
        if (oops_dns_parse_response(resp, (size_t)r, tx_id, out_ip, out_len) == 0) {
          success = 1;
        }
        break;
      }
      oops_time_sleep_ms(10);
    }

    oops_close(sock);
    if (success) {
      oops_log_info("DNS", "resolved '%s' -> %s (via %s)", hostname, out_ip, servers[s]);
      return 0;
    }
  }

  oops_log_warn("DNS", "failed to resolve host '%s' across %d servers", hostname, num_servers);
  return -1;
#endif
}
