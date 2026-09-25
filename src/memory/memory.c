#include "oops/memory.h"
#include "oops/system.h"

typedef int64_t sce_off_t;

struct obs_batch_map_entry {
  void *vaddr;
  sce_off_t paddr;
  size_t len;
  uint8_t prot;
  uint8_t pad[3];
  uint32_t flags;
};

__attribute__((weak)) int sceKernelAllocateMainDirectMemory(size_t len,
                                                            size_t alignment,
                                                            int memoryType,
                                                            sce_off_t *paddr);
__attribute__((weak)) int
sceKernelAllocateDirectMemory(sce_off_t searchStart, sce_off_t searchEnd,
                              size_t len, size_t alignment, int memoryType,
                              sce_off_t *paddr);
/* size_t, not int: the direct memory pool is larger than 2 GB, and an int
 * prototype truncates the search end that bounds the fallback allocation. */
__attribute__((weak)) size_t sceKernelGetDirectMemorySize(void);
__attribute__((weak)) int sceKernelReleaseDirectMemory(sce_off_t paddr,
                                                       size_t len);
__attribute__((weak)) int sceKernelMapDirectMemory(void **addr, size_t len,
                                                   int prot, int flags,
                                                   sce_off_t directMemoryStart,
                                                   size_t alignment);
__attribute__((weak)) int sceKernelBatchMap(struct obs_batch_map_entry *entries,
                                            int num_entries, int *completed);
__attribute__((weak)) int sceKernelReserveVirtualRange(void **addr, size_t len,
                                                       int flags,
                                                       size_t alignment);
__attribute__((weak)) int sceKernelMunmap(void *addr, size_t len);

int oops_mem_alloc_direct(size_t size, size_t alignment, oops_mem_type_t type,
                          int64_t *out_phys) {
  if (!out_phys || size == 0)
    return -1;
  oops_log_trace("MEM", "alloc direct: size=%zu align=0x%zx type=%d", size, alignment, (int)type);
  sce_off_t p = 0;
  int rc = -1;
  if (sceKernelAllocateMainDirectMemory) {
    rc = sceKernelAllocateMainDirectMemory(size, alignment, (int)type, &p);
  } else if (sceKernelAllocateDirectMemory && sceKernelGetDirectMemorySize) {
    rc = sceKernelAllocateDirectMemory(
        0, (sce_off_t)sceKernelGetDirectMemorySize(), size, alignment,
        (int)type, &p);
  }
  if (rc == 0) {
    *out_phys = (int64_t)p;
    oops_log_debug("MEM", "alloc direct ok: phys=0x%llx size=%zu", (unsigned long long)p, size);
  } else {
    oops_log_warn("MEM", "alloc direct failed: rc=%d size=%zu type=%d", rc, size, (int)type);
  }
  return rc;
}

int oops_mem_free_direct(int64_t phys, size_t size) {
  oops_log_trace("MEM", "free direct: phys=0x%llx size=%zu", (unsigned long long)phys, size);
  if (!sceKernelReleaseDirectMemory) {
    oops_log_warn("MEM", "sceKernelReleaseDirectMemory symbol not found");
    return -1;
  }
  int rc = sceKernelReleaseDirectMemory((sce_off_t)phys, size);
  if (rc != 0) {
    oops_log_warn("MEM", "free direct failed: rc=%d phys=0x%llx", rc, (unsigned long long)phys);
  }
  return rc;
}

int oops_mem_map_direct(void **out_vaddr, size_t size, int prot, int flags,
                        int64_t phys, size_t alignment) {
  if (!out_vaddr || !sceKernelMapDirectMemory)
    return -1;
  oops_log_trace("MEM", "map direct: phys=0x%llx size=%zu prot=0x%x flags=0x%x align=0x%zx",
                 (unsigned long long)phys, size, prot, flags, alignment);
  void *v = *out_vaddr;
  int rc = sceKernelMapDirectMemory(&v, size, prot, flags, (sce_off_t)phys,
                                    alignment);
  if (rc == 0) {
    *out_vaddr = v;
    oops_log_debug("MEM", "map direct ok: vaddr=%p phys=0x%llx size=%zu", v, (unsigned long long)phys, size);
  } else {
    oops_log_warn("MEM", "map direct failed: rc=%d phys=0x%llx size=%zu", rc, (unsigned long long)phys, size);
  }
  return rc;
}

