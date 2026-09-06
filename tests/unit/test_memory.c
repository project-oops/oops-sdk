#include "tests/test_common.h"
#include "oops/memory.h"

static void test_memory_constants(void) {
    ASSERT_EQ(OOPS_MEM_WB_ONION, 0);
    ASSERT_EQ(OOPS_MEM_WC_GARLIC, 3);
    ASSERT_EQ(OOPS_MEM_WB_GARLIC, 10);

    ASSERT_EQ(OOPS_PROT_CPU_READ, 0x11);
    ASSERT_EQ(OOPS_PROT_CPU_WRITE, 0x22);
    ASSERT_EQ(OOPS_PROT_CPU_RW, 0x33);
}

static void test_memory_alloc_zero_and_null(void) {
    /* 0 bytes requested returns NULL */
    void *ptr = oops_mem_alloc(0, 0x10000, OOPS_MEM_WC_GARLIC);
    ASSERT_TRUE(ptr == NULL);

    /* Freeing NULL does not crash */
    oops_mem_free(NULL);

    /* Querying phys for NULL returns 0 */
    ASSERT_EQ(oops_mem_get_phys(NULL), 0);
}

void run_unit_tests_memory(void) {
    TEST_SUITE_BEGIN("Direct Memory & Coherent Allocator");
    RUN_TEST(test_memory_constants);
    RUN_TEST(test_memory_alloc_zero_and_null);
}

