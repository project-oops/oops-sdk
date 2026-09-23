/*
 * oops-gl: RDNA2 instruction encoding for the GLSL back end
 *
 * The first piece of GL 2.0 that produces machine code. Everything above it - lexer,
 * preprocessor, parser, semantic stage - turns text into a tree; this turns the tree's
 * arithmetic into gfx1030 words.
 *
 * # Why an encoder rather than literals
 *
 * oops-gl's existing shaders are literal `uint32_t` arrays, which is right for a fixed
 * instrument: the stream is readable as evidence and nothing stands between a GL call and the
 * packet. A compiler cannot work that way - the words depend on the shader - so the hazard the
 * literals avoided comes back: **a wrong encoding cannot fail loudly.** It assembles into the
 * payload, the hardware does something else, and the frame is wrong rather than the build
 * stopping.
 *
 * So every field position here was read out of a real assembler rather than written from
 * memory. `tools/shader/gl2-transform.s` contains the instructions, and the test beside this
 * code asserts the encoder reproduces the exact words clang produced:
 *
 *     clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c tools/shader/gl2-transform.s -o /tmp/t.o
 *     objdump -s -j .text /tmp/t.o
 *
 * The cross-check that this is right rather than merely self-consistent is `s_endpgm`:
 * `0xbf810000`, which is the same word the hand-written shaders in this repository have always
 * ended with.
 *
 * # Wave32
 *
 * These shaders run wave32 - `VGT_SHADER_STAGES_EN` sets `GS_W32` and `VS_W32` - so a mask
 * operand is `vcc_lo` or `exec_lo`. Nothing here takes one yet; when something does, assembling
 * for the wrong width is the mistake to avoid, and it is silent.
 */

#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * Operands
 *
 * A source operand is nine bits and its meaning depends on the value: 0..101 are SGPRs and
 * special registers, 128..192 are inline constants, 255 says a 32-bit literal follows the
 * instruction, and 256..511 are VGPRs. Encoding a VGPR number directly - forgetting the 256 -
 * silently names an SGPR instead, which is the kind of mistake that produces a shader that runs
 * and computes rubbish.
 * ------------------------------------------------------------------------- */

uint32_t glsl_vgpr(uint32_t n) { return 256u + n; }

/* An SGPR's operand value is its own number - the low end of the same nine-bit space. Identity,
 * and written out anyway: a call site that passes a bare number is one where nobody can tell
 * whether the 256 was forgotten, and this is the file where that mistake is silent. */
uint32_t glsl_sgpr(uint32_t n) { return n; }

/* The inline constant for 1.0f, read back from `v_mov_b32 v12, 1.0` which assembled with
 * src0 = 0xf2. The inline constants are a small closed table; anything outside it needs a
 * literal, and `glsl_emit_mov_imm` decides which. */
#define GLSL_INLINE_ONE_F32 242u
/* The inline constant for integer zero, read back from `v_mov_b32 v15, 0` = 0x7e1e0280, whose
 * src0 is 0x80. **It is also the float zero**: the bit pattern of 0.0f is all zeros, so the one
 * constant serves both, which is not true of any other value in the table. */
#define GLSL_INLINE_ZERO 128u
/* src0 = 255 means "the next dword is the operand". */
#define GLSL_SRC_LITERAL 255u
/* The same inline zero as a scalar operand, where the field is eight bits rather than nine. */
#define GLSL_SRC_INLINE_ZERO 128u

/* -------------------------------------------------------------------------
 * The code buffer
 * ------------------------------------------------------------------------- */

void glsl_code_init(glsl_code_t *c, uint32_t *words, uint32_t capacity) {
    if (!c) return;
    c->words = words;
    c->capacity = capacity;
    c->count = 0;
    c->overflow = GL_FALSE;
}

/* **Overflow is recorded, never wrapped or truncated silently.** A shader that ran off the end
 * of its buffer would otherwise emit a prefix of itself, which is a valid instruction stream
 * that stops in the middle - the GPU would execute it and hang or draw nothing. */
static void put(glsl_code_t *c, uint32_t word) {
    if (!c || !c->words) return;
    if (c->count >= c->capacity) {
        c->overflow = GL_TRUE;
        return;
    }
    c->words[c->count++] = word;
}

