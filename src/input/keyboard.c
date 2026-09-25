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

/*
 * The record's fields, confirmed 2026-09-22 - **by an application, not a capture.**
 *
 * This was 0 for as long as the subsystem existed, on the reasoning that the 96-byte size was
 * measured but the meaning of the bytes was not, so a read would be parsing a guess. That was
 * right when it was written and had stopped being true without anyone noticing.
 *
 * `oops_keyboard_poll_buttons` below reads this same record through the same
 * `sceKeyboardReadState` call and has never been gated. `oops-apps`' SeaShell has been calling it
 * in its input loop (`src/oops-utilities/seashell/home_main.c`), and its `decode_keycode` maps
 * **22 distinct USB HID usage codes** - arrows, WASD, Enter, Escape, Backspace, F1-F3, Page
 * Up/Down, Q/E, Tab, Space, Pause, Home. Those do not come out of a wrong offset, a wrong stride
 * or a wrong `connected`/`intercepted`: the navigation would be noise, and it is not.
 *
 * So `keycodes` at 0x20, `connected` at 0x10 and `intercepted` at 0x08 are confirmed in the
 * strongest way available - a shipping application depending on them on this hardware.
 * `modifiers` at 0x1c is **not**, and is gated separately at `OOPS_KEY_MODIFIERS_CONFIRMED`;
 * `timestamp_us` and `leds` are read by nothing and reported as zero.
 *
 * The cost of the old arrangement, for the record: `oops_keyboard_read` is the only route a
 * character has into a GLUT, SDL2 or GLFW program, so every port framework in the collection had
 * a dead keyboard, while the one application that happened to take the button path worked.
 * (`REQ-20260922T2015Z-b4d7`.)
 */
#define OOPS_KEY_RECORD_FIELDS_CONFIRMED 1

#define OOPS_MAX_HW_KEYS 16
#define OOPS_KEYBOARD_MAX_HANDLES 2

/*
 * Verified 96-byte Prospero native hardware keyboard report layout
 * (from native-gamepad-input-research hardware measurements).
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
/* Held keys as `oops_keyboard_poll_buttons` last saw them. */
static uint16_t s_kbd_previous_keys[OOPS_KEYBOARD_MAX_HANDLES][OOPS_MAX_HW_KEYS];
/* And as `oops_keyboard_read` last saw them. Two histories on purpose - see that function. */
static uint16_t s_kbd_event_keys[OOPS_KEYBOARD_MAX_HANDLES][OOPS_MAX_HW_KEYS];
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
      /* `oops/input.h` is included above and owns this bit; it was a bare literal here, with a
       * comment naming a constant private to one application. */
      return OOPS_BUTTON_BIT16;

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

/* See `oops_input_set_log_level` in oops/keyboard.h. The scale is `oops_log_level_t`. */
int s_input_log_level = (int)OOPS_LOG_INFO;

void oops_input_set_log_level(int level) {
  s_input_log_level = level < (int)OOPS_LOG_NONE ? (int)OOPS_LOG_NONE
                    : (level > (int)OOPS_LOG_TRACE ? (int)OOPS_LOG_TRACE : level);
}

int oops_input_get_log_level(void) { return s_input_log_level; }

int oops_keyboard_available(void) {
  int can_open = (sceKeyboardOpen && oops_symbol_is_resolved((const void *)sceKeyboardOpen));
  int can_read = (sceKeyboardReadState && oops_symbol_is_resolved((const void *)sceKeyboardReadState)) ||
                 (sceKeyboardRead && oops_symbol_is_resolved((const void *)sceKeyboardRead));
  return (can_open && can_read) ? 1 : 0;
}

