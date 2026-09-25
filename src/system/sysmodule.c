#include "oops/sysmodule.h"
#include "oops/system.h"

__attribute__((weak)) int sceSysmoduleLoadModule(uint16_t id);
__attribute__((weak)) int sceSysmoduleUnloadModule(uint16_t id);
__attribute__((weak)) int sceSysmoduleIsLoaded(uint16_t id);

int oops_sysmodule_load(uint16_t id) {
  if (!sceSysmoduleLoadModule) {
    oops_log_warn("SYSMOD", "load(0x%04x): sceSysmoduleLoadModule entry point unavailable", id);
    return -1;
  }
  int rc = sceSysmoduleLoadModule(id);
  if (rc == 0) {
    oops_log_info("SYSMOD", "loaded module 0x%04x successfully", id);
  } else {
    oops_log_debug("SYSMOD", "load module 0x%04x returned 0x%08x", id, rc);
  }
  return rc;
}

int oops_sysmodule_unload(uint16_t id) {
  if (!sceSysmoduleUnloadModule) {
    oops_log_warn("SYSMOD", "unload(0x%04x): sceSysmoduleUnloadModule unavailable", id);
    return -1;
  }
  int rc = sceSysmoduleUnloadModule(id);
  oops_log_info("SYSMOD", "unloaded module 0x%04x (rc=0x%08x)", id, rc);
  return rc;
}

int oops_sysmodule_is_loaded(uint16_t id) {
  if (!sceSysmoduleIsLoaded) {
    return -1;
  }
  /* sceSysmoduleIsLoaded returns 0 when loaded, or negative error / positive
   * when not */
  int loaded = (sceSysmoduleIsLoaded(id) == 0) ? 1 : 0;
  oops_log_trace("SYSMOD", "is_loaded(0x%04x) -> %d", id, loaded);
  return loaded;
}
