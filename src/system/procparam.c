/*
 * Native process parameters and SDK versioning for Prospero / Orbis.
 *
 * Populates the .sce_process_param section mapped to PT_SCE_PROCPARAM (0x61000001).
 * Hardware-verified layout measured on Prospero FW 12.40:
 * - Magic: "ORBI" (0x4942524f)
 * - Size: 0x60 bytes, 5 entries
 * - Non-null libc_param (0xa8 bytes) and mem_param (0x38 bytes)
 */

#include <stdint.h>
#include <stddef.h>

#define OOPS_PROC_PARAM_MAGIC 0x4942524FU /* "ORBI" */
#define OOPS_PROC_PARAM_SIZE 0x60
#define OOPS_PROC_PARAM_ENTRIES 5

#define OOPS_LIBC_PARAM_SIZE 0xA8
#define OOPS_MEM_PARAM_SIZE 0x38
#define OOPS_THIRD_PARAM_SIZE 0x10

#ifndef OOPS_PROC_PARAM_SDK_ORBIS
#define OOPS_PROC_PARAM_SDK_ORBIS 0x08050001U
#endif

#ifndef OOPS_PROC_PARAM_SDK_PROSPERO
#define OOPS_PROC_PARAM_SDK_PROSPERO 0x02000009U
#endif

/*
 * libc_param, mem_param, third_param must reside in mutable data (.data),
 * because libkernel writes to *(libc_param + 0x28) during initialization.
 * A null pointer produces an immediate SIGSEGV at address 0x0000000000000028.
 */

/*
 * The libc heap, sized for a real workload rather than the fallback.
 *
 * Left all-zero, this structure put `libSceLibcInternal` in its internal-memory
 * fallback: a static ~8-13 MiB heap that ignores the application's parameters. Mesa
 * exhausted it - a title that brings GL up through the DRI frontend crashed in
 * `driConcatConfigs` on a `malloc` that returned NULL, and a heap-ceiling probe could
 * not even allocate one 8 MiB block (oops-mesa worklog 050).
 *
 * The fields below select **Application Heap Mode** (`mode = 0`, `version = 14`) and
 * point libc at a grow-on-demand heap: `heap_size = UINT64_MAX` (up to the container
 * budget, ~432-448 MiB measured on FW 12.40) and `extended_alloc = 1`. Every
 * configuration entry is a *pointer* to its value, not the value inline, populated by
 * link-time relocations - which is why the values are separate mutable statics.
 * `init_alloc` is written to by libkernel during init, so it too is mutable.
 *
 * Layout, offsets, sentinels and the two trailing parameter blocks are the
 * authoritative reading of the 0xA8-byte `libc_param` from obSCEne
 * REQ-20260919T1335Z-7b90 (retail `AgcCompositor.elf` PT_SCE_PROCPARAM analysis,
 * obscene#D298, and a measured 432-448 MiB budget). The old "size-only, rest zeroed"
 * form is what that request diagnosed as the fallback trigger.
 */
static uint64_t oops_libc_heap_size =
    0xFFFFFFFFFFFFFFFFULL;                         /* grow to container capacity */
static uint32_t oops_libc_heap_extended_alloc = 1; /* enable grow-on-demand */
static uint64_t oops_libc_heap_init_alloc =
    0x80000; /* 512 KiB; libkernel writes here */
static const uint32_t oops_libc_alloc_settings[2] = {4, 1};

__attribute__((used)) static struct {
    uint64_t size;
    uint32_t version;
    uint8_t rest[0x78 - 12];
} oops_param_block_4 = {.size = 0x78, .version = 2, .rest = {0}};

__attribute__((used)) static struct {
    uint64_t size;
    uint32_t version;
    uint8_t rest[0xC0 - 12];
} oops_param_block_5 = {.size = 0xC0, .version = 3, .rest = {0}};

__attribute__((used)) static struct {
    uint64_t size;
    uint32_t version;
    uint32_t mode;
    const uint64_t *heap_size;
    uint64_t reserved_18;
    const uint32_t *extended_alloc;
    uint64_t *init_alloc;
    void *param_block_4;
    void *param_block_5;
    uint64_t reserved_40;
    void *module_anchor;
    uint64_t reserved_50[4];
    const void *alloc_settings;
    uint64_t reserved_78[6];
} oops_libc_param = {
    .size = OOPS_LIBC_PARAM_SIZE,
    .version = 14,
    .mode = 0, /* 0 = Application Heap Mode; 1 = internal-memory fallback */
    .heap_size = &oops_libc_heap_size,
    .reserved_18 = 0,
    .extended_alloc = &oops_libc_heap_extended_alloc,
    .init_alloc = &oops_libc_heap_init_alloc,
    .param_block_4 = &oops_param_block_4,
    .param_block_5 = &oops_param_block_5,
    .reserved_40 = 0,
    .module_anchor = NULL,
    .reserved_50 = {0},
    .alloc_settings = oops_libc_alloc_settings,
    .reserved_78 = {0},
};
_Static_assert(sizeof(oops_libc_param) == OOPS_LIBC_PARAM_SIZE,
               "oops_libc_param size mismatch");

__attribute__((used)) static struct {
    uint64_t size;
    uint8_t rest[OOPS_MEM_PARAM_SIZE - 8];
} oops_mem_param = {.size = OOPS_MEM_PARAM_SIZE, .rest = {0}};

__attribute__((used)) static struct {
    uint64_t size;
    uint8_t rest[OOPS_THIRD_PARAM_SIZE - 8];
} oops_third_param = {.size = OOPS_THIRD_PARAM_SIZE, .rest = {0}};

__attribute__((used, section(".sce_process_param"))) static const struct {
    uint64_t size;
    uint32_t magic;
    uint32_t entry_count;
    uint32_t sdk_version;
    uint32_t sdk_version_second;
    uint64_t unknown[4];
    const void *libc_param;
    const void *mem_param;
    const void *third_param;
    uint64_t trailing[2];
} oops_process_param = {
    .size = OOPS_PROC_PARAM_SIZE,
    .magic = OOPS_PROC_PARAM_MAGIC,
    .entry_count = OOPS_PROC_PARAM_ENTRIES,
    .sdk_version = OOPS_PROC_PARAM_SDK_ORBIS,
    .sdk_version_second = OOPS_PROC_PARAM_SDK_PROSPERO,
    .unknown = {0},
    .libc_param = &oops_libc_param,
    .mem_param = &oops_mem_param,
    .third_param = &oops_third_param,
    .trailing = {0},
};
