#include "oops/dialog.h"
#include "tests/test_common.h"

/* Unit tests for the IME and message dialogs in `oops/dialog.h`, which fail on a host
 * without the platform library. */

/* The IME calls refuse NULL and zero-length buffers. */
static void test_dialog_ime_null_safety(void) {
    ASSERT_EQ(oops_dialog_ime_open(NULL), -1);
    ASSERT_EQ(oops_dialog_ime_poll(), OOPS_IME_STATUS_NONE);
    ASSERT_EQ(oops_dialog_ime_get_result(NULL, 0, NULL), -1);

    char buf[64];
    ASSERT_EQ(oops_dialog_ime_get_result(buf, 0, NULL), -1);
}

/* Opening the IME fails without the platform, and abort and close are safe after. */
static void test_dialog_ime_unsupported(void) {
    oops_ime_param_t param;
    for (size_t i = 0; i < sizeof(param); i++)
        ((uint8_t *)&param)[i] = 0;
    param.user_id = -1;
    param.type = OOPS_IME_TYPE_DEFAULT;
    param.title = "Test";
    param.placeholder = "Enter text";

    /* Weak symbols not present on host -> returns -1 */
    ASSERT_EQ(oops_dialog_ime_open(&param), -1);
    ASSERT_EQ(oops_dialog_ime_poll(), OOPS_IME_STATUS_NONE);
    oops_dialog_ime_abort();
    oops_dialog_ime_close();
}

/* A message dialog refuses NULL and fails without the platform. */
static void test_dialog_message_null_safety(void) {
    ASSERT_EQ(oops_dialog_message_show(NULL, OOPS_MSG_DIALOG_BTN_OK), -1);
    ASSERT_EQ(oops_dialog_message_poll(NULL), -1);

    /* Weak symbols not present on host -> returns -1 */
    ASSERT_EQ(oops_dialog_message_show("Hello", OOPS_MSG_DIALOG_BTN_OK), -1);
    oops_dialog_message_close();
}

void run_unit_tests_dialog(void) {
    TEST_SUITE_BEGIN("System Dialogs (IME Keyboard & Message Dialogs)");
    RUN_TEST(test_dialog_ime_null_safety);
    RUN_TEST(test_dialog_ime_unsupported);
    RUN_TEST(test_dialog_message_null_safety);
}