/* -------------------------------------------------------------------------
 * Instruction formats
 *
 * Field positions verified against clang's output; see the file comment.
 * ------------------------------------------------------------------------- */

/* VOP2: `0 opcode[30:25] vdst[24:17] vsrc1[16:9] src0[8:0]`
 *
 * `vsrc1` is a bare VGPR *number*, not a 256-biased operand - the two source fields are encoded
 * differently and swapping the convention is the easiest mistake in this format. Verified from
 * `v_mul_f32 v4, v8, v0` = 0x10080108: src0 = 0x108 (v8, biased) and vsrc1 = 0x00 (v0, bare). */
void glsl_emit_vop2(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0,
                    uint32_t vsrc1) {
    put(c, ((opcode & 0x3fu) << 25) | ((vdst & 0xffu) << 17) | ((vsrc1 & 0xffu) << 9) |
              (src0 & 0x1ffu));
}

/* VOP1: `0111111 vdst[24:17] opcode[16:9] src0[8:0]`
 *
 * The leading 0111111 is what distinguishes it from VOP2, whose top bit is also 0 - so a VOP1
 * opcode written into a VOP2 word decodes as some entirely different VOP2 instruction rather
 * than as anything invalid. Verified from `v_mov_b32 v14, v4` = 0x7e1c0304. */
void glsl_emit_vop1(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0) {
    put(c, (0x3fu << 25) | ((vdst & 0xffu) << 17) | ((opcode & 0xffu) << 9) | (src0 & 0x1ffu));
}

/* -------------------------------------------------------------------------
 * The instructions the back end needs so far
 * ------------------------------------------------------------------------- */

void glsl_emit_mul_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_MUL_F32, d, glsl_vgpr(s0), s1);
}

void glsl_emit_add_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_ADD_F32, d, glsl_vgpr(s0), s1);
}

/* `d = s0 - s1`. **Not an add with a negated operand**, and the order is part of the meaning:
 * VOP2 subtracts `vsrc1` from `src0`, and there is a separate `v_subrev_f32` at the next opcode
 * for the other order. Writing the operands the wrong way round computes `s1 - s0` and nothing
 * anywhere complains. Verified from `v_sub_f32 v4, v8, v9` = 0x08081308. */
void glsl_emit_sub_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_SUB_F32, d, glsl_vgpr(s0), s1);
}

/* `d = -s`, as zero minus the operand with the zero inline - so unary minus costs one
 * instruction and no register. Verified from `v_sub_f32 v5, 0, v8` = 0x080a1080. */
void glsl_emit_neg_f32(glsl_code_t *c, uint32_t d, uint32_t s) {
    glsl_emit_vop2(c, GLSL_VOP2_SUB_F32, d, GLSL_INLINE_ZERO, s);
}

/* `d += s0 * s1`, fused. The accumulator is the destination, which is why this is the shape a
 * matrix multiply wants: three registers, not four. */
void glsl_emit_fmac_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_FMAC_F32, d, glsl_vgpr(s0), s1);
}

void glsl_emit_mov(glsl_code_t *c, uint32_t d, uint32_t s) {
    glsl_emit_vop1(c, GLSL_VOP1_MOV_B32, d, glsl_vgpr(s));
}

/* **An inline constant where one exists, a literal otherwise.** The two encode differently -
 * an inline constant is a src0 value on its own, a literal is src0 = 255 plus a following
 * dword - and using a literal where an inline constant would do costs a dword per use in a
 * shader where instruction count is the budget. 1.0 is the one this needs today; the table is
 * larger and can grow when something wants another. */
void glsl_emit_mov_imm(glsl_code_t *c, uint32_t d, uint32_t bits) {
    if (bits == 0x3f800000u) {
        glsl_emit_vop1(c, GLSL_VOP1_MOV_B32, d, GLSL_INLINE_ONE_F32);
        return;
    }
    if (bits == 0u) {
        glsl_emit_vop1(c, GLSL_VOP1_MOV_B32, d, GLSL_INLINE_ZERO);
        return;
    }
    glsl_emit_vop1(c, GLSL_VOP1_MOV_B32, d, GLSL_SRC_LITERAL);
    put(c, bits);
}

void glsl_emit_vop1_op(glsl_code_t *c, uint32_t opcode, uint32_t d, uint32_t s) {
    glsl_emit_vop1(c, opcode, d, glsl_vgpr(s));
}

