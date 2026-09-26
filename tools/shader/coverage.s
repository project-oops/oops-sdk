.text
// Antialiasing's coverage in the untextured pixel shader: the fragment's alpha weighted by how
// much of the pixel the primitive covers, and the fragments it does not cover at all killed.
//
// The geometry arrives interpolated. GL's coverage for a smooth point is `r + 1/2 - |p - c|`
// clamped to [0, 1], and for a smooth line `w/2 + 1/2 - |across|`, so the shader needs the
// fragment's offset from the primitive's centre, which is linear across the quad the CPU widened
// the primitive into. The CPU puts the offset at each corner into the texture-coordinate
// parameter's x and y, and `r + 1/2` (or `w/2 + 1/2`) into its w. A line sets y to zero, which
// makes sqrt(x*x + y*y) the absolute across distance, so one form serves both.
//
// That parameter is free because this is the untextured shader: attr1.x, .y and .w are the
// texture coordinate and its q, which a draw with no texture does not read. attr1.z is fog's
// factor and is left alone. The textured shader uses `coverage-tex.s`.
//
// The kill matches the software rasteriser's. gl_draw.c drops a fragment whose coverage is
// not greater than zero rather than blending nothing, so the depth buffer does not take a write
// from a pixel the primitive misses. `v_cmp_lt_f32` and `s_and_b32 exec_lo` are the same two
// instructions the alpha test and the polygon stipple use for their own discards.
//
// It sits after fog and before the alpha test, which is where GL applies it (1.x, 3.12) and
// where gl_draw.c applies it: the alpha the test sees is the weighted one.
v_interp_p1_f32 v12, v0, attr1.x        // the offset from the centre, across
v_interp_p2_f32 v12, v1, attr1.x
v_interp_p1_f32 v13, v0, attr1.y        // and along, zero for a line
v_interp_p2_f32 v13, v1, attr1.y
v_interp_p1_f32 v14, v0, attr1.w        // the radius or half-width, already plus a half
v_interp_p2_f32 v14, v1, attr1.w
v_mul_f32_e32 v12, v12, v12
v_fma_f32 v12, v13, v13, v12            // x*x + y*y
v_sqrt_f32_e32 v12, v12
v_sub_f32_e32 v12, v14, v12             // (r + 1/2) - distance
v_min_f32_e32 v12, 1.0, v12             // the upper clamp; the lower one is the kill below
v_cmp_lt_f32_e32 vcc_lo, 0, v12
s_and_b32 exec_lo, exec_lo, vcc_lo      // a fragment the primitive misses is gone
v_mul_f32_e32 v7, v7, v12               // alpha weighted by the coverage
// Cross-checks against words already in the tree: the untextured shader's own interpolation of
// the red channel, and the lane kill the alpha test and the stipple both end with.
v_interp_p1_f32 v4, v0, attr0.x
s_and_b32 exec_lo, exec_lo, vcc_lo
