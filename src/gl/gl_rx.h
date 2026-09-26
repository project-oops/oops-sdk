/*
 * Drawing straight into the display's scanout buffers.
 *
 * A title on this console draws into the buffers VideoOut scans out: WC_GARLIC direct
 * memory (obSCEne 130-layout/memory-type) in the GPU's 64KB_R_X render-target swizzle.
 * The colour target is the next scanout buffer, COLOR_SW_MODE 27, and a swap flips it
 * as drawn, with no CPU re-tiling.
 *
 * `tools/rx-check` detiles hardware dumps through the display tiler's own vectors
 * (`agc_tile_pixel`): inside a 64 KiB block every pixel matches a linear control
 * (tools/rx-check/rx_check_7e21.txt), blocks run row-major across the pitch
 * (tools/rx-check/rx_check_4b19.txt, `rx-check --shape`), and a full 1920 x 1080 frame
 * matches its linear control at every pixel of all 135 blocks
 * (obscene/reports/hardware/20260921-run18-eboot.obs.log:5417-5442).
 */
#ifndef OOPS_GL_RX_H
#define OOPS_GL_RX_H

/* The layout checks above pass; the verdict is tools/rx-check/rx_check_7e21.txt. */
#define OOPS_GL_RX_MEASURED 1

/* oops-gl's linear CB_COLOR0_ATTRIB3, 0x08c00000, with COLOR_SW_MODE (bits 18:14, Mesa
 * src/amd/registers/gfx103.json) set to ADDR_SW_64KB_R_X, 27
 * (src/amd/addrlib/inc/addrtypes.h:254).
 *
 * RESOURCE_TYPE (bits 25:24) is 2D: Mesa picks the resource type in
 * ac_surface.c:2739-2744 and reaches 1D only for a texture declared 1D, so a render
 * target is `ADDR_RSRC_TEX_2D`, and `RADEON_RESOURCE_2D` is 1 (ac_surface.h:145). The
 * whole-frame measurement above covers the swizzle bits, which this does not touch. */
#define OOPS_GL_RX_ATTRIB3 0x09c6c000u

#endif