void glsl_emit_vop2_op(glsl_code_t *c, uint32_t opcode, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, opcode, d, glsl_vgpr(s0), s1);
}

/* `d = vcc_lo ? s1 : s0`. **The false value is `src0` and the true one is `vsrc1`**, which is the
 * opposite of how the C conditional reads left to right - and an implementation that swapped
 * them would compile every `?:` in every shader to the other branch. Verified from
 * `v_cndmask_b32_e32 v4, v5, v6, vcc_lo` = 0x02080d05: src0 is v5 and vsrc1 is v6. */
void glsl_emit_cndmask(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_CNDMASK, d, glsl_vgpr(s0), s1);
}

/* VOPC: `0111110 opcode[24:17] vsrc1[16:9] src0[8:0]`, the result into `vcc_lo`. Wave32, so it
 * is `vcc_lo` and not `vcc`; the encodings differ and assembling for the wrong width is the
 * mistake `tools/shader/README.md` exists to prevent. Verified from
 * `v_cmp_lt_f32_e32 vcc_lo, v4, v5` = 0x7c020b04. */
void glsl_emit_cmp(glsl_code_t *c, uint32_t opcode, uint32_t s0, uint32_t s1) {
    put(c, (0x3eu << 25) | ((opcode & 0xffu) << 17) | ((s1 & 0xffu) << 9) |
              (glsl_vgpr(s0) & 0x1ffu));
}

/* `s_and_b32 exec_lo, exec_lo, vcc_lo` - the lane kill. Not computed: this exact word is
 * already in the tree behind `glAlphaFunc` and the polygon stipple, which is the cross-check
 * that the comparison above lands where those two land. */
void glsl_emit_kill_from_vcc(glsl_code_t *c) {
    put(c, 0x877e6a7eu);
}

/* `s_mov_b32 m0, s<n>` - the parameter cache address every `v_interp` below reads.
 *
 * `s_mov_b32 m0, s0` = 0xbefc0300 and `s_mov_b32 m0, s2` = 0xbefc0302, which are the words the
 * payload's own `ps_untex` and `ps_tex` carry (`gl_context.c`) and what clang assembles for
 * gfx1030. **No wait state separates it from the interpolation**: LLVM emits `s_mov_b32 m0, s0`
 * immediately before `v_interp_p1_f32` for an `amdgpu_ps` function, and its hazard recogniser
 * is what would have padded the pair if the part needed it.
 *
 * The header says what leaving this out looks like, which is not a blank screen. */
void glsl_emit_s_mov_m0(glsl_code_t *c, uint32_t ssrc) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, GLSL_SREG_M0, ssrc);
}

/* VINTRP: `110010 vdst[25:18] opcode[17:16] attr[15:10] chan[9:8] vsrc[7:0]`, opcode 0 for
 * `p1` and 1 for `p2`.
 *
 * **The two halves are a pair and the second reads what the first wrote.** `p1` takes the i
 * barycentric from v0 and `p2` the j from v1, and putting anything between them that touches
 * `vdst` produces a value that is neither. Field positions verified across four channels and a
 * high attribute - `tools/shader/gl2-fragment.s` - rather than inferred from one example, which
 * would not have separated the attribute from its channel. */
void glsl_emit_interp(glsl_code_t *c, uint32_t vdst, uint32_t attr, uint32_t chan,
                      GLboolean p2) {
    put(c, (0x32u << 26) | ((vdst & 0xffu) << 18) | ((p2 ? 1u : 0u) << 16) |
              ((attr & 0x3fu) << 10) | ((chan & 0x3u) << 8) | (p2 ? 1u : 0u));
}

void glsl_emit_interp_pair(glsl_code_t *c, uint32_t vdst, uint32_t attr, uint32_t chan) {
    glsl_emit_interp(c, vdst, attr, chan, GL_FALSE);
    glsl_emit_interp(c, vdst, attr, chan, GL_TRUE);
}

/* EXP: `111110 ... | en[3:0] | target[9:4] | compr[10] | done[11] | vm[12]`, then a second dword
 * holding the four source registers as four bytes.
 *
 * `done` says this is the shader's last export and `vm` that the exec mask is valid - both are
 * what every pixel shader in this repository sets, and a shader that exports without `done`
 * does not retire. Verified from `exp mrt0 v4, v5, v6, v7 done vm` = 0xf800180f, 0x07060504. */
