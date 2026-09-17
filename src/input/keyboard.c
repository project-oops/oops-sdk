#include "oops/keyboard.h"
#include "oops/input.h"
#include "oops/system.h"
#include <stddef.h>

/*
 * Platform symbols from libSceKeyboard, libSceUserService, and libSceSysmodule.
 */
__attribute__((weak)) int sceKeyboardInit(void);
__attribute__((weak)) int sceKeyboardOpen(int userId, int type, int index,
                                          const void *param);
__attribute__((weak)) int sceKeyboardClose(int handle);
__attribute__((weak)) int sceKeyboardReadState(int handle, void *state);
__attribute__((weak)) int sceKeyboardSetProcessFocus(int focus);
__attribute__((weak)) int sceKeyboardConnectPort(int port);
__attribute__((weak)) int sceKeyboardGetConnection(int handle, int *connection);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceGetLoginUserIdList(void *list);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);
__attribute__((weak)) int sceSysmoduleLoadModule(uint16_t id);

#define OOPS_KEY_RECORD_BYTES 96
#define OOPS_KEY_RECORD_FIELDS_CONFIRMED 0

static int s_kbd_handle = -1;
static int s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;

int oops_keyboard_available(void) {
  return (sceKeyboardOpen && sceKeyboardReadState) ? 1 : 0;
}

int oops_keyboard_init(void) {
  if (s_kbd_handle >= 0) {
    return OOPS_KEYBOARD_OK;
  }

  if (sceSysmoduleLoadModule) {
    (void)sceSysmoduleLoadModule(0x0106); /* OOPS_SYSMODULE_KEYBOARD */
  }

  if (!oops_keyboard_available()) {
    oops_kprintf("KBD", "keyboard entry points unavailable (open=%p read=%p)\n",
                 (const void *)sceKeyboardOpen, (const void *)sceKeyboardReadState);
    s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
    return s_kbd_init_rc;
  }

  if (sceKeyboardInit) {
    int irc = sceKeyboardInit();
    oops_kprintf("KBD", "sceKeyboardInit returned %d\n", irc);
  }

  if (sceKeyboardSetProcessFocus) {
    int frc = sceKeyboardSetProcessFocus(1);
    oops_kprintf("KBD", "sceKeyboardSetProcessFocus(1) returned %d\n", frc);
  }

  if (sceUserServiceInitialize) {
    sceUserServiceInitialize(NULL);
  }

  int32_t user = -1;
  if (sceUserServiceGetInitialUser &&
      sceUserServiceGetInitialUser(&user) == 0 && user >= 0) {
    /* resolved */
  } else if (sceUserServiceGetLoginUserIdList) {
    struct {
      int32_t userId[4];
    } loginList;
    for (size_t i = 0; i < sizeof(loginList); i++) {
      ((unsigned char *)&loginList)[i] = 0;
    }
    if (sceUserServiceGetLoginUserIdList(&loginList) == 0) {
      for (int i = 0; i < 4; i++) {
        if (loginList.userId[i] >= 0 && loginList.userId[i] != 0xFF) {
          user = loginList.userId[i];
          break;
        }
      }
    }
  }

  int rc = -1;
  int32_t candidates[3] = { user, 0x10000000, 0xFF };
  for (size_t u = 0; u < 3 && rc < 0; u++) {
    int32_t uid = candidates[u];
    if (uid < 0) continue;
    for (int idx = 0; idx < 4 && rc < 0; idx++) {
      rc = sceKeyboardOpen(uid, 0, idx, NULL);
      oops_kprintf("KBD", "sceKeyboardOpen(uid=0x%x, type=0, idx=%d) -> %d\n",
                   (unsigned int)uid, idx, rc);
    }
  }

  if (rc < 0) {
    s_kbd_init_rc = rc;
    return rc;
  }

  s_kbd_handle = rc;
  s_kbd_init_rc = OOPS_KEYBOARD_OK;
  oops_kprintf("KBD", "keyboard handle opened: %d\n", s_kbd_handle);
  return OOPS_KEYBOARD_OK;
}