int oops_keyboard_init(void) {
  /* The `input` channel of `/app0/oops-log` - see `oops_log_channel_level`. Same arrangement as
   * oops-gl's: asked for here, so logging depends on nothing, and an explicit
   * `oops_input_set_log_level` afterwards still wins. */
  s_input_log_level = (int)oops_log_channel_level("input", (oops_log_level_t)s_input_log_level);
  if (!s_keyboard_module_loaded) {
    if (sceSysmoduleLoadModule && oops_symbol_is_resolved((const void *)sceSysmoduleLoadModule)) {
      (void)sceSysmoduleLoadModule(0x0106); /* OOPS_SYSMODULE_KEYBOARD */
    }
    if (sceKeyboardInit && oops_symbol_is_resolved((const void *)sceKeyboardInit)) {
      int irc = sceKeyboardInit();
      oops_log_debug("KBD", "sceKeyboardInit returned %d", irc);
    }
    if (sceUserServiceInitialize && oops_symbol_is_resolved((const void *)sceUserServiceInitialize)) {
      sceUserServiceInitialize(NULL);
    }
    if (sceKeyboardSetProcessPrivilege && oops_symbol_is_resolved((const void *)sceKeyboardSetProcessPrivilege)) {
      int prc = sceKeyboardSetProcessPrivilege(1);
      oops_log_debug("KBD", "sceKeyboardSetProcessPrivilege(1) returned %d", prc);
    }
    if (sceKeyboardSetProcessFocus && oops_symbol_is_resolved((const void *)sceKeyboardSetProcessFocus)) {
      int frc = sceKeyboardSetProcessFocus(1);
      oops_log_debug("KBD", "sceKeyboardSetProcessFocus(1) returned %d", frc);
    }
    s_keyboard_module_loaded = 1;
  }

  if (!oops_keyboard_available()) {
    oops_log_warn("KBD", "keyboard entry points unavailable (open=%p read=%p readState=%p)",
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
        oops_log_info("KBD", "sceKeyboardOpen(uid=0x%x, idx=%d) -> handle %d",
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

/* One record from a handle, newest first: the polled state if it answers, else the newest sample
 * from the event queue. 1 when `out` was filled. The same two-step `oops_keyboard_poll_buttons`
 * uses, factored out so the two readers cannot drift apart. */
static int kbd_read_record(int handle, oops_kbd_hw_record_t *out) {
  for (size_t s = 0; s < sizeof(*out); s++) {
    ((uint8_t *)out)[s] = 0;
  }
  if (sceKeyboardReadState &&
      oops_symbol_is_resolved((const void *)sceKeyboardReadState)) {
    if (sceKeyboardReadState(handle, out) == 0) {
      return 1;
    }
  }
  if (sceKeyboardRead && oops_symbol_is_resolved((const void *)sceKeyboardRead)) {
    oops_kbd_hw_record_t samples[16];
    for (size_t s = 0; s < sizeof(samples); s++) {
      ((uint8_t *)samples)[s] = 0;
    }
    int count = sceKeyboardRead(handle, samples, 16);
    if (count > 0) {
      *out = samples[(count > 16 ? 16 : count) - 1];
      return 1;
    }
  }
  return 0;
}

static int kbd_usage_present(const uint16_t *keys, uint16_t usage) {
  for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
    if (keys[k] == usage) {
      return 1;
    }
  }
  return 0;
}

/*
 * The modifier mask for an event.
 *
 * `modifiers` at 0x1c is the **one field of the record nothing has ever exercised**. SeaShell
 * validates `keycodes`, `connected` and `intercepted` by using them; it never reads this. So it
 * is reported as zero rather than decoded, which costs shifted characters and costs nothing
 * else - a caller sees "no modifier held", which is wrong only in the same direction as a
 * keyboard with no shift key, never in the direction of a character that was not typed.
 *
 * Flip this when `REQ-20260922T1905Z-9c31` lands. The decode below is written against the USB HID
 * boot-protocol modifier byte (bit 0 LCtrl … bit 7 RGUI), which is what a 96-byte report of this
 * shape would carry, and it is a prediction until that capture agrees with it.
 */
#define OOPS_KEY_MODIFIERS_CONFIRMED 0

static uint8_t kbd_event_modifiers(const oops_kbd_hw_record_t *rec) {
#if OOPS_KEY_MODIFIERS_CONFIRMED
  const uint32_t p = rec->modifiers;
  uint8_t m = 0u;
  if (p & 0x11u) m |= (uint8_t)OOPS_KMOD_CTRL;  /* left | right */
  if (p & 0x22u) m |= (uint8_t)OOPS_KMOD_SHIFT;
  if (p & 0x44u) m |= (uint8_t)OOPS_KMOD_ALT;
  if (p & 0x88u) m |= (uint8_t)OOPS_KMOD_GUI;
  return m;
#else
  (void)rec;
  return 0u;
#endif
}

/*
 * Key transitions, by diffing the active-key set against the one this function last saw.
 *
 * The platform reports which keys are *down*, not which changed, so the edges are ours to find.
 * `s_kbd_event_keys` is this function's own history and is deliberately **not**
 * `s_kbd_previous_keys`: `oops_keyboard_poll_buttons` owns that one, and an application calling
 * both - SeaShell does - must not have one reader eat the other's edges.
 *
 * A handle is emitted whole or not at all. A handle can produce at most `OOPS_MAX_HW_KEYS`
 * releases plus as many presses, so it is counted before anything is written and skipped if it
 * will not fit; the next call re-diffs against an unchanged history and reports it then. Emitting
 * half a handle and advancing the history would drop the remainder silently, which is the one
 * failure a caller could not see.
 */
int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events) {
  if (!out_events || max_events == 0) {
    return OOPS_KEYBOARD_EPARAM;
  }
  if (!OOPS_KEY_RECORD_FIELDS_CONFIRMED) {
    return OOPS_KEYBOARD_ELAYOUT;
  }
  if (!oops_keyboard_available()) {
    return OOPS_KEYBOARD_EUNAVAIL;
  }

  int any_valid = 0;
  for (int i = 0; i < OOPS_KEYBOARD_MAX_HANDLES; i++) {
    if (s_kbd_handles[i] >= 0) { any_valid = 1; break; }
  }
  if (!any_valid) {
    return OOPS_KEYBOARD_EUNAVAIL;
  }

  const unsigned int cap =
      (max_events > OOPS_MAX_KEY_EVENTS) ? (unsigned int)OOPS_MAX_KEY_EVENTS : max_events;
  unsigned int n = 0u;

  /*
   * **The handles are merged into one keyboard, and the edges are found once.**
   *
   * `sceKeyboardOpen` is called for index 0 and index 1 of the same user, because a user may
   * have more than one keyboard. With one attached, index 1 opens anyway and mirrors index 0 -
   * the log shows both succeeding with different handles. This used to keep a history per handle
   * and walk them in turn, so one press produced one event per handle and every keypress was
   * delivered twice.
   *
   * Suppressing the repeat within a call was not enough, and the reason is worth keeping: the
   * two handles are sampled independently, so the same press can arrive from one of them in this
   * call and the other in the next. A duplicate separated by a poll looks exactly like a real
   * second press. Only one history can settle that, so there is one - the union of what every
   * handle reports is what "the keyboard" is holding, and an edge against that is a keypress.
   *
   * Two real keyboards still work, and the same key held on both is one key held, which is the
   * only answer that means anything to a caller.
   *
   * What this cost before it was found: Neverball's on-screen keyboard typed `YY` for one press
   * of `Y`, and one press of Enter on Play activated the level select and then that screen's
   * Back, so the menu appeared to bounce off itself. Nothing in the title was wrong, and from
   * above this function two presses are indistinguishable from someone pressing twice.
   */
  uint16_t merged[OOPS_MAX_HW_KEYS];
  for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
    merged[k] = 0u;
  }
  unsigned int merged_n = 0u;
  uint8_t mods = 0u;
  int saw_any = 0;

  for (int idx = 0; idx < OOPS_KEYBOARD_MAX_HANDLES; idx++) {
    const int handle = s_kbd_handles[idx];
    if (handle < 0) {
      continue;
    }

    oops_kbd_hw_record_t record;
    if (!kbd_read_record(handle, &record)) {
      continue;
    }
    saw_any = 1;

    /* A disconnected keyboard, or one the system has taken, releases everything it was holding
     * rather than leaving a key stuck down for as long as the overlay is up - so it contributes
     * nothing to the union rather than contributing what it last held. */
    if (!is_usable_sample(&record)) {
      continue;
    }

    mods = (uint8_t)(mods | kbd_event_modifiers(&record));

    for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
      const uint16_t is = record.keycodes[k];
      if (is == 0u || kbd_usage_present(merged, is)) {
        continue;
      }
      if (merged_n < (unsigned int)OOPS_MAX_HW_KEYS) {
        merged[merged_n++] = is;
      }
    }
  }

  if (!saw_any) {
    return 0;
  }

  unsigned int needed = 0u;
  for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
    const uint16_t was = s_kbd_event_keys[0][k];
    if (was != 0u && !kbd_usage_present(merged, was)) needed++;
    const uint16_t is = merged[k];
    if (is != 0u && !kbd_usage_present(s_kbd_event_keys[0], is)) needed++;
  }
  if (needed == 0u) {
    return 0;
  }
  if (needed > cap) {
    return 0; /* history untouched: the next call reports the whole change */
  }

  for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
    const uint16_t was = s_kbd_event_keys[0][k];
    if (was != 0u && !kbd_usage_present(merged, was)) {
      out_events[n].usage = was;
      out_events[n].transition = (uint8_t)OOPS_KEY_UP;
      out_events[n].modifiers = mods;
      out_events[n].timestamp = 0u;
      n++;
    }
  }
  for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
    const uint16_t is = merged[k];
    if (is != 0u && !kbd_usage_present(s_kbd_event_keys[0], is)) {
      out_events[n].usage = is;
      out_events[n].transition = (uint8_t)OOPS_KEY_DOWN;
      out_events[n].modifiers = mods;
      out_events[n].timestamp = 0u;
      n++;
    }
  }

  /* **What the title is about to be told**, at OOPS_LOG_DEBUG and above. Off by
     default: a held key is quiet but a typed sentence is two lines a character, and the reason
     this exists is that a duplicated press is invisible from above the SDK and
     indistinguishable, in a log, from someone pressing twice. */
  for (unsigned int e = 0u; e < n; e++) {
    oops_log_debug("KBD", "event usage=0x%x %s mods=0x%x", (unsigned int)out_events[e].usage,
                   out_events[e].transition == (uint8_t)OOPS_KEY_DOWN ? "down" : "up",
                   (unsigned int)out_events[e].modifiers);
  }

  for (int k = 0; k < OOPS_MAX_HW_KEYS; k++) {
    s_kbd_event_keys[0][k] = merged[k];
  }

  /*
   * Said once, on the first key this function ever delivers.
   *
   * Establishing that a keystroke reached a title on 2026-09-22 took inferring it from a demo
   * having presented two frames instead of one, because nothing on the path says anything. One
   * line removes that inference for every future run and for every port - and it is the line
   * that distinguishes "the keyboard is not working" from "the program ignored the key".
   */
  static int s_said_first;
  if (n > 0u && !s_said_first) {
    s_said_first = 1;
    oops_log_info("KBD", "first key delivered: usage 0x%02x - the read path works",
                  (unsigned int)out_events[0].usage);
  }

  return (int)n;
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
  oops_log_debug("KBD", "oops_keyboard_close: closing keyboard handles");
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
