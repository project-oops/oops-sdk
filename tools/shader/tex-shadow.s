.text
// Sampling a depth texture with GL 1.4's comparison: the reference coordinate built, the
// sampler's own DEPTH_COMPARE_FUNC applied per texel by image_sample_c, and the one value it
// returns spread across v4..v7 as GL_DEPTH_TEXTURE_MODE says.
//
// The comparison is the sampler's: SQ_IMG_SAMP_WORD0's DEPTH_COMPARE_FUNC carries
// GL_TEXTURE_COMPARE_FUNC (gl_state.c). The shader adds the reference and the _c form of the
// instruction that hands it over.
//
// The reference is r/q, clamped to [0, 1] - GL 1.4, 3.8.14, and the clamp softpipe applies at
// sp_tex_sample.c:2803-2806. The prolog left 1/q in v12, so the divide is one multiply, as in
// tex-3d.s. It is the first address register, ahead of s and t: the VADDR order the RDNA2 ISA
// gives for the _C forms, and the operand order of LLVM's image.sample.c.2d intrinsic.
//
// Not the _lz form: the hardware derives the level of detail, so a depth texture's mip chain,
// minification filter and GL 1.4's LOD bias take effect. The prolog's whole-quad mode makes
// those derivatives exist here.
//
// dmask is 0x1: a comparison returns one value, and the three forms below spread it the way
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
