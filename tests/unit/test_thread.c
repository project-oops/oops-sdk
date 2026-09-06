#include "tests/test_common.h"
#include "oops/thread.h"

static void test_thread_primitives_null_safety(void) {
    ASSERT_EQ(oops_mutex_init(NULL, NULL), -1);
    ASSERT_EQ(oops_mutex_lock(NULL), -1);
    ASSERT_EQ(oops_mutex_trylock(NULL), -1);
    ASSERT_EQ(oops_mutex_unlock(NULL), -1);
    ASSERT_EQ(oops_mutex_destroy(NULL), -1);

    ASSERT_EQ(oops_cond_init(NULL, NULL), -1);
    ASSERT_EQ(oops_cond_wait(NULL, NULL), -1);
    ASSERT_EQ(oops_cond_timedwait(NULL, NULL, 100), -1);
    ASSERT_EQ(oops_cond_signal(NULL), -1);
    ASSERT_EQ(oops_cond_broadcast(NULL), -1);
    ASSERT_EQ(oops_cond_destroy(NULL), -1);

    ASSERT_EQ(oops_sem_init(NULL, NULL, 0, 1), -1);
    ASSERT_EQ(oops_sem_wait(NULL, 1), -1);
    ASSERT_EQ(oops_sem_poll(NULL, 1), -1);
    ASSERT_EQ(oops_sem_signal(NULL, 1), -1);
    ASSERT_EQ(oops_sem_destroy(NULL), -1);
}

void run_unit_tests_thread(void) {
    TEST_SUITE_BEGIN("Threading & Synchronization");
    RUN_TEST(test_thread_primitives_null_safety);
}

