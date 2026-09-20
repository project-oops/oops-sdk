.text
// The longer texture combine (gl_ps_patch_tex_env's general form): GL_BLEND, GL_DECAL of an RGBA
// texture and GL_COMBINE, which the four-word forms of tex-env.s cannot express. The program is
// generated from a combiner description, so the words come from a small encoder rather than a
// table - and this file is what the encoder is checked against, one line per instruction form and
// operand kind it emits (test_pm4_gl_combine_encoder_matches_the_assembler). Each line's words are
// in that test in the same order.
//
// Registers: the texel is v4..v7, the fragment colour v8..v11, the three arguments v16..v27
// (argument i's channel c is v[16 + 4i + c]), v13 and v14 temporaries; the result goes to v4..v7.
// 0.3 (0x3e99999a) stands in for a literal: it has no inline encoding, so the assembler emits a
// literal word, which is the form the encoder uses for every constant.

// VOP1 - the argument moves.
v_mov_b32 v16, v4
v_mov_b32 v27, v11
v_mov_b32 v20, 0x3e99999a

// VOP2 - one minus a source, into an argument.
v_sub_f32 v16, 1.0, v4
v_sub_f32 v27, 1.0, v11

// VOP2 - the functions, between arguments.
v_mul_f32 v4, v16, v20
v_add_f32 v5, v17, v21
v_sub_f32 v6, v18, v22
v_sub_f32 v13, v19, v23
v_fmac_f32 v7, v13, v27

// VOP2 - ADD_SIGNED's bias, DOT3's centring, the scales, the clamp.
v_add_f32 v4, -0.5, v4
v_add_f32 v13, -0.5, v16
v_mul_f32 v4, 2.0, v4
v_mul_f32 v7, 4.0, v7
v_max_f32 v4, 0, v4
v_min_f32 v7, 1.0, v7

// VOP1 - a result copied across channels (DOT3).
v_mov_b32 v5, v4

// SOPP - the branch over the slot's unused words: 0, 2 and 40 words skipped.
s_branch skip0
skip0:
s_branch skip2
s_nop 0
s_nop 0
skip2:
s_branch skip40
.rept 40
s_nop 0
.endr
skip40:
s_nop 0
