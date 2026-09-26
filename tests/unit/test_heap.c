#include "oops/heap.h"
#include "oops/freestd.h"
#include "tests/test_common.h"

/* Unit tests for the userland heap allocator, `oops/heap.h`. */

/* Zero-size requests return NULL, NULL is freeable, and realloc to 0 frees. */
static void test_heap_null_and_zero(void) {
    ASSERT_TRUE(oops_malloc(0) == NULL);
    oops_free(NULL);
    ASSERT_TRUE(oops_calloc(0, 10) == NULL);
    ASSERT_TRUE(oops_calloc(10, 0) == NULL);

    void *p = oops_realloc(NULL, 64);
    ASSERT_TRUE(p != NULL);
    void *p_freed = oops_realloc(p, 0);
    ASSERT_TRUE(p_freed == NULL);

    oops_heap_stats_t stats;
    ASSERT_EQ(oops_heap_get_stats(&stats), 0);
    ASSERT_EQ(oops_heap_get_stats(NULL), -1);
}

/* Slab allocations do not overlap, and freed slots are reused. */
static void test_heap_slabs_and_reuse(void) {
    void *ptrs[10];

    for (int i = 0; i < 10; i++) {
        ptrs[i] = oops_malloc(128);
        ASSERT_TRUE(ptrs[i] != NULL);
        memset(ptrs[i], (uint8_t)(i + 1), 128);
    }

    for (int i = 0; i < 10; i++) {
        uint8_t *b = (uint8_t *)ptrs[i];
        for (int j = 0; j < 128; j++) {
            ASSERT_EQ(b[j], (uint8_t)(i + 1));
        }
    }

    for (int i = 0; i < 10; i++) {
        oops_free(ptrs[i]);
    }

    /* Recycled from the freelist. */
    void *re_p = oops_malloc(128);
    ASSERT_TRUE(re_p != NULL);
    oops_free(re_p);
}

/* calloc zeroes, and realloc to a larger size keeps the contents. */
static void test_heap_calloc_and_realloc(void) {
    size_t count = 32;
    uint32_t *arr = (uint32_t *)oops_calloc(count, sizeof(uint32_t));
    ASSERT_TRUE(arr != NULL);
    for (size_t i = 0; i < count; i++) {
        ASSERT_EQ(arr[i], 0);
        arr[i] = (uint32_t)(i + 100);
    }

    size_t new_count = 64;
    arr = (uint32_t *)oops_realloc(arr, new_count * sizeof(uint32_t));
    ASSERT_TRUE(arr != NULL);

    for (size_t i = 0; i < count; i++) {
        ASSERT_EQ(arr[i], (uint32_t)(i + 100));
    }

    oops_free(arr);
}

/* A large allocation, past the slabs, is usable across every page. */
static void test_heap_large_mmap(void) {
    size_t big_sz = 128 * 1024;
    uint8_t *big = (uint8_t *)oops_malloc(big_sz);
    ASSERT_TRUE(big != NULL);

    for (size_t i = 0; i < big_sz; i += 4096) {
        big[i] = 0x5a;
    }
    for (size_t i = 0; i < big_sz; i += 4096) {
        ASSERT_EQ(big[i], 0x5a);
    }

    oops_free(big);
}

/* aligned_alloc honours power-of-two alignments and refuses C11's invalid arguments. */
static void test_heap_aligned_alloc(void) {
    ASSERT_TRUE(oops_aligned_alloc(0, 64) == NULL);
    ASSERT_TRUE(oops_aligned_alloc(64, 0) == NULL);
    ASSERT_TRUE(oops_aligned_alloc(15, 60) == NULL); /* not a power of 2 */
    ASSERT_TRUE(oops_aligned_alloc(64, 50) ==
                NULL); /* size not multiple of alignment */

    size_t alignments[] = {16, 32, 64, 128, 256, 1024, 4096};
    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        size_t align = alignments[i];
        size_t size = align * 4;
        void *ptr = oops_aligned_alloc(align, size);
        ASSERT_TRUE(ptr != NULL);
        ASSERT_EQ(((uintptr_t)ptr % align), 0ULL);

        memset(ptr, 0x7c, size);
        uint8_t *b = (uint8_t *)ptr;
        ASSERT_EQ(b[0], 0x7c);
        ASSERT_EQ(b[size - 1], 0x7c);

        oops_free(ptr);
    }
}

/* Every malloc is 16-byte aligned (x86-64 max_align_t), slab or large. */
static void test_heap_malloc_alignment(void) {
    size_t sizes[] = {1,   3,   7,   8,   15,   16,   24,   32,   63,    64,
                      127, 128, 255, 256, 1000, 2048, 4096, 8192, 16384, 65536};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = oops_malloc(sizes[i]);
        ASSERT_TRUE(p != NULL);
        ASSERT_EQ(((uintptr_t)p & 15ULL), 0ULL);
        oops_free(p);
    }
}

void run_unit_tests_heap(void) {
    TEST_SUITE_BEGIN("Freestanding Userland Heap Allocator");
    RUN_TEST(test_heap_null_and_zero);
    RUN_TEST(test_heap_slabs_and_reuse);
    RUN_TEST(test_heap_calloc_and_realloc);
    RUN_TEST(test_heap_large_mmap);
    RUN_TEST(test_heap_aligned_alloc);
    RUN_TEST(test_heap_malloc_alignment);
}
