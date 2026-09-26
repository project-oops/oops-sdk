/*
 * **Drawing straight into the display's scanout buffers** - measured 2026-09-20, on.
 *
 * A title on this console draws into the buffers VideoOut scans out: WC_GARLIC direct
 * memory (obSCEne 130-layout/memory-type) in the GPU's 64KB_R_X render-target swizzle.
 * oops-gl drew a linear buffer that the display re-tiled on the CPU at every flip; the
 * scanout path replaces that - the colour target is the next scanout buffer,
 * COLOR_SW_MODE 27, and a swap flips it as drawn.
 *
 * # The two halves of the layout, and how each was settled
 *
 * **Inside a 64 KiB block**, obSCEne's `REQ-20260920T0745Z-2d7f` (re-filing `-7e21`,
 * sweep `20260920-085011`, `166-agc/primitive-draw`) drew one triangle into a 128 x 128
 * target three times: linear, and tiled under each candidate CB_COLOR0_ATTRIB3, dumping
 * all 65,536 bytes of each. `tools/rx-check` detiles the tiled arms through the display
 * tiler's own vectors
 * (`agc_tile_pixel`) and finds **every one of the 16,384 pixels equal to the linear
 * control**, while the same bytes read as rows match only 10,408 - so the picture can
 * tell the two layouts apart, and the colour block writes the one the display scans.
 *
 * **Across blocks**, gap-analysis's `-4b19` (same sweep) drew a triangle straddling all
 * four blocks of a 256 x 256 target and dumped all 262,144 bytes. There is no linear
 * control at that extent, so `rx-check --shape` uses the shape instead: it detiles
 * under **every** block order and keeps those that give one run of drawn pixels in each
 * row and no gap between drawn rows. Exactly one order survives - the row-major one
 * this library assumes - and the 24,512 pixels it finds are the number the check itself
 * reported, which is also the triangle's area computed from its own geometry rows. A
 * first pass, before the gap rule, kept two orders: swapping whole rows of blocks moves
 * image rows without splitting any of them, so "one run a row" cannot see it. That is
 * why the rule is there.
 *
 * # Across the whole frame, settled 2026-09-21
 *
 * The two rows above leave a gap, and it is worth saying what it was, because the shape
 * of it is the shape of a test that cannot fail:
 *
 *   - `-2d7f` is pixel-exact over **one** 64 KiB block - and inside one block an `_X`
 * mode's pipe rotation is constant, so it could not have appeared there at all.
 *   - `-4b19` is **shape** agreement over four, which settles the order blocks run in
 * and says nothing about the layout inside one.
 *
 * So the across-block half rested on evidence that could not have shown the thing it
 * was being read as showing. obSCEne's `-8b52` then drew a whole 1920 x 1080 target and
 * matched all 135 blocks - but with **Mesa's** `0x0dc6c000`, not this file's value, and
 * the bits between them (RESOURCE_TYPE, CMASK_PIPE_ALIGNED) are exactly the ones a
 * single block cannot separate.
 *
 * `REQ-20260921T1640Z-1d5e` asked for the one arm that closes it, and **run 18
 * answered**: `166-agc/primitive-draw`, `arm1-rx-1080p`, `cb0-attrib3 0x8c6c000`, 1920
 * x 1080 against a linear control - `detile-matches 0x1fa400`, which is 2,073,600 and
 * so every pixel of the frame, with `detile-mismatches`, `block0-mismatches` and
 * `multiblock-mismatches` all zero
 * (`obscene/reports/hardware/20260921-run18-eboot.obs.log:5417-5442`). The same sweep
 * ran Mesa's value beside it and got the same numbers.
 *
 * **So the constant below is measured at display size, across every block, and neither
 * of the bits that differ between the two candidates moves a pixel.** Arm 1 was taken
 * originally because it was the smaller change from the linear value oops-gl already
 * drew with; it is kept now because it was measured.
 */
#ifndef OOPS_GL_RX_H
#define OOPS_GL_RX_H

/* 1 since 2026-09-20: `-2d7f`'s and `-4b19`'s rows pass tools/rx-check, above. The
 * tracked verdict is tools/rx-check/rx_check_7e21.txt. */
#define OOPS_GL_RX_MEASURED 1

/* oops-gl's working linear CB_COLOR0_ATTRIB3, 0x08c00000, with COLOR_SW_MODE (bits
 * 18:14, Mesa src/amd/registers/gfx103.json) set to ADDR_SW_64KB_R_X, 27
 * (src/amd/addrlib/inc/addrtypes.h:254) - `-7e21`'s arm1, which tools/rx-check names.
 *
 * **Measured over a whole 1920 x 1080 frame** - `REQ-20260921T1640Z-1d5e`, run 18, all
 * 135 blocks and zero mismatches. See "Across the whole frame" above.
 *
 * **RESOURCE_TYPE (bits 25:24) became 2D on 2026-09-23**, which is why this is
 * 0x09c6c000 and not 0x08c6c000. It had said 1D since the value was first derived. Mesa
 * picks the resource type in ac_surface.c:2739-2744 and reaches 1D only for a texture
 * the caller declared 1D on a generation after gfx9; everything else, a render target
 * included, is `ADDR_RSRC_TEX_2D`, and `RADEON_RESOURCE_2D` is 1 (ac_surface.h:145).
 * The frame measurement above is unaffected - it was a measurement of the swizzle, in
 * bits 18:14, which this does not touch. */
#define OOPS_GL_RX_ATTRIB3 0x09c6c000u

#endif
