/*
 * **Drawing straight into the display's scanout buffers** - measured 2026-09-20, on.
 *
 * A title on this console draws into the buffers VideoOut scans out: WC_GARLIC direct memory
 * (obSCEne 130-layout/memory-type) in the GPU's 64KB_R_X render-target swizzle. oops-gl drew a
 * linear buffer that the display re-tiled on the CPU at every flip; the scanout path replaces
 * that - the colour target is the next scanout buffer, COLOR_SW_MODE 27, and a swap flips it as
 * drawn.
 *
 * # The two halves of the layout, and how each was settled
 *
 * **Inside a 64 KiB block**, obSCEne's `REQ-20260920T0745Z-2d7f` (re-filing `-7e21`, sweep
 * `20260920-085011`, `166-agc/primitive-draw`) drew one triangle into a 128 x 128 target three
 * times: linear, and tiled under each candidate CB_COLOR0_ATTRIB3, dumping all 65,536 bytes of
 * each. `tools/rx-check` detiles the tiled arms through the display tiler's own vectors
 * (`agc_tile_pixel`) and finds **every one of the 16,384 pixels equal to the linear control**,
 * while the same bytes read as rows match only 10,408 - so the picture can tell the two layouts
 * apart, and the colour block writes the one the display scans.
 *
 * **Across blocks**, gap-analysis's `-4b19` (same sweep) drew a triangle straddling all four
 * blocks of a 256 x 256 target and dumped all 262,144 bytes. There is no linear control at that
 * extent, so `rx-check --shape` uses the shape instead: it detiles under **every** block order
 * and keeps those that give one run of drawn pixels in each row and no gap between drawn rows.
 * Exactly one order survives - the row-major one this library assumes - and the 24,512 pixels it
 * finds are the number the check itself reported, which is also the triangle's area computed
 * from its own geometry rows. A first pass, before the gap rule, kept two orders: swapping whole
 * rows of blocks moves image rows without splitting any of them, so "one run a row" cannot see
 * it. That is why the rule is there.
 *
 * # What is *not* settled
 *
 * Both tiled arms matched, so these rows cannot separate `0x08c6c000` from `0x0dc6c000`: the
 * bits arm 2 adds (RESOURCE_TYPE, CMASK_PIPE_ALIGNED) change no pixel in this picture. Arm 1's
 * value is taken because it is the smaller change from the linear value oops-gl already draws
 * with, not because the other was refused.
 */
#ifndef OOPS_GL_RX_H
#define OOPS_GL_RX_H

/* 1 since 2026-09-20: `-2d7f`'s and `-4b19`'s rows pass tools/rx-check, above. The tracked
 * verdict is tools/rx-check/rx_check_7e21.txt. */
#define OOPS_GL_RX_MEASURED 1

/* oops-gl's working linear CB_COLOR0_ATTRIB3, 0x08c00000, with COLOR_SW_MODE (bits 18:14, Mesa
 * src/amd/registers/gfx103.json) set to ADDR_SW_64KB_R_X, 27
 * (src/amd/addrlib/inc/addrtypes.h:254) - `-7e21`'s arm1, which tools/rx-check names. */
#define OOPS_GL_RX_ATTRIB3 0x08c6c000u

#endif
