/*
 * Controller input: per-port pad state, batched low-latency reads, rumble, light bar,
 * orientation and adaptive triggers, with a keyboard folded into port 0.
 */
#ifndef OOPS_INPUT_H
#define OOPS_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controller button bitmasks, matching the ScePad layout (bit 16 excepted, see its
 * note). */
#define OOPS_BUTTON_L3 (1u << 1)
#define OOPS_BUTTON_R3 (1u << 2)
#define OOPS_BUTTON_OPTIONS (1u << 3)
#define OOPS_BUTTON_UP (1u << 4)
#define OOPS_BUTTON_RIGHT (1u << 5)
#define OOPS_BUTTON_DOWN (1u << 6)
#define OOPS_BUTTON_LEFT (1u << 7)
#define OOPS_BUTTON_L2 (1u << 8)
#define OOPS_BUTTON_R2 (1u << 9)
#define OOPS_BUTTON_L1 (1u << 10)
#define OOPS_BUTTON_R1 (1u << 11)
#define OOPS_BUTTON_TRIANGLE (1u << 12)
#define OOPS_BUTTON_CIRCLE (1u << 13)
#define OOPS_BUTTON_CROSS (1u << 14)
#define OOPS_BUTTON_SQUARE (1u << 15)
/*
 * Bit 16 is not in the public ScePad button layout, and which physical button it
 * carries is unsettled: Create in one reading, Home in Prosperous's
 * (`pros-link/src/pad.rs`). `OOPS_BUTTON_BIT16` names the bit rather than a button
 * because the bit is what is known; the aliases below carry the same caveat. Binding
 * it is allowed (SeaShell binds it to the Control Centre overlay), but it is a bet the
 * name makes visible.
 */
#define OOPS_BUTTON_BIT16 (1u << 16)
/* Prospero Create / Orbis Share - one reading of OOPS_BUTTON_BIT16, not confirmed. */
#define OOPS_BUTTON_CREATE OOPS_BUTTON_BIT16
/* PlayStation / Home - the other reading of OOPS_BUTTON_BIT16, not confirmed. */
#define OOPS_BUTTON_PS OOPS_BUTTON_BIT16
#define OOPS_BUTTON_HOME OOPS_BUTTON_BIT16 /* Alias for OOPS_BUTTON_PS. */
#define OOPS_BUTTON_TOUCHPAD (1u << 20)

#define OOPS_MAX_PADS 4

/* The platform's batched read returns up to this many samples in one driver
 * request - the low-latency path that preserves short button transitions a
 * per-frame poll would miss. */
#define OOPS_MAX_PAD_SAMPLES 64

typedef struct oops_touch_point {
    uint16_t x;     /* 0 to 1919 */
    uint16_t y;     /* 0 to 941 */
    uint8_t id;     /* Finger tracking ID */
    uint8_t active; /* 1 if touched, 0 otherwise */
} oops_touch_point_t;

typedef struct oops_pad_state {
    uint32_t buttons;
    int8_t left_stick_x;  /* -128 to 127 */
    int8_t left_stick_y;  /* -128 to 127 */
    int8_t right_stick_x; /* -128 to 127 */
    int8_t right_stick_y; /* -128 to 127 */
    uint8_t l2_trigger;   /* 0 to 255 */
    uint8_t r2_trigger;   /* 0 to 255 */
    int connected;
    oops_touch_point_t touch[2];
    /* Motion sensors (DualShock 4 and DualSense) */
    float orientation[4];      /* Quaternion [x, y, z, w] */
    float acceleration[3];     /* Accelerometer [x, y, z] in G's */
    float angular_velocity[3]; /* Gyroscope [x, y, z] in rad/s */
} oops_pad_state_t;

/*
 * Adaptive-trigger effect modes (DualSense L2/R2). OOPS's own values; the
 * mapping to the platform's trigger-effect parameter belongs to
 * `oops_input_set_trigger_effect`, which is gated on a capture of that parameter.
 */
