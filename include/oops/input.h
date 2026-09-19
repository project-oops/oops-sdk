#ifndef OOPS_INPUT_H
#define OOPS_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard controller button bitmasks (matching ScePad layout; CREATE excepted,
 * see its note) */
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
 * Capture-gated and contested. Bit 16 is not in the public ScePad button
 * layout. This SDK holds it unconfirmed; Prosperous (pros-link/src/pad.rs) maps
 * the same bit to Home and its enum claims the bit was confirmed empirically on
 * a target. obSCEne has not settled it - its button-bits probe needs a
 * controller attached and resolved not-possible on the test rig - so which of
 * Create / Home bit 16 carries is genuinely open. Do not bind it as a shell or
 * system button until an obSCEne button-bits sweep with a controller confirms
 * it.
 */
#define OOPS_BUTTON_CREATE                                                     \
  (1u << 16) /* Prospero Create / Orbis Share; bit contested with Prosperous's \
                Home */
#define OOPS_BUTTON_PS (1u << 16)   /* PlayStation / Home button (when pad privilege is enabled) */
#define OOPS_BUTTON_HOME (1u << 16) /* Alias for OOPS_BUTTON_PS */
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
  /* 6-Axis Motion & Orientation IMU Telemetry (DualShock 4 & DualSense) */
  float orientation[4];      /* Quaternion [x, y, z, w] */
  float acceleration[3];     /* Accelerometer [x, y, z] in G's */
  float angular_velocity[3]; /* Gyroscope [x, y, z] in rad/s */
} oops_pad_state_t;

/*
 * Adaptive-trigger effect modes (DualSense L2/R2). OOPS's own values; the
 * mapping to the platform's trigger-effect parameter lives in the effect call,
 * which is capture-gated.
 */
enum {
  OOPS_TRIGGER_OFF = 0, /* release any effect - the resistance-free default */
  OOPS_TRIGGER_FEEDBACK =
      1, /* constant resistance from `position`, at `strength` */
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
 * remembered and reported again by later calls until oops_input_close(); it
 * does not turn into success because the first call ran.
 */
int oops_input_init(void);
int oops_input_poll(unsigned int port, oops_pad_state_t *out_state);

/*
 * Batched low-latency read: fills up to `max_samples` (cap
 * OOPS_MAX_PAD_SAMPLES) states from the driver in one request, oldest first,
 * and returns the count. Processing every returned record is what preserves a
 * press-and-release that falls between two per-frame polls; the last record is
 * the current state. Reuses the same pad-state layout as oops_input_poll.
 *
 * The stride is the driver's record: 120 bytes on 12.40, measured by obSCEne's
 * write-extent probes of both the single and the batched read (sweep
 * 20260909-110725), which also showed the batched read returning one record
 * when nothing is attached. Returns the driver's count, or -1 when the port is
 * not open or the read did not resolve.
 */
int oops_input_poll_batch(unsigned int port, oops_pad_state_t *out_states,
                          unsigned int max_samples);

int oops_input_set_rumble(unsigned int port, uint8_t small_motor,
                          uint8_t large_motor);
int oops_input_set_lightbar(unsigned int port, uint8_t r, uint8_t g, uint8_t b);
int oops_input_reset_orientation(unsigned int port);

/*
 * Whether the adaptive-trigger effect entry point resolves and this port is
 * open. Real detection - the entry point is confirmed present in the app
 * context on 12.40 and absent in the eboot and payload contexts - but it does
 * not check that the pad is a DualSense. Returns 1/0.
 */
int oops_input_adaptive_triggers_available(unsigned int port);

/*
 * Apply an adaptive-trigger effect to L2 and/or R2 (`triggers` is a mask of
 * OOPS_TRIGGER_L2/ R2). `mode` is one of OOPS_TRIGGER_*;
 * `position`/`position_end` are 0..255 along the pull, `strength` 0..255,
 * `frequency` 0..255 (used by VIBRATION).
 *
 * NOTE: capture-gated. The entry point is confirmed; its parameter layout is
 * not, so this returns a negative code rather than pass a guessed struct. A
 * write-extent capture of the parameter is what completes it.
 */
int oops_input_set_trigger_effect(unsigned int port, unsigned int triggers,
                                  int mode, uint8_t position,
                                  uint8_t position_end, uint8_t strength,
                                  uint8_t frequency);

void oops_input_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_INPUT_H */