int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events) {
  if (!out_events || max_events == 0) {
    return OOPS_KEYBOARD_EPARAM;
  }
  if (!OOPS_KEY_RECORD_FIELDS_CONFIRMED) {
    return OOPS_KEYBOARD_ELAYOUT;
  }
  if (s_kbd_handle < 0 || !sceKeyboardReadState) {
    return OOPS_KEYBOARD_EUNAVAIL;
  }
  return 0;
}

uint32_t oops_keyboard_poll_buttons(void) {
  if (!sceKeyboardReadState || !sceKeyboardOpen) {
    return 0u;
  }

  if (s_kbd_handle < 0) {
    static int s_retry_tick = 0;
    if ((s_retry_tick++ % 60) == 0) {
      (void)oops_keyboard_init();
    }
    if (s_kbd_handle < 0) {
      return 0u;
    }
  }

  uint8_t state[96];
  for (size_t i = 0; i < sizeof(state); i++) {
    state[i] = 0;
  }

  int rrc = sceKeyboardReadState(s_kbd_handle, state);
  if (rrc != 0) {
    static int s_read_err_count = 0;
    if (s_read_err_count++ < 3) {
      oops_kprintf("KBD", "sceKeyboardReadState(%d) returned %d\n", s_kbd_handle, rrc);
    }
    return 0u;
  }

  uint32_t buttons = 0u;

  /* Scan across state bytes from offset 8 up to 95 for USB HID usage codes */
  for (size_t i = 8; i < sizeof(state); i++) {
    uint8_t code = state[i];
    switch (code) {
      /* D-pad: Standard USB HID Arrow Keys */
      case 79: /* Right arrow (0x4F) */
        buttons |= OOPS_BUTTON_RIGHT;
        break;
      case 80: /* Left arrow (0x50) */
        buttons |= OOPS_BUTTON_LEFT;
        break;
      case 81: /* Down arrow (0x51) */
        buttons |= OOPS_BUTTON_DOWN;
        break;
      case 82: /* Up arrow (0x52) */
        buttons |= OOPS_BUTTON_UP;
        break;

      /* Cross (X / Confirm): Enter and Keypad Enter */
      case 40: /* Return / Enter (0x28) */
      case 88: /* Keypad Enter (0x58) */
        buttons |= OOPS_BUTTON_CROSS;
        break;

      /* Circle (Back / Cancel): Escape and Backspace */
      case 41: /* Escape (0x29) */
      case 42: /* Backspace (0x2A) */
        buttons |= OOPS_BUTTON_CIRCLE;
        break;

      /* Triangle (Search): F1 */
      case 58: /* F1 (0x3A) */
        buttons |= OOPS_BUTTON_TRIANGLE;
        break;

      /* Square (Library): F2 */
      case 59: /* F2 (0x3B) */
        buttons |= OOPS_BUTTON_SQUARE;
        break;

      /* Options: F3 */
      case 60: /* F3 (0x3C) */
        buttons |= OOPS_BUTTON_OPTIONS;
        break;

      /* PS Button: Pause/Break key and Home key */
      case 72: /* Pause / Break (0x48) */
      case 74: /* Home key (0x4A) */
        buttons |= (1u << 16); /* HOME_BUTTON_PS */
        break;

      /* L1 / R1: Page Up / Page Down */
      case 75: /* Page Up (0x4B) */
        buttons |= OOPS_BUTTON_L1;
        break;
      case 78: /* Page Down (0x4E) */
        buttons |= OOPS_BUTTON_R1;
        break;

      default:
        break;
    }
  }

  return buttons;
}

void oops_keyboard_close(void) {
  if (s_kbd_handle >= 0 && sceKeyboardClose) {
    sceKeyboardClose(s_kbd_handle);
  }
  s_kbd_handle = -1;
  s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
}
