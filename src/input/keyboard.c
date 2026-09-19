#include "oops/keyboard.h"
#include "oops/input.h"
#include "oops/system.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Platform symbols from libSceKeyboard, libSceUserService, and libSceSysmodule.
 */
__attribute__((weak)) int sceKeyboardInit(void);
__attribute__((weak)) int sceKeyboardOpen(int userId, int type, int index,
                                          const void *param);
__attribute__((weak)) int sceKeyboardClose(int handle);
__attribute__((weak)) int sceKeyboardRead(int handle, void *data, int capacity);
__attribute__((weak)) int sceKeyboardReadState(int handle, void *state);
__attribute__((weak)) int sceKeyboardConnectPort(int port);
__attribute__((weak)) int sceKeyboardGetConnection(int handle, int *connection);
__attribute__((weak)) int sceKeyboardGetHandle(int userId, int type, int index);
__attribute__((weak)) int sceKeyboardSetProcessPrivilege(int privilege);
__attribute__((weak)) int sceKeyboardSetProcessFocus(int focus);
__attribute__((weak)) int sceUserServiceGetForegroundUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceGetLoginUserIdList(void *list);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);
__attribute__((weak)) int sceSysmoduleLoadModule(uint16_t id);

#define OOPS_KEY_RECORD_BYTES 96
#define OOPS_KEY_RECORD_FIELDS_CONFIRMED 0
#define OOPS_MAX_HW_KEYS 16
#define OOPS_KEYBOARD_MAX_HANDLES 2

/*
 * Verified 96-byte PS5 native hardware keyboard report layout
 * (from ps5-native-gamepad-input-research hardware measurements).
 */
typedef struct {
  uint64_t timestamp_us;               /* 0x00: Native report timestamp */
  uint8_t  intercepted;                /* 0x08: Nonzero when system owns report */
  uint8_t  reserved0[7];               /* 0x09 */
  uint8_t  connected;                  /* 0x10: Nonzero while device is available */
  uint8_t  reserved1[3];               /* 0x11 */
  int32_t  length;                     /* 0x14: Number of active keys */
  uint32_t leds;                       /* 0x18: Num/Caps/Scroll lock */
  uint32_t modifiers;                  /* 0x1c: Left/Right Ctrl, Shift, Alt, GUI */
  uint16_t keycodes[OOPS_MAX_HW_KEYS]; /* 0x20: USB HID usage codes */
  uint8_t  reserved2[32];              /* 0x40 */
} oops_kbd_hw_record_t;

static int s_kbd_handles[OOPS_KEYBOARD_MAX_HANDLES] = {-1, -1};
static uint16_t s_kbd_previous_keys[OOPS_KEYBOARD_MAX_HANDLES][OOPS_MAX_HW_KEYS];
static int s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
static int s_keyboard_module_loaded = 0;

static uint32_t decode_keycode(uint16_t code) {
  switch (code) {
    /* D-pad: Standard USB HID Arrow Keys + WASD */
    case 79: /* Right arrow (0x4F) */
    case 7:  /* D key */
      return OOPS_BUTTON_RIGHT;
    case 80: /* Left arrow (0x50) */
    case 4:  /* A key */
      return OOPS_BUTTON_LEFT;
    case 81: /* Down arrow (0x51) */
    case 22: /* S key */
      return OOPS_BUTTON_DOWN;
    case 82: /* Up arrow (0x52) */
    case 26: /* W key */
      return OOPS_BUTTON_UP;

    /* Cross (X / Confirm): Enter, Keypad Enter, Space */
    case 40: /* Return / Enter (0x28) */
    case 88: /* Keypad Enter (0x58) */
      return OOPS_BUTTON_CROSS;

    /* Circle (Back / Cancel): Escape and Backspace */
    case 41: /* Escape (0x29) */
    case 42: /* Backspace (0x2A) */
      return OOPS_BUTTON_CIRCLE;

    /* Triangle (Search): F1 */
    case 58: /* F1 (0x3A) */
      return OOPS_BUTTON_TRIANGLE;

    /* Square (Library): F2 */
    case 59: /* F2 (0x3B) */
      return OOPS_BUTTON_SQUARE;

    /* Options: F3 */
    case 60: /* F3 (0x3C) */
      return OOPS_BUTTON_OPTIONS;

    /* PS Button: Pause/Break key and Home key */
    case 72: /* Pause / Break (0x48) */
    case 74: /* Home key (0x4A) */
      return (1u << 16); /* HOME_BUTTON_PS */

    /* L1 / R1: Page Up / Page Down, Q / E */
    case 75: /* Page Up (0x4B) */
    case 20: /* Q key */
      return OOPS_BUTTON_L1;
    case 78: /* Page Down (0x4E) */
    case 8:  /* E key */
      return OOPS_BUTTON_R1;

    /* L2 / R2: Tab / Space */
    case 43: /* Tab */
      return OOPS_BUTTON_L2;
    case 44: /* Space */
      return OOPS_BUTTON_R2;

    default:
      return 0u;
  }
}