/* **DPP: the same instruction, reading a neighbour's register instead of its own.**
 *
 * Eight bytes - the ordinary VALU word with `src0` set to 0xfa, then a second dword holding the
 * real source register, the permute, and the row and bank masks. `row_mask` and `bank_mask` are
 * 0xf here, which is every lane; a derivative wants the whole quad and nothing narrower.
 *
 * Verified against the assembler: `v_sub_f32_dpp v4, v5, v5 quad_perm:[1,1,3,3] row_mask:0xf
 * bank_mask:0xf` is 0x08080afa / 0xff0005f5, and `v_mov_b32_dpp v4, v5 quad_perm:[0,0,2,2]` is
 * 0x7e0802fa / 0xff0005a0.
 *
 * **Only `src0` can be permuted**, which is why a derivative is two instructions and not one:
 * the far value has to be moved into a register of its own before the subtract can read the
 * near one through the permute. */
static void put_dpp_tail(glsl_code_t *c, uint32_t src, uint32_t ctrl) {
    put(c, (0xffu << 24) | ((ctrl & 0xffu) << 8) | (src & 0xffu));
}

void glsl_emit_dpp_mov(glsl_code_t *c, uint32_t dst, uint32_t src, uint32_t ctrl) {
    put(c, (0x3fu << 25) | ((dst & 0xffu) << 17) | ((GLSL_VOP1_MOV_B32 & 0xffu) << 9) | 0xfau);
    put_dpp_tail(c, src, ctrl);
}

void glsl_emit_dpp_sub(glsl_code_t *c, uint32_t dst, uint32_t src0, uint32_t vsrc1,
                       uint32_t ctrl) {
    put(c, ((GLSL_VOP2_SUB_F32 & 0x3fu) << 25) | ((dst & 0xffu) << 17) |
              ((vsrc1 & 0xffu) << 9) | 0xfau);
    put_dpp_tail(c, src0, ctrl);
}

/* `exp mrtz <reg>, off, off, off` - the depth a shader wrote, on target 8 with only the first
 * channel enabled. Verified as 0xf8000081 / 0x00000004 for `v4`.
 *
 * **No `done` and no `vm`.** `done` marks a shader's *last* export and the colour export that
 * follows carries it; two exports both claiming to be last is a shader that does not retire.
 * The depth goes first because that is the order the export order says - and because the colour
 * export is the one that has to be able to say `done`. */
void glsl_emit_export_mrtz(glsl_code_t *c, uint32_t reg) {
    const uint32_t en = 0x1u;      /* the first channel only: Z is one value */
    const uint32_t target = 8u;    /* MRTZ */
    put(c, (0x3eu << 26) | en | (target << 4));
    put(c, reg & 0xffu);
}

void glsl_emit_export_mrt0(glsl_code_t *c, uint32_t base) {
    const uint32_t en = 0xfu;      /* all four channels */
    const uint32_t target = 0u;    /* MRT0 */
    put(c, (0x3eu << 26) | en | (target << 4) | (1u << 11) | (1u << 12));
    put(c, ((base + 0u) & 0xffu) | (((base + 1u) & 0xffu) << 8) |
              (((base + 2u) & 0xffu) << 16) | (((base + 3u) & 0xffu) << 24));
}

/* -------------------------------------------------------------------------
 * Scalar memory: how a uniform reaches a compiled shader
 * ------------------------------------------------------------------------- */

/* SMEM: `111101 op[25:18] . glc[16] . sdata[12:6] sbase[5:0]`, then a second dword
 * `soffset[31:25] . offset[20:0]`.
 *
 * Three things here are not what they look like:
 *
 *   - **`sbase` is a *pair* index**, so the address in `s[0:1]` is `sbase` 0 and the one in
 *     `s[2:3]` is 1. An SGPR number written here names the pair at twice it.
 *   - **`soffset` is 0x7d - SGPR_NULL - and not zero.** Zero would name `s0` as a second,
 *     register-held offset, which is the address's own low half: the load would come from an
 *     address plus itself.
 *   - **The width is the opcode**, and the five are consecutive - so an off-by-one loads twice
 *     or half as many SGPRs as the shader then reads, which faults nothing and reads whatever
 *     those registers held.
 *   - **The destination's alignment is four, not the width.** Anything four dwords or wider
 *     needs a 4-aligned first register whatever its size, a pair needs 2, a single needs
 *     nothing - read out of the assembler by trying them rather than assumed from the widths.
 *
 * Verified from `tools/shader/gl2-fragment.s` across three destinations, three offsets and all
 * five widths, so the fields are pinned rather than inferred from one example. */
