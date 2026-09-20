/*
 * zs-tiling: where addrlib puts each pixel of oops-gl's depth and stencil surfaces, and whether a
 * handful of basis vectors say it exactly.
 *
 * # The question
 *
 * oops-gl's depth surface (Z_32_FLOAT) and stencil surface (STENCIL_8) are 64KB_Z_X - DB_Z_INFO's
 * and DB_STENCIL_INFO's SW_MODE 24 - so the CPU cannot read or write them as rows. glReadPixels of
 * depth, glDrawPixels of stencil, a depth texture copied from the frame: every pixel operation on
 * either was refused on the console for that reason until this tool's vectors went into
 * gl_zs_tiling.h (2026-09-19). Serving them needs each pixel's byte offset in the tiled surface.
 *
 * # Where the answer comes from
 *
 * Mesa's own addrlib, under the chip identity this collection reports and the GB_ADDR_CONFIG
 * oops-mesa derived by inverting addrlib against the display tiler (`drm_device.c`,
 * OOPS_GB_ADDR_CONFIG, worklog 017) - the value `tools/tiling-compare` in oops-mesa shows puts a
 * 64KB_R_X surface exactly where the hardware-verified display tiler does. The surfaces are set
 * up as `ac_surface.c` sets up a depth-stencil pair on this generation (ac_surface.c:1591,
 * :2894-2911): depth with `flags.depth`, 32 bits a pixel; stencil with `flags.stencil`, 8 bits,
 * in the depth surface's swizzle mode. pipeBankXor 0: oops-gl's bases are 64 KiB aligned and it
 * programs no surface XOR.
 *
 * # What is checked
 *
 * 1. That addrlib's block is the size oops-gl allocates by (128 x 128 at 32 bits, 256 x 256 at 8).
 * 2. **That each block is a linear map**: every pixel's offset is the block's base plus the XOR of
 *    one basis vector per set bit of its in-block x and y - the form the display tiler takes, and
 *    the only form cheap enough to use per pixel. Checked over every pixel of a 3 x 2 block
 *    surface, so that blocks are known to be laid out row by row, not merely assumed.
 * 3. **A control**: the same under eight pipes instead of sixteen must produce a different layout,
 *    or the comparison could not tell a right configuration from a wrong one.
 *
 * The output is tracked (zs_tiling_gfx1013.txt) and the vectors in it are what
 * oops-sdk's src/gl/gl_zs_tiling.h carries.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "addrinterface.h"

#ifndef CIASICIDGFXENGINE_ARCTICISLAND
#define CIASICIDGFXENGINE_ARCTICISLAND 0x0000000D
#endif

static const unsigned kChipFamily = 0x8F;   /* FAMILY_NV, device_info.c's */
static const unsigned kChipRevision = 0x82; /* first of AMDGPU_GFX1013_RANGE */
static const unsigned kGbAddrConfig = 0x00000004u;      /* oops-mesa drm_device.c: 16 pipes */
static const unsigned kGbAddrConfigControl = 0x00000003u; /* 8 pipes */

static void *alloc_sysmem(const ADDR_ALLOCSYSMEM_INPUT *in) { return malloc(in->sizeInBytes); }
static ADDR_E_RETURNCODE free_sysmem(const ADDR_FREESYSMEM_INPUT *in)
{
    free(in->pVirtAddr);
    return ADDR_OK;
}

static ADDR_HANDLE create(unsigned gb_addr_config)
{
    ADDR_CREATE_INPUT ci = {};
    ADDR_CREATE_OUTPUT co = {};
    ADDR_REGISTER_VALUE rv = {};
    ci.size = sizeof(ci);
    co.size = sizeof(co);
    rv.gbAddrConfig = gb_addr_config;
    ci.chipFamily = kChipFamily;
    ci.chipRevision = kChipRevision;
    ci.chipEngine = CIASICIDGFXENGINE_ARCTICISLAND;
    ci.regValue = rv;
    ci.callbacks.allocSysMem = alloc_sysmem;
    ci.callbacks.freeSysMem = free_sysmem;
    if (AddrCreate(&ci, &co) != ADDR_OK) return nullptr;
    return co.hLib;
}

struct Surf {
    const char *name;
    unsigned bpp;
    bool stencil;
};

static void fill_flags(ADDR2_SURFACE_FLAGS *f, const Surf &s)
{
    if (s.stencil) f->stencil = 1;
    else f->depth = 1;
}

/* One pixel's byte offset, or ~0 if addrlib refused. */
static uint64_t addr_of(ADDR_HANDLE lib, const Surf &s, unsigned w, unsigned h, unsigned pitch,
                        unsigned x, unsigned y)
{
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in = {};
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.x = x;
    in.y = y;
    in.swizzleMode = ADDR_SW_64KB_Z_X;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.bpp = s.bpp;
    in.unalignedWidth = w;
    in.unalignedHeight = h;
    in.numSlices = 1;
    in.numMipLevels = 1;
    in.numSamples = 1;
    in.numFrags = 1;
    in.pipeBankXor = 0;
    in.pitchInElement = pitch;
    fill_flags(&in.flags, s);
    if (Addr2ComputeSurfaceAddrFromCoord(lib, &in, &out) != ADDR_OK) return ~0ull;
    return out.addr;
}