int oops_mem_reserve_va(void **addr_inout, size_t len, int flags,
                        size_t alignment) {
  if (!addr_inout || len == 0 || !sceKernelReserveVirtualRange)
    return -1;
  oops_log_trace("MEM", "reserve VA: len=%zu flags=0x%x align=0x%zx", len, flags, alignment);
  int rc = sceKernelReserveVirtualRange(addr_inout, len, flags, alignment);
  if (rc != 0) {
    oops_log_warn("MEM", "reserve VA failed: rc=%d len=%zu", rc, len);
  } else {
    oops_log_debug("MEM", "reserve VA ok: addr=%p len=%zu", *addr_inout, len);
  }
  return rc;
}

int oops_mem_release_va(void *vaddr, size_t len) {
  if (!vaddr || len == 0 || !sceKernelMunmap)
    return -1;
  oops_log_trace("MEM", "release VA: vaddr=%p len=%zu", vaddr, len);
  int rc = sceKernelMunmap(vaddr, len);
  if (rc != 0) {
    oops_log_warn("MEM", "release VA failed: rc=%d vaddr=%p len=%zu", rc, vaddr, len);
  }
  return rc;
}

int oops_mem_batch_map(void *vaddr_base, int64_t phys_base, size_t total_size,
                       size_t page_size, uint8_t prot) {
  if (!sceKernelBatchMap || !vaddr_base || page_size == 0 || total_size == 0)
    return -1;
  oops_log_trace("MEM", "batch map: vaddr=%p phys=0x%llx total_size=%zu page_size=%zu prot=0x%x",
                 vaddr_base, (unsigned long long)phys_base, total_size, page_size, (unsigned)prot);
  size_t total_pages = (total_size + page_size - 1) / page_size;
  size_t page_idx = 0;

  while (page_idx < total_pages) {
    size_t batch = total_pages - page_idx;
    if (batch > 64)
      batch = 64;

    struct obs_batch_map_entry entries[64];
    for (size_t i = 0; i < batch; i++) {
      size_t p = page_idx + i;
      entries[i].vaddr = (void *)((uintptr_t)vaddr_base + p * page_size);
      entries[i].paddr =
          (sce_off_t)(phys_base + (int64_t)p * (int64_t)page_size);
      entries[i].len = page_size;
      entries[i].prot = prot;
      entries[i].flags = 0;
    }

    int completed = 0;
    int rc = sceKernelBatchMap(entries, (int)batch, &completed);
    if (rc != 0) {
      oops_log_warn("MEM", "batch map call failed: rc=%d", rc);
      return rc;
    }
    /* The kernel says how many entries it mapped. Fewer than asked is a failure
     * even when the call itself returned 0: the rest of the range is not there.
     */
    if (completed != (int)batch) {
      oops_log_warn("MEM", "batch map incomplete: completed %d of %zu", completed, batch);
      return -1;
    }
    page_idx += batch;
  }
  return 0;
}

int oops_mem_unmap(void *vaddr, size_t size) {
  if (!sceKernelMunmap || !vaddr)
    return -1;
  oops_log_trace("MEM", "unmap: vaddr=%p size=%zu", vaddr, size);
  int rc = sceKernelMunmap(vaddr, size);
  if (rc != 0) {
    oops_log_warn("MEM", "unmap failed: rc=%d vaddr=%p size=%zu", rc, vaddr, size);
  }
  return rc;
}

/* Allocation tracking for high-level allocator. Not thread-safe: a caller that
 * allocates from more than one thread serialises these calls itself. */
#define OOPS_MAX_ALLOCS 512

struct oops_alloc_slot {
  void *vaddr;
  int64_t phys;
  size_t size;
  oops_mem_type_t type;
  int in_use;
};

static struct oops_alloc_slot s_alloc_slots[OOPS_MAX_ALLOCS];

