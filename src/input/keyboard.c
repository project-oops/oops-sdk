#include "oops/keyboard.h"
#include <stddef.h>

/*
 * Platform symbols from libSceKeyboard and libSceUserService.
 *
 * These names are a documented expectation, not yet confirmed on hardware (100-input measures
 * only scePad today). obSCEne's keyboard presence census will confirm or correct them; until
 * then the weak binding simply resolves to null where absent, and availability reports honestly.
 */
__attribute__((weak)) int sceKeyboardInit(void);
__attribute__((weak)) int sceKeyboardOpen(int userId, int type, int index, const void *param);
__attribute__((weak)) int sceKeyboardClose(int handle);
__attribute__((weak)) int sceKeyboardReadState(int handle, void *state);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

/*
 * The platform key record. Documented at 96 bytes, unconfirmed here, so 0 keeps the read
 * refusing with OOPS_KEYBOARD_ELAYOUT. When the obSCEne capture lands: set the size, define
 * the record beside it, and translate it in oops_keyboard_read - the only place that reads it.
 */
#define OOPS_KEY_RECORD_BYTES 0

static int s_kbd_handle = -1;
static int s_kbd_inited = 0;
static int s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;  /* what the first init reported */

int oops_keyboard_available(void) {
    /* Real once the names are confirmed: the capability exists iff the open/read entry points
     * resolve. */
    return (sceKeyboardOpen && sceKeyboardReadState) ? 1 : 0;
}

int oops_keyboard_init(void) {
    if (s_kbd_inited) {
        return s_kbd_init_rc;
    }
    s_kbd_inited = 1;

    if (!oops_keyboard_available()) {
        s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
        return s_kbd_init_rc;
    }
    if (sceKeyboardInit) {
        sceKeyboardInit();
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
        s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
        return s_kbd_init_rc;
    }

    int rc = sceKeyboardOpen(user, 0, 0, NULL);
    if (rc < 0) {
        s_kbd_init_rc = rc;   /* the platform's own code, passed through */
        return rc;
    }
    s_kbd_handle = rc;
    s_kbd_init_rc = OOPS_KEYBOARD_OK;
    return OOPS_KEYBOARD_OK;
}

int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events) {
    if (!out_events || max_events == 0) {
        return OOPS_KEYBOARD_EPARAM;
    }
    /* Capture-gated: a distinct code rather than zero events, so a caller can tell "no keys"
     * from "no reader". Checked before the handle because it is true on every firmware. */
    if (OOPS_KEY_RECORD_BYTES == 0) {
        return OOPS_KEYBOARD_ELAYOUT;
    }
    if (s_kbd_handle < 0 || !sceKeyboardReadState) {
        return OOPS_KEYBOARD_EUNAVAIL;
    }
    /* The translation from the platform record lands here with the capture. */
    return 0;
}

void oops_keyboard_close(void) {
    if (s_kbd_handle >= 0 && sceKeyboardClose) {
        sceKeyboardClose(s_kbd_handle);
    }
    s_kbd_handle = -1;
    s_kbd_inited = 0;
    s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
}
