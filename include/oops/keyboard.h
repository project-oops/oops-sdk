#ifndef OOPS_KEYBOARD_H
#define OOPS_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB keyboard input (libSceKeyboard): key-transition events in OOPS's own shape, and
 * a mapping of keys to pad buttons.
 *
 * The library and its entry points resolve in the app context on 12.40 (obSCEne probe
 * 101-input-ext); in the eboot and payload contexts nothing in it resolves, so a
 * payload cannot reach the keyboard, and `oops_keyboard_available` says so. A read
 * writes a 96-byte record. Its key, connection and interception fields are confirmed
 * by an application using them; the modifier field is not, and is reported as zero.
 */

enum {
    OOPS_KEYBOARD_OK = 0,
    OOPS_KEYBOARD_EUNAVAIL = -1, /* the library, its entry points or a signed-in
                                    user did not resolve */
    OOPS_KEYBOARD_ELAYOUT =
        -2, /* the read is real but the platform record layout is unconfirmed */
    OOPS_KEYBOARD_EPARAM =
        -3, /* a caller argument was rejected before any platform call */
};

/* Key transition kind. */
enum {
    OOPS_KEY_UP = 0,
    OOPS_KEY_DOWN = 1,
};

/* Modifier bits, OOPS's own values. */
enum {
    OOPS_KMOD_CTRL = 1u << 0,
    OOPS_KMOD_SHIFT = 1u << 1,
    OOPS_KMOD_ALT = 1u << 2,
    OOPS_KMOD_GUI = 1u << 3,
};

/* A single key transition in OOPS's shape - not the platform record. */
typedef struct oops_key_event {
    uint16_t usage;     /* USB HID usage code */
    uint8_t transition; /* OOPS_KEY_UP / OOPS_KEY_DOWN */
    uint8_t modifiers;  /* OOPS_KMOD_* mask */
    uint64_t timestamp; /* platform sample time, 0 if unavailable */
} oops_key_event_t;

#define OOPS_MAX_KEY_EVENTS 32

/*
 * How much the input layer logs about what it delivers.
 *
 * The level is an `oops_log_level_t` from `oops/system.h` (`OOPS_LOG_NONE` through
 * `OOPS_LOG_TRACE`), taken as an `int` so this header need not include that one.
 * `OOPS_LOG_INFO` is the default and covers open, close and failures.
 *
 * `OOPS_LOG_DEBUG` adds every key transition handed to the caller, with its usage code
 * and modifiers: two lines a typed character, for when what the title receives and
 * what the player pressed disagree (a duplicated press is invisible above this layer).
 */
void oops_input_set_log_level(int level);
int oops_input_get_log_level(void);

/*
 * Open the keyboard for the signed-in user. 0 when at least one handle is open,
 * OOPS_KEYBOARD_EUNAVAIL when the library, its entry points or a keyboard are
 * absent. A later call opens any handle that is not yet open.
 */
int oops_keyboard_init(void);

/* Whether keyboard input is reachable: the library and its entry points
 * resolve. 1/0. */
int oops_keyboard_available(void);

/*
 * Drain up to `max_events` (cap OOPS_MAX_KEY_EVENTS) key transitions, releases
 * before presses, and return the count; 0 when nothing changed or the change does not
 * fit (it is reported whole on a later call). Negative is one of the codes above.
 * Modifiers are 0 and timestamps are 0.
 */
int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events);

/*
 * Read the held keys and map directional and action keys to an OOPS_BUTTON_*
 * bitmask. Returns 0 when no keyboard is attached or no mapped key is held.
 */
uint32_t oops_keyboard_poll_buttons(void);

/* Close the keyboard. */
void oops_keyboard_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_KEYBOARD_H */
