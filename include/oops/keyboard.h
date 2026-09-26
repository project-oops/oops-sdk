#ifndef OOPS_KEYBOARD_H
#define OOPS_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB keyboard input (libSceKeyboard).
 *
 * A homebrew UI beyond a gamepad needs real key events. This binds the keyboard
 * device and exposes a timestamped key-transition queue in OOPS's own shape.
 *
 * # State of this subsystem, honestly
 *
 * obSCEne's 101-input-ext measured it on 12.40. The library and its four entry
 * points resolve in the app context; in the eboot and payload contexts the
 * library loads but nothing in it resolves, so a payload cannot reach the
 * keyboard on this firmware, and availability below says so honestly. A read
 * writes a 96-byte record, so the size is confirmed; what the bytes mean is
 * not, because no keyboard was attached. The read path is therefore
 * capture-gated and refuses with OOPS_KEYBOARD_ELAYOUT rather than parse a
 * record whose fields are a guess - a distinct code, not zero events, so a
 * caller can tell "no keys" from "no reader".
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
 * **How much the input layer says about what it delivers**, on the SDK's own scale.
 *
 * The level is an `oops_log_level_t` from `oops/system.h` - `OOPS_LOG_NONE`, `_ERROR`,
 * `_WARN`,
 * `_INFO`, `_DEBUG`, `_TRACE` - taken as an `int` so this header need not include that
 * one. `OOPS_LOG_INFO` is the default and covers open, close and failures: the lines
 * that matter when input does not work at all.
 *
 * `OOPS_LOG_DEBUG` adds every key transition handed to the caller, with its usage code
 * and its modifiers. A held key is silent but a typed sentence is two lines a
 * character, so it is not a level to leave on; it is the level to reach for when what
 * the title receives and what the player pressed have stopped agreeing. That is not
 * hypothetical - a handle is opened per keyboard index, and when the second index
 * mirrored the first every press was delivered twice, which from above this layer is
 * indistinguishable from a player pressing twice and left no trace in any log.
 */
void oops_input_set_log_level(int level);
int oops_input_get_log_level(void);

/*
 * Open the keyboard for the signed-in user. 0 on success,
 * OOPS_KEYBOARD_EUNAVAIL when the library or the user is absent, otherwise the
 * platform's own negative code from the open. The result is remembered and
 * repeated by later calls until oops_keyboard_close().
 */
int oops_keyboard_init(void);

/* Whether keyboard input is reachable: the library and its entry points
 * resolve. 1/0. */
int oops_keyboard_available(void);

/*
 * Drain up to `max_events` (cap OOPS_MAX_KEY_EVENTS) key transitions, oldest
 * first, and return the count. Negative is one of the codes above.
 *
 * NOTE: capture-gated. The platform key-record layout is unconfirmed, so this
 * returns OOPS_KEYBOARD_ELAYOUT until the obSCEne capture lands, never a
 * fabricated count.
 */
int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events);

/*
 * Read active keycodes directly and map directional and action keys to
 * OOPS_BUTTON_* bitmask. Returns 0 if keyboard is unattached or no keys held.
 */
uint32_t oops_keyboard_poll_buttons(void);

/* Close the keyboard. */
void oops_keyboard_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_KEYBOARD_H */
