.text
// The vertex shader for a draw that needs a third interpolant (since 2026-09-19): the NGG
// program oops-gl has drawn with since 2026-09-14, with a 64-byte vertex instead of 48 and a
// fourth vec4 loaded and exported as param2. obSCEne measured the interface
// (REQ-20260919T1745Z-9c3e, sweep 20260919-212620): SPI_VS_OUT_CONFIG 0x4, SPI_PS_IN_CONTROL
// 0x3 and SPI_PS_INPUT_CNTL_2 0x2 - the *unpacked* form - carry a third parameter's value to the
// pixel shader byte for byte. The packed form hung the GPU in the same sweep, and is not used.
//
// The vertex: position (0), colour (16), unit 0's texture parameter {s, t, fog, q} (32), and
// param2 (48) - {secondary r, g, b, unit 0's r}.
//
// Everything outside the marked lines is the two-parameter shader's own sequence, and
// assembles to the words gl_context.c already writes for it.
s_inst_prefetch 0x1
s_mov_b32 s12, exec_lo
s_mov_b32 m0, 0x1003
s_nop 0
s_sendmsg sendmsg(MSG_GS_ALLOC_REQ)
s_mov_b32 exec_lo, 1
v_mov_b32 v1, 0x20280600
exp prim v1, off, off, off done
s_waitcnt expcnt(0)
s_mov_b32 exec_lo, 7
v_mbcnt_lo_u32_b32 v14, -1, 0
// changed: lane * 4 for the diagnostic store, lane * 64 for the vertex.
v_lshlrev_b32 v15, 2, v14
v_lshlrev_b32 v14, 6, v14
v_add_nc_u32 v14, s8, v14
s_mov_b32 s2, 0x12345678
s_mov_b32 s3, 0x9abcdef0
v_mov_b32 v1, 0
v_add_co_u32 v18, vcc_lo, s2, v14
v_add_co_ci_u32 v19, vcc_lo, s3, v1, vcc_lo
global_load_dwordx4 v[2:5], v[18:19], off
global_load_dwordx4 v[10:13], v[18:19], off offset:16
global_load_dwordx4 v[6:9], v[18:19], off offset:32
// added: param2's vec4.
global_load_dwordx4 v[20:23], v[18:19], off offset:48
s_waitcnt vmcnt(0)
s_mov_b32 s6, 0x0badc0de
s_mov_b32 s7, 0x0badf00d
// changed: v15 is lane * 4 already.
v_add_co_u32 v18, vcc_lo, s6, v15
v_add_co_ci_u32 v19, vcc_lo, s7, v1, vcc_lo
global_store_dword v[18:19], v2, off offset:8
s_waitcnt vmcnt(0)
exp param0 v10, v11, v12, v13
exp param1 v6, v7, v8, v9
// added: the third parameter.
exp param2 v20, v21, v22, v23
exp pos0 v2, v3, v4, v5 done
s_waitcnt expcnt(0)
s_mov_b32 exec_lo, 1
v_mov_b32 v18, s6
v_mov_b32 v19, s7
v_mov_b32 v1, 0xbeef0001
global_store_dword v[18:19], v1, off
s_waitcnt vmcnt(0)
s_mov_b32 exec_lo, s12
s_endpgm
