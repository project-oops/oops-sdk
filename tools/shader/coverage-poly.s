.text
// **GL_POLYGON_SMOOTH's coverage**: the product of three edge fades, where `coverage.s` and
// `coverage-tex.s` use one distance.
//
// GL's coverage for a smooth polygon is the fraction of the pixel the polygon covers, and the
// software rasteriser approximates it the way the specification describes: for each antialiased
// edge, the signed distance `d` from the pixel centre to that edge, faded over the pixel either
// side as `clamp(d + 1/2, 0, 1)`, and the three multiplied. `gl_draw.c` computes exactly that,
// which is what this has to agree with.
//
// **The three distances are screen-space linear, and the interpolator is not.** A signed
// distance to a line is affine in window coordinates, so it wants linear interpolation; RDNA2's
// `v_interp_*` use the perspective-correct barycentrics, which is a different function wherever
// the triangle's three w differ. Enabling the linear barycentrics would move the shader
// interface and needs the register layout measured, so this does what the textured prolog
// already does for `q`: the vertex carries `d * w` and `w`, and the shader divides. Perspective
// interpolation of `d*w` over interpolation of `w` is the screen-linear `d` exactly - the same
// identity that makes a projected texture coordinate come out right.
//
// So a smooth polygon's fourth parameter is `{d0*w, d1*w, d2*w, w}`, which is why it takes the
// whole of `attr3` and why such a draw escalates to four parameters. That parameter is the
// second texture unit's, so a smooth polygon using two units is refused, exactly as a smooth
// textured point or line is.
//
// **An edge that is not antialiased carries a large distance**, so its fade clamps to one and
// multiplies by nothing. A polygon triangulated into several triangles antialiases only its own
// outline, and `gl_draw_polygon_tri`'s edge mask is what says which those are; the CPU turns a
// masked-out edge into a distance no fade can reach.
//
// **The fragments outside the edge have to exist.** The hardware rasteriser raises a fragment
// only where the pixel centre is inside the triangle, so the outer half of every fade would
// simply not be drawn - which is what the roadmap gave as the reason this could not be done. The
// CPU widens the triangle outward instead, the way it already widens a point and a line into a
// quad, and the kill below removes whatever the widening added beyond the fade.
//
// v12..v15 are scratch: fog uses v13 and v14 above this slot, and the textured shader's combine
// has finished with v16..v27 by the time this runs.
v_interp_p1_f32 v12, v0, attr3.x        // d0 * w
v_interp_p2_f32 v12, v1, attr3.x
v_interp_p1_f32 v13, v0, attr3.y        // d1 * w
v_interp_p2_f32 v13, v1, attr3.y
v_interp_p1_f32 v14, v0, attr3.z        // d2 * w
v_interp_p2_f32 v14, v1, attr3.z
v_interp_p1_f32 v15, v0, attr3.w        // w
v_interp_p2_f32 v15, v1, attr3.w
v_rcp_f32 v15, v15
v_mul_f32 v12, v12, v15                 // d0
v_mul_f32 v13, v13, v15                 // d1
v_mul_f32 v14, v14, v15                 // d2
v_add_f32 v12, 0.5, v12                 // d0 + 1/2
v_add_f32 v13, 0.5, v13
v_add_f32 v14, 0.5, v14
v_med3_f32 v12, v12, 0, 1.0             // clamped to [0, 1]
v_med3_f32 v13, v13, 0, 1.0
v_med3_f32 v14, v14, 0, 1.0
v_mul_f32 v12, v12, v13
v_mul_f32 v12, v12, v14                 // the three fades multiplied
v_cmp_lt_f32_e32 vcc_lo, 0, v12
s_and_b32 exec_lo, exec_lo, vcc_lo      // a pixel the polygon misses entirely is gone
v_mul_f32_e32 v7, v7, v12               // alpha weighted by the coverage

// Cross-checks against words already in the tree: `coverage.s`'s own alpha weighting and lane
// kill, and the textured prolog's reciprocal and its interpolation of the fourth parameter's x.
// If these come out as the words `gl_ps_patch_coverage_where` and `tex-prolog2.s` already carry,
// the arithmetic above is assembled the same way the rest of this payload is.
v_mul_f32_e32 v7, v7, v12
s_and_b32 exec_lo, exec_lo, vcc_lo
v_interp_p1_f32 v12, v0, attr3.x
v_interp_p2_f32 v12, v1, attr3.x
