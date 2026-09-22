#include "oops/input.h"
#include "oops/keyboard.h" /* oops_input_poll folds the keyboard's buttons in - see it */
#include "oops/system.h"
#include "pad_layout.h"
#include <stddef.h>

/* Platform symbols from libScePad and libSceUserService */
__attribute__((weak)) int scePadInit(void);
__attribute__((weak)) int scePadSetProcessPrivilege(int privilege);
__attribute__((weak)) int scePadGetHandle(int userId, int type, int index);
__attribute__((weak)) int scePadOpen(int userId, int type, int index,
                                     const void *param);
__attribute__((weak)) int scePadClose(int handle);
__attribute__((weak)) int scePadReadState(int handle, void *state);
__attribute__((weak)) int scePadRead(int handle, void *data, int num);
__attribute__((weak)) int scePadSetVibration(int handle, const void *param);
__attribute__((weak)) int scePadSetLightBar(int handle, const void *param);
__attribute__((weak)) int scePadResetLightBar(int handle);
__attribute__((weak)) int scePadResetOrientation(int handle);
/* Present in the app context on 12.40 (obSCEne 100-input/dualsense-symbols
 * resolved all seven DualSense entry points there) and absent in the eboot and
 * payload contexts, so whether it resolves is the availability check. Its
 * parameter layout is still unconfirmed. */
__attribute__((weak)) int scePadSetTriggerEffect(int handle, const void *param);
__attribute__((weak)) int sceUserServiceGetForegroundUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceGetLoginUserIdList(void *list);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

static int s_pad_handles[OOPS_MAX_PADS] = {-1, -1, -1, -1};
static int s_pad_retry_cooldown[OOPS_MAX_PADS] = {0, 0, 0, 0};
static int32_t s_user_id = -1;
static int s_initialized = 0;
static int s_init_rc =
    -1; /* what the first init reported; repeated until close */

static void try_resolve_user_id(void) {
  if (s_user_id >= 0) {
    return;
  }
  if (sceUserServiceInitialize) {
    struct {
      int priority;
    } params = {256};
    sceUserServiceInitialize(&params);
  }
  if (sceUserServiceGetForegroundUser &&
      sceUserServiceGetForegroundUser(&s_user_id) == 0 && s_user_id >= 0) {
    return;
  }
  if (sceUserServiceGetInitialUser &&
      sceUserServiceGetInitialUser(&s_user_id) == 0 && s_user_id >= 0) {
    return;
  }
  if (sceUserServiceGetLoginUserIdList) {
    struct {
      int32_t userId[4];
    } loginList;
    for (size_t i = 0; i < sizeof(loginList); i++) {
      ((unsigned char *)&loginList)[i] = 0;
    }
    if (sceUserServiceGetLoginUserIdList(&loginList) == 0) {
      for (int i = 0; i < 4; i++) {
        if (loginList.userId[i] >= 0 && loginList.userId[i] != 0xFF) {
          s_user_id = loginList.userId[i];
          return;
        }
      }
    }
  }
  /* Fallback to primary retail user ID if user service list unpopulated */
  if (s_user_id < 0 && (scePadOpen || scePadInit || scePadGetHandle)) {
    s_user_id = 0x10000000;
  }
}

/* Declared in pad_layout.h; shared by the single-state poll and the batched
 * read. */
void oops_input_map_record(oops_pad_state_t *out_state,
                           const ScePadDataInternal *raw) {
  out_state->buttons = raw->buttons;
  out_state->left_stick_x = (int8_t)((int)raw->leftStick.x - 128);
  out_state->left_stick_y = (int8_t)((int)raw->leftStick.y - 128);
  out_state->right_stick_x = (int8_t)((int)raw->rightStick.x - 128);
  out_state->right_stick_y = (int8_t)((int)raw->rightStick.y - 128);
  out_state->l2_trigger = raw->analogButtons.l2;
  out_state->r2_trigger = raw->analogButtons.r2;

  /* The driver's flag, alone. */
  out_state->connected = raw->connected ? 1 : 0;

  /* Touchpad touch points */
  for (int t = 0; t < 2; t++) {
    out_state->touch[t].x = raw->touchData.touch[t].x;
    out_state->touch[t].y = raw->touchData.touch[t].y;
    out_state->touch[t].id = raw->touchData.touch[t].finger;
    out_state->touch[t].active = (t < raw->touchData.fingers) ? 1 : 0;
  }

  /* 6-Axis Motion IMU telemetry */
  out_state->orientation[0] = raw->quat.x;
  out_state->orientation[1] = raw->quat.y;
  out_state->orientation[2] = raw->quat.z;
  out_state->orientation[3] = raw->quat.w;

  out_state->acceleration[0] = raw->accel.x;
  out_state->acceleration[1] = raw->accel.y;
  out_state->acceleration[2] = raw->accel.z;

  out_state->angular_velocity[0] = raw->vel.x;
  out_state->angular_velocity[1] = raw->vel.y;
  out_state->angular_velocity[2] = raw->vel.z;
}

