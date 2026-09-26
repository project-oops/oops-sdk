#include "tests/test_common.h"
#include <pthread.h>
#include <unistd.h>

#define NUM_WORKERS 4
#define TOTAL_TASKS 100

static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_pool_cond = PTHREAD_COND_INITIALIZER;
static int s_tasks_remaining = TOTAL_TASKS;
static int s_tasks_completed = 0;
static int s_stop_workers = 0;

static void *worker_entry(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_pool_lock);
        while (s_tasks_remaining == 0 && !s_stop_workers) {
            pthread_cond_wait(&s_pool_cond, &s_pool_lock);
        }
        if (s_stop_workers && s_tasks_remaining == 0) {
            pthread_mutex_unlock(&s_pool_lock);
            break;
        }
        s_tasks_remaining--;
        s_tasks_completed++;
        pthread_mutex_unlock(&s_pool_lock);
    }
    return NULL;
}

static void test_thread_pool_dispatch(void) {
    pthread_t threads[NUM_WORKERS];
    s_tasks_remaining = TOTAL_TASKS;
    s_tasks_completed = 0;
    s_stop_workers = 0;

    for (int i = 0; i < NUM_WORKERS; i++) {
        int rc = pthread_create(&threads[i], NULL, worker_entry, NULL);
        ASSERT_EQ(rc, 0);
    }

    /* Wait until all work completes */
    for (;;) {
        pthread_mutex_lock(&s_pool_lock);
        int done = (s_tasks_completed == TOTAL_TASKS);
        pthread_mutex_unlock(&s_pool_lock);
        if (done)
            break;
        usleep(1000);
    }

    /* Signal threads to shut down */
    pthread_mutex_lock(&s_pool_lock);
    s_stop_workers = 1;
    pthread_cond_broadcast(&s_pool_cond);
    pthread_mutex_unlock(&s_pool_lock);

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }

    ASSERT_EQ(s_tasks_completed, TOTAL_TASKS);
}

void run_integration_tests_thread_pool(void) {
    TEST_SUITE_BEGIN("Integration: Thread & Synchronization Worker Pool");
    RUN_TEST(test_thread_pool_dispatch);
}
