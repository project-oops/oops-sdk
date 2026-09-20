.text
// The textured pixel shader's sampling (since 2026-09-19): whole-quad mode for the sample's
// implicit derivatives, the texture coordinate divided by its q per fragment, and the sample
// itself with the level of detail the hardware derives - not image_sample_lz's level zero, which
// left the mip chain, the minification filter and GL 1.4's LOD bias unused on the console.
//
// The exec discipline is ACO's (mesa/src/amd/compiler/aco_insert_exec_mask.cpp:61-97,
// :150-163): keep the live pixels' mask, s_wqm for everything a derivative reads, then back to
// the kept mask before anything writes - the combine, fog, the alpha test and the export.
s_mov_b32 s16, exec_lo
s_wqm_b32 exec_lo, exec_lo
// q: the texture parameter's w, which the vertex fills since the same day.
v_interp_p1_f32 v12, v0, attr1.w
v_interp_p2_f32 v12, v1, attr1.w
v_rcp_f32 v12, v12
v_mul_f32 v2, v2, v12
v_mul_f32 v3, v3, v12
image_sample v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D
s_mov_b32 exec_lo, s16
// Cross-checks against words already in the tree: the sample this replaces (0xf09c0f08
// 0x00610402) and the exec restore after the canary store (0xbefe0304).
image_sample_lz v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D
s_mov_b32 exec_lo, s4
s_nop 0
