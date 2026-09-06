#ifndef OOPS_KEYBOARD_H
#define OOPS_KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB keyboard input (libSceKeyboard).
 *
 * A homebrew UI beyond a gamepad needs real key events. This binds the keyboard device and
 * exposes a timestamped key-transition queue in OOPS's own shape.
 *
 * # State of this subsystem, honestly
 *
 * Unlike the pad, libSceKeyboard has not yet been measured on hardware - obSCEne's 100-input
 * covers scePad only. So two things await an obSCEne probe: whether the library and its entry
 * points resolve at all (a presence census, as 107/108 did for decode), and the layout of the
 * platform's key record (documented at 96 bytes, unconfirmed here). Capability detection
 * below is real *once the symbol names are confirmed*; the read path is capture-gated and
 * returns 0 events rather than parse a record whose layout is a guess.
 */

/* Key transition kind. */
enum {
    OOPS_KEY_UP   = 0,
    OOPS_KEY_DOWN = 1,
};

/* Modifier bits, OOPS's own values. */
enum {
    OOPS_KMOD_CTRL  = 1u << 0,
    OOPS_KMOD_SHIFT = 1u << 1,
    OOPS_KMOD_ALT   = 1u << 2,
    OOPS_KMOD_GUI   = 1u << 3,
};

/* A single key transition in OOPS's shape - not the platform record. */
typedef struct oops_key_event {
    uint16_t usage;      /* USB HID usage code */
    uint8_t  transition; /* OOPS_KEY_UP / OOPS_KEY_DOWN */
    uint8_t  modifiers;  /* OOPS_KMOD_* mask */
    uint64_t timestamp;  /* platform sample time, 0 if unavailable */
} oops_key_event_t;

#define OOPS_MAX_KEY_EVENTS 32

/* Open the keyboard for the signed-in user. Returns 0 on success, negative on failure. */
int oops_keyboard_init(void);

/* Whether keyboard input is reachable: the library and its entry points resolve. 1/0. */
int oops_keyboard_available(void);

/*
 * Drain up to `max_events` (cap OOPS_MAX_KEY_EVENTS) key transitions, oldest first, and return
 * the count.
 *
 * NOTE: capture-gated. The platform key-record layout is unconfirmed, so this returns 0 until
 * the obSCEne capture lands rather than parse a guessed record.
 */
int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events);

/* Close the keyboard. */
void oops_keyboard_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_KEYBOARD_H */