struct Result {
    bool ok;
    unsigned blk_w, blk_h, pitch, height;
    uint64_t size;
    std::vector<uint32_t> xb, yb;
    size_t checked, mismatched;
};

static Result measure(unsigned gb, const Surf &s)
{
    Result r = {};
    ADDR_HANDLE lib = create(gb);
    if (!lib) return r;

    /* Three blocks across, two down (the shape tiling-compare uses, and for its reason: a 2 x 2
     * grid cannot tell row-major blocks from column-major). */
    const unsigned bw = (s.bpp == 32) ? 128 : 256;
    const unsigned w = 3 * bw, h = 2 * bw;

    ADDR2_COMPUTE_SURFACE_INFO_INPUT si = {};
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT so = {};
    si.size = sizeof(si);
    so.size = sizeof(so);
    si.swizzleMode = ADDR_SW_64KB_Z_X;
    si.resourceType = ADDR_RSRC_TEX_2D;
    si.format = (s.bpp == 32) ? ADDR_FMT_32_FLOAT : ADDR_FMT_8;
    si.bpp = s.bpp;
    si.width = w;
    si.height = h;
    si.numSlices = 1;
    si.numMipLevels = 1;
    si.numSamples = 1;
    si.numFrags = 1;
    fill_flags(&si.flags, s);
    if (Addr2ComputeSurfaceInfo(lib, &si, &so) != ADDR_OK) {
        AddrDestroy(lib);
        return r;
    }
    r.blk_w = so.blockWidth;
    r.blk_h = so.blockHeight;
    r.pitch = so.pitch;
    r.height = so.height;
    r.size = so.surfSize;

    unsigned xbits = 0, ybits = 0;
    while ((1u << xbits) < r.blk_w) xbits++;
    while ((1u << ybits) < r.blk_h) ybits++;
    for (unsigned b = 0; b < xbits; b++) r.xb.push_back((uint32_t)addr_of(lib, s, w, h, so.pitch, 1u << b, 0));
    for (unsigned b = 0; b < ybits; b++) r.yb.push_back((uint32_t)addr_of(lib, s, w, h, so.pitch, 0, 1u << b));

    const unsigned blocks_x = so.pitch / r.blk_w;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            const uint64_t got = addr_of(lib, s, w, h, so.pitch, x, y);
            const unsigned lx = x % r.blk_w, ly = y % r.blk_h;
            uint64_t want = (uint64_t)((y / r.blk_h) * blocks_x + (x / r.blk_w)) * 65536u;
            uint32_t in_blk = 0;
            for (unsigned b = 0; b < xbits; b++) if ((lx >> b) & 1u) in_blk ^= r.xb[b];
            for (unsigned b = 0; b < ybits; b++) if ((ly >> b) & 1u) in_blk ^= r.yb[b];
            want += in_blk;
            r.checked++;
            if (got != want) r.mismatched++;
        }
    }
    AddrDestroy(lib);
    r.ok = true;
    return r;
}

static void print_vec(const char *label, const std::vector<uint32_t> &v)
{
    printf("    %s {", label);
    for (size_t i = 0; i < v.size(); i++) printf("%s0x%05x", i ? ", " : "", v[i]);
    printf("}\n");
}

int main(void)
{
    const Surf surfs[2] = {{"depth, Z_32_FLOAT", 32, false}, {"stencil, STENCIL_8", 8, true}};
    printf("zs-tiling: ADDR_SW_64KB_Z_X, family 0x%02X revision 0x%02X, pipeBankXor 0\n\n",
           kChipFamily, kChipRevision);
    int bad = 0;
    for (const Surf &s : surfs) {
        const Result d = measure(kGbAddrConfig, s);
        const Result c = measure(kGbAddrConfigControl, s);
        printf("  %s, %u bits a pixel\n", s.name, s.bpp);
        if (!d.ok) {
            printf("    addrlib refused the derived configuration\n");
            bad = 1;
            continue;
        }
        printf("    block %u x %u; a 3 x 2 block surface is pitch %u, height %u, %llu bytes\n",
               d.blk_w, d.blk_h, d.pitch, d.height, (unsigned long long)d.size);
        print_vec("x basis", d.xb);
        print_vec("y basis", d.yb);
        printf("    linear per block, blocks row-major: %zu of %zu pixels disagree\n",
               d.mismatched, d.checked);
        if (d.mismatched) bad = 1;
        const bool same = c.ok && c.xb == d.xb && c.yb == d.yb;
        printf("    control, 8 pipes: %s\n\n",
               !c.ok ? "addrlib refused it" : (same ? "SAME basis - the control sees nothing"
                                                    : "a different basis, as it must be"));
        if (same) bad = 1;
    }
    printf("  VERDICT: %s\n", bad ? "NOT usable as basis vectors - see above"
                                  : "each surface is block-linear in these vectors; usable");
    return 0;
}
