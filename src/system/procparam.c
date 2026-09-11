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
__attribute__((used)) static struct {
  uint64_t size;
  uint8_t rest[OOPS_LIBC_PARAM_SIZE - 8];
} oops_libc_param = {.size = OOPS_LIBC_PARAM_SIZE, .rest = {0}};

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
