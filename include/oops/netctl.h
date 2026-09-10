#ifndef OOPS_NETCTL_H
#define OOPS_NETCTL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_net_info {
  char ip_address[16];
  char netmask[16];
  char default_gateway[16];
  char primary_dns[16];
  char secondary_dns[16];
  char mac_address[18];
  int device_type; /* 1 = Wired, 2 = Wireless, 0 = Unknown */
  int link_status; /* 1 = Connected, 0 = Disconnected */
  char ssid[33];
  int rssi_percentage;
} oops_net_info_t;

/**
 * Initialize network control service (libSceNetCtl).
 * Returns: 0 on success, -1 on failure.
 */
int oops_net_ctl_init(void);

/**
 * Query active network interface information. A field the platform does not
 * answer is left empty (strings) or zero (integers). Returns: 0 if at least one
 * field was answered, -1 if the service is absent or nothing was.
 */
int oops_net_ctl_get_info(oops_net_info_t *out_info);

/**
 * Terminate network control service.
 */
void oops_net_ctl_term(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_NETCTL_H */
