#ifndef OOPS_MOUSE_H
#define OOPS_MOUSE_H

#include <stddef.h>
#include <stdint.h>

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
 * obSCEne's 101-input-ext measured it on 12.40. The library and its four entry
 * points resolve in the app context. In the payload context the mouse symbols
 * in libkernel are unlinked stubs (obSCEne 101-input-ext, sweep
 * 20260909-090807), and in the eboot context nothing resolves. So availability
 * below is real detection, and a payload should expect it to say no. The record
 * (documented at 40 bytes) is still unconfirmed: with no mouse attached the
 * read wrote nothing, so its extent was not measured. The read path is
 * capture-gated and refuses with OOPS_MOUSE_ELAYOUT rather than parse a guess.
 * A distinct code, not zero samples, so a caller can tell "no motion" from "no
 * reader".
 */

enum {
  OOPS_MOUSE_OK = 0,
  OOPS_MOUSE_EUNAVAIL = -1, /* the library, its entry points or a signed-in user
                               did not resolve */
  OOPS_MOUSE_ELAYOUT =
      -2, /* the read is real but the platform record layout is unconfirmed */
  OOPS_MOUSE_EPARAM =
      -3, /* a caller argument was rejected before any platform call */
};

/* Mouse button bits, OOPS's own values. */
enum {
  OOPS_MOUSE_LEFT = 1u << 0,
  OOPS_MOUSE_RIGHT = 1u << 1,
  OOPS_MOUSE_MIDDLE = 1u << 2,
};

/* One mouse sample in OOPS's shape - not the platform record. Motion is
 * relative. */
typedef struct oops_mouse_state {
  int32_t dx;         /* relative X since last sample */
  int32_t dy;         /* relative Y since last sample */
  int32_t wheel;      /* vertical wheel delta */
  int32_t tilt;       /* horizontal tilt delta */
  uint8_t buttons;    /* OOPS_MOUSE_* mask */
  uint8_t connected;  /* 1 if a mouse is present */
  uint64_t timestamp; /* platform sample time, 0 if unavailable */
} oops_mouse_state_t;

#define OOPS_MAX_MOUSE_SAMPLES 32

/*
 * Open the mouse for the signed-in user. 0 on success, OOPS_MOUSE_EUNAVAIL when
 * the library or the user is absent, otherwise the platform's own negative code
 * from the open. The result is remembered and repeated by later calls until
 * oops_mouse_close().
 */
int oops_mouse_init(void);

/* Whether mouse input is reachable: the library and its entry points resolve.
 * 1/0. */
int oops_mouse_available(void);

/*
 * Drain up to `max_samples` (cap OOPS_MAX_MOUSE_SAMPLES) mouse samples, oldest
 * first, and return the count. Negative is one of the codes above.
 *
 * NOTE: capture-gated. The platform mouse-record layout is unconfirmed, so this
 * returns OOPS_MOUSE_ELAYOUT until the obSCEne capture lands, never a
 * fabricated count.
 */
int oops_mouse_read(oops_mouse_state_t *out_samples, unsigned int max_samples);

/* Close the mouse. */
void oops_mouse_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_MOUSE_H */
