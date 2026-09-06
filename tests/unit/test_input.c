#include "tests/test_common.h"
#include "oops/input.h"
#include "oops/keyboard.h"
#include "oops/mouse.h"

static void test_input_button_bitmasks(void) {
    ASSERT_EQ(OOPS_BUTTON_CROSS,    1u << 14);
    ASSERT_EQ(OOPS_BUTTON_CIRCLE,   1u << 13);
    ASSERT_EQ(OOPS_BUTTON_TRIANGLE, 1u << 12);
    ASSERT_EQ(OOPS_BUTTON_SQUARE,   1u << 15);
    ASSERT_EQ(OOPS_BUTTON_CREATE,   1u << 16);
    ASSERT_EQ(OOPS_BUTTON_TOUCHPAD, 1u << 20);
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

/* Batched low-latency read: argument rejection is checked here; the mapping and oldest-first
 * ordering need a stubbed pad and are exercised where a fake device can be injected, not in
 * this no-platform host suite. */
static void test_input_batch_bounds(void) {
    oops_pad_state_t batch[OOPS_MAX_PAD_SAMPLES];
    ASSERT_EQ(oops_input_poll_batch(0, NULL, 8), -1);   /* null buffer */
    ASSERT_EQ(oops_input_poll_batch(99, batch, 8), -1); /* bad port */
    ASSERT_EQ(oops_input_poll_batch(0, batch, 0), -1);  /* zero samples */
    /* Valid args on a host with no pad attached: rejected, never a positive count. */
    ASSERT_TRUE(oops_input_poll_batch(0, batch, OOPS_MAX_PAD_SAMPLES) <= 0);
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

/* Keyboard: with no platform symbols on host, availability is honestly false, init fails,
 * and the read rejects rather than fabricating events. */
static void test_input_keyboard_contract(void) {
    ASSERT_EQ(oops_keyboard_available(), 0);
    ASSERT_NE(oops_keyboard_init(), 0);
    oops_key_event_t ev[OOPS_MAX_KEY_EVENTS];
    ASSERT_TRUE(oops_keyboard_read(ev, OOPS_MAX_KEY_EVENTS) < 0);
    ASSERT_EQ(oops_keyboard_read(NULL, OOPS_MAX_KEY_EVENTS), -1);
    ASSERT_EQ(oops_keyboard_read(ev, 0), -1);
    oops_keyboard_close();
}

/* Mouse: same contract. */
static void test_input_mouse_contract(void) {
    ASSERT_EQ(oops_mouse_available(), 0);
    ASSERT_NE(oops_mouse_init(), 0);
    oops_mouse_state_t ms[OOPS_MAX_MOUSE_SAMPLES];
    ASSERT_TRUE(oops_mouse_read(ms, OOPS_MAX_MOUSE_SAMPLES) < 0);
    ASSERT_EQ(oops_mouse_read(NULL, OOPS_MAX_MOUSE_SAMPLES), -1);
    ASSERT_EQ(oops_mouse_read(ms, 0), -1);
    oops_mouse_close();
}

void run_unit_tests_input(void) {
    TEST_SUITE_BEGIN("Controller & Input Subsystem");
    RUN_TEST(test_input_button_bitmasks);
    RUN_TEST(test_input_poll_bounds);
    RUN_TEST(test_input_rumble_and_lightbar);
    RUN_TEST(test_input_motion_telemetry);
    RUN_TEST(test_input_batch_bounds);
    RUN_TEST(test_input_trigger_contract);
    RUN_TEST(test_input_keyboard_contract);
    RUN_TEST(test_input_mouse_contract);
}

