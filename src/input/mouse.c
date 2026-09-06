#include "oops/mouse.h"
#include "oops/sysmodule.h"
#include <stddef.h>

/*
 * Platform symbols from libSceMouse and libSceUserService.
 *
 * Names are a documented expectation pending an obSCEne presence census (100-input measures
 * only scePad today). Weak binding resolves to null where absent; availability reports honestly.
 */
__attribute__((weak)) int sceMouseInit(void);
__attribute__((weak)) int sceMouseOpen(int userId, int type, int index, const void *param);
__attribute__((weak)) int sceMouseClose(int handle);
__attribute__((weak)) int sceMouseRead(int handle, void *data, int num);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);

static int s_mouse_handle = -1;
static int s_mouse_inited = 0;

int oops_mouse_available(void) {
    (void)oops_sysmodule_load(OOPS_SYSMODULE_MOUSE);
    return (sceMouseOpen && sceMouseRead) ? 1 : 0;
}

int oops_mouse_init(void) {
    if (s_mouse_inited) {
        return (s_mouse_handle >= 0) ? 0 : -1;
    }
    s_mouse_inited = 1;

    (void)oops_sysmodule_load(OOPS_SYSMODULE_MOUSE);
    if (sceMouseInit) {
        sceMouseInit();
    }

    int32_t user = -1;
    if (sceUserServiceGetInitialUser) {
        (void)sceUserServiceGetInitialUser(&user);
    }
    if (user >= 0 && sceMouseOpen) {
        s_mouse_handle = sceMouseOpen(user, 0, 0, NULL);
    }
    return (s_mouse_handle >= 0) ? 0 : -1;
}

int oops_mouse_read(oops_mouse_state_t *out_samples, unsigned int max_samples) {
    if (!out_samples || max_samples == 0) {
        return -1;
    }
    if (s_mouse_handle < 0 || !sceMouseRead) {
        return -1;
    }
    /* Capture-gated: the platform mouse-record layout (documented at 40 bytes) is unconfirmed.
     * Zero records is the honest answer until the obSCEne capture confirms it. */
    return 0;
}

void oops_mouse_close(void) {
    if (s_mouse_handle >= 0 && sceMouseClose) {
        sceMouseClose(s_mouse_handle);
        s_mouse_handle = -1;
    }
    s_mouse_inited = 0;
}