int oops_input_init(void) {
  if (s_initialized)
    return s_init_rc;

  if (scePadSetProcessPrivilege) {
    int prc = scePadSetProcessPrivilege(1);
    oops_log_debug("INPUT", "scePadSetProcessPrivilege(1) returned %d", prc);
  }

  if (scePadInit) {
    int irc = scePadInit();
    oops_log_debug("INPUT", "scePadInit returned %d", irc);
  }

  if (sceUserServiceGetInitialUser) {
    if (sceUserServiceGetInitialUser(&s_user_id) != 0 &&
        sceUserServiceInitialize) {
      struct {
        int priority;
      } params = {256};
      sceUserServiceInitialize(&params);
      (void)sceUserServiceGetInitialUser(&s_user_id);
    }
  }

  try_resolve_user_id();
  oops_log_debug("INPUT", "resolved user_id=0x%x (%d)", (unsigned int)s_user_id, (int)s_user_id);

  if (s_user_id >= 0) {
    if (scePadGetHandle) {
      s_pad_handles[0] = scePadGetHandle(s_user_id, 0, 0);
      oops_log_debug("INPUT", "scePadGetHandle(user=0x%x, port=0) returned handle %d",
                     (unsigned int)s_user_id, s_pad_handles[0]);
    }
    if (s_pad_handles[0] < 0 && scePadOpen) {
      s_pad_handles[0] = scePadOpen(s_user_id, 0, 0, NULL);
      oops_log_debug("INPUT", "scePadOpen(user=0x%x, port=0) returned handle %d",
                     (unsigned int)s_user_id, s_pad_handles[0]);
    }
  }

  /* A failed init stays failed: later calls report this result, not a success
   * because a flag was set on the way out. */
  s_initialized = 1;
  s_init_rc = (s_pad_handles[0] >= 0) ? 0 : -1;
  return s_init_rc;
}

/*
 * A keyboard's directional and action keys, as pad buttons, for a port-0 poll that found no pad.
 *
 * `out_state` is already zeroed when this is called, and `connected` stays 0 on purpose: there is
 * no pad, and saying otherwise would be a lie a caller could act on. What changes is the return
 * code - a poll that has buttons to report succeeds, whatever produced them.
 */
/* See `oops_input_set_keyboard_as_pad` in the header for why this defaults on and who turns it
 * off. */
static int s_keyboard_as_pad = 1;

void oops_input_set_keyboard_as_pad(int enable) { s_keyboard_as_pad = enable ? 1 : 0; }

static int input_keyboard_only(oops_pad_state_t *out_state, uint32_t kbd) {
  if (kbd == 0u) {
    return -1;
  }
  out_state->buttons = kbd;
  return 0;
}