static uint32_t decode_keys_array(const uint16_t *keys, int count) {
  if (!keys) return 0u;
  uint32_t b = 0u;
  for (int k = 0; k < count && k < OOPS_MAX_HW_KEYS; k++) {
    if (keys[k] != 0) {
      b |= decode_keycode(keys[k]);
    }
  }
  return b;
}

static int is_usable_sample(const oops_kbd_hw_record_t *rec) {
  return (rec && rec->connected != 0 && rec->intercepted == 0);
}

int oops_keyboard_available(void) {
  int can_open = (sceKeyboardOpen && oops_symbol_is_resolved((const void *)sceKeyboardOpen));
  int can_read = (sceKeyboardReadState && oops_symbol_is_resolved((const void *)sceKeyboardReadState)) ||
                 (sceKeyboardRead && oops_symbol_is_resolved((const void *)sceKeyboardRead));
  return (can_open && can_read) ? 1 : 0;
}

int oops_keyboard_init(void) {
  if (!s_keyboard_module_loaded) {
    if (sceSysmoduleLoadModule && oops_symbol_is_resolved((const void *)sceSysmoduleLoadModule)) {
      (void)sceSysmoduleLoadModule(0x0106); /* OOPS_SYSMODULE_KEYBOARD */
    }
    if (sceKeyboardInit && oops_symbol_is_resolved((const void *)sceKeyboardInit)) {
      int irc = sceKeyboardInit();
      oops_kprintf("KBD", "sceKeyboardInit returned %d\n", irc);
    }
    if (sceUserServiceInitialize && oops_symbol_is_resolved((const void *)sceUserServiceInitialize)) {
      sceUserServiceInitialize(NULL);
    }
    if (sceKeyboardSetProcessPrivilege && oops_symbol_is_resolved((const void *)sceKeyboardSetProcessPrivilege)) {
      int prc = sceKeyboardSetProcessPrivilege(1);
      oops_kprintf("KBD", "sceKeyboardSetProcessPrivilege(1) returned %d\n", prc);
    }
    if (sceKeyboardSetProcessFocus && oops_symbol_is_resolved((const void *)sceKeyboardSetProcessFocus)) {
      int frc = sceKeyboardSetProcessFocus(1);
      oops_kprintf("KBD", "sceKeyboardSetProcessFocus(1) returned %d\n", frc);
    }
    s_keyboard_module_loaded = 1;
  }

  if (!oops_keyboard_available()) {
    oops_kprintf("KBD", "keyboard entry points unavailable (open=%p read=%p readState=%p)\n",
                 (const void *)sceKeyboardOpen, (const void *)sceKeyboardRead,
                 (const void *)sceKeyboardReadState);
    s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
    return s_kbd_init_rc;
  }

  int32_t uids[8];
  int uid_count = 0;

  int32_t fg_user = -1;
  if (sceUserServiceGetForegroundUser &&
      oops_symbol_is_resolved((const void *)sceUserServiceGetForegroundUser) &&
      sceUserServiceGetForegroundUser(&fg_user) == 0 && fg_user >= 0) {
    uids[uid_count++] = fg_user;
  }

  int32_t init_user = -1;
  if (sceUserServiceGetInitialUser &&
      oops_symbol_is_resolved((const void *)sceUserServiceGetInitialUser) &&
      sceUserServiceGetInitialUser(&init_user) == 0 && init_user >= 0) {
    int already = 0;
    for (int i = 0; i < uid_count; i++) {
      if (uids[i] == init_user) { already = 1; break; }
    }
    if (!already) uids[uid_count++] = init_user;
  }

  if (sceUserServiceGetLoginUserIdList &&
      oops_symbol_is_resolved((const void *)sceUserServiceGetLoginUserIdList)) {
    struct {
      int32_t userId[4];
    } loginList;
    for (size_t i = 0; i < sizeof(loginList); i++) {
      ((unsigned char *)&loginList)[i] = 0;
    }
    if (sceUserServiceGetLoginUserIdList(&loginList) == 0) {
      for (int i = 0; i < 4; i++) {
        int32_t uid = loginList.userId[i];
        if (uid >= 0 && uid != 0xFF) {
          int already = 0;
          for (int j = 0; j < uid_count; j++) {
            if (uids[j] == uid) { already = 1; break; }
          }
          if (!already && uid_count < 8) uids[uid_count++] = uid;
        }
      }
    }
  }

  /* Fallback defaults if no user service resolution */
  if (uid_count == 0) {
    uids[uid_count++] = 0x10000000;
    uids[uid_count++] = 0xFF;
  }

  int opened = 0;
  for (int idx = 0; idx < OOPS_KEYBOARD_MAX_HANDLES; idx++) {
    if (s_kbd_handles[idx] >= 0) {
      opened++;
      continue;
    }
    for (int u = 0; u < uid_count; u++) {
      int32_t uid = uids[u];
      int rc = sceKeyboardOpen(uid, 0, idx, NULL);
      if (rc >= 0) {
        s_kbd_handles[idx] = rc;
        for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
          s_kbd_previous_keys[idx][k] = 0;
        }
        opened++;
        oops_kprintf("KBD", "sceKeyboardOpen(uid=0x%x, idx=%d) -> handle %d\n",
                     (unsigned int)uid, idx, rc);
        break;
      }
    }
  }

  if (opened == 0) {
    s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
    return s_kbd_init_rc;
  }

  s_kbd_init_rc = OOPS_KEYBOARD_OK;
  return OOPS_KEYBOARD_OK;
}