void *oops_mem_alloc(size_t size, size_t alignment, oops_mem_type_t type) {
  if (size == 0)
    return NULL;

  /* Align to standard page boundary (64KB); a size that would wrap when rounded
   * is refused. */
  size_t page_mask = 0xFFFF;
  if (size > SIZE_MAX - page_mask)
    return NULL;
  size_t aligned_size = (size + page_mask) & ~page_mask;
  size_t align = (alignment < 0x10000) ? 0x10000 : alignment;

  oops_log_trace("MEM", "oops_mem_alloc: req_size=%zu aligned_size=%zu align=0x%zx type=%d",
                 size, aligned_size, align, (int)type);

  /* Find an available tracking slot */
  int slot_idx = -1;
  for (int i = 0; i < OOPS_MAX_ALLOCS; i++) {
    if (!s_alloc_slots[i].in_use) {
      slot_idx = i;
      break;
    }
  }
  if (slot_idx < 0) {
    oops_log_warn("MEM", "oops_mem_alloc: out of tracking slots (max %d)", OOPS_MAX_ALLOCS);
    return NULL;
  }

  int64_t phys = 0;
  int rc = oops_mem_alloc_direct(aligned_size, align, type, &phys);
  if (rc != 0) {
    oops_log_warn("MEM", "oops_mem_alloc: failed to allocate direct memory");
    return NULL;
  }

  void *vaddr = NULL;
  int prot = OOPS_PROT_CPU_RW | OOPS_PROT_GPU_RW;
  rc = oops_mem_map_direct(&vaddr, aligned_size, prot, 0, phys, align);
  if (rc != 0 || !vaddr) {
    oops_log_warn("MEM", "oops_mem_alloc: failed to map direct memory");
    oops_mem_free_direct(phys, aligned_size);
    return NULL;
  }

  s_alloc_slots[slot_idx].vaddr = vaddr;
  s_alloc_slots[slot_idx].phys = phys;
  s_alloc_slots[slot_idx].size = aligned_size;
  s_alloc_slots[slot_idx].type = type;
  s_alloc_slots[slot_idx].in_use = 1;

  oops_log_debug("MEM", "oops_mem_alloc ok: slot=%d vaddr=%p phys=0x%llx size=%zu",
                 slot_idx, vaddr, (unsigned long long)phys, aligned_size);
  return vaddr;
}

void oops_mem_free(void *ptr) {
  if (!ptr)
    return;

  oops_log_trace("MEM", "oops_mem_free: ptr=%p", ptr);

  for (int i = 0; i < OOPS_MAX_ALLOCS; i++) {
    if (s_alloc_slots[i].in_use && s_alloc_slots[i].vaddr == ptr) {
      oops_log_debug("MEM", "oops_mem_free releasing slot=%d vaddr=%p size=%zu",
                     i, ptr, s_alloc_slots[i].size);
      oops_mem_unmap(s_alloc_slots[i].vaddr, s_alloc_slots[i].size);
      oops_mem_free_direct(s_alloc_slots[i].phys, s_alloc_slots[i].size);
      s_alloc_slots[i].vaddr = NULL;
      s_alloc_slots[i].phys = 0;
      s_alloc_slots[i].size = 0;
      s_alloc_slots[i].in_use = 0;
      return;
    }
  }

  oops_log_warn("MEM", "oops_mem_free: ptr %p not found in tracking table", ptr);
}

int64_t oops_mem_get_phys(const void *ptr) {
  if (!ptr)
    return -1;
  uintptr_t addr = (uintptr_t)ptr;

  for (int i = 0; i < OOPS_MAX_ALLOCS; i++) {
    if (s_alloc_slots[i].in_use) {
      uintptr_t base = (uintptr_t)s_alloc_slots[i].vaddr;
      if (addr >= base && addr < base + s_alloc_slots[i].size) {
        return s_alloc_slots[i].phys + (int64_t)(addr - base);
      }
    }
  }
  /* Not inside a managed allocation. 0 is a valid physical offset, so it cannot
   * mean this. */
  return -1;
}
