#include "oops/keyboard.h"
#include <stddef.h>

/*
 * Platform symbols from libSceKeyboard and libSceUserService.
 *
 * Confirmed on 12.40 by obSCEne 101-input-ext: libSceKeyboard loads and all
 * four entry points resolve in the app context; in the eboot and payload
 * contexts the library loads but nothing resolves, which the weak binding
 * reports as unavailable rather than guessing.
 */
__attribute__((weak)) int sceKeyboardInit(void);
__attribute__((weak)) int sceKeyboardOpen(int userId, int type, int index,
                                          const void *param);
__attribute__((weak)) int sceKeyboardClose(int handle);
__attribute__((weak)) int sceKeyboardReadState(int handle, void *state);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

/*
 * The platform key record is 96 bytes: obSCEne 101-input-ext/kbd-read on 12.40
 * handed sceKeyboardReadState a 4 KB fill and it rewrote exactly 96 bytes, in
 * two runs. The record came back all zero except a 1 at bytes 16 and 20, and in
 * the second run at byte 8 as well; no key was held in either. The size is
 * confirmed; which bytes mean what is not, so the read still refuses. When a
 * capture with a keyboard attached lands: define the record beside this, set
 * the fields flag, and translate it in oops_keyboard_read - the only place that
 * reads it.
 */
#define OOPS_KEY_RECORD_BYTES 96
#define OOPS_KEY_RECORD_FIELDS_CONFIRMED 0

static int s_kbd_handle = -1;
static int s_kbd_inited = 0;
static int s_kbd_init_rc =
    OOPS_KEYBOARD_EUNAVAIL; /* what the first init reported */

int oops_keyboard_available(void) {
  /* Real once the names are confirmed: the capability exists iff the open/read
   * entry points resolve. */
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
    s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
    return s_kbd_init_rc;
  }

  int rc = sceKeyboardOpen(user, 0, 0, NULL);
  if (rc < 0) {
    s_kbd_init_rc = rc; /* the platform's own code, passed through */
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
  /* Capture-gated: a distinct code rather than zero events, so a caller can
   * tell "no keys" from "no reader". Checked before the handle because it is
   * true on every firmware. */
  if (!OOPS_KEY_RECORD_FIELDS_CONFIRMED) {
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
