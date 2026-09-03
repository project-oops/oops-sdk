#include "oops/system.h"

__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);
__attribute__((weak)) int sceSysUtilSendSystemNotificationWithText(int type, const char *msg);

int32_t oops_user_get_initial_user_id(void) {
    int32_t user = -1;
    if (sceUserServiceGetInitialUser) {
        if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceInitialize) {
            sceUserServiceInitialize(NULL);
            (void)sceUserServiceGetInitialUser(&user);
        }
    }
    return user;
}

int oops_system_get_info(oops_system_info_t *out_info) {
    if (!out_info) return -1;
    for (size_t i = 0; i < sizeof(*out_info); i++) {
        ((unsigned char *)out_info)[i] = 0;
    }
#if defined(OBSCENE_GEN) && (OBSCENE_GEN == 5)
    out_info->generation = 5;
#elif defined(OBSCENE_GEN) && (OBSCENE_GEN == 4)
    out_info->generation = 4;
#else
    out_info->generation = 0;
#endif
    return 0;
}

int oops_system_notify(const char *text) {
    if (!text || !sceSysUtilSendSystemNotificationWithText) return -1;
    return sceSysUtilSendSystemNotificationWithText(0xDEADC0DE, text);
}
