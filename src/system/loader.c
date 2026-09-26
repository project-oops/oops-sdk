/*
 * In-memory ELF Loader Implementation.
 *
 * Maps ELF64 segments into a target process, applies relocations,
 * and sets memory protections.
 */

#include "oops/inject.h"
/* procctl.h in oops/inject.h */
/* injector.h replaced by oops/krw.h */
#include "oops/freestd.h"
#include "oops/krw.h"
/* nid_table.gen.h unused */

#define PAGE_SIZE 0x4000UL /* 16 KiB console page granularity */
#define ROUND_PG(x) (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))
#define TRUNC_PG(x) ((x) & ~(PAGE_SIZE - 1))

#define EI_NIDENT 16
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define SHT_RELA 4
#define SHT_DYNSYM 11
#define SHT_STRTAB 3
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLO 7
#define R_X86_64_RELATIVE 8
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xffffffffUL))
#define ELF64_R_SYM(i) ((uint32_t)((i) >> 32))

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} Elf64_Rela;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} Elf64_Dyn;

int loader_validate_elf(const uint8_t *elf_data, size_t elf_size) {
    if (elf_data == NULL || elf_size < sizeof(Elf64_Ehdr)) {
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        return -1;
    }

    if (ehdr->e_ident[4] != 2) { /* ELFCLASS64 */
        return -1;
    }
    if (ehdr->e_machine != EM_X86_64) {
        return -1;
    }
    if (ehdr->e_type != ET_DYN && ehdr->e_type != ET_EXEC) {
        return -1;
    }

    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > elf_size) {
        return -1;
    }

    return 0;
}

static int to_proc_prot(uint32_t p_flags) {
    int prot = 0;
    if (p_flags & PF_R)
        prot |= PROC_PROT_READ;
    if (p_flags & PF_W)
        prot |= PROC_PROT_WRITE;
    if (p_flags & PF_X)
        prot |= PROC_PROT_EXEC;
    return prot;
}

