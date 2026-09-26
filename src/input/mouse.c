/*
 * USB mouse input through libSceMouse. Open and availability are real; the read
 * refuses with OOPS_MOUSE_ELAYOUT because the platform record is unmeasured.
 */
#include "oops/mouse.h"
#include "oops/sysmodule.h"
#include "oops/system.h"
#include <stddef.h>

/*
 * Platform symbols from libSceMouse and libSceUserService. On 12.40 (obSCEne probe
 * 101-input-ext) all four mouse entry points resolve in the app context and none in
 * the eboot or payload contexts, which the weak binding reports as unavailable.
 */
__attribute__((weak)) int sceMouseInit(void);
__attribute__((weak)) int sceMouseOpen(int userId, int type, int index,
                                       const void *param);
__attribute__((weak)) int sceMouseClose(int handle);
__attribute__((weak)) int sceMouseRead(int handle, void *data, int num);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

/*
 * The platform mouse record, documented at 40 bytes and unconfirmed: with no
 * mouse attached sceMouseRead writes nothing, so no extent is measured. 0 keeps
 * the read refusing with OOPS_MOUSE_ELAYOUT. With a capture, the size is set here,
 * the record defined beside it, and translated in oops_mouse_read. The batched
 * read strides by this size, so it must be exact, not oversized.
 */
#define OOPS_MOUSE_RECORD_BYTES 0

static int s_mouse_handle = -1;
static int s_mouse_inited = 0;
static int s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL; /* what the first init reported */

int oops_mouse_available(void) {
    (void)oops_sysmodule_load(OOPS_SYSMODULE_MOUSE);
    int avail = (sceMouseOpen && sceMouseRead) ? 1 : 0;
    oops_log_trace("MOUSE", "oops_mouse_available -> %d", avail);
    return avail;
}

int oops_mouse_init(void) {
    if (s_mouse_inited) {
        return s_mouse_init_rc;
    }
    s_mouse_inited = 1;
    oops_log_debug("MOUSE", "initializing mouse subsystem");

    if (!oops_mouse_available()) {
        oops_log_warn("MOUSE", "mouse entry points unavailable");
        s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL;
        return s_mouse_init_rc;
    }
    if (sceMouseInit) {
        sceMouseInit();
    }

    /* The same user resolution the pad uses, including bringing the user service
     * up. */
    int32_t user = -1;
    if (sceUserServiceGetInitialUser) {
        if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceInitialize) {
            sceUserServiceInitialize(NULL);
            (void)sceUserServiceGetInitialUser(&user);
        }
    }
    if (user < 0) {
        oops_log_warn("MOUSE", "failed to resolve user for mouse");
        s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL;
        return s_mouse_init_rc;
    }

    int rc = sceMouseOpen(user, 0, 0, NULL);
    if (rc < 0) {
        oops_log_warn("MOUSE", "sceMouseOpen(user=0x%x) failed: %d", (unsigned int)user,
                      rc);
        s_mouse_init_rc = rc; /* the platform's own code, passed through */
        return rc;
    }
    s_mouse_handle = rc;
    s_mouse_init_rc = OOPS_MOUSE_OK;
    oops_log_info("MOUSE", "mouse opened successfully (user=0x%x, handle=%d)",
                  (unsigned int)user, rc);
    return OOPS_MOUSE_OK;
}

int oops_mouse_read(oops_mouse_state_t *out_samples, unsigned int max_samples) {
    if (!out_samples || max_samples == 0) {
        return OOPS_MOUSE_EPARAM;
    }
    /* A distinct code rather than zero samples, so a caller can tell "no motion"
     * from "no reader". Checked before the handle because it holds on every
     * firmware. */
    if (OOPS_MOUSE_RECORD_BYTES == 0) {
        return OOPS_MOUSE_ELAYOUT;
    }
    if (s_mouse_handle < 0 || !sceMouseRead) {
        return OOPS_MOUSE_EUNAVAIL;
    }
    oops_log_trace("MOUSE", "mouse read: max_samples=%u", max_samples);
    /* The translation from the platform record belongs here. */
    return 0;
}

void oops_mouse_close(void) {
    oops_log_debug("MOUSE", "closing mouse (handle=%d)", s_mouse_handle);
    if (s_mouse_handle >= 0 && sceMouseClose) {
        sceMouseClose(s_mouse_handle);
    }
    s_mouse_handle = -1;
    s_mouse_inited = 0;
    s_mouse_init_rc = OOPS_MOUSE_EUNAVAIL;
}
