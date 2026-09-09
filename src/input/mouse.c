#include "oops/mouse.h"
#include "oops/sysmodule.h"
#include <stddef.h>

/*
 * Platform symbols from libSceMouse and libSceUserService.
 *
 * Confirmed on 12.40 by obSCEne 101-input-ext: libSceMouse loads and all four entry points
 * resolve in the app context, none in the eboot or payload contexts, which the weak binding
 * reports as unavailable rather than guessing.
 */
__attribute__((weak)) int sceMouseInit(void);
__attribute__((weak)) int sceMouseOpen(int userId, int type, int index, const void *param);
__attribute__((weak)) int sceMouseClose(int handle);
__attribute__((weak)) int sceMouseRead(int handle, void *data, int num);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

/*
 * The platform mouse record. Documented at 40 bytes and still unconfirmed: the 12.40 capture
 * called sceMouseRead with no mouse attached and it wrote nothing, so no extent was measured.
 * A run with a mouse plugged in is the capture that lands this; 0 keeps the read refusing
 * with OOPS_MOUSE_ELAYOUT. When the obSCEne capture lands: set the size, define the
 * record beside it, and translate it in oops_mouse_read - the only place that reads it. The
 * batched read strides by this size, so it must be exact, not oversized.
 */
#define OOPS_MOUSE_RECORD_BYTES 0

static int s_mouse_handle = -1;
static int s_mouse_inited = 0;
static int s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL;  /* what the first init reported */

int oops_mouse_available(void) {
    (void)oops_sysmodule_load(OOPS_SYSMODULE_MOUSE);
    return (sceMouseOpen && sceMouseRead) ? 1 : 0;
}

int oops_mouse_init(void) {
    if (s_mouse_inited) {
        return s_mouse_init_rc;
    }
    s_mouse_inited = 1;

    if (!oops_mouse_available()) {
        s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL;
        return s_mouse_init_rc;
    }
    if (sceMouseInit) {
        sceMouseInit();
    }

    /* The same user resolution the pad uses, including bringing the user service up. */
    int32_t user = -1;
    if (sceUserServiceGetInitialUser) {
        if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceInitialize) {
            sceUserServiceInitialize(NULL);
            (void)sceUserServiceGetInitialUser(&user);
        }
    }
    if (user < 0) {
        s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL;
        return s_mouse_init_rc;
    }

    int rc = sceMouseOpen(user, 0, 0, NULL);
    if (rc < 0) {
        s_mouse_init_rc = rc;   /* the platform's own code, passed through */
        return rc;
    }
    s_mouse_handle = rc;
    s_mouse_init_rc = OOPS_MOUSE_OK;
    return OOPS_MOUSE_OK;
}

int oops_mouse_read(oops_mouse_state_t *out_samples, unsigned int max_samples) {
    if (!out_samples || max_samples == 0) {
        return OOPS_MOUSE_EPARAM;
    }
    /* Capture-gated: a distinct code rather than zero samples, so a caller can tell "no
     * motion" from "no reader". Checked before the handle because it is true on every firmware. */
    if (OOPS_MOUSE_RECORD_BYTES == 0) {
        return OOPS_MOUSE_ELAYOUT;
    }
    if (s_mouse_handle < 0 || !sceMouseRead) {
        return OOPS_MOUSE_EUNAVAIL;
    }
    /* The translation from the platform record lands here with the capture. */
    return 0;
}

void oops_mouse_close(void) {
    if (s_mouse_handle >= 0 && sceMouseClose) {
        sceMouseClose(s_mouse_handle);
    }
    s_mouse_handle = -1;
    s_mouse_inited = 0;
    s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL;
}