void glsl_emit_s_load(glsl_code_t *c, uint32_t op, uint32_t sdata, uint32_t sbase_pair,
                      uint32_t offset) {
    put(c, (0x3du << 26) | ((op & 0xffu) << 18) | ((sdata & 0x7fu) << 6) | (sbase_pair & 0x3fu));
    put(c, (0x7du << 25) | (offset & 0x1fffffu));
}

/* `s_waitcnt lgkmcnt(0)` - the wait a scalar load needs before anything reads what it loaded.
 *
 * **The counters not being waited on are held at their maximum**, which is what makes the
 * immediate 0xc07f rather than 0: vmcnt is 63 across its two fields (bits 15:14 and 3:0) and
 * expcnt is 7 (bits 6:4). Writing zero there would wait for every outstanding memory and export
 * operation as well - correct, slower, and not what the shader asked for. */
void glsl_emit_s_waitcnt_lgkm(glsl_code_t *c) {
    put(c, 0xbf8cc07fu);
}

/* -------------------------------------------------------------------------
 * Sampling a texture
 * ------------------------------------------------------------------------- */

/* MIMG:
 *
 *     word0: `111100 . opcode[24:18] . glc[13] unrm[12] dmask[11:8] . dim[5:3] .`
 *     word1: `ssamp[25:21] srsrc[20:16] vdata[15:8] vaddr[7:0]`
 *
 * **`srsrc` and `ssamp` are SGPR numbers divided by four**, because both fields are five bits
 * and both operands are register *groups*. Writing the register number straight in names a
 * descriptor four times further up the file - which is not a fault, just a sample of whatever
 * is there.
 *
 * `vaddr` is the first of a run of **consecutive** VGPRs holding the coordinate: two for 2D,
 * three for 3D and cube. `vdata` is the first of four, because `dmask` is 0xf here and the mask
 * decides how many registers come back - a narrower one returns fewer and leaves the rest of the
 * destination holding what it held.
 *
 * Field positions verified across two destinations, two coordinate pairs, two descriptor sets,
 * two masks and all four dimensions - `tools/shader/gl2-fragment.s`. The cross-check that this
 * is right rather than self-consistent is `image_sample_lz` at opcode 39, which assembles to
 * `0xf09c0f08 0x00610402`: the two words `tex-prolog.s` records as what the textured pixel
 * shader used to carry. */
void glsl_emit_image_sample(glsl_code_t *c, uint32_t opcode, uint32_t dim, uint32_t vdata,
                            uint32_t vaddr, uint32_t srsrc, uint32_t ssamp) {
    const uint32_t dmask = 0xfu;    /* all four channels: a vec4 result */
    put(c, (0x3cu << 26) | ((opcode & 0x7fu) << 18) | (dmask << 8) | ((dim & 0x7u) << 3));
    put(c, (((ssamp / 4u) & 0x1fu) << 21) | (((srsrc / 4u) & 0x1fu) << 16) |
              ((vdata & 0xffu) << 8) | (vaddr & 0xffu));
}

/* `s_waitcnt vmcnt(0)` - the wait a sample needs before anything reads what it returned. A
 * different counter from the scalar loads' `lgkmcnt`, and waiting on the wrong one waits for
 * something that has already happened. */
void glsl_emit_s_waitcnt_vm(glsl_code_t *c) {
    put(c, 0xbf8c3f70u);
}

/* `s_wqm_b32 exec_lo, exec_lo` - **whole-quad mode**, which is what makes an implicit derivative
 * exist at all. `image_sample` takes its level of detail from how the coordinate changes across
 * the 2x2 quad, and a fragment's neighbours in that quad may be outside the primitive and so not
 * running. This turns them on; the caller puts the real mask back before anything writes. */
void glsl_emit_wqm(glsl_code_t *c) {
    glsl_emit_sop1(c, GLSL_SOP1_WQM_B32, GLSL_SREG_EXEC_LO, GLSL_SREG_EXEC_LO);
}

