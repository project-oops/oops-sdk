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

/* With no platform threads on host: creation fails rather than returning a handle, self is
 * NULL, equality falls back to pointer comparison, and a timed wait refuses rather than
 * degrading into a wait that could never return. */
static void test_thread_host_contract(void) {
    ASSERT_TRUE(oops_thread_create("t", NULL, NULL, 0, 0) == NULL);
    ASSERT_TRUE(oops_thread_self() == NULL);
    ASSERT_EQ(oops_thread_equal(NULL, NULL), 1);
    ASSERT_EQ(oops_thread_join(NULL, NULL), -1);
    ASSERT_EQ(oops_thread_detach(NULL), -1);

    oops_mutex_t m;
    oops_cond_t c;
    for (size_t i = 0; i < sizeof(m); i++) ((unsigned char *)&m)[i] = 0;
    for (size_t i = 0; i < sizeof(c); i++) ((unsigned char *)&c)[i] = 0;
    ASSERT_EQ(oops_mutex_init(&m, "m"), -1);
    ASSERT_EQ(oops_cond_init(&c, "c"), -1);
    ASSERT_EQ(oops_cond_timedwait(&c, &m, 10), -1);   /* returns; never blocks */

    oops_sem_t s;
    for (size_t i = 0; i < sizeof(s); i++) ((unsigned char *)&s)[i] = 0;
    ASSERT_EQ(oops_sem_init(&s, "s", 0, 1), -1);
    ASSERT_EQ(oops_sem_wait(&s, 1), -1);               /* handle 0 is not a semaphore */
    oops_thread_yield();
}

/* Exception handling: the constants are the confirmed raw platform codes, the handler type
 * compiles, and on a host with no libkernel every call fails rather than pretending, install
 * refuses a NULL handler locally. */
static void handler_probe(int signum, void *a1, void *a2) { (void)signum; (void)a1; (void)a2; }

static void test_thread_exception_contract(void) {
    ASSERT_EQ(OOPS_EXCEPTION_SIGNAL, 30);
    ASSERT_EQ((int)OOPS_EXC_EAGAIN, (int)0x80020023);
    ASSERT_EQ((int)OOPS_EXC_EINVAL, (int)0x80020016);
    ASSERT_EQ((int)OOPS_EXC_ESRCH,  (int)0x80020003);

    ASSERT_EQ(oops_thread_install_exception_handler(OOPS_EXCEPTION_SIGNAL, NULL), -1);
    ASSERT_EQ(oops_thread_install_exception_handler(OOPS_EXCEPTION_SIGNAL, handler_probe), -1);
    ASSERT_EQ(oops_thread_remove_exception_handler(OOPS_EXCEPTION_SIGNAL), -1);
    ASSERT_EQ(oops_thread_raise_exception(NULL, OOPS_EXCEPTION_SIGNAL), -1);
}

void run_unit_tests_thread(void) {
    TEST_SUITE_BEGIN("Threading & Synchronization");
    RUN_TEST(test_thread_primitives_null_safety);
    RUN_TEST(test_thread_host_contract);
    RUN_TEST(test_thread_exception_contract);
}
