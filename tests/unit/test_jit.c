#include "oops/jit.h"
#include "tests/test_common.h"
#include <stdint.h>
#include <string.h>

static void test_jit_constants(void) {
  ASSERT_EQ(OOPS_JIT_METHOD_NONE, 0);
  ASSERT_EQ(OOPS_JIT_METHOD_SHARED_MEM, 1);
  ASSERT_EQ(OOPS_JIT_METHOD_MPROTECT, 2);
  ASSERT_EQ(OOPS_JIT_METHOD_HOST, 3);
}

static void test_jit_null_safety(void) {
  oops_jit_memory_t mem;
  memset(&mem, 0, sizeof(mem));

  /* Null target or 0 size */
  ASSERT_EQ(oops_jit_alloc(0, NULL), -1);
  ASSERT_EQ(oops_jit_alloc(1024, NULL), -1);
  ASSERT_EQ(oops_jit_alloc(0, &mem), -1);

  /* Free null or empty */
  ASSERT_EQ(oops_jit_free(NULL), -1);
  ASSERT_EQ(oops_jit_free(&mem), -1);

  /* Flush cache null or empty */
  ASSERT_EQ(oops_jit_flush_icache(NULL, 0), -1);
  ASSERT_EQ(oops_jit_flush_icache(NULL, 100), -1);
}

static void test_jit_alloc_execute_free(void) {
  ASSERT_TRUE(oops_jit_is_available() != 0);

  int method = oops_jit_get_method();
  ASSERT_TRUE(method != OOPS_JIT_METHOD_NONE);

  oops_jit_memory_t mem;
  memset(&mem, 0, sizeof(mem));

  /* Allocate small buffer (should round to page size) */
  ASSERT_EQ(oops_jit_alloc(64, &mem), 0);
  ASSERT_TRUE(mem.rx_addr != NULL);
  ASSERT_TRUE(mem.rw_addr != NULL);
  ASSERT_TRUE(mem.size >= 64);

#if defined(__x86_64__) || defined(_M_X64)
  /*
   * x86_64 machine code for:
   * int test_func(void) { return 42; }
   *
   * mov eax, 42 (0xb8, 0x2a, 0x00, 0x00, 0x00)
   * ret         (0xc3)
   */
  const uint8_t code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
  memcpy(mem.rw_addr, code, sizeof(code));

  /* Flush cache */
  ASSERT_EQ(oops_jit_flush_icache(mem.rx_addr, sizeof(code)), 0);

  /* Execute the generated function */
  typedef int (*jit_fn_t)(void);
  jit_fn_t fn = (jit_fn_t)(uintptr_t)mem.rx_addr;
  int result = fn();
  ASSERT_EQ(result, 42);
#endif

  /* Free the memory */
  ASSERT_EQ(oops_jit_free(&mem), 0);
  ASSERT_TRUE(mem.rx_addr == NULL);
  ASSERT_TRUE(mem.rw_addr == NULL);
  ASSERT_EQ(mem.size, 0);
}

void run_unit_tests_jit(void) {
  TEST_SUITE_BEGIN("JIT & Dynamic Code Execution");
  RUN_TEST(test_jit_constants);
  RUN_TEST(test_jit_null_safety);
  RUN_TEST(test_jit_alloc_execute_free);
}

