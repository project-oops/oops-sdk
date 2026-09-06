/* installer.c - install a local package through the *ungated* path.
 *
 * shsrv's `pkg_install` calls `sceAppInstUtilInstallByPackage`, gated by a console's PlayGo
 * HTTP pre-flight (the `0x80b211c8` a correct fake package hits). `sceAppInstUtilAppInstallPkg`
 * takes a bare local path, parses no URI and runs no HTTP pre-flight, so it does not reach that
 * gate - the path Itemzflow and elf-arsenal install through.
 *
 * The library is loaded and its functions resolved **by hand** rather than declared as imports,
 * because obSCEne's crt0 auto-load of `libSceAppInstUtil.sprx` failed (rtld "loadability error
 * 13") before its three dependencies - `libSceLibcInternal`, `libSceRegMgr`, `libSceIpmi` - were
 * present. Loading those first, in dependency order, then the library itself, is what a proper
 * rtld does for a DT_NEEDED chain; done here explicitly.
 */
extern int sceKernelLoadStartModule(const char *path, unsigned long argc, const void *argv,
                                    unsigned int flags, const void *opt, int *res);
extern int sceKernelDlsym(int handle, const char *symbol, void **out);
extern int sceKernelDebugOutText(int channel, const char *msg);

static void say(const char *s) {
    sceKernelDebugOutText(0, s);
}

/* Freestanding: "<label>0x<16 hex>\n", by hand. */
static void say_hex(const char *label, unsigned long value) {
    char buf[96];
    int n = 0;
    for (const char *p = label; *p; ++p) {
        buf[n++] = *p;
    }
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned nybble = (unsigned)((value >> shift) & 0xF);
        buf[n++] = nybble < 10 ? (char)('0' + nybble) : (char)('a' + nybble - 10);
    }
    buf[n++] = '\n';
    buf[n] = 0;
    say(buf);
}

static int load_lib(const char *path) {
    int res = 0;
    int handle = sceKernelLoadStartModule(path, 0, 0, 0, 0, &res);
    say(path);
    say_hex("  -> handle=", (unsigned long)(unsigned int)handle);
    return handle;
}


