.text
// The polygon stipple's discard (GL 1.0, 3.5.2; since 2026-09-20). RDNA2 has no stipple
// hardware: radeonsi discards in the pixel shader, looking the fragment's window position up in
// the 32x32 mask, and this is that lookup.
//
// **The fragment's position** arrives because the draw sets SPI_PS_INPUT_ENA and
// SPI_PS_INPUT_ADDR to 0x302 - PERSP_CENTER_ENA with POS_X_FLOAT_ENA and POS_Y_FLOAT_ENA - which
// obSCEne measured on hardware (REQ-20260919T2258Z-c7d4, sweep 20260920-082906,
// 166-agc/ps-pos-xy): v0 and v1 stay the barycentrics, v2 arrives as POS_X and v3 as POS_Y, at
// pixel centres (39.5, 31.5 for the pixel it sampled). The control arm, at 0x2, had v2 zero.
// Nothing else is enabled, so v4 and v5 are undefined there - this reads neither.
//
// **Where it sits**: word 16 of both pixel shaders, after the canary block and before the
// interpolation that uses v2 and v3 as scratch. Before whole-quad mode, so the mask kept in s16
// for the sample is the one this leaves behind: the helper pixels of a quad still run, and the
// fragments this discards stay discarded.
//
// **What the CPU hands it** is not the mask as glPolygonStipple stored it. The software
// rasteriser tests `(polygon_stipple[(height - 1 - y) & 31] >> (31 - (x & 31))) & 1` with y
// counting down from the top; gl_ps_patch_stipple writes the same 32 rows rotated for the
// window height and with each row's bits reversed, so that this shader is a shift right by
// x & 31 of the row at y & 31 - two operations instead of four, in a slot every fragment runs.
v_cvt_u32_f32_e32 v2, v2                // x, truncated from the pixel centre
v_cvt_u32_f32_e32 v3, v3                // y
v_and_b32_e32 v3, 31, v3
v_lshlrev_b32_e32 v3, 2, v3             // the row's byte offset in the 128-byte table
s_mov_b32 s2, 0x12345678                // the table's address, patched per context
s_mov_b32 s3, 0x9abcdef0
global_load_dword v3, v3, s[2:3]        // the saddr form: no 64-bit address arithmetic per lane
v_and_b32_e32 v2, 31, v2
s_waitcnt vmcnt(0)
v_lshrrev_b32_e32 v3, v2, v3
v_and_b32_e32 v3, 1, v3
v_cmp_ne_u32_e32 vcc_lo, 0, v3
s_and_b32 exec_lo, exec_lo, vcc_lo      // the lanes whose bit was clear are gone
// Cross-checks against words already in the tree: the wait both shaders do after their canary
// store (0xbf8c3f70), and s_endpgm (0xbf810000).
s_waitcnt vmcnt(0)
s_endpgm