int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events) {
  if (!out_events || max_events == 0) {
    return OOPS_KEYBOARD_EPARAM;
  }
  if (!OOPS_KEY_RECORD_FIELDS_CONFIRMED) {
    return OOPS_KEYBOARD_ELAYOUT;
  }
  int any_valid = 0;
  for (int i = 0; i < OOPS_KEYBOARD_MAX_HANDLES; i++) {
    if (s_kbd_handles[i] >= 0) { any_valid = 1; break; }
  }
  if (!any_valid || (!sceKeyboardReadState && !sceKeyboardRead)) {
    return OOPS_KEYBOARD_EUNAVAIL;
  }
  return 0;
}

uint32_t oops_keyboard_poll_buttons(void) {
  if (!oops_keyboard_available()) {
    return 0u;
  }

  /*
   * Periodic reconnect check: only attempt if NO keyboard handles are open.
   * Capped to once every 180 frames (~3s) to prevent system call flooding.
   */
  static int s_poll_tick = 0;
  int any_open = 0;
  for (int i = 0; i < OOPS_KEYBOARD_MAX_HANDLES; i++) {
    if (s_kbd_handles[i] >= 0) {
      any_open = 1;
      break;
    }
  }
  if (!any_open && (++s_poll_tick % 180) == 0) {
    (void)oops_keyboard_init();
  }

  uint32_t total_buttons = 0u;

  for (int idx = 0; idx < OOPS_KEYBOARD_MAX_HANDLES; idx++) {
    int handle = s_kbd_handles[idx];
    if (handle < 0) continue;

    /*
     * Primary Path: sceKeyboardReadState queries the instantaneous physical
     * driver state. Zero latency, immediate release reporting on the next frame
     * (allowing rapid double-taps), and continuous held state on every frame
     * (allowing smooth hold-to-repeat across titles).
     */
    if (sceKeyboardReadState && oops_symbol_is_resolved((const void *)sceKeyboardReadState)) {
      oops_kbd_hw_record_t record;
      for (size_t s = 0; s < sizeof(record); s++) {
        ((uint8_t *)&record)[s] = 0;
      }
      int rc = sceKeyboardReadState(handle, &record);
      if (rc == 0) {
        if (is_usable_sample(&record)) {
          total_buttons |= decode_keys_array(record.keycodes, OOPS_MAX_HW_KEYS);
        }
        continue;
      }
      /* If sceKeyboardReadState fails with an error, fall through to fallback path */
    }

    /*
     * Secondary / Fallback Path: sceKeyboardRead event queue
     */
    if (sceKeyboardRead && oops_symbol_is_resolved((const void *)sceKeyboardRead)) {
      oops_kbd_hw_record_t samples[16];
      for (size_t s = 0; s < sizeof(samples); s++) {
        ((uint8_t *)samples)[s] = 0;
      }
      int sample_count = sceKeyboardRead(handle, samples, 16);
      if (sample_count > 0) {
        int limit = (sample_count > 16) ? 16 : sample_count;
        for (int s = 0; s < limit; s++) {
          const oops_kbd_hw_record_t *sample = &samples[s];
          if (!is_usable_sample(sample)) {
            for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
              s_kbd_previous_keys[idx][k] = 0;
            }
            continue;
          }
          for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
            s_kbd_previous_keys[idx][k] = sample->keycodes[k];
          }
        }
      }
      total_buttons |= decode_keys_array(s_kbd_previous_keys[idx], OOPS_MAX_HW_KEYS);
    }
  }

  return total_buttons;
}

void oops_keyboard_close(void) {
  for (int i = 0; i < OOPS_KEYBOARD_MAX_HANDLES; i++) {
    if (s_kbd_handles[i] >= 0 && sceKeyboardClose &&
        oops_symbol_is_resolved((const void *)sceKeyboardClose)) {
      sceKeyboardClose(s_kbd_handles[i]);
    }
    s_kbd_handles[i] = -1;
    for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
      s_kbd_previous_keys[i][k] = 0;
    }
  }
  s_kbd_init_rc = OOPS_KEYBOARD_EUNAVAIL;
  s_keyboard_module_loaded = 0;
}