/* `s_mov_b32 sN, exec_lo` - keeping the live-lane mask across the whole-quad section. */
void glsl_emit_exec_save(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, saved, GLSL_SREG_EXEC_LO);
}

/* -------------------------------------------------------------------------
 * Control flow, which on this machine is the exec mask
 * ------------------------------------------------------------------------- */

/* SOP1: `101111101 sdst[22:16] op[15:8] ssrc0[7:0]`. Verified from
 * `s_mov_b32 exec_lo, s4` = 0xbefe0304 and `s_and_saveexec_b32 s4, vcc_lo` = 0xbe843c6a. */
void glsl_emit_sop1(glsl_code_t *c, uint32_t op, uint32_t sdst, uint32_t ssrc0) {
    put(c, (0x17du << 23) | ((sdst & 0x7fu) << 16) | ((op & 0xffu) << 8) | (ssrc0 & 0xffu));
}

/* SOP2: `10 op[29:23] sdst[22:16] ssrc1[15:8] ssrc0[7:0]`. Verified from
 * `s_andn2_b32 exec_lo, s4, exec_lo` = 0x8a7e7e04 - and the cross-check that the fields are
 * right rather than merely consistent is `s_and_b32 exec_lo, exec_lo, vcc_lo` = 0x877e6a7e,
 * which is the word already in the tree behind glAlphaFunc and the polygon stipple.
 *
 * **`ssrc0` is the one that is not negated.** `s_andn2_b32 d, a, b` is `a & ~b`, so the saved
 * mask goes in `ssrc0` and the mask to remove in `ssrc1`; the other way round computes
 * `~saved & mask`, which is a set of lanes that were not running in the first place. */
void glsl_emit_sop2(glsl_code_t *c, uint32_t op, uint32_t sdst, uint32_t ssrc0, uint32_t ssrc1) {
    put(c, (0x2u << 30) | ((op & 0x7fu) << 23) | ((sdst & 0x7fu) << 16) |
              ((ssrc1 & 0xffu) << 8) | (ssrc0 & 0xffu));
}

/* `s_and_saveexec_b32 sN, vcc_lo` - the whole of an `if`'s entry in one instruction: `sN` takes
 * the exec mask as it was, and exec becomes the lanes that were running **and** satisfy the
 * comparison just made. */
void glsl_emit_exec_save_and_vcc(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop1(c, GLSL_SOP1_AND_SAVEEXEC_B32, saved, GLSL_SREG_VCC_LO);
}

/* The `else`: `exec = saved & ~exec`, the lanes that were running and did *not* take the then
 * arm. Reading the current exec, so it has to come after the then arm and before the else one. */
void glsl_emit_exec_else(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop2(c, GLSL_SOP2_ANDN2_B32, GLSL_SREG_EXEC_LO, saved, GLSL_SREG_EXEC_LO);
}

void glsl_emit_exec_restore(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, GLSL_SREG_EXEC_LO, saved);
}

/* `saved &= ~exec` - **what makes a discard permanent.** A discarded lane taken only out of
 * `exec` comes back the moment the enclosing `if` restores; taking it out of the saved mask as
 * well is what stops that, and it has to happen at every enclosing level. */
void glsl_emit_exec_drop_live(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop2(c, GLSL_SOP2_ANDN2_B32, saved, saved, GLSL_SREG_EXEC_LO);
}

/* -------------------------------------------------------------------------
 * Branches, which an `if` does not need and a loop cannot do without
 *
 * Everything above this point is branchless on purpose: a body run with `exec` zero writes
 * nothing, so an `if` is a mask and not a jump. A **loop** is the case where that stops being
 * enough. Going round again is not something a mask can express, so the tail of a loop is a
 * real backward branch - and a backward branch is the one construct in this back end that can
 * fail in a way worse than drawing wrongly. A condition that never goes false does not produce
 * a bad frame; it does not produce a frame.
 *
 * Words from `tools/shader/branch.s`. SOPP is `101111111 op[22:16] simm16[15:0]`, the same
 * family as `s_nop` (op 0) and `s_endpgm` (op 1) already in this file - which is the
 * cross-check that the opcode field is where this thinks it is.
 * ------------------------------------------------------------------------- */

