#include "oops/netctl.h"
#include "oops/sysmodule.h"
#include "oops/system.h"
#include <stdbool.h>

__attribute__((weak)) int sceNetCtlInit(void);
__attribute__((weak)) int sceNetCtlGetInfo(int code, void *info);
__attribute__((weak)) void sceNetCtlTerm(void);

#define SCE_NET_CTL_INFO_DEVICE 1
#define SCE_NET_CTL_INFO_ETHER_ADDR 2
#define SCE_NET_CTL_INFO_LINK 4
#define SCE_NET_CTL_INFO_SSID 6
#define SCE_NET_CTL_INFO_RSSI_PERCENT 9
#define SCE_NET_CTL_INFO_IP_ADDRESS 14
#define SCE_NET_CTL_INFO_NETMASK 15
#define SCE_NET_CTL_INFO_DEFAULT_ROUTE 16
#define SCE_NET_CTL_INFO_PRIMARY_DNS 17
#define SCE_NET_CTL_INFO_SECONDARY_DNS 18

#define NETCTL_QUERY_BYTES 256

static bool s_netctl_initialized = false;
static bool s_netctl_module_loaded = false;

static void safe_strcpy(char *dst, const char *src, size_t max_len) {
  if (!dst || max_len == 0)
    return;
  size_t i = 0;
  if (src) {
    while (src[i] && i + 1 < max_len) {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = '\0';
}

/* The integer answers arrive as four little-endian bytes at the start of the
 * query buffer. Assembled by hand rather than cast: a byte buffer carries no
 * int alignment. */
static int read_int_le(const uint8_t *b) {
  return (int)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
               ((uint32_t)b[3] << 24));
}

/* One query into a zeroed buffer. 1 if the platform answered, 0 if not. */
static int query(int code, uint8_t *buffer) {
  for (size_t i = 0; i < NETCTL_QUERY_BYTES; i++)
    buffer[i] = 0;
  return (sceNetCtlGetInfo(code, buffer) == 0) ? 1 : 0;
}

int oops_net_ctl_init(void) {
  if (s_netctl_initialized) {
    return 0;
  }

  oops_log_debug("NETCTL", "initializing netctl subsystem");

  if (oops_sysmodule_load(OOPS_SYSMODULE_NET_CTL) == 0) {
    s_netctl_module_loaded = true;
  }

  if (!sceNetCtlInit) {
    oops_log_warn("NETCTL", "sceNetCtlInit symbol not found");
    return -1;
  }

  int rc = sceNetCtlInit();
  if (rc == 0) {
    s_netctl_initialized = true;
    oops_log_info("NETCTL", "netctl initialized successfully");
  } else {
    oops_log_warn("NETCTL", "sceNetCtlInit failed: %d", rc);
  }
  return rc;
}

int oops_net_ctl_get_info(oops_net_info_t *out_info) {
  if (!out_info)
    return -1;
  for (size_t i = 0; i < sizeof(*out_info); i++) {
    ((unsigned char *)out_info)[i] = 0;
  }

  if (!s_netctl_initialized) {
    if (oops_net_ctl_init() != 0) {
      return -1;
    }
  }

  if (!sceNetCtlGetInfo) {
    oops_log_warn("NETCTL", "sceNetCtlGetInfo symbol not found");
    return -1;
  }

  oops_log_trace("NETCTL", "querying netctl info parameters");

  uint8_t buffer[NETCTL_QUERY_BYTES];
  int answered =
      0; /* a field the platform did not answer stays empty or zero */

  if (query(SCE_NET_CTL_INFO_IP_ADDRESS, buffer)) {
    safe_strcpy(out_info->ip_address, (const char *)buffer,
                sizeof(out_info->ip_address));
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_NETMASK, buffer)) {
    safe_strcpy(out_info->netmask, (const char *)buffer,
                sizeof(out_info->netmask));
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_DEFAULT_ROUTE, buffer)) {
    safe_strcpy(out_info->default_gateway, (const char *)buffer,
                sizeof(out_info->default_gateway));
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_PRIMARY_DNS, buffer)) {
    safe_strcpy(out_info->primary_dns, (const char *)buffer,
                sizeof(out_info->primary_dns));
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_SECONDARY_DNS, buffer)) {
    safe_strcpy(out_info->secondary_dns, (const char *)buffer,
                sizeof(out_info->secondary_dns));
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_DEVICE, buffer)) {
    out_info->device_type = read_int_le(buffer);
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_LINK, buffer)) {
    out_info->link_status = read_int_le(buffer);
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_SSID, buffer)) {
    safe_strcpy(out_info->ssid, (const char *)buffer, sizeof(out_info->ssid));
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_RSSI_PERCENT, buffer)) {
    out_info->rssi_percentage = read_int_le(buffer);
    answered++;
  }
  if (query(SCE_NET_CTL_INFO_ETHER_ADDR, buffer)) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
      out_info->mac_address[i * 3] = hex[(buffer[i] >> 4) & 0xF];
      out_info->mac_address[i * 3 + 1] = hex[buffer[i] & 0xF];
      if (i < 5) {
        out_info->mac_address[i * 3 + 2] = ':';
      }
    }
    out_info->mac_address[17] = '\0';
    answered++;
  }

  /* Nothing answered is a failure, not an interface with no address. */
  if (answered == 0) {
    oops_log_warn("NETCTL", "sceNetCtlGetInfo queries yielded 0 answers");
    return -1;
  }

  oops_log_info("NETCTL", "net info: IP=%s netmask=%s gw=%s DNS1=%s link=%d type=%d",
                out_info->ip_address, out_info->netmask, out_info->default_gateway,
                out_info->primary_dns, out_info->link_status, out_info->device_type);
  return 0;
}

void oops_net_ctl_term(void) {
  oops_log_debug("NETCTL", "terminating netctl subsystem");
  if (s_netctl_initialized && sceNetCtlTerm) {
    sceNetCtlTerm();
    s_netctl_initialized = false;
  }
  if (s_netctl_module_loaded) {
    (void)oops_sysmodule_unload(OOPS_SYSMODULE_NET_CTL);
    s_netctl_module_loaded = false;
  }
}