enum {
    OOPS_TRIGGER_OFF = 0,      /* release any effect - the resistance-free default */
    OOPS_TRIGGER_FEEDBACK = 1, /* constant resistance from `position`, at `strength` */
    OOPS_TRIGGER_WEAPON =
        2, /* a resistance wall between `position` and `position_end` */
    OOPS_TRIGGER_VIBRATION =
        3, /* vibration from `position`, at `strength` and `frequency` */
};

/* Which trigger an effect targets. */
enum {
    OOPS_TRIGGER_L2 = 1u << 0,
    OOPS_TRIGGER_R2 = 1u << 1,
};

/*
 * Open pad 0 for the initial user. Returns 0 on success. A failure is
 * remembered and reported again by later calls until oops_input_close().
 */
int oops_input_init(void);
/*
 * The current state of `port`, opening it on first use. Returns 0 when a pad was read
 * or (port 0) keyboard-as-pad produced buttons, -1 otherwise; `out_state` is zeroed
 * first either way.
 */
int oops_input_poll(unsigned int port, oops_pad_state_t *out_state);

/*
 * Batched low-latency read: fills up to `max_samples` (cap
 * OOPS_MAX_PAD_SAMPLES) states from the driver in one request, oldest first,
 * and returns the count. Processing every returned record is what preserves a
 * press-and-release that falls between two per-frame polls; the last record is
 * the current state. Reuses the same pad-state layout as oops_input_poll.
 *
 * The stride is the driver's record: 120 bytes on 12.40, measured by obSCEne's
 * write-extent probes of both the single and the batched read; the batched read
 * returns one record when nothing is attached. Returns the driver's count, or -1
 * when the port is not open or the read did not resolve.
 */
int oops_input_poll_batch(unsigned int port, oops_pad_state_t *out_states,
                          unsigned int max_samples);

/*
 * Whether `oops_input_poll` folds a keyboard's keys in as pad buttons on port 0.
 * Default on.
 *
 * Keyboard-as-pad is a fallback for an application that only understands a pad:
 * arrows and WASD become the D-pad, Enter becomes Cross, Escape becomes Circle, and a
 * console with no controller is still usable.
 *
 * An application that reads real characters turns it off, because otherwise the same
 * key arrives twice with two meanings: `a` in a GLUT program is both the letter and
 * `GLUT_KEY_LEFT`. oops-sdk's GLUT calls this with 0 from `glutKeyboardFunc`; nothing
 * else in the SDK changes it.
 */
void oops_input_set_keyboard_as_pad(int enable);

/* Rumble motor speeds, 0..255. Returns the platform's result, or -1 when the port is
 * not open or the entry point did not resolve; the same holds for the two below. */
int oops_input_set_rumble(unsigned int port, uint8_t small_motor, uint8_t large_motor);
/* Light bar colour. */
int oops_input_set_lightbar(unsigned int port, uint8_t r, uint8_t g, uint8_t b);
/* Resets the orientation quaternion's reference to the pad's current pose. */
int oops_input_reset_orientation(unsigned int port);

/*
 * Whether the adaptive-trigger effect entry point resolves and this port is
 * open. The entry point is present in the app context on 12.40 and absent in the
 * eboot and payload contexts; whether the pad is a DualSense is not checked.
 * Returns 1/0.
 */
int oops_input_adaptive_triggers_available(unsigned int port);

/*
 * Apply an adaptive-trigger effect to L2 and/or R2 (`triggers` is a mask of
 * OOPS_TRIGGER_L2/ R2). `mode` is one of OOPS_TRIGGER_*;
 * `position`/`position_end` are 0..255 along the pull, `strength` 0..255,
 * `frequency` 0..255 (used by VIBRATION).
 *
 * The entry point is confirmed but its parameter layout is not, so this returns a
 * negative code rather than pass a guessed struct, until a write-extent capture of
 * the parameter confirms the layout.
 */
int oops_input_set_trigger_effect(unsigned int port, unsigned int triggers, int mode,
                                  uint8_t position, uint8_t position_end,
                                  uint8_t strength, uint8_t frequency);

/* Closes every open pad and forgets the init result. */
void oops_input_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_INPUT_H */