void glsl_emit_sopp(glsl_code_t *c, uint32_t op, uint32_t simm16) {
    put(c, (0x17fu << 23) | ((op & 0x7fu) << 16) | (simm16 & 0xffffu));
}

/* The next word's index - a label, taken before or after emitting the instruction it names. */
uint32_t glsl_code_here(const glsl_code_t *c) {
    return c ? c->count : 0u;
}

/* **`simm16` counts from the instruction *after* the branch, not from the branch.** A branch to
 * itself is -1 and a branch over nothing is 0; assembling `branch.s` gives -2 for a jump back
 * over one instruction and +4, +3, +2 for the three forward jumps, which is this rule and not
 * the tempting one. Getting it off by one lands mid-loop, which is a hang rather than a fault.
 */
static uint32_t branch_offset(uint32_t from, uint32_t target) {
    return (uint32_t)((int32_t)target - (int32_t)from - 1) & 0xffffu;
}

/* A branch whose target is already behind us: the loop tail. */
void glsl_emit_branch_back(glsl_code_t *c, uint32_t op, uint32_t target) {
    glsl_emit_sopp(c, op, branch_offset(glsl_code_here(c), target));
}

/* A branch whose target is not emitted yet. Returns the word to hand to
 * `glsl_patch_branch_here` once it is; the placeholder is `s_nop`-shaped so that a stream left
 * unpatched by a bug stops rather than jumping somewhere arbitrary. */
uint32_t glsl_emit_branch_fwd(glsl_code_t *c, uint32_t op) {
    const uint32_t at = glsl_code_here(c);
    glsl_emit_sopp(c, op, 0u);
    return at;
}

/* Fills in a forward branch's offset now that its target is the next word.
 *
 * **Returns false rather than patching a lie.** If the buffer overflowed, `count` stopped
 * advancing somewhere in between, so the distance between the branch and here is not the
 * distance the hardware will see - and `at` may be past the end entirely. The caller treats a
 * false as a failed compile, which it already is by the time this can happen.
 */
GLboolean glsl_patch_branch_here(glsl_code_t *c, uint32_t at) {
    if (!c || !c->words || c->overflow || at >= c->count) return GL_FALSE;
    c->words[at] = (c->words[at] & ~0xffffu) | branch_offset(at, c->count);
    return GL_TRUE;
}

/* SOPC: `101111110 op[22:16] ssrc1[15:8] ssrc0[7:0]`, the scalar compare that sets SCC.
 * Verified from `s_cmp_ge_u32 s20, 0x100` = 0xbf09ff14 followed by the literal 0x00000100, and
 * `s_cmp_lg_u32 s20, 0` = 0xbf078014 where the 0 rides in the operand as inline constant 0x80. */
/* VOP3: `110101 op[25:16] ... vdst[7:0]`, then `src0[8:0] src1[17:9] src2[26:18]`. Verified from
 * `tools/shader/tex-cube.s`, where `v_cubeid_f32 v19, v16, v17, v18` assembles to 0xd5440013
 * followed by 0x044a2310 - the three operands being 0x110, 0x111 and 0x112, which is 256 plus
 * the register number, because a VOP3 source names the whole operand space and not just a VGPR.
 *
 * **Three sources is why these need VOP3 at all.** The face selection reads x, y and z at once;
 * there is no two-operand form of it, and no way to reach it from a library that only ever
 * emitted VOP1 and VOP2. */
void glsl_emit_vop3(glsl_code_t *c, uint32_t op, uint32_t vdst, uint32_t src0, uint32_t src1,
                    uint32_t src2) {
    put(c, (0x35u << 26) | ((op & 0x3ffu) << 16) | (vdst & 0xffu));
    put(c, (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9) | ((src2 & 0x1ffu) << 18));
}

void glsl_emit_sopc(glsl_code_t *c, uint32_t op, uint32_t ssrc0, uint32_t ssrc1) {
    put(c, (0x17eu << 23) | ((op & 0x7fu) << 16) | ((ssrc1 & 0xffu) << 8) | (ssrc0 & 0xffu));
}

/* `s_cmp_ge_u32 sN, imm`, taking the inline constants when it can and a trailing literal when
 * it cannot. The scalar inline range is 0..64 in operands 128..192, so a trip ceiling above 64
 * costs a second word - which is why the ceiling is a constant this file knows rather than
 * something the shader computes. */