int oops_input_poll(unsigned int port, oops_pad_state_t *out_state) {
  if (!out_state || port >= OOPS_MAX_PADS)
    return -1;

  for (size_t i = 0; i < sizeof(*out_state); i++) {
    ((unsigned char *)out_state)[i] = 0;
  }

  /*
   * **The keyboard is an input device, so it arrives through the input call.**
   *
   * It did not, until 2026-09-22. `oops_keyboard_poll_buttons` decodes arrows, WASD, Enter,
   * Escape and the rest into `OOPS_BUTTON_*`, and an application wanting both had to know that
   * and OR the two together by hand. Exactly one did (`REQ-20260922T2015Z-b4d7`), which is what a
   * second public path to the same capability gets you: it works for whoever found it.
   *
   * Port 0 only - a keyboard is not per-port, and folding it into every port would report the
   * same keypress four times. It costs one `sceKeyboardReadState` per poll, and nothing at all
   * when no keyboard library resolved, which `oops_keyboard_poll_buttons` checks first.
   *
   * An application that still calls `oops_keyboard_poll_buttons` itself is not broken by this -
   * the bits are the same and OR is idempotent - but the call is now redundant.
   */
  const uint32_t kbd =
      (port == 0u && s_keyboard_as_pad) ? oops_keyboard_poll_buttons() : 0u;

  /* Lazy-open port if uninitialized but requested */
  if (s_pad_handles[port] < 0 && (scePadOpen || scePadGetHandle)) {
    if (s_pad_retry_cooldown[port] > 0) {
      s_pad_retry_cooldown[port]--;
      return input_keyboard_only(out_state, kbd);
    }
    try_resolve_user_id();
    if (s_user_id >= 0) {
      if (scePadGetHandle) {
        s_pad_handles[port] = scePadGetHandle(s_user_id, 0, (int)port);
      }
      if (s_pad_handles[port] < 0 && scePadOpen) {
        s_pad_handles[port] = scePadOpen(s_user_id, 0, (int)port, NULL);
        if (s_pad_handles[port] < 0 && s_user_id != 0x10000000) {
          s_pad_handles[port] = scePadOpen(0x10000000, 0, (int)port, NULL);
        }
      }
    }
    if (s_pad_handles[port] < 0) {
      s_pad_retry_cooldown[port] = 120; /* retry at most once every 120 frames (~2 seconds) */
      return input_keyboard_only(out_state, kbd);
    }
    oops_log_debug("INPUT", "lazy open port %u succeeded, handle=%d", port, s_pad_handles[port]);
  }

  int handle = s_pad_handles[port];
  if (handle < 0 || (!scePadReadState && !scePadRead)) {
    static int s_unavail_tick = 0;
    if ((s_unavail_tick++ % 300) == 0) {
      oops_log_debug("INPUT", "poll: port %u unavailable (handle=%d)", port, handle);
    }
    return input_keyboard_only(out_state, kbd);
  }

  ScePadDataInternal raw;
  for (size_t i = 0; i < sizeof(raw); i++)
    ((unsigned char *)&raw)[i] = 0;

  int rc = -1;
  if (scePadReadState) {
    rc = scePadReadState(handle, &raw);
  }
  if (rc != 0 && scePadRead) {
    rc = (scePadRead(handle, &raw, 1) > 0) ? 0 : -1;
  }
  if (rc != 0) {
    static int s_err_tick = 0;
    if ((s_err_tick++ % 180) == 0) {
      oops_log_debug("INPUT", "poll: port %u read failed rc=0x%x (%d)", port, (unsigned int)rc, rc);
      if (sceUserServiceGetForegroundUser) {
        int32_t fg_user = -1;
        if (sceUserServiceGetForegroundUser(&fg_user) == 0 && fg_user >= 0 && fg_user != s_user_id) {
          s_user_id = fg_user;
          if (scePadGetHandle) {
            s_pad_handles[port] = scePadGetHandle(s_user_id, 0, (int)port);
          }
        }
      }
    }
    return input_keyboard_only(out_state, kbd);
  }

  oops_input_map_record(out_state, &raw);
  out_state->buttons |= kbd;

  /* Verbose telemetry when buttons change, when non-zero, or periodically */
  static uint32_t s_last_polled_buttons[OOPS_MAX_PADS] = {0};
  static int s_poll_ticks[OOPS_MAX_PADS] = {0};
  s_poll_ticks[port]++;
  if (out_state->buttons != s_last_polled_buttons[port] ||
      (out_state->buttons != 0 && (s_poll_ticks[port] % 30) == 0) ||
      (s_poll_ticks[port] % 300) == 0) {
    oops_log_debug("INPUT", "port %u: conn=%d raw_conn=%d btn=0x%04x sticks=(%d,%d) rc=%d",
                   port, out_state->connected, (int)raw.connected,
                   (unsigned int)out_state->buttons, (int)out_state->left_stick_x, (int)out_state->left_stick_y, rc);
    s_last_polled_buttons[port] = out_state->buttons;
  }
  return 0;
}

