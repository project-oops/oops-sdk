#include "oops/memory.h"
#include "tests/test_common.h"

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
}

/* A size that would wrap when rounded up to the page is refused, not rounded to
 * something small and handed back as if it fit. */
static void test_memory_alloc_refuses_wrapping_size(void) {
  ASSERT_TRUE(oops_mem_alloc(SIZE_MAX, 0x10000, OOPS_MEM_WC_GARLIC) == NULL);
  ASSERT_TRUE(oops_mem_alloc(SIZE_MAX - 0x100, 0x10000, OOPS_MEM_WC_GARLIC) ==
              NULL);
}

/* Unknown is -1: 0 is a valid physical offset and must stay one. */
static void test_memory_phys_unknown_is_minus_one(void) {
  int local = 0;
  ASSERT_EQ(oops_mem_get_phys(NULL), -1);
  ASSERT_EQ(oops_mem_get_phys(&local), -1);
}

/* On a host with no platform, every direct call rejects rather than fabricating
 * a mapping. */
static void test_memory_direct_contract_on_host(void) {
  int64_t phys = 0;
  void *vaddr = NULL;
  uint8_t page[16];
  ASSERT_EQ(oops_mem_alloc_direct(0x10000, 0x10000, OOPS_MEM_WC_GARLIC, NULL),
            -1);
  ASSERT_EQ(oops_mem_alloc_direct(0, 0x10000, OOPS_MEM_WC_GARLIC, &phys), -1);
  ASSERT_EQ(oops_mem_alloc_direct(0x10000, 0x10000, OOPS_MEM_WC_GARLIC, &phys),
            -1);
  ASSERT_EQ(
      oops_mem_map_direct(&vaddr, 0x10000, OOPS_PROT_CPU_RW, 0, 0, 0x10000),
      -1);
  ASSERT_EQ(oops_mem_batch_map(NULL, 0, 0x10000, 0x4000, 0x33), -1);
  ASSERT_EQ(oops_mem_batch_map(page, 0, 0x10000, 0, 0x33),
            -1); /* zero page size */
  ASSERT_EQ(oops_mem_batch_map(page, 0, 0, 0x4000, 0x33),
            -1); /* nothing to map */
  ASSERT_EQ(oops_mem_unmap(NULL, 0x10000), -1);
  ASSERT_TRUE(oops_mem_alloc(0x10000, 0x10000, OOPS_MEM_WC_GARLIC) == NULL);
}

static void test_memory_reserve_va_contract_on_host(void) {
  void *va = NULL;
  ASSERT_EQ(oops_mem_reserve_va(NULL, 0x10000, 0, 0x4000), -1);
  ASSERT_EQ(oops_mem_reserve_va(&va, 0, 0, 0x4000), -1);
  ASSERT_EQ(oops_mem_reserve_va(&va, 0x10000, 0, 0x4000), -1);
  ASSERT_EQ(oops_mem_release_va(NULL, 0x10000), -1);
  ASSERT_EQ(oops_mem_release_va((void *)0x20000000, 0), -1);
  ASSERT_EQ(oops_mem_release_va((void *)0x20000000, 0x10000), -1);
}

void run_unit_tests_memory(void) {
  TEST_SUITE_BEGIN("Direct Memory & Coherent Allocator");
  RUN_TEST(test_memory_constants);
  RUN_TEST(test_memory_alloc_zero_and_null);
  RUN_TEST(test_memory_alloc_refuses_wrapping_size);
  RUN_TEST(test_memory_phys_unknown_is_minus_one);
  RUN_TEST(test_memory_direct_contract_on_host);
  RUN_TEST(test_memory_reserve_va_contract_on_host);
}
