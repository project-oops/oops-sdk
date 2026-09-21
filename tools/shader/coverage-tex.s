.text
// Antialiasing's coverage in the **textured** pixel shader: the same arithmetic as
// `coverage.s`, reading the offset from a different interpolant.
//
// **Why it needs its own file.** The untextured shader takes the primitive's offset from the
// texture-coordinate parameter, `attr1`, which a draw with no texture does not read. A textured
// draw reads all four of those components - s and t in x and y, fog's factor in z, q in w - so
// the offset has nowhere to sit, and a smooth textured point or line has been drawn aliased
// since antialiasing landed on 2026-09-20. `gl_smoothing` says so in the log.
//
// **`attr3` is where it goes.** That parameter carries the second texture unit's coordinate,
// and a draw with one unit does not read it. Escalating such a draw to four parameters costs
// eighty bytes a vertex and the four-parameter vertex shader - both of which exist and both of
// which ran on a console on 2026-09-21, which is what makes this worth doing now rather than
// when the paragraph in GL_ROADMAP.md was written. A draw that *does* use two units keeps only
// `attr3.z` free, which is one float and not the three this needs, so that case stays aliased.
//
// The layout inside the parameter is the one `coverage.s` already uses - across in x, along in
// y, the radius plus a half in w - so these are its fifteen words with the six interpolations'
// ATTR field moved from 1 to 3 and nothing else touched. v12, v13 and v14 are free here: fog
// occupies the slot immediately above this one and uses v13 and v14 as its own scratch.
v_interp_p1_f32 v12, v0, attr3.x        // the offset from the centre, across
v_interp_p2_f32 v12, v1, attr3.x
v_interp_p1_f32 v13, v0, attr3.y        // and along, zero for a line
v_interp_p2_f32 v13, v1, attr3.y
v_interp_p1_f32 v14, v0, attr3.w        // the radius or half-width, already plus a half
v_interp_p2_f32 v14, v1, attr3.w
v_mul_f32_e32 v12, v12, v12
v_fma_f32 v12, v13, v13, v12            // x*x + y*y
v_sqrt_f32_e32 v12, v12
v_sub_f32_e32 v12, v14, v12             // (r + 1/2) - distance
v_min_f32_e32 v12, 1.0, v12             // the upper clamp; the lower one is the kill below
v_cmp_lt_f32_e32 vcc_lo, 0, v12
s_and_b32 exec_lo, exec_lo, vcc_lo      // a fragment the primitive misses is gone
v_mul_f32_e32 v7, v7, v12               // alpha weighted by the coverage

// Cross-checks against words already in the tree. The first six are `coverage.s`'s own
// interpolations, which are in `gl_ps_patch_coverage`'s table; if these assemble to those, then
// the only difference between the two programs is the ATTR field, which is the claim this file
// makes. The seventh is the lane kill the alpha test and the stipple both end with.
v_interp_p1_f32 v12, v0, attr1.x
v_interp_p2_f32 v12, v1, attr1.x
v_interp_p1_f32 v13, v0, attr1.y
v_interp_p2_f32 v13, v1, attr1.y
v_interp_p1_f32 v14, v0, attr1.w
v_interp_p2_f32 v14, v1, attr1.w
s_and_b32 exec_lo, exec_lo, vcc_lo
