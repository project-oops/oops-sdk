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

static int s_kbd_handle = -1;
static int s_kbd_inited = 0;

int oops_keyboard_available(void) {
    /* Real once the names are confirmed: the capability exists iff the open/read entry points
     * resolve. */
    return (sceKeyboardOpen && sceKeyboardReadState) ? 1 : 0;
}

int oops_keyboard_init(void) {
    if (s_kbd_inited) {
        return (s_kbd_handle >= 0) ? 0 : -1;
    }
    s_kbd_inited = 1;

    if (sceKeyboardInit) {
        sceKeyboardInit();
    }

    int32_t user = -1;
    if (sceUserServiceGetInitialUser) {
        (void)sceUserServiceGetInitialUser(&user);
    }
    if (user >= 0 && sceKeyboardOpen) {
        s_kbd_handle = sceKeyboardOpen(user, 0, 0, NULL);
    }
    return (s_kbd_handle >= 0) ? 0 : -1;
}

int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events) {
    if (!out_events || max_events == 0) {
        return -1;
    }
    if (s_kbd_handle < 0 || !sceKeyboardReadState) {
        return -1;
    }
    /* Capture-gated: the platform key-record layout (documented at 96 bytes) is unconfirmed.
     * Reporting zero events is the honest answer until the obSCEne capture confirms the record,
     * rather than translating a guessed layout into fabricated key presses. */
    return 0;
}

void oops_keyboard_close(void) {
    if (s_kbd_handle >= 0 && sceKeyboardClose) {
        sceKeyboardClose(s_kbd_handle);
        s_kbd_handle = -1;
    }
    s_kbd_inited = 0;
}
