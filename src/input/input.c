#include "oops/input.h"
#include <stddef.h>
#include "pad_layout.h"

/* Platform symbols from libScePad and libSceUserService */
__attribute__((weak)) int scePadInit(void);
__attribute__((weak)) int scePadOpen(int userId, int type, int index, const void *param);
__attribute__((weak)) int scePadClose(int handle);
__attribute__((weak)) int scePadReadState(int handle, void *state);
__attribute__((weak)) int scePadRead(int handle, void *data, int num);
__attribute__((weak)) int scePadSetVibration(int handle, const void *param);
__attribute__((weak)) int scePadSetLightBar(int handle, const void *param);
__attribute__((weak)) int scePadResetLightBar(int handle);
__attribute__((weak)) int scePadResetOrientation(int handle);
/* Present in the app context on 12.40 (obSCEne 100-input/dualsense-symbols resolved all seven
 * DualSense entry points there) and absent in the eboot and payload contexts, so whether it
 * resolves is the availability check. Its parameter layout is still unconfirmed. */
__attribute__((weak)) int scePadSetTriggerEffect(int handle, const void *param);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

static int s_pad_handles[OOPS_MAX_PADS] = { -1, -1, -1, -1 };
static int32_t s_user_id = -1;
static int s_initialized = 0;
static int s_init_rc = -1;  /* what the first init reported; repeated until close */

/* Declared in pad_layout.h; shared by the single-state poll and the batched read. */
void oops_input_map_record(oops_pad_state_t *out_state, const ScePadDataInternal *raw) {
    out_state->buttons = raw->buttons;
    out_state->left_stick_x  = (int8_t)((int)raw->leftStick.x - 128);
    out_state->left_stick_y  = (int8_t)((int)raw->leftStick.y - 128);
    out_state->right_stick_x = (int8_t)((int)raw->rightStick.x - 128);
    out_state->right_stick_y = (int8_t)((int)raw->rightStick.y - 128);
    out_state->l2_trigger    = raw->analogButtons.l2;
    out_state->r2_trigger    = raw->analogButtons.r2;

    /* The driver's flag, alone. The old fallback took a non-zero stick byte as a sign of life,
     * but a centred stick reads 128, so it reported a pad on every successful read: obSCEne
     * 100-input/oops-sdk-poll on 12.40 showed connected=1 with nothing attached. */
    out_state->connected = raw->connected ? 1 : 0;

    /* Touchpad touch points */
    for (int t = 0; t < 2; t++) {
        out_state->touch[t].x      = raw->touchData.touch[t].x;
        out_state->touch[t].y      = raw->touchData.touch[t].y;
        out_state->touch[t].id     = raw->touchData.touch[t].finger;
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
    if (s_initialized) return s_init_rc;

    if (scePadInit) {
        scePadInit();
    }

    if (sceUserServiceGetInitialUser) {
        if (sceUserServiceGetInitialUser(&s_user_id) != 0 && sceUserServiceInitialize) {
            sceUserServiceInitialize(NULL);
            (void)sceUserServiceGetInitialUser(&s_user_id);
        }
    }

    if (s_user_id >= 0 && scePadOpen) {
        s_pad_handles[0] = scePadOpen(s_user_id, 0, 0, NULL);
    }

    /* A failed init stays failed: later calls report this result, not a success because a
     * flag was set on the way out. */
    s_initialized = 1;
    s_init_rc = (s_pad_handles[0] >= 0) ? 0 : -1;
    return s_init_rc;
}

int oops_input_poll(unsigned int port, oops_pad_state_t *out_state) {
    if (!out_state || port >= OOPS_MAX_PADS) return -1;

    for (size_t i = 0; i < sizeof(*out_state); i++) {
        ((unsigned char *)out_state)[i] = 0;
    }

    /* Lazy-open port if uninitialized but requested */
    if (s_pad_handles[port] < 0 && s_user_id >= 0 && scePadOpen) {
        s_pad_handles[port] = scePadOpen(s_user_id, 0, (int)port, NULL);
    }

    int handle = s_pad_handles[port];
    if (handle < 0 || !scePadReadState) {
        return -1;
    }

    ScePadDataInternal raw;
    for (size_t i = 0; i < sizeof(raw); i++) ((unsigned char *)&raw)[i] = 0;

    int rc = scePadReadState(handle, &raw);
    if (rc != 0) {
        return -1;
    }

    oops_input_map_record(out_state, &raw);
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

    /* The stride is the driver's 120-byte record, held to that size in pad_layout.h. */
    /* Lazy-open port if uninitialized but requested */
    if (s_pad_handles[port] < 0 && s_user_id >= 0 && scePadOpen) {
        s_pad_handles[port] = scePadOpen(s_user_id, 0, (int)port, NULL);
    }
    int handle = s_pad_handles[port];
    if (handle < 0 || !scePadRead) {
        return -1;
    }

    /* One driver request for up to max_samples records of the same layout the single-state
     * read returns. The buffer is bounded by OOPS_MAX_PAD_SAMPLES, so the stack cost is fixed. */
    ScePadDataInternal raw[OOPS_MAX_PAD_SAMPLES];
    for (size_t i = 0; i < sizeof(raw); i++) ((unsigned char *)raw)[i] = 0;

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

int oops_input_set_rumble(unsigned int port, uint8_t small_motor, uint8_t large_motor) {
    if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0 || !scePadSetVibration) {
        return -1;
    }
    struct {
        uint8_t largeMotor;
        uint8_t smallMotor;
        uint8_t reserved[6];
    } vib = { large_motor, small_motor, {0} };
    return scePadSetVibration(s_pad_handles[port], &vib);
}

int oops_input_set_lightbar(unsigned int port, uint8_t r, uint8_t g, uint8_t b) {
    if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0 || !scePadSetLightBar) {
        return -1;
    }
    struct {
        uint8_t r, g, b, a;
    } col = { r, g, b, 255 };
    return scePadSetLightBar(s_pad_handles[port], &col);
}

