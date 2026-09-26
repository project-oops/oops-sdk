/*
 * Minimal stub module for /app0/sce_module/libc.prx.
 *
 * Retail rtld requires /app0/sce_module/libc.prx for any Big App (category 0)
 * title before entering the executable entry point (D298, D301).
 *
 * This stub provides:
 * - PT_SCE_MODULE_PARAM (.data.sce_module_param) required by libSceSysmodule
 * - module_start and module_stop entry points
 * - PLT anchor so procedure linkage tables exist
 */

#include <stddef.h>
#include <stdint.h>

struct sce_module_param {
    uint64_t size;
    uint32_t magic;
    uint32_t version;
    uint32_t sdk_version;
    uint32_t sdk_version_second;
    uint64_t flags;
};

__attribute__((section(".data.sce_module_param"),
               used)) static const struct sce_module_param s_mod_param = {
    .size = 0x20,
    .magic = 0x3c13f4bf,
    .version = 0x3,
    .sdk_version = 0x08050001,
    .sdk_version_second = 0x02000009,
    .flags = 0x01,
};

__attribute__((weak)) long sceKernelWrite(int fd, const void *buf, unsigned long len);

int module_start(unsigned long argc, const void *argv);
int module_stop(unsigned long argc, const void *argv);

/*
 * Relocation anchor: forces an entry in .rela.dyn (R_X86_64_RELATIVE)
 * so that mkmodule emits DT_SCE_RELA, DT_SCE_RELASZ, and DT_SCE_RELAENT.
 * Retail rtld preprocess_dt_entries:9632 mandates all 12 tags.
 */
__attribute__((used)) void *const oops_rela_anchor = (void *)module_start;

int module_start(unsigned long argc, const void *argv) {
    if (argc == ~0UL) {
        (void)sceKernelWrite(-1, argv, 0);
    }
    return 0;
}

int module_stop(unsigned long argc, const void *argv) {
    (void)argc;
    (void)argv;
    return 0;
}
