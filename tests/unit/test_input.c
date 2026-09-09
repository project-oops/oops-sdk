#include "tests/test_common.h"
#include "oops/input.h"
#include "oops/keyboard.h"
#include "oops/mouse.h"
#include "src/input/pad_layout.h"

static void test_input_button_bitmasks(void) {
    ASSERT_EQ(OOPS_BUTTON_CROSS,    1u << 14);
    ASSERT_EQ(OOPS_BUTTON_CIRCLE,   1u << 13);
    ASSERT_EQ(OOPS_BUTTON_TRIANGLE, 1u << 12);
    ASSERT_EQ(OOPS_BUTTON_SQUARE,   1u << 15);
    ASSERT_EQ(OOPS_BUTTON_CREATE,   1u << 16);
    ASSERT_EQ(OOPS_BUTTON_TOUCHPAD, 1u << 20);
}

/* On host there is no pad, so init fails - and keeps failing. A second call must report the
 * same result, not a success because the first call left a flag behind. */
static void test_input_init_is_consistent(void) {
    ASSERT_EQ(oops_input_init(), -1);
    ASSERT_EQ(oops_input_init(), -1);
    oops_input_close();
    ASSERT_EQ(oops_input_init(), -1);
    oops_input_close();
}

static void test_input_poll_bounds(void) {
    oops_pad_state_t state;
    /* Uninitialized or unattached port returns non-zero error without faulting */
    int rc = oops_input_poll(99, &state);
    ASSERT_NE(rc, 0);

    /* NULL state pointer returns error safely */
    rc = oops_input_poll(0, NULL);
    ASSERT_NE(rc, 0);
}

static void test_input_rumble_and_lightbar(void) {
    /* Invalid port rejection */
    ASSERT_NE(oops_input_set_rumble(-1, 100, 100), 0);
    ASSERT_NE(oops_input_set_lightbar(10, 255, 0, 0), 0);
}

static void test_input_motion_telemetry(void) {
    oops_pad_state_t state;
    for (size_t i = 0; i < sizeof(state); i++) ((unsigned char *)&state)[i] = 0xAA;

    /* Poll invalid port clears/rejects */
    ASSERT_NE(oops_input_poll(99, &state), 0);

    /* Orientation reset check on invalid port or host */
    ASSERT_EQ(oops_input_reset_orientation(99), -1);
    ASSERT_EQ(oops_input_reset_orientation(0), -1);
}

/* Batched low-latency read: argument rejection, and on a host with no driver the valid call is
 * unavailable rather than a fabricated count. The stride is pinned to the driver's 120-byte
 * record by the static asserts in pad_layout.h. */
static void test_input_batch_bounds(void) {
    oops_pad_state_t batch[OOPS_MAX_PAD_SAMPLES];
    ASSERT_EQ(oops_input_poll_batch(0, NULL, 8), -1);   /* null buffer */
    ASSERT_EQ(oops_input_poll_batch(99, batch, 8), -1); /* bad port */
    ASSERT_EQ(oops_input_poll_batch(0, batch, 0), -1);  /* zero samples */
    ASSERT_EQ(oops_input_poll_batch(0, batch, OOPS_MAX_PAD_SAMPLES), -1); /* valid, no driver */
}

/* Adaptive triggers are capture-gated: no false-positive availability, and the effect call
 * refuses every argument shape rather than pass a guessed struct. */
static void test_input_trigger_contract(void) {
    ASSERT_EQ(oops_input_adaptive_triggers_available(0), 0);
    ASSERT_EQ(oops_input_adaptive_triggers_available(99), 0);
    ASSERT_EQ(oops_input_set_trigger_effect(99, OOPS_TRIGGER_L2, OOPS_TRIGGER_FEEDBACK,
                                            0, 0, 0, 0), -1);           /* bad port */
    ASSERT_EQ(oops_input_set_trigger_effect(0, 0, OOPS_TRIGGER_FEEDBACK,
                                            0, 0, 0, 0), -1);           /* empty trigger mask */
    ASSERT_EQ(oops_input_set_trigger_effect(0, OOPS_TRIGGER_L2, 99,
                                            0, 0, 0, 0), -1);           /* bad mode */
    ASSERT_EQ(oops_input_set_trigger_effect(0, OOPS_TRIGGER_L2, OOPS_TRIGGER_FEEDBACK,
                                            128, 255, 200, 0), -1);     /* valid args, gated */
}

/* Keyboard: with no platform symbols on host, availability is honestly false, init reports
 * unavailable and keeps reporting it, and the read refuses with the layout code rather than
 * fabricating zero events. */
static void test_input_keyboard_contract(void) {
    ASSERT_EQ(oops_keyboard_available(), 0);
    ASSERT_EQ(oops_keyboard_init(), OOPS_KEYBOARD_EUNAVAIL);
    ASSERT_EQ(oops_keyboard_init(), OOPS_KEYBOARD_EUNAVAIL);
    oops_key_event_t ev[OOPS_MAX_KEY_EVENTS];
    ASSERT_EQ(oops_keyboard_read(ev, OOPS_MAX_KEY_EVENTS), OOPS_KEYBOARD_ELAYOUT);
    ASSERT_EQ(oops_keyboard_read(NULL, OOPS_MAX_KEY_EVENTS), OOPS_KEYBOARD_EPARAM);
    ASSERT_EQ(oops_keyboard_read(ev, 0), OOPS_KEYBOARD_EPARAM);
    oops_keyboard_close();
}

