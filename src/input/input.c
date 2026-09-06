#include "oops/input.h"
#include <stddef.h>

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
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

/* Hardware ScePadTouch and ScePadData layout */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  finger;
    uint8_t  pad[3];
} ScePadTouch;

typedef struct {
    uint8_t     fingers;
    uint8_t     pad1[3];
    uint32_t    pad2;
    ScePadTouch touch[2];
} ScePadTouchData;

typedef struct {
    uint32_t buttons;                              /* offset  0 */
    struct { uint8_t x; uint8_t y; } leftStick;    /* offset  4 */
    struct { uint8_t x; uint8_t y; } rightStick;   /* offset  6 */
    struct { uint8_t l2; uint8_t r2; } analogButtons; /* offset  8 */
    uint16_t    padding;                           /* offset 10 */
    struct { float x, y, z, w; } quat;            /* offset 12 (orientation) */
    struct { float x, y, z; }    accel;           /* offset 28 (acceleration) */
    struct { float x, y, z; }    vel;             /* offset 40 (angular velocity) */
    ScePadTouchData touchData;                     /* offset 52 */
    uint8_t     connected;                         /* offset 76 */
    uint8_t     _align[3];                         /* offset 77 */
    uint64_t    timestamp;                         /* offset 80 */
    uint8_t     reserved[64];                      /* oversize for safety */
} ScePadDataInternal;

static int s_pad_handles[OOPS_MAX_PADS] = { -1, -1, -1, -1 };
static int32_t s_user_id = -1;
static int s_initialized = 0;

/* Map one raw platform record into the SDK's pad-state shape. Shared by the single-state
 * poll and the batched read, so the layout lives in exactly one place. Does not clear
 * out_state - every field it reports is written here, and callers zero first. */
static void oops_fill_pad_state(oops_pad_state_t *out_state, const ScePadDataInternal *raw) {
    out_state->buttons = raw->buttons;
    out_state->left_stick_x  = (int8_t)((int)raw->leftStick.x - 128);
    out_state->left_stick_y  = (int8_t)((int)raw->leftStick.y - 128);
    out_state->right_stick_x = (int8_t)((int)raw->rightStick.x - 128);
    out_state->right_stick_y = (int8_t)((int)raw->rightStick.y - 128);
    out_state->l2_trigger    = raw->analogButtons.l2;
    out_state->r2_trigger    = raw->analogButtons.r2;

    /* Connection heuristic: explicit flag, non-zero buttons, or sticks active */
    out_state->connected = raw->connected ? 1 :
        (raw->buttons != 0 || raw->leftStick.x != 0 || raw->leftStick.y != 0) ? 1 : 0;

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
    if (s_initialized) return 0;

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

    s_initialized = 1;
    return (s_pad_handles[0] >= 0) ? 0 : -1;
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

    oops_fill_pad_state(out_state, &raw);
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
        oops_fill_pad_state(&out_states[i], &raw[i]);
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
    /* Honestly 0 until the capture below lands: which libScePad entry point drives the
     * DualSense adaptive triggers on this firmware is not yet confirmed, so there is no symbol
     * to resolve against and no way to claim the capability without guessing. When obSCEne
     * confirms the entry point, this resolves it (as videodec/audiodec do) and the check
     * becomes real. */
    return 0;
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
    /* Capture-gated: the effect entry point and its parameter layout are unconfirmed. Passing
     * a guessed output-report struct corrupts state rather than failing, so this refuses.
     * Completing it needs the obSCEne adaptive-trigger capture. */
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
}
