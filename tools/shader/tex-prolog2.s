.text
// The second texture unit's sample (since 2026-09-20): unit 1's coordinate interpolated from the
// fourth parameter, its own image and sampler descriptors loaded from the second pair in the
// table, and the texel left in v28..v31 for the second combine stage - see the paragraph on the
// register range below, which is the reason it is not v16..v19.
//
// **Where it sits, and why there.** Inside the prolog's whole-quad region, immediately after
// unit 0's sample and *before* exec is restored from s16. A sample taken outside whole-quad mode
// has no helper pixels, so its implicit derivatives - its level of detail - are wrong along every
// quad edge, which shows as a seam of the wrong mip level. Hoisting both samples above both
// combines costs nothing: a sample depends on the coordinate, not on the combine before it, and
// GL's order (unit 0 combined, then unit 1 against that result) is kept by the combine slots
// that follow.
//
// The descriptors are the second pair of the table at 0x900: image at +0x40, sampler at +0x60,
// where unit 0's are at +0x00 and +0x20 - the layout obSCEne's REQ-20260919T2258Z-8b1c asked
// about and REQ-20260920T0745Z-9a41 re-asks, because that sweep reported no descriptor words at
// all for its two-sample arm.
//
// v2 and v3 are unit 0's coordinate registers, dead once its sample has retired; v13 is fog's
// scratch, which fog writes later. The texel goes to **v28..v31**, and the range matters: the
// general combine form gathers its arguments into v16..v27 (gl_ps_combine_program), so a texel
// left in v16..v19 would be overwritten by unit 0's own combine whenever its environment is
// GL_COMBINE, GL_BLEND or GL_DECAL of an RGBA texture - correct for GL_MODULATE, wrong for the
// modes that need a program, which is the worst shape a bug can have. The pixel shader's
// SPI_SHADER_PGM_RSRC1 is 0x000c0010: VGPRS 0x10, which is 136 registers in wave32, so v28..v31
// are allocated.
v_interp_p1_f32 v2, v0, attr3.x         // unit 1's s
v_interp_p2_f32 v2, v1, attr3.x
v_interp_p1_f32 v3, v0, attr3.y         // t
v_interp_p2_f32 v3, v1, attr3.y
v_interp_p1_f32 v13, v0, attr3.w        // q
v_interp_p2_f32 v13, v1, attr3.w
v_rcp_f32 v13, v13
v_mul_f32 v2, v2, v13
v_mul_f32 v3, v3, v13
s_load_dwordx8 s[20:27], s[0:1], 0x40
s_load_dwordx4 s[28:31], s[0:1], 0x60
s_waitcnt lgkmcnt(0)
image_sample v[28:31], v[2:3], s[20:27], s[28:31] dmask:0xf dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
// Cross-checks against words already in the tree: unit 0's own descriptor loads and sample, from
// the prolog gl_context.c writes, which have to assemble to 0xf40c0100 / 0xfa000000,
// 0xf4080300 / 0xfa000020 and 0xf0800f08 / 0x00610402.
s_load_dwordx8 s[4:11], s[0:1], 0x00
s_load_dwordx4 s[12:15], s[0:1], 0x20
image_sample v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D