void glsl_emit_s_cmp_ge_u32_imm(glsl_code_t *c, uint32_t sreg, uint32_t imm) {
    if (imm <= 64u) {
        glsl_emit_sopc(c, GLSL_SOPC_CMP_GE_U32, sreg, GLSL_SRC_INLINE_ZERO + imm);
    } else {
        glsl_emit_sopc(c, GLSL_SOPC_CMP_GE_U32, sreg, GLSL_SRC_LITERAL);
        put(c, imm);
    }
}

/* `s_add_u32 sN, sN, 1` - the trip counter's step. From `s_add_u32 s20, s20, 1` = 0x80148114. */
void glsl_emit_s_inc_u32(glsl_code_t *c, uint32_t sreg) {
    glsl_emit_sop2(c, GLSL_SOP2_ADD_U32, sreg, sreg, GLSL_SRC_INLINE_ZERO + 1u);
}

/* `s_mov_b32 exec_lo, 0` - no lane writes anything after this. The export still runs and still
 * carries `done`, which is what retires the wave; a shader that discarded every lane and then
 * skipped its export would not. */
void glsl_emit_exec_clear(glsl_code_t *c) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, GLSL_SREG_EXEC_LO, GLSL_SRC_INLINE_ZERO);
}

void glsl_emit_nop(glsl_code_t *c) {
    /* 0xbf800000, which is what every unused patch slot in the payload already holds. */
    put(c, 0xbf800000u);
}

void glsl_emit_endpgm(glsl_code_t *c) {
    /* Not computed: this exact word ends every hand-written shader in this repository, and
     * clang assembles `s_endpgm` to the same thing. It is the cross-check that the encodings
     * above are right rather than merely consistent with each other. */
    put(c, 0xbf810000u);
}

/* -------------------------------------------------------------------------
 * `mat4 * vec4`, which is what a vertex shader is mostly made of
 * ------------------------------------------------------------------------- */

/* Emits `dst[0..3] = m * v`, column-major, with `m` a block of sixteen consecutive VGPRs and
 * `v` a block of four.
 *
 * **Column-major, which is the whole difficulty.** GL stores a matrix as four columns laid end
 * to end, so `m[0..3]` is the first *column* and the product is
 * `col0*v.x + col1*v.y + col2*v.z + col3*v.w`. Treating the block as rows transposes the
 * matrix, and a transposed model-view matrix still draws a cube - just the wrong way round,
 * which is a bug that survives a screenshot.
 *
 * `dst` must not overlap `v`, because the first component of the result is written before the
 * last component of the vector is read. Overlapping `m` is fine.
 */
void glsl_emit_mat_mul_vec(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v, uint32_t n) {
    /* The first column multiplies, the rest accumulate: one instruction per element either
     * way, and no separate zeroing pass.
     *
     * **Column-major**, which is GLSL's storage and the reason this is a sum over columns
     * rather than a dot product per row: element `row` of the result takes `m[col][row]` for
     * every column, and those are `n` apart in the register run. */
    for (uint32_t row = 0; row < n; row++) {
        glsl_emit_mul_f32(c, dst + row, m + row, v + 0u);
    }
    for (uint32_t col = 1; col < n; col++) {
        for (uint32_t row = 0; row < n; row++) {
            glsl_emit_fmac_f32(c, dst + row, m + col * n + row, v + col);
        }
    }
}

void glsl_emit_mat4_mul_vec4(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v) {
    glsl_emit_mat_mul_vec(c, dst, m, v, 4u);
}

/* `v * m`, which is **not** `m * v`: it is the product with the transpose, so component `i` of
 * the result is the dot of `v` with column `i` rather than with row `i`. A back end that
 * treated the two as one operation would give the same answer for both, and would be right
 * only for a symmetric matrix.
 *
 * A column is contiguous here - column `i` starts at `m + i*n` - so each component is a
 * multiply and `n-1` accumulates over consecutive registers. */
void glsl_emit_vec_mul_mat(glsl_code_t *c, uint32_t dst, uint32_t v, uint32_t m, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        glsl_emit_mul_f32(c, dst + i, v + 0u, m + i * n);
        for (uint32_t row = 1; row < n; row++) {
            glsl_emit_fmac_f32(c, dst + i, v + row, m + i * n + row);
        }
    }
}