int oops_input_reset_orientation(unsigned int port) {
    if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0 || !scePadResetOrientation) {
        return -1;
    }
    return scePadResetOrientation(s_pad_handles[port]);
}

int oops_input_adaptive_triggers_available(unsigned int port) {
    if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0) {
        return 0;
    }
    /* Real detection: the effect entry point resolved here. Whether the pad on this port is a
     * DualSense is not asked - the controller-information call that would say so returned an
     * error and wrote nothing in the same capture. */
    return scePadSetTriggerEffect ? 1 : 0;
}

int oops_input_set_trigger_effect(unsigned int port, unsigned int triggers, int mode,
                                  uint8_t position, uint8_t position_end, uint8_t strength,
                                  uint8_t frequency) {
    (void)position; (void)position_end; (void)strength; (void)frequency;
    if (port >= OOPS_MAX_PADS || s_pad_handles[port] < 0) {
        return -1;
    }
    if ((triggers & (OOPS_TRIGGER_L2 | OOPS_TRIGGER_R2)) == 0) {
        return -1;
    }
    if (mode < OOPS_TRIGGER_OFF || mode > OOPS_TRIGGER_VIBRATION) {
        return -1;
    }
    /* Capture-gated: the entry point is confirmed (see its declaration) but its parameter
     * layout is not. Passing a guessed struct corrupts state rather than failing, so this
     * refuses. A write-extent capture of the parameter is what completes it. */
    return -1;
}

void oops_input_close(void) {
    for (unsigned int i = 0; i < OOPS_MAX_PADS; i++) {
        if (s_pad_handles[i] >= 0 && scePadClose) {
            scePadClose(s_pad_handles[i]);
            s_pad_handles[i] = -1;
        }
    }
    s_initialized = 0;
    s_init_rc = -1;
}
