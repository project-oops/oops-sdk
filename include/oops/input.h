#ifndef OOPS_INPUT_H
#define OOPS_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard controller button bitmasks (matching ScePad layout) */
#define OOPS_BUTTON_L3        (1u << 1)
#define OOPS_BUTTON_R3        (1u << 2)
#define OOPS_BUTTON_OPTIONS   (1u << 3)
#define OOPS_BUTTON_UP        (1u << 4)
#define OOPS_BUTTON_RIGHT     (1u << 5)
#define OOPS_BUTTON_DOWN      (1u << 6)
#define OOPS_BUTTON_LEFT      (1u << 7)
#define OOPS_BUTTON_L2        (1u << 8)
#define OOPS_BUTTON_R2        (1u << 9)
#define OOPS_BUTTON_L1        (1u << 10)
#define OOPS_BUTTON_R1        (1u << 11)
#define OOPS_BUTTON_TRIANGLE  (1u << 12)
#define OOPS_BUTTON_CIRCLE    (1u << 13)
#define OOPS_BUTTON_CROSS     (1u << 14)
#define OOPS_BUTTON_SQUARE    (1u << 15)
#define OOPS_BUTTON_TOUCHPAD  (1u << 20)

typedef struct oops_pad_state {
    uint32_t buttons;
    int8_t   left_stick_x;   /* -128 to 127 */
    int8_t   left_stick_y;   /* -128 to 127 */
    int8_t   right_stick_x;  /* -128 to 127 */
    int8_t   right_stick_y;  /* -128 to 127 */
    uint8_t  l2_trigger;     /* 0 to 255 */
    uint8_t  r2_trigger;     /* 0 to 255 */
    int      connected;
} oops_pad_state_t;

int oops_input_init(void);
int oops_input_poll(unsigned int port, oops_pad_state_t *out_state);
int oops_input_set_rumble(unsigned int port, uint8_t small_motor, uint8_t large_motor);
int oops_input_set_lightbar(unsigned int port, uint8_t r, uint8_t g, uint8_t b);
void oops_input_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_INPUT_H */
