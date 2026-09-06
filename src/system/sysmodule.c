#include "oops/sysmodule.h"

__attribute__((weak)) int sceSysmoduleLoadModule(uint16_t id);
__attribute__((weak)) int sceSysmoduleUnloadModule(uint16_t id);
__attribute__((weak)) int sceSysmoduleIsLoaded(uint16_t id);

int oops_sysmodule_load(uint16_t id) {
    if (!sceSysmoduleLoadModule) {
        return -1;
    }
    return sceSysmoduleLoadModule(id);
}

int oops_sysmodule_unload(uint16_t id) {
    if (!sceSysmoduleUnloadModule) {
        return -1;
    }
    return sceSysmoduleUnloadModule(id);
}

int oops_sysmodule_is_loaded(uint16_t id) {
    if (!sceSysmoduleIsLoaded) {
        return -1;
    }
    /* sceSysmoduleIsLoaded returns 0 when loaded, or negative error / positive when not */
    return (sceSysmoduleIsLoaded(id) == 0) ? 1 : 0;
}