int oops_input_poll_batch(unsigned int port, oops_pad_state_t *out_states,
                          unsigned int max_samples) {
  if (!out_states || port >= OOPS_MAX_PADS || max_samples == 0) {
    return -1;
  }
  if (max_samples > OOPS_MAX_PAD_SAMPLES) {
    max_samples = OOPS_MAX_PAD_SAMPLES;
  }

  /* The stride is the driver's 120-byte record, held to that size in
   * pad_layout.h. */
  /* Lazy-open port if uninitialized but requested */
  if (s_pad_handles[port] < 0 && scePadOpen) {
    if (s_pad_retry_cooldown[port] > 0) {
      s_pad_retry_cooldown[port]--;
      return -1;
    }
    try_resolve_user_id();
    if (s_user_id >= 0) {
      s_pad_handles[port] = scePadOpen(s_user_id, 0, (int)port, NULL);
      if (s_pad_handles[port] < 0 && s_user_id != 0x10000000) {
        s_pad_handles[port] = scePadOpen(0x10000000, 0, (int)port, NULL);
      }
    }
    if (s_pad_handles[port] < 0) {
      s_pad_retry_cooldown[port] = 120;
      return -1;
    }
  }
  int handle = s_pad_handles[port];
  if (handle < 0 || !scePadRead) {
    return -1;
  }

  /* One driver request for up to max_samples records of the same layout the
   * single-state read returns. The buffer is bounded by OOPS_MAX_PAD_SAMPLES,
   * so the stack cost is fixed. */
  ScePadDataInternal raw[OOPS_MAX_PAD_SAMPLES];
  for (size_t i = 0; i < sizeof(raw); i++)
    ((unsigned char *)raw)[i] = 0;

  int count = scePadRead(handle, raw, (int)max_samples);
  if (count < 0) {
    return -1;
  }
  if ((unsigned int)count > max_samples) {
    count = (int)max_samples;
  }

  for (int i = 0; i < count; i++) {
    for (size_t b = 0; b < sizeof(out_states[i]); b++) {
      ((unsigned char *)&out_states[i])[b] = 0;
    }
    oops_input_map_record(&out_states[i], &raw[i]);
  }
  return count;
}

int oops_input_set_rumble(unsigned int port, uint8_t small_motor,
                          uint8_t large_motor) {
  if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0 || !scePadSetVibration) {
    return -1;
  }
  struct {
    uint8_t largeMotor;
    uint8_t smallMotor;
    uint8_t reserved[6];
  } vib = {large_motor, small_motor, {0}};
  return scePadSetVibration(s_pad_handles[port], &vib);
}

int oops_input_set_lightbar(unsigned int port, uint8_t r, uint8_t g,
                            uint8_t b) {
  if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0 || !scePadSetLightBar) {
    return -1;
  }
  struct {
    uint8_t r, g, b, a;
  } col = {r, g, b, 255};
  return scePadSetLightBar(s_pad_handles[port], &col);
}

int oops_input_reset_orientation(unsigned int port) {
  if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0 ||
      !scePadResetOrientation) {
    return -1;
  }
  return scePadResetOrientation(s_pad_handles[port]);
}

int oops_input_adaptive_triggers_available(unsigned int port) {
  if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0) {
    return 0;
  }
  /* Real detection: the effect entry point resolved here. Whether the pad on
   * this port is a DualSense is not asked - the controller-information call
   * that would say so returned an error and wrote nothing in the same capture.
   */
  return scePadSetTriggerEffect ? 1 : 0;
}

int oops_input_set_trigger_effect(unsigned int port, unsigned int triggers,
                                  int mode, uint8_t position,
                                  uint8_t position_end, uint8_t strength,
                                  uint8_t frequency) {
  (void)position;
  (void)position_end;
  (void)strength;
  (void)frequency;
  if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0) {
    return -1;
  }
  if ((triggers & (OOPS_TRIGGER_L2 | OOPS_TRIGGER_R2)) == 0) {
    return -1;
  }
  if (mode < OOPS_TRIGGER_OFF || mode > OOPS_TRIGGER_VIBRATION) {
    return -1;
  }
  /* Capture-gated: the entry point is confirmed (see its declaration) but its
   * parameter layout is not. Passing a guessed struct corrupts state rather
   * than failing, so this refuses. A write-extent capture of the parameter is
   * what completes it. */
  return -1;
}

void oops_input_close(void) {
  for (unsigned int i = 0; i < OOPS_MAX_PADS; i++) {
    if (s_pad_handles[i] >= 0 && scePadClose) {
      scePadClose(s_pad_handles[i]);
      s_pad_handles[i] = -1;
    }
    s_pad_retry_cooldown[i] = 0;
  }
  s_initialized = 0;
  s_init_rc = -1;
}