uintptr_t loader_load_into_proc(pid_t pid, const uint8_t *elf_data, size_t elf_size,
                                uintptr_t target_libkernel_base,
                                const obs_kexport_table_t *kexport_table,
                                uintptr_t *out_base_addr, size_t *out_base_size) {
    if (loader_validate_elf(elf_data, elf_size) != 0) {
        return 0;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(elf_data + ehdr->e_phoff);

    uint64_t min_vaddr = ~(uint64_t)0;
    uint64_t max_vaddr = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }
        if (phdrs[i].p_vaddr < min_vaddr) {
            min_vaddr = phdrs[i].p_vaddr;
        }
        if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) {
            max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        }
    }

    if (min_vaddr > max_vaddr) {
        return 0;
    }

    min_vaddr = TRUNC_PG(min_vaddr);
    max_vaddr = ROUND_PG(max_vaddr);
    size_t total_size = (size_t)(max_vaddr - min_vaddr);

    klog_write_hex("loader: ELF entry=", ehdr->e_entry);
    klog_write_num("loader: ELF phnum=", (int64_t)ehdr->e_phnum);
    klog_write_hex("loader: total_size=", (uint64_t)total_size);

    /* Allocate virtual address space in target process */
    uintptr_t target_base =
        procctl_remote_mmap(pid, (ehdr->e_type == ET_EXEC) ? min_vaddr : 0, total_size,
                            PROC_PROT_READ | PROC_PROT_WRITE,
                            PROC_MAP_ANONYMOUS | PROC_MAP_PRIVATE |
                                ((ehdr->e_type == ET_EXEC) ? PROC_MAP_FIXED : 0),
                            -1, 0);

    klog_write_hex("loader: target_base=", target_base);
    if (target_base == 0) {
        return 0;
    }

    /* Copy PT_LOAD segments */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) {
            continue;
        }

        uintptr_t seg_dest = target_base + phdrs[i].p_vaddr;
        klog_write_num("  mapping seg ", (int64_t)i);
        klog_write_hex("    dest=", seg_dest);
        klog_write_hex("    filesz=", phdrs[i].p_filesz);
        klog_write_hex("    memsz=", phdrs[i].p_memsz);

        if (phdrs[i].p_filesz > 0) {
            if (procctl_copyin(pid, elf_data + phdrs[i].p_offset, seg_dest,
                               phdrs[i].p_filesz) != 0) {
                procctl_remote_munmap(pid, target_base, total_size);
                return 0;
            }
        }

        /* Zero remaining BSS */
        if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
            size_t bss_size = (size_t)(phdrs[i].p_memsz - phdrs[i].p_filesz);
            uintptr_t bss_dest = seg_dest + phdrs[i].p_filesz;
            static const uint8_t zeros[512] = {0};
            while (bss_size > 0) {
                size_t chunk = (bss_size > sizeof(zeros)) ? sizeof(zeros) : bss_size;
                procctl_copyin(pid, zeros, bss_dest, chunk);
                bss_dest += chunk;
                bss_size -= chunk;
            }
        }
    }

    /* Apply relocations (from SHT_RELA sections if present) */
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0 &&
        (ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(Elf64_Shdr) <= elf_size)) {
        const Elf64_Shdr *shdrs = (const Elf64_Shdr *)(elf_data + ehdr->e_shoff);
        const Elf64_Shdr *dynsym_sh = NULL;
        const Elf64_Shdr *dynstr_sh = NULL;
        for (int i = 0; i < ehdr->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_DYNSYM) {
                dynsym_sh = &shdrs[i];
                if (dynsym_sh->sh_link < ehdr->e_shnum) {
                    dynstr_sh = &shdrs[dynsym_sh->sh_link];
                }
                break;
            }
        }
        const Elf64_Sym *syms =
            (dynsym_sh != NULL) ? (const Elf64_Sym *)(elf_data + dynsym_sh->sh_offset)
                                : NULL;
        const char *dynstr = (dynstr_sh != NULL)
                                 ? (const char *)(elf_data + dynstr_sh->sh_offset)
                                 : NULL;

        for (int i = 0; i < ehdr->e_shnum; i++) {
            if (shdrs[i].sh_type != SHT_RELA) {
                continue;
            }

            const Elf64_Rela *relas =
                (const Elf64_Rela *)(elf_data + shdrs[i].sh_offset);
            size_t num_relas = (size_t)(shdrs[i].sh_size / sizeof(Elf64_Rela));

            for (size_t j = 0; j < num_relas; j++) {
                uint32_t r_type = ELF64_R_TYPE(relas[j].r_info);
                uint32_t sym_idx = ELF64_R_SYM(relas[j].r_info);

                if (r_type == R_X86_64_RELATIVE) {
                    uintptr_t loc = target_base + (uintptr_t)relas[j].r_offset;
                    uint64_t val =
                        (uint64_t)(target_base + (uintptr_t)relas[j].r_addend);
                    procctl_setlong(pid, loc, val);
                } else if (syms != NULL && dynstr != NULL && sym_idx > 0) {
                    const char *sname = dynstr + syms[sym_idx].st_name;
                    uintptr_t sym_val = 0;
                    if (kexport_table != NULL) {
                        char nid[12];
                        obs_compute_nid(sname, nid);
                        const void *kaddr = obs_kexport_lookup(kexport_table, nid);
                        if (kaddr != NULL) {
                            sym_val = (uintptr_t)kaddr;
                        }
                    }
                    if (sym_val == 0 && target_libkernel_base != 0) {
                        if (obs_strcmp(sname, "getpid") == 0) {
                            sym_val = target_libkernel_base + 0x5b0;
                        } else if (obs_strcmp(sname, "sceKernelOpen") == 0) {
                            sym_val = target_libkernel_base + 0x16e30;
                        } else if (obs_strcmp(sname, "sceKernelClose") == 0) {
                            sym_val = target_libkernel_base + 0x16e90;
                        } else if (obs_strcmp(sname, "sceKernelRead") == 0) {
                            sym_val = target_libkernel_base + 0x16dd0;
                        } else if (obs_strcmp(sname, "sceKernelWrite") == 0) {
                            sym_val = target_libkernel_base + 0x16e00;
                        } else if (obs_strcmp(sname, "sceKernelUsleep") == 0) {
                            sym_val = target_libkernel_base + 0x15ff0;
                        }
                    }
                    if (sym_val != 0) {
                        uintptr_t loc = target_base + (uintptr_t)relas[j].r_offset;
                        uint64_t val =
                            (uint64_t)(sym_val + (uintptr_t)relas[j].r_addend);
                        procctl_setlong(pid, loc, val);
                    }
                }
            }
        }
    } else {
        /* Fallback: extract relocations from PT_DYNAMIC */
        uintptr_t rela_vaddr = 0;
        size_t rela_size = 0;
        size_t rela_ent = sizeof(Elf64_Rela);

        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type != PT_DYNAMIC) {
                continue;
            }
            const Elf64_Dyn *dyns = (const Elf64_Dyn *)(elf_data + phdrs[i].p_offset);
            size_t dyn_count = (size_t)(phdrs[i].p_filesz / sizeof(Elf64_Dyn));

            for (size_t j = 0; j < dyn_count; j++) {
                if (dyns[j].d_tag == DT_NULL)
                    break;
                if (dyns[j].d_tag == DT_RELA)
                    rela_vaddr = (uintptr_t)dyns[j].d_val;
                if (dyns[j].d_tag == DT_RELASZ)
                    rela_size = (size_t)dyns[j].d_val;
                if (dyns[j].d_tag == DT_RELAENT)
                    rela_ent = (size_t)dyns[j].d_val;
            }
        }

        if (rela_vaddr != 0 && rela_size > 0) {
            size_t count = rela_size / rela_ent;
            for (size_t j = 0; j < count; j++) {
                Elf64_Rela rela;
                if (procctl_copyout(pid, target_base + rela_vaddr + (j * rela_ent),
                                    &rela, sizeof(rela)) == 0) {
                    if (ELF64_R_TYPE(rela.r_info) == R_X86_64_RELATIVE) {
                        uintptr_t loc = target_base + (uintptr_t)rela.r_offset;
                        uint64_t val =
                            (uint64_t)(target_base + (uintptr_t)rela.r_addend);
                        procctl_setlong(pid, loc, val);
                    }
                }
            }
        }
    }

    /* Apply segment memory protections */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) {
            continue;
        }

        uintptr_t seg_start = target_base + TRUNC_PG(phdrs[i].p_vaddr);
        size_t seg_size = (size_t)(ROUND_PG(phdrs[i].p_vaddr + phdrs[i].p_memsz) -
                                   TRUNC_PG(phdrs[i].p_vaddr));
        int prot = to_proc_prot(phdrs[i].p_flags);

        procctl_remote_mprotect(pid, seg_start, seg_size, prot);
    }

    if (out_base_addr != NULL) {
        *out_base_addr = target_base;
    }
    if (out_base_size != NULL) {
        *out_base_size = total_size;
    }

    return target_base + (uintptr_t)ehdr->e_entry;
}
