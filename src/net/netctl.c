#include "oops/netctl.h"
#include "oops/sysmodule.h"

__attribute__((weak)) int sceNetCtlInit(void);
__attribute__((weak)) int sceNetCtlGetInfo(int code, void *info);
__attribute__((weak)) void sceNetCtlTerm(void);

#define SCE_NET_CTL_INFO_DEVICE        1
#define SCE_NET_CTL_INFO_ETHER_ADDR    2
#define SCE_NET_CTL_INFO_LINK          4
#define SCE_NET_CTL_INFO_SSID          6
#define SCE_NET_CTL_INFO_RSSI_PERCENT  9
#define SCE_NET_CTL_INFO_IP_ADDRESS    14
#define SCE_NET_CTL_INFO_NETMASK       15
#define SCE_NET_CTL_INFO_DEFAULT_ROUTE 16
#define SCE_NET_CTL_INFO_PRIMARY_DNS   17
#define SCE_NET_CTL_INFO_SECONDARY_DNS 18

static bool s_netctl_initialized = false;
static bool s_netctl_module_loaded = false;

static void safe_strcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max_len) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

int oops_net_ctl_init(void) {
    if (s_netctl_initialized) {
        return 0;
    }

    if (oops_sysmodule_load(OOPS_SYSMODULE_NET_CTL) == 0) {
        s_netctl_module_loaded = true;
    }

    if (!sceNetCtlInit) {
        return -1;
    }

    int rc = sceNetCtlInit();
    if (rc == 0) {
        s_netctl_initialized = true;
    }
    return rc;
}

int oops_net_ctl_get_info(oops_net_info_t *out_info) {
    if (!out_info) return -1;
    for (size_t i = 0; i < sizeof(*out_info); i++) {
        ((unsigned char *)out_info)[i] = 0;
    }

    if (!s_netctl_initialized) {
        if (oops_net_ctl_init() != 0) {
            return -1;
        }
    }

    if (!sceNetCtlGetInfo) {
        return -1;
    }

    uint8_t buffer[256];

    /* IP Address */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_IP_ADDRESS, buffer) == 0) {
        safe_strcpy(out_info->ip_address, (const char *)buffer, sizeof(out_info->ip_address));
    }

    /* Netmask */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_NETMASK, buffer) == 0) {
        safe_strcpy(out_info->netmask, (const char *)buffer, sizeof(out_info->netmask));
    }

    /* Default Gateway / Route */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_DEFAULT_ROUTE, buffer) == 0) {
        safe_strcpy(out_info->default_gateway, (const char *)buffer, sizeof(out_info->default_gateway));
    }

    /* Primary DNS */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_PRIMARY_DNS, buffer) == 0) {
        safe_strcpy(out_info->primary_dns, (const char *)buffer, sizeof(out_info->primary_dns));
    }

    /* Secondary DNS */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_SECONDARY_DNS, buffer) == 0) {
        safe_strcpy(out_info->secondary_dns, (const char *)buffer, sizeof(out_info->secondary_dns));
    }

    /* Device Type */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_DEVICE, buffer) == 0) {
        out_info->device_type = *(int *)buffer;
    }

    /* Link Status */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_LINK, buffer) == 0) {
        out_info->link_status = *(int *)buffer;
    }

    /* SSID */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_SSID, buffer) == 0) {
        safe_strcpy(out_info->ssid, (const char *)buffer, sizeof(out_info->ssid));
    }

    /* RSSI Percentage */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_RSSI_PERCENT, buffer) == 0) {
        out_info->rssi_percentage = *(int *)buffer;
    }

    /* MAC Address */
    for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_ETHER_ADDR, buffer) == 0) {
        static const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 6; i++) {
            out_info->mac_address[i * 3]     = hex[(buffer[i] >> 4) & 0xF];
            out_info->mac_address[i * 3 + 1] = hex[buffer[i] & 0xF];
            if (i < 5) {
                out_info->mac_address[i * 3 + 2] = ':';
            }
        }
        out_info->mac_address[17] = '\0';
    }

    return 0;
}

void oops_net_ctl_term(void) {
    if (s_netctl_initialized && sceNetCtlTerm) {
        sceNetCtlTerm();
        s_netctl_initialized = false;
    }
    if (s_netctl_module_loaded) {
        (void)oops_sysmodule_unload(OOPS_SYSMODULE_NET_CTL);
        s_netctl_module_loaded = false;
    }
}

