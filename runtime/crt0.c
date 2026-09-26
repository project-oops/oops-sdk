/*
 * The payload crt0 for the console's homebrew loader (elfldr).
 *
 * elfldr maps a plain ET_DYN payload, applies only R_X86_64_RELATIVE, and jumps to the
 * entry with a `payload_args` pointer in rdi. It resolves no imports, so this walks the
 * payload's own relocation tables, fills every import from the target's libraries, and
 * then calls the real entry (obscene#D211).
 *
 * The format facts (the args layout, the 16 KB pages, that only RELATIVE is applied)
 * belong to selfish; the per-firmware vaddrs arrive in the resolution table the caller
 * supplies. The caller links this with the payload objects and a generated
 * `obs_payload_table.c` that defines the symbols declared `extern` below.
 */

/* The resolution table, generated per payload and firmware by selfish. */

struct obs_import {
    const char *name; /* the payload's plain import name */
    unsigned lib;     /* index into obs_lib_paths; 0 is libkernel (base from getpid) */
    unsigned long vaddr; /* the symbol's vaddr within that library */
};
extern const struct obs_import obs_imports[];
extern const unsigned obs_import_count;
extern const char *const obs_lib_paths[]; /* [0] unused; [n] full path to load */
extern const unsigned obs_lib_count;
extern const unsigned long
    obs_getpid_vaddr; /* to turn payload_args[0] into libkernel base */
extern const unsigned long
    obs_loadstart_vaddr; /* sceKernelLoadStartModule, in libkernel */
extern const unsigned long obs_modinfo_vaddr; /* sceKernelGetModuleInfo, in libkernel */

/* ELF dynamic and relocation shapes. */

extern char __ehdr_start[];
extern char _DYNAMIC[];
extern char __bss_start[];
extern char _end[];
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define R_GLOB_DAT 6
#define R_JUMP_SLOT 7
typedef struct {
    long tag;
    unsigned long val;
} Dyn;
typedef struct {
    unsigned long off;
    unsigned long info;
    long addend;
} Rela;
typedef struct {
    unsigned int name;
    unsigned char info;
    unsigned char other;
    unsigned short shndx;
    unsigned long value;
    unsigned long size;
} Sym;

typedef int (*load_t)(const char *, unsigned long, const void *, unsigned int,
                      const void *, int *);
typedef int (*modinfo_t)(int, void *);

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Library runtime bases, filled lazily. Index 0 = libkernel. */
static unsigned long lib_base[16];

static unsigned long resolve_lib_base(unsigned lib, unsigned long kbase) {
    if (lib < 16 && lib_base[lib])
        return lib_base[lib];
    if (lib == 0) {
        if (lib < 16)
            lib_base[0] = kbase;
        return kbase;
    }
    /* Load the module and read its base from GetModuleInfo's segment table. */
    load_t loadstart = (load_t)(kbase + obs_loadstart_vaddr);
    modinfo_t getinfo = (modinfo_t)(kbase + obs_modinfo_vaddr);
    int res = 0;
    int h = loadstart(obs_lib_paths[lib], 0, 0, 0, 0, &res);
    if (h < 0)
        return 0;
    static unsigned char info[1024];
    for (int i = 0; i < 1024; i++)
        info[i] = 0;
    *(unsigned long *)info = 0x160;
    if (getinfo(h, info) != 0)
        return 0;
    unsigned long b = *(unsigned long *)&info[264]; /* first segment base */
    if (lib < 16)
        lib_base[lib] = b;
    return b;
}

static unsigned long resolve_name(const char *name, unsigned long kbase) {
    for (unsigned i = 0; i < obs_import_count; i++) {
        if (streq(name, obs_imports[i].name)) {
            unsigned long b = resolve_lib_base(obs_imports[i].lib, kbase);
            return b ? b + obs_imports[i].vaddr : 0;
        }
    }
    return 0;
}

static void walk(const Rela *r, unsigned long n, unsigned long base, const Sym *symtab,
                 const char *strtab, unsigned long kbase) {
    for (unsigned long i = 0; i < n; i++) {
        unsigned int type = (unsigned int)(r[i].info & 0xffffffffUL);
        if (type != R_JUMP_SLOT && type != R_GLOB_DAT)
            continue;
        unsigned long sym = r[i].info >> 32;
        unsigned long addr;
        if (symtab[sym].shndx != 0)
            addr = base + symtab[sym].value; /* internal */
        else
            addr = resolve_name(strtab + symtab[sym].name, kbase); /* import */
        if (addr)
            *(unsigned long *)(base + r[i].off) = addr;
    }
}

/* The payload's real entry, provided by the payload objects. */
void obs_payload_main(void);

void _start(void);
void _start(void) {
    unsigned long arg0;
    __asm__ volatile("mov %%rdi, %0" : "=r"(arg0));
    unsigned long base = (unsigned long)&__ehdr_start;

    /* Zero the .bss. elfldr maps the LOAD segments but does not zero the region where a
     * segment's MemSiz exceeds its FileSiz, so every zero-initialised static (the
     * library base cache above included) holds garbage until this runs. */
    for (char *p = (char *)&__bss_start; p < (char *)&_end; p++)
        *p = 0;
    unsigned long kbase = ((unsigned long *)arg0)[0] - obs_getpid_vaddr;
    (void)obs_lib_count;

    const Dyn *dyn = (const Dyn *)&_DYNAMIC;
    const Sym *symtab = 0;
    const char *strtab = 0;
    const Rela *jmprel = 0, *rela = 0;
    unsigned long pltsz = 0, relasz = 0;
    for (const Dyn *d = dyn; d->tag != 0; d++) {
        unsigned long p = base + d->val;
        if (d->tag == DT_SYMTAB)
            symtab = (const Sym *)p;
        else if (d->tag == DT_STRTAB)
            strtab = (const char *)p;
        else if (d->tag == DT_JMPREL)
            jmprel = (const Rela *)p;
        else if (d->tag == DT_RELA)
            rela = (const Rela *)p;
        else if (d->tag == DT_PLTRELSZ)
            pltsz = d->val;
        else if (d->tag == DT_RELASZ)
            relasz = d->val;
    }
    if (jmprel && symtab && strtab)
        walk(jmprel, pltsz / sizeof(Rela), base, symtab, strtab, kbase);
    if (rela && symtab && strtab)
        walk(rela, relasz / sizeof(Rela), base, symtab, strtab, kbase);

    obs_payload_main();
    __asm__ volatile("int3");
}
