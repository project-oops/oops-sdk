.text
// The vertex shader for a draw with two texture units: the three-parameter program of
// vs-param3.s with an 80-byte vertex instead of 64 and a fifth vec4 loaded and exported as
// param3 - the second unit's texture coordinate. The interface is SPI_VS_OUT_CONFIG 0x6,
// SPI_PS_IN_CONTROL 0x4 and SPI_PS_INPUT_CNTL_3 0x3, the unpacked form.
//
// The vertex: position (0), colour (16), unit 0's {s, t, fog, q} (32), param2 (48) -
// {secondary r, g, b, unit 0's r} - and unit 1's {s, t, unused, q} (64).
//
// Everything outside the marked lines is the three-parameter shader's own sequence, and
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
v_lshlrev_b32 v15, 2, v14
// changed: the vertex is 80 bytes, which is not a shift - lane * 64 plus lane * 16.
v_lshlrev_b32 v16, 4, v14
v_lshlrev_b32 v14, 6, v14
v_add_nc_u32 v14, v16, v14
v_add_nc_u32 v14, s8, v14
s_mov_b32 s2, 0x12345678
s_mov_b32 s3, 0x9abcdef0
v_mov_b32 v1, 0
v_add_co_u32 v18, vcc_lo, s2, v14
v_add_co_ci_u32 v19, vcc_lo, s3, v1, vcc_lo
global_load_dwordx4 v[2:5], v[18:19], off
global_load_dwordx4 v[10:13], v[18:19], off offset:16
global_load_dwordx4 v[6:9], v[18:19], off offset:32
global_load_dwordx4 v[20:23], v[18:19], off offset:48
// added: the second unit's vec4.
global_load_dwordx4 v[24:27], v[18:19], off offset:64
s_waitcnt vmcnt(0)
s_mov_b32 s6, 0x0badc0de
s_mov_b32 s7, 0x0badf00d
v_add_co_u32 v18, vcc_lo, s6, v15
v_add_co_ci_u32 v19, vcc_lo, s7, v1, vcc_lo
global_store_dword v[18:19], v2, off offset:8
s_waitcnt vmcnt(0)
exp param0 v10, v11, v12, v13
exp param1 v6, v7, v8, v9
exp param2 v20, v21, v22, v23
// added: the fourth parameter.
exp param3 v24, v25, v26, v27
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
