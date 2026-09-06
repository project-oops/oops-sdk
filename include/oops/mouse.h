#ifndef OOPS_MOUSE_H
#define OOPS_MOUSE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB mouse input (libSceMouse).
 *
 * Relative motion, buttons, wheel and tilt in OOPS's own shape.
 *
 * # State of this subsystem, honestly
 *
 * As with the keyboard, libSceMouse has not been measured on hardware yet (100-input covers
 * scePad only). The presence census and the platform mouse-record layout (documented at 40
 * bytes) both await an obSCEne probe. Capability detection is real once the symbol names are
 * confirmed; the read path is capture-gated and returns 0 records rather than parse a guess.
 */

/* Mouse button bits, OOPS's own values. */
enum {
    OOPS_MOUSE_LEFT   = 1u << 0,
    OOPS_MOUSE_RIGHT  = 1u << 1,
    OOPS_MOUSE_MIDDLE = 1u << 2,
};

/* One mouse sample in OOPS's shape - not the platform record. Motion is relative. */
typedef struct oops_mouse_state {
    int32_t  dx;         /* relative X since last sample */
    int32_t  dy;         /* relative Y since last sample */
    int32_t  wheel;      /* vertical wheel delta */
    int32_t  tilt;       /* horizontal tilt delta */
    uint8_t  buttons;    /* OOPS_MOUSE_* mask */
    uint8_t  connected;  /* 1 if a mouse is present */
    uint64_t timestamp;  /* platform sample time, 0 if unavailable */
} oops_mouse_state_t;

#define OOPS_MAX_MOUSE_SAMPLES 32

/* Open the mouse for the signed-in user. Returns 0 on success, negative on failure. */
int oops_mouse_init(void);

/* Whether mouse input is reachable: the library and its entry points resolve. 1/0. */
int oops_mouse_available(void);

/*
 * Drain up to `max_samples` (cap OOPS_MAX_MOUSE_SAMPLES) mouse samples, oldest first, and
 * return the count.
 *
 * NOTE: capture-gated. The platform mouse-record layout is unconfirmed, so this returns 0
 * until the obSCEne capture lands.
 */
int oops_mouse_read(oops_mouse_state_t *out_samples, unsigned int max_samples);

/* Close the mouse. */
void oops_mouse_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_MOUSE_H */
