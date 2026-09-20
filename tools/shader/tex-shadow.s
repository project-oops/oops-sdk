.text
// Sampling a depth texture with GL 1.4's comparison (since 2026-09-20): the reference
// coordinate built, the sampler's own DEPTH_COMPARE_FUNC applied per texel by image_sample_c,
// and the one value it returns spread across v4..v7 as GL_DEPTH_TEXTURE_MODE says.
//
// **The comparison is the sampler's, not the shader's.** SQ_IMG_SAMP_WORD0's DEPTH_COMPARE_FUNC
// already carries GL_TEXTURE_COMPARE_FUNC (gl_state.c, measured by obSCEne's
// REQ-20260920T0745Z-6c80: function 3 with one reference either side of the stored depth came
// back 0xffffffff and 0xff000000). What the shader adds is the reference itself and the _c form
// of the instruction that hands it over.
//
// **The reference is r/q, clamped to [0, 1]** - GL 1.4, 3.8.14, and the clamp softpipe applies
// at sp_tex_sample.c:2803-2806. The prolog left 1/q in v12, so the divide is one multiply, the
// same as tex-3d.s's.
//
// **The reference is the first address register**, ahead of s and t: that is the VADDR order the
// RDNA2 ISA gives for the _C forms, and the order LLVM's image.sample.c.2d intrinsic takes its
// operands in. -6c80's two arms already prove the reference is read from whichever register they
// varied - their texture was a uniform 0.5 and only ref_z changed between the passing and the
// failing row - but not which index that was, so REQ-20260920T1340Z-b4e1 asks for it.
//
// **Not the _lz form.** -6c80 sampled with image_sample_c_lz, level zero, because that was all
// its fixture needed; this shader takes the level of detail the hardware derives, for the same
// reason the plain 2D sample stopped being _lz on 2026-09-19 - otherwise a depth texture's mip
// chain, its minification filter and GL 1.4's LOD bias never take effect. The prolog's whole-quad
// mode is what makes those derivatives exist here.
//
// **dmask is 0x1**: a comparison returns one value, and the three forms below spread it the way
// GL_DEPTH_TEXTURE_MODE says - (v, v, v, 1) luminance, (v, v, v, v) intensity, (0, 0, 0, v)
// alpha. The waitcnt is the slot's own, because the spread reads what the sample wrote and the
// shader's next waitcnt is a word past the end of this slot.
v_interp_p1_f32 v16, v0, attr2.w        // r
v_interp_p2_f32 v16, v1, attr2.w
v_mul_f32_e32 v16, v16, v12             // r/q
v_max_f32_e32 v16, 0, v16               // clamped to [0, 1], low end first as softpipe does
v_min_f32_e32 v16, 1.0, v16
v_mov_b32_e32 v17, v2                   // s/q, already divided by the prolog
v_mov_b32_e32 v18, v3                   // t/q
image_sample_c v4, v[16:18], s[4:11], s[12:15] dmask:0x1 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
// GL_LUMINANCE: (v, v, v, 1)
v_mov_b32_e32 v5, v4
v_mov_b32_e32 v6, v4
v_mov_b32_e32 v7, 1.0
// GL_INTENSITY: (v, v, v, v) - the first two moves above, then
v_mov_b32_e32 v7, v4
// GL_ALPHA: (0, 0, 0, v) - the alpha first, because it reads v4
v_mov_b32_e32 v7, v4
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0
v_mov_b32_e32 v6, 0
// Cross-checks against words already in the tree: the plain 2D sample this replaces, the
// prolog's own divide of s by q, and its interpolation of attr1.x.
image_sample v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D
v_mul_f32_e32 v2, v2, v12
v_interp_p1_f32 v2, v0, attr1.x