/* Mouse: same contract. */
static void test_input_mouse_contract(void) {
    ASSERT_EQ(oops_mouse_available(), 0);
    ASSERT_EQ(oops_mouse_init(), OOPS_MOUSE_EUNAVAIL);
    ASSERT_EQ(oops_mouse_init(), OOPS_MOUSE_EUNAVAIL);
    oops_mouse_state_t ms[OOPS_MAX_MOUSE_SAMPLES];
    ASSERT_EQ(oops_mouse_read(ms, OOPS_MAX_MOUSE_SAMPLES), OOPS_MOUSE_ELAYOUT);
    ASSERT_EQ(oops_mouse_read(NULL, OOPS_MAX_MOUSE_SAMPLES), OOPS_MOUSE_EPARAM);
    ASSERT_EQ(oops_mouse_read(ms, 0), OOPS_MOUSE_EPARAM);
    oops_mouse_close();
}

/* The raw-record mapping, fed one record directly: this is where a wrong sign or offset
 * would otherwise first show on a controller. Uses the private layout header. */
static void test_input_record_mapping(void) {
    ScePadDataInternal raw;
    oops_pad_state_t st;
    ASSERT_EQ(sizeof(ScePadDataInternal), OOPS_PAD_RECORD_BYTES);   /* the measured 120 */
    for (size_t i = 0; i < sizeof(raw); i++) ((unsigned char *)&raw)[i] = 0;
    for (size_t i = 0; i < sizeof(st); i++) ((unsigned char *)&st)[i] = 0xAA;

    raw.buttons = OOPS_BUTTON_CROSS | OOPS_BUTTON_L1;
    raw.leftStick.x = 0;    raw.leftStick.y = 128;
    raw.rightStick.x = 255; raw.rightStick.y = 200;
    raw.analogButtons.l2 = 17; raw.analogButtons.r2 = 255;
    raw.quat.x = 0.1f; raw.quat.y = 0.2f; raw.quat.z = 0.3f; raw.quat.w = 0.4f;
    raw.accel.x = 1.0f; raw.accel.y = -1.0f; raw.accel.z = 0.5f;
    raw.vel.x = 2.0f; raw.vel.y = 3.0f; raw.vel.z = -4.0f;
    raw.touchData.fingers = 1;
    raw.touchData.touch[0].x = 1919; raw.touchData.touch[0].y = 941; raw.touchData.touch[0].finger = 7;
    raw.touchData.touch[1].x = 5;    raw.touchData.touch[1].y = 6;   raw.touchData.touch[1].finger = 8;
    raw.connected = 1;

    oops_input_map_record(&st, &raw);

    ASSERT_EQ(st.buttons, OOPS_BUTTON_CROSS | OOPS_BUTTON_L1);
    ASSERT_EQ(st.left_stick_x, -128);   /* unsigned 0..255 recentred to signed */
    ASSERT_EQ(st.left_stick_y, 0);
    ASSERT_EQ(st.right_stick_x, 127);
    ASSERT_EQ(st.right_stick_y, 72);
    ASSERT_EQ(st.l2_trigger, 17);
    ASSERT_EQ(st.r2_trigger, 255);
    ASSERT_EQ(st.connected, 1);
    ASSERT_EQ(st.touch[0].x, 1919);
    ASSERT_EQ(st.touch[0].y, 941);
    ASSERT_EQ(st.touch[0].id, 7);
    ASSERT_EQ(st.touch[0].active, 1);
    ASSERT_EQ(st.touch[1].id, 8);
    ASSERT_EQ(st.touch[1].active, 0);   /* one finger down */
    ASSERT_TRUE(st.orientation[0] == 0.1f && st.orientation[3] == 0.4f);
    ASSERT_TRUE(st.acceleration[1] == -1.0f && st.angular_velocity[2] == -4.0f);

    /* Connected follows the driver's flag alone: centred sticks read 128 and a held button is
     * not a pad, so neither may stand in for it. */
    raw.connected = 0;
    raw.buttons = OOPS_BUTTON_OPTIONS;
    raw.leftStick.x = 128;
    raw.leftStick.y = 128;
    oops_input_map_record(&st, &raw);
    ASSERT_EQ(st.connected, 0);
    raw.connected = 1;
    raw.buttons = 0;
    oops_input_map_record(&st, &raw);
    ASSERT_EQ(st.connected, 1);
}

void run_unit_tests_input(void) {
    TEST_SUITE_BEGIN("Controller & Input Subsystem");
    RUN_TEST(test_input_button_bitmasks);
    RUN_TEST(test_input_init_is_consistent);
    RUN_TEST(test_input_record_mapping);
    RUN_TEST(test_input_poll_bounds);
    RUN_TEST(test_input_rumble_and_lightbar);
    RUN_TEST(test_input_motion_telemetry);
    RUN_TEST(test_input_batch_bounds);
    RUN_TEST(test_input_trigger_contract);
    RUN_TEST(test_input_keyboard_contract);
    RUN_TEST(test_input_mouse_contract);
}
