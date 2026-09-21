.text
// The instructions the GL 2.0 pixel-shader back end emits.
//
// Every other file here is a *program* - a sequence that ships as literal words and does one
// job. This one is a **table**: the back end in `glsl_gen.c` builds a shader whose words depend
// on the GLSL it was given, so what has to be pinned is each instruction's encoding rather than
// any particular order of them. `test_glsl_emit_matches_the_assembler` asserts the encoder
// reproduces exactly these words.
//
// The hazard is the one this directory exists for and it is worse here than anywhere else: a
// wrong encoding in a hand-written shader is wrong once, and a wrong encoding in a compiler is
// wrong in every shader it ever emits.
//
// # What a compiled pixel shader is made of
//
// Three parts, and only the middle one depends on the GLSL:
//
//   - **The prologue interpolates.** A varying arrives as a parameter, and `v_interp_p1_f32`
//     followed by `v_interp_p2_f32` turns the two barycentrics in v0 and v1 into its value at
//     this fragment. One pair per component.
//   - **The body** is whatever the shader computes, from the instructions below.
//   - **The epilogue exports.** `exp mrt0` with the four colour registers, `done` and `vm`, then
//     `s_endpgm`.
//
// # Wave32
//
// These shaders run wave32 - `VGT_SHADER_STAGES_EN` sets `GS_W32` and `VS_W32` - so a mask
// operand is `vcc_lo` or `exec_lo`. `discard` compiles to a compare into `vcc_lo` and an
// `s_and_b32` on `exec_lo`, which is the same pair the alpha test and the polygon stipple use.

// ---------------------------------------------------------------------------
// Interpolation. The four channels and a high attribute, so the field positions
// are pinned rather than inferred from one example: vdst is bits 25:18, the
// opcode 17:16 (p1 = 0, p2 = 1), the attribute 15:10 and its channel 9:8.
// ---------------------------------------------------------------------------
v_interp_p1_f32 v4, v0, attr0.x
v_interp_p2_f32 v4, v1, attr0.x
v_interp_p1_f32 v5, v0, attr0.y
v_interp_p2_f32 v5, v1, attr0.y
v_interp_p1_f32 v6, v0, attr0.z
v_interp_p2_f32 v6, v1, attr0.z
v_interp_p1_f32 v7, v0, attr0.w
v_interp_p2_f32 v7, v1, attr0.w
v_interp_p1_f32 v20, v0, attr3.z
v_interp_p2_f32 v20, v1, attr3.z

// ---------------------------------------------------------------------------
// The export. `done` says this is the last export of the shader and `vm` that
// the exec mask is valid - both are what the existing pixel shaders set, and
// both are in the first dword; the second holds the four source registers as
// four bytes.
// ---------------------------------------------------------------------------
exp mrt0 v4, v5, v6, v7 done vm
exp mrt0 v0, v1, v2, v3 done vm

// ---------------------------------------------------------------------------
// VOP1: one operand. `v_rcp_f32` is a reciprocal and not a divide - GLSL's `/`
// compiles to a reciprocal and a multiply, which is what the hardware has.
// ---------------------------------------------------------------------------
v_rcp_f32_e32 v4, v5
v_sqrt_f32_e32 v4, v5
v_rsq_f32_e32 v4, v5
v_fract_f32_e32 v4, v5
v_floor_f32_e32 v4, v5
v_ceil_f32_e32 v4, v5
v_trunc_f32_e32 v4, v5
v_sin_f32_e32 v4, v5
v_cos_f32_e32 v4, v5
v_exp_f32_e32 v4, v5
v_log_f32_e32 v4, v5
v_mov_b32_e32 v4, v5
v_mov_b32_e32 v4, 1.0
v_mov_b32_e32 v4, 0

// ---------------------------------------------------------------------------
// VOP2: two operands. `v_sub_f32` subtracts vsrc1 from src0 and the next opcode
// along is `v_subrev_f32` with them the other way round, so the order is part
// of the encoding rather than a convention.
// ---------------------------------------------------------------------------
v_mul_f32_e32 v4, v5, v6
v_add_f32_e32 v4, v5, v6
v_sub_f32_e32 v4, v5, v6
v_max_f32_e32 v4, v5, v6
v_min_f32_e32 v4, v5, v6
v_cndmask_b32_e32 v4, v5, v6, vcc_lo

// ---------------------------------------------------------------------------
// VOPC and the lane kill. `v_cmp_neq_f32` is the unordered not-equal, which is
// what GLSL's `!=` is: a NaN is not equal to anything including itself.
// ---------------------------------------------------------------------------
v_cmp_lt_f32_e32 vcc_lo, v4, v5
v_cmp_eq_f32_e32 vcc_lo, v4, v5
v_cmp_le_f32_e32 vcc_lo, v4, v5
v_cmp_gt_f32_e32 vcc_lo, v4, v5
v_cmp_ge_f32_e32 vcc_lo, v4, v5
v_cmp_neq_f32_e32 vcc_lo, v4, v5
s_and_b32 exec_lo, exec_lo, vcc_lo

// ---------------------------------------------------------------------------
// Scalar memory: how a uniform reaches a compiled shader.
//
// A uniform is the same value in every lane, so it belongs in an SGPR and not a
// VGPR - and an SGPR is loaded from memory, not interpolated. `s[0:1]` holds a
// 64-bit address the draw puts in the pixel shader's first user SGPR pair, the
// same pair `tex-prolog.s` already loads its descriptors from, and the offset is
// a byte offset from it.
//
// **The offset is in the second dword, the destination and the width in the
// first.** The five widths are one opcode apart, so an off-by-one loads twice or
// half as many registers as the shader then reads - which is not a fault, just
// whatever those SGPRs happened to hold.
//
// These are here in every width because the width is chosen by how many floats
// the uniform block has, so all five are reachable from one shader's worth of
// uniforms and none of them is the "normal" one.
// ---------------------------------------------------------------------------
s_load_dword    s4,       s[0:1], 0x0
s_load_dword    s4,       s[0:1], 0x10
s_load_dword    s12,      s[0:1], 0x40
s_load_dwordx2  s[4:5],   s[0:1], 0x0
s_load_dwordx4  s[4:7],   s[0:1], 0x0
s_load_dwordx8  s[4:11],  s[0:1], 0x0
s_load_dwordx8  s[12:19], s[0:1], 0x20
s_load_dwordx16 s[16:31], s[0:1], 0x0

// The wait. A scalar load is not in order with the instructions after it, so
// **every one of these needs an `lgkmcnt(0)` before the first read** - without
// it the shader computes with whatever those SGPRs held, which on a second draw
// is the previous draw's uniforms and on the first is nothing in particular.
s_waitcnt lgkmcnt(0)
s_waitcnt vmcnt(0) lgkmcnt(0)

// And the SGPR in use. A VOP2's `src0` is a nine-bit operand whose low values
// name SGPRs, so a uniform is an operand directly and costs no register and no
// move - but **only in `src0`**: `vsrc1` is eight bits and always a VGPR, so
// `uniform * uniform` cannot be one instruction.
v_mov_b32_e32 v4, s4
v_mul_f32_e32 v4, s4, v5
v_add_f32_e32 v4, s12, v5
v_cmp_lt_f32_e32 vcc_lo, s4, v5

// ---------------------------------------------------------------------------
// Control flow, which on this machine is the exec mask and not a branch.
//
// A fragment shader's `if` is not a jump: every lane runs the same
// instructions, and which lanes *write* is the exec mask. So `if (c) A else B`
// narrows exec to the lanes where c holds, runs A, flips exec to the rest, runs
// B, and puts exec back:
//
//   s_and_saveexec_b32 sN, vcc_lo      sN = exec; exec &= vcc
//   <A>
//   s_andn2_b32 exec_lo, sN, exec_lo   exec = sN & ~exec, the other lanes
//   <B>
//   s_mov_b32 exec_lo, sN              back to what came in
//
// **There is no branch in that, and none is needed.** `s_cbranch_execz` would
// skip a body no lane is running, which is a saving and not a correctness
// requirement - a body executed with exec = 0 writes nothing. Leaving it out is
// what lets the generator emit straight through with no labels and no offsets
// to backpatch, which is a whole mechanism's worth of mistakes not made.
//
// **`discard` is the one that cannot just narrow exec.** A discarded lane must
// stay dead after the enclosing `if` restores, so it is taken out of every
// saved mask on the way down and only then is exec cleared - `s_andn2_b32 sN,
// sN, exec_lo` per enclosing level, then `s_mov_b32 exec_lo, 0`. Clearing exec
// alone would kill the lanes until the next restore brought them back.
//
// Both destination forms of each instruction are here, because the register
// number sits in a different field in SOP1 than in SOP2 and one example would
// not have told them apart.
// ---------------------------------------------------------------------------
s_and_saveexec_b32 s4, vcc_lo
s_and_saveexec_b32 s5, vcc_lo
s_and_saveexec_b32 s15, vcc_lo
s_andn2_b32 exec_lo, s4, exec_lo
s_andn2_b32 exec_lo, s15, exec_lo
s_andn2_b32 s4, s4, exec_lo
s_andn2_b32 s15, s15, exec_lo
s_mov_b32 exec_lo, s4
s_mov_b32 exec_lo, s15
s_mov_b32 exec_lo, 0

// ---------------------------------------------------------------------------
// The cross-checks. Each of these words is already in the tree from a
// hand-written shader, so agreeing with them says this pipeline is right rather
// than merely self-consistent.
// ---------------------------------------------------------------------------
s_endpgm                    // 0xbf810000, the word every shader here ends with
s_nop 0                     // 0xbf800000, what an unused patch slot holds
s_waitcnt vmcnt(0)          // 0xbf8c3f70, after every image_sample in the tree
