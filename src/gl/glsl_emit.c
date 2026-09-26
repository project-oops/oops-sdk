/*
 * oops-gl: RDNA2 instruction encoding for the GLSL back end
 *
 * Turns the tree's arithmetic into gfx1030 words. A wrong encoding cannot fail loudly -
 * it assembles, and the hardware does something else - so every field position here is
 * read out of clang's assembler, not written from memory. The `.s` listings under
 * `tools/shader/` hold the instructions, and the tests assert the encoder reproduces
 * clang's words (`clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c <file>.s`).
 *
 * The shaders run wave32 (`VGT_SHADER_STAGES_EN` sets `GS_W32` and `VS_W32`), so a mask
 * operand is `vcc_lo` or `exec_lo`.
 */

#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * Operands
 *
 * A source operand is nine bits and its meaning depends on the value: 0..101 are SGPRs
 * and special registers, 128..192 are inline constants, 255 says a 32-bit literal
 * follows the instruction, and 256..511 are VGPRs. A VGPR number without the 256 names
 * an SGPR instead.
 * ------------------------------------------------------------------------- */

uint32_t glsl_vgpr(uint32_t n) {
    return 256u + n;
}

/* An SGPR's operand value is its own number, the low end of the same nine-bit space.
 * Identity, and written out anyway so a call site says which space it means. */
uint32_t glsl_sgpr(uint32_t n) {
    return n;
}

/* The inline constant for 1.0f, read back from `v_mov_b32 v12, 1.0` which assembled
 * with src0 = 0xf2. The inline constants are a small closed table; anything outside it
 * needs a literal, and `glsl_emit_mov_imm` decides which. */
#define GLSL_INLINE_ONE_F32 242u
/* The inline constant for integer zero, read back from `v_mov_b32 v15, 0` = 0x7e1e0280,
 * whose src0 is 0x80. It is also the float zero: 0.0f is all zero bits. */
#define GLSL_INLINE_ZERO 128u
/* src0 = 255 means "the next dword is the operand". */
#define GLSL_SRC_LITERAL 255u
/* The same inline zero as a scalar operand, where the field is eight bits rather than
 * nine. */
#define GLSL_SRC_INLINE_ZERO 128u

/* -------------------------------------------------------------------------
 * The code buffer
 * ------------------------------------------------------------------------- */

void glsl_code_init(glsl_code_t *c, uint32_t *words, uint32_t capacity) {
    if (!c)
        return;
    c->words = words;
    c->capacity = capacity;
    c->count = 0;
    c->overflow = GL_FALSE;
}

/* Overflow is recorded, never truncated silently: a prefix of a shader is a valid
 * instruction stream that stops in the middle, and the GPU would run it. */
static void put(glsl_code_t *c, uint32_t word) {
    if (!c || !c->words)
        return;
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
 * `vsrc1` is a bare VGPR number, not a 256-biased operand like `src0`. Verified from
 * `v_mul_f32 v4, v8, v0` = 0x10080108: src0 = 0x108 (v8, biased) and vsrc1 = 0x00 (v0,
 * bare). */
void glsl_emit_vop2(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0,
                    uint32_t vsrc1) {
    put(c, ((opcode & 0x3fu) << 25) | ((vdst & 0xffu) << 17) | ((vsrc1 & 0xffu) << 9) |
               (src0 & 0x1ffu));
}

/* VOP1: `0111111 vdst[24:17] opcode[16:9] src0[8:0]`
 *
 * The leading 0111111 distinguishes it from VOP2, whose top bit is also 0. Verified
 * from `v_mov_b32 v14, v4` = 0x7e1c0304. */
void glsl_emit_vop1(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0) {
    put(c, (0x3fu << 25) | ((vdst & 0xffu) << 17) | ((opcode & 0xffu) << 9) |
               (src0 & 0x1ffu));
}

/* -------------------------------------------------------------------------
 * Vector ALU instructions
 * ------------------------------------------------------------------------- */

void glsl_emit_mul_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_MUL_F32, d, glsl_vgpr(s0), s1);
}

void glsl_emit_add_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_ADD_F32, d, glsl_vgpr(s0), s1);
}

/* `d = s0 - s1`: VOP2 subtracts `vsrc1` from `src0`, and `v_subrev_f32` at the next
 * opcode is the other order. Verified from `v_sub_f32 v4, v8, v9` = 0x08081308. */
void glsl_emit_sub_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_SUB_F32, d, glsl_vgpr(s0), s1);
}

/* `d = -s`, as zero minus the operand with the zero inline - so unary minus costs one
 * instruction and no register. Verified from `v_sub_f32 v5, 0, v8` = 0x080a1080. */
void glsl_emit_neg_f32(glsl_code_t *c, uint32_t d, uint32_t s) {
    glsl_emit_vop2(c, GLSL_VOP2_SUB_F32, d, GLSL_INLINE_ZERO, s);
}

/* `d += s0 * s1`, fused. The accumulator is the destination, which is why this is the
 * shape a matrix multiply wants: three registers, not four. */
void glsl_emit_fmac_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_FMAC_F32, d, glsl_vgpr(s0), s1);
}

void glsl_emit_mov(glsl_code_t *c, uint32_t d, uint32_t s) {
    glsl_emit_vop1(c, GLSL_VOP1_MOV_B32, d, glsl_vgpr(s));
}

/* An inline constant where one exists, a literal otherwise. An inline constant is a
 * src0 value on its own; a literal is src0 = 255 plus a following dword. */
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

void glsl_emit_vop2_op(glsl_code_t *c, uint32_t opcode, uint32_t d, uint32_t s0,
                       uint32_t s1) {
    glsl_emit_vop2(c, opcode, d, glsl_vgpr(s0), s1);
}

/* `d = vcc_lo ? s1 : s0`. The false value is `src0` and the true one is `vsrc1`, the
 * opposite of how the C conditional reads. Verified from
 * `v_cndmask_b32_e32 v4, v5, v6, vcc_lo` = 0x02080d05: src0 is v5 and vsrc1 is v6. */
void glsl_emit_cndmask(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1) {
    glsl_emit_vop2(c, GLSL_VOP2_CNDMASK, d, glsl_vgpr(s0), s1);
}

/* VOPC: `0111110 opcode[24:17] vsrc1[16:9] src0[8:0]`, the result into `vcc_lo`.
 * Wave32, so it is `vcc_lo` and not `vcc` (`tools/shader/README.md`). Verified from
 * `v_cmp_lt_f32_e32 vcc_lo, v4, v5` = 0x7c020b04. */
void glsl_emit_cmp(glsl_code_t *c, uint32_t opcode, uint32_t s0, uint32_t s1) {
    put(c, (0x3eu << 25) | ((opcode & 0xffu) << 17) | ((s1 & 0xffu) << 9) |
               (glsl_vgpr(s0) & 0x1ffu));
}

/* `s_and_b32 exec_lo, exec_lo, vcc_lo` - the lane kill. The same word `glAlphaFunc`
 * and the polygon stipple use. */
void glsl_emit_kill_from_vcc(glsl_code_t *c) {
    put(c, 0x877e6a7eu);
}

/* `s_mov_b32 m0, s<n>` - the parameter cache address every `v_interp` below reads.
 *
 * `s_mov_b32 m0, s0` = 0xbefc0300 and `s_mov_b32 m0, s2` = 0xbefc0302, the words the
 * payload's own `ps_untex` and `ps_tex` carry (`gl_context.c`). No wait state separates
 * it from the interpolation: LLVM emits it immediately before `v_interp_p1_f32` for an
 * `amdgpu_ps` function, and its hazard recogniser would pad the pair if the part needed
 * it. */
void glsl_emit_s_mov_m0(glsl_code_t *c, uint32_t ssrc) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, GLSL_SREG_M0, ssrc);
}

/* VINTRP: `110010 vdst[25:18] opcode[17:16] attr[15:10] chan[9:8] vsrc[7:0]`, opcode 0
 * for `p1` and 1 for `p2`.
 *
 * The two halves are a pair and the second reads what the first wrote: `p1` takes the i
 * barycentric from v0 and `p2` the j from v1, and nothing between them may touch
 * `vdst`. Field positions verified across four channels and a high attribute in
 * `tools/shader/gl2-fragment.s`. */
void glsl_emit_interp(glsl_code_t *c, uint32_t vdst, uint32_t attr, uint32_t chan,
                      GLboolean p2) {
    put(c, (0x32u << 26) | ((vdst & 0xffu) << 18) | ((p2 ? 1u : 0u) << 16) |
               ((attr & 0x3fu) << 10) | ((chan & 0x3u) << 8) | (p2 ? 1u : 0u));
}

void glsl_emit_interp_pair(glsl_code_t *c, uint32_t vdst, uint32_t attr,
                           uint32_t chan) {
    glsl_emit_interp(c, vdst, attr, chan, GL_FALSE);
    glsl_emit_interp(c, vdst, attr, chan, GL_TRUE);
}

/* DPP: the same instruction, reading a neighbour's register instead of its own. The
 * ordinary VALU word with `src0` set to 0xfa, then a second dword holding the real
 * source register, the permute, and the row and bank masks (0xf, every lane: a
 * derivative wants the whole quad). Verified: `v_sub_f32_dpp v4, v5, v5
 * quad_perm:[1,1,3,3] row_mask:0xf bank_mask:0xf` is 0x08080afa / 0xff0005f5, and
 * `v_mov_b32_dpp v4, v5 quad_perm:[0,0,2,2]` is 0x7e0802fa / 0xff0005a0.
 *
 * Only `src0` can be permuted, so a derivative is two instructions: the far value is
 * moved into its own register before the subtract reads the near one through the
 * permute. */
static void put_dpp_tail(glsl_code_t *c, uint32_t src, uint32_t ctrl) {
    put(c, (0xffu << 24) | ((ctrl & 0xffu) << 8) | (src & 0xffu));
}

void glsl_emit_dpp_mov(glsl_code_t *c, uint32_t dst, uint32_t src, uint32_t ctrl) {
    put(c, (0x3fu << 25) | ((dst & 0xffu) << 17) | ((GLSL_VOP1_MOV_B32 & 0xffu) << 9) |
               0xfau);
    put_dpp_tail(c, src, ctrl);
}

void glsl_emit_dpp_sub(glsl_code_t *c, uint32_t dst, uint32_t src0, uint32_t vsrc1,
                       uint32_t ctrl) {
    put(c, ((GLSL_VOP2_SUB_F32 & 0x3fu) << 25) | ((dst & 0xffu) << 17) |
               ((vsrc1 & 0xffu) << 9) | 0xfau);
    put_dpp_tail(c, src0, ctrl);
}

/* EXP: `111110 ... | en[3:0] | target[9:4] | compr[10] | done[11] | vm[12]`, then a
 * second dword holding the four source registers as four bytes. `done` marks the
 * shader's last export and `vm` a valid exec mask; a shader that never exports with
 * `done` does not retire. Verified from `exp mrt0 v4, v5, v6, v7 done vm` = 0xf800180f,
 * 0x07060504. */

/* `exp mrtz <reg>, off, off, off` - the depth a shader wrote, on target 8 with only the
 * first channel enabled. Verified as 0xf8000081 / 0x00000004 for `v4`. No `done` and no
 * `vm`: the colour export follows it and carries `done`. */
void glsl_emit_export_mrtz(glsl_code_t *c, uint32_t reg) {
    const uint32_t en = 0x1u;   /* the first channel only: Z is one value */
    const uint32_t target = 8u; /* MRTZ */
    put(c, (0x3eu << 26) | en | (target << 4));
    put(c, reg & 0xffu);
}

/* Four floats are packed into two registers first, because an 8_8_8_8 target on a part
 * with RB+ requires the half-float export format. Mesa's `ac_choose_spi_color_formats`
 * (amd/common/ac_shader_util.c:672-693) calls these "required values for RB+", and RB+
 * is allowed on every GFX10_3 part (ac_gpu_info.c:1117-1125). Four 32-bit floats land
 * correctly unblended but scatter a blended result across RB+'s sixteen-byte
 * transaction. This matches `gl_ps_patch_export` in gl_context.c, and both stay in step
 * with `SPI_SHADER_COL_FORMAT`, 4 (`FP16_ABGR`) in gl_draw.c's frame table.
 *
 * The sequence is Mesa's (`ac_nir_lower_ps_late.c:479-506`): pack (R,G) and (B,A) with
 * `pack_half_2x16_rtz_split`, enable all four channels, and set the compressed flag
 * below GFX11. `v_cvt_pkrtz_f16_f32_e32` is VOP2 opcode 0x2f, so the word is
 * `0x5e000000 | vdst << 17 | vsrc1 << 9 | (256 + vsrc0)`, LLVM's encoding for gfx1030.
 */
void glsl_emit_export_mrt0(glsl_code_t *c, uint32_t base) {
    const uint32_t en = 0xfu;   /* all four channels, in two packed registers */
    const uint32_t target = 0u; /* MRT0 */
    const uint32_t r = (base + 0u) & 0xffu, g = (base + 1u) & 0xffu;
    const uint32_t b = (base + 2u) & 0xffu, a = (base + 3u) & 0xffu;

    /* The packs land in the first two of the four, which the export then reads. Nothing
       runs after the export, so overwriting the colour it was given costs nothing. */
    put(c, 0x5e000000u | (r << 17) | (g << 9) |
               (256u + r)); /* v_cvt_pkrtz_f16_f32 r, r, g */
    put(c, 0x5e000000u | (g << 17) | (a << 9) |
               (256u + b)); /* v_cvt_pkrtz_f16_f32 g, b, a */

    put(c, (0x3eu << 26) | en | (target << 4) | (1u << 10) | (1u << 11) | (1u << 12));
    put(c, r | (g << 8));
}

/* -------------------------------------------------------------------------
 * Scalar memory: how a uniform reaches a compiled shader
 * ------------------------------------------------------------------------- */

/* SMEM: `111101 op[25:18] . glc[16] . sdata[12:6] sbase[5:0]`, then a second dword
 * `soffset[31:25] . offset[20:0]`.
 *
 * `sbase` is a pair index: the address in `s[0:1]` is `sbase` 0 and `s[2:3]` is 1.
 * `soffset` is 0x7d (SGPR_NULL), not zero, which would name `s0` as a second offset.
 * The width is the opcode, and the five are consecutive. The destination's alignment
 * is four for anything four dwords or wider, two for a pair, none for a single.
 *
 * Verified from `tools/shader/gl2-fragment.s` across three destinations, three offsets
 * and all five widths.
 */
void glsl_emit_s_load(glsl_code_t *c, uint32_t op, uint32_t sdata, uint32_t sbase_pair,
                      uint32_t offset) {
    put(c, (0x3du << 26) | ((op & 0xffu) << 18) | ((sdata & 0x7fu) << 6) |
               (sbase_pair & 0x3fu));
    put(c, (0x7du << 25) | (offset & 0x1fffffu));
}

/* `s_waitcnt lgkmcnt(0)` - the wait a scalar load needs before anything reads what it
 * loaded. The other counters are held at their maximum, which makes the immediate
 * 0xc07f: vmcnt is 63 across its two fields (bits 15:14 and 3:0) and expcnt is 7
 * (bits 6:4). Zero there would also wait for every memory and export operation. */
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
 * `srsrc` and `ssamp` are SGPR numbers divided by four: both fields are five bits and
 * both operands are register groups.
 *
 * `vaddr` is the first of a run of consecutive VGPRs holding the coordinate: two for
 * 2D, three for 3D and cube. `vdata` is the first of as many registers as `dmask` has
 * bits; a comparison returns one where a texel returns four.
 * `tools/shader/tex-shadow.s`: `image_sample_c v4, v[16:18], ... dmask:0x1` assembles
 * to 0xf0a00108, against the plain sample's 0xf0800f08.
 *
 * Field positions verified across two destinations, two coordinate pairs, two
 * descriptor sets, two masks and all four dimensions in `tools/shader/gl2-fragment.s`;
 * `image_sample_lz` (opcode 39) assembles to `0xf09c0f08 0x00610402`, the words
 * `tex-prolog.s` records. */
void glsl_emit_image_sample_masked(glsl_code_t *c, uint32_t opcode, uint32_t dim,
                                   uint32_t dmask, uint32_t vdata, uint32_t vaddr,
                                   uint32_t srsrc, uint32_t ssamp) {
    put(c, (0x3cu << 26) | ((opcode & 0x7fu) << 18) | ((dmask & 0xfu) << 8) |
               ((dim & 0x7u) << 3));
    put(c, (((ssamp / 4u) & 0x1fu) << 21) | (((srsrc / 4u) & 0x1fu) << 16) |
               ((vdata & 0xffu) << 8) | (vaddr & 0xffu));
}

void glsl_emit_image_sample(glsl_code_t *c, uint32_t opcode, uint32_t dim,
                            uint32_t vdata, uint32_t vaddr, uint32_t srsrc,
                            uint32_t ssamp) {
    /* All four channels: a vec4 result. */
    glsl_emit_image_sample_masked(c, opcode, dim, 0xfu, vdata, vaddr, srsrc, ssamp);
}

/* `s_waitcnt vmcnt(0)` - the wait a sample needs before anything reads what it
 * returned. A different counter from the scalar loads' `lgkmcnt`. */
void glsl_emit_s_waitcnt_vm(glsl_code_t *c) {
    put(c, 0xbf8c3f70u);
}

/* `s_wqm_b32 exec_lo, exec_lo` - whole-quad mode, which an implicit derivative needs.
 * `image_sample` takes its level of detail from how the
 * coordinate changes across the 2x2 quad, and a fragment's neighbours in that quad may
 * be outside the primitive and so not running. This turns them on; the caller puts the
 * real mask back before anything writes. */
void glsl_emit_wqm(glsl_code_t *c) {
    glsl_emit_sop1(c, GLSL_SOP1_WQM_B32, GLSL_SREG_EXEC_LO, GLSL_SREG_EXEC_LO);
}

/* `s_mov_b32 sN, exec_lo` - keeping the live-lane mask across the whole-quad section.
 */
void glsl_emit_exec_save(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, saved, GLSL_SREG_EXEC_LO);
}

/* -------------------------------------------------------------------------
 * Control flow, which on this machine is the exec mask
 * ------------------------------------------------------------------------- */

/* SOP1: `101111101 sdst[22:16] op[15:8] ssrc0[7:0]`. Verified from
 * `s_mov_b32 exec_lo, s4` = 0xbefe0304 and `s_and_saveexec_b32 s4, vcc_lo` =
 * 0xbe843c6a. */
void glsl_emit_sop1(glsl_code_t *c, uint32_t op, uint32_t sdst, uint32_t ssrc0) {
    put(c, (0x17du << 23) | ((sdst & 0x7fu) << 16) | ((op & 0xffu) << 8) |
               (ssrc0 & 0xffu));
}

/* SOP2: `10 op[29:23] sdst[22:16] ssrc1[15:8] ssrc0[7:0]`. Verified from
 * `s_andn2_b32 exec_lo, s4, exec_lo` = 0x8a7e7e04 and `s_and_b32 exec_lo, exec_lo,
 * vcc_lo` = 0x877e6a7e.
 *
 * `ssrc0` is the one that is not negated: `s_andn2_b32 d, a, b` is `a & ~b`, so the
 * saved mask goes in `ssrc0` and the mask to remove in `ssrc1`. */
void glsl_emit_sop2(glsl_code_t *c, uint32_t op, uint32_t sdst, uint32_t ssrc0,
                    uint32_t ssrc1) {
    put(c, (0x2u << 30) | ((op & 0x7fu) << 23) | ((sdst & 0x7fu) << 16) |
               ((ssrc1 & 0xffu) << 8) | (ssrc0 & 0xffu));
}

/* `s_and_saveexec_b32 sN, vcc_lo` - the whole of an `if`'s entry in one instruction:
 * `sN` takes the exec mask as it was, and exec becomes the lanes that were running
 * and satisfy the comparison just made. */
void glsl_emit_exec_save_and_vcc(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop1(c, GLSL_SOP1_AND_SAVEEXEC_B32, saved, GLSL_SREG_VCC_LO);
}

/* The `else`: `exec = saved & ~exec`, the lanes that were running and did not take the
 * then arm. It reads the current exec, so it comes after the then arm and before the
 * else one. */
void glsl_emit_exec_else(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop2(c, GLSL_SOP2_ANDN2_B32, GLSL_SREG_EXEC_LO, saved, GLSL_SREG_EXEC_LO);
}

void glsl_emit_exec_restore(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, GLSL_SREG_EXEC_LO, saved);
}

/* `saved &= ~exec` - what makes a discard permanent. A lane taken only out of `exec`
 * comes back when the enclosing `if` restores, so it is taken out of the saved mask at
 * every enclosing level. */
void glsl_emit_exec_drop_live(glsl_code_t *c, uint32_t saved) {
    glsl_emit_sop2(c, GLSL_SOP2_ANDN2_B32, saved, saved, GLSL_SREG_EXEC_LO);
}

/* -------------------------------------------------------------------------
 * Branches, which an `if` does not need and a loop cannot do without
 *
 * A body run with `exec` zero writes nothing, so an `if` is a mask and not a jump.
 * Going round a loop again is not something a mask can express, so a loop's tail is a
 * real backward branch, and a condition that never goes false hangs the GPU.
 *
 * Words from `tools/shader/branch.s`. SOPP is `101111111 op[22:16] simm16[15:0]`, the
 * same family as `s_nop` (op 0) and `s_endpgm` (op 1).
 * ------------------------------------------------------------------------- */

void glsl_emit_sopp(glsl_code_t *c, uint32_t op, uint32_t simm16) {
    put(c, (0x17fu << 23) | ((op & 0x7fu) << 16) | (simm16 & 0xffffu));
}

/* The next word's index - a label, taken before or after emitting the instruction it
 * names. */
uint32_t glsl_code_here(const glsl_code_t *c) {
    return c ? c->count : 0u;
}

/* `simm16` counts from the instruction after the branch, not from the branch: a branch
 * to itself is -1 and a branch over nothing is 0. Assembling `branch.s` gives -2 for a
 * jump back over one instruction and +4, +3, +2 for the three forward jumps. */
static uint32_t branch_offset(uint32_t from, uint32_t target) {
    return (uint32_t)((int32_t)target - (int32_t)from - 1) & 0xffffu;
}

/* A branch whose target is already behind us: the loop tail. */
void glsl_emit_branch_back(glsl_code_t *c, uint32_t op, uint32_t target) {
    glsl_emit_sopp(c, op, branch_offset(glsl_code_here(c), target));
}

/* A branch whose target is not emitted yet. Returns the word to hand to
 * `glsl_patch_branch_here` once it is; the placeholder is `s_nop`-shaped so that a
 * stream left unpatched by a bug stops rather than jumping somewhere arbitrary. */
uint32_t glsl_emit_branch_fwd(glsl_code_t *c, uint32_t op) {
    const uint32_t at = glsl_code_here(c);
    glsl_emit_sopp(c, op, 0u);
    return at;
}

/* Fills in a forward branch's offset now that its target is the next word. Returns
 * false after an overflow: `count` stopped advancing, so the distance is wrong and `at`
 * may be past the end. The caller treats false as a failed compile. */
GLboolean glsl_patch_branch_here(glsl_code_t *c, uint32_t at) {
    if (!c || !c->words || c->overflow || at >= c->count)
        return GL_FALSE;
    c->words[at] = (c->words[at] & ~0xffffu) | branch_offset(at, c->count);
    return GL_TRUE;
}

/* VOP3: `110101 op[25:16] ... vdst[7:0]`, then `src0[8:0] src1[17:9] src2[26:18]`.
 * Verified from `tools/shader/tex-cube.s`, where `v_cubeid_f32 v19, v16, v17, v18`
 * assembles to 0xd5440013 followed by 0x044a2310: a VOP3 source names the whole operand
 * space, so each VGPR is 256 plus its number. The cube face selection reads x, y and z
 * at once and has no two-operand form. */
void glsl_emit_vop3(glsl_code_t *c, uint32_t op, uint32_t vdst, uint32_t src0,
                    uint32_t src1, uint32_t src2) {
    put(c, (0x35u << 26) | ((op & 0x3ffu) << 16) | (vdst & 0xffu));
    put(c, (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9) | ((src2 & 0x1ffu) << 18));
}

/* SOPC: `101111110 op[22:16] ssrc1[15:8] ssrc0[7:0]`, the scalar compare that sets SCC.
 * Verified from `s_cmp_ge_u32 s20, 0x100` = 0xbf09ff14 followed by the literal
 * 0x00000100, and `s_cmp_lg_u32 s20, 0` = 0xbf078014 where the 0 is inline constant
 * 0x80. */
void glsl_emit_sopc(glsl_code_t *c, uint32_t op, uint32_t ssrc0, uint32_t ssrc1) {
    put(c, (0x17eu << 23) | ((op & 0x7fu) << 16) | ((ssrc1 & 0xffu) << 8) |
               (ssrc0 & 0xffu));
}

/* `s_cmp_ge_u32 sN, imm`, taking the inline constants when it can and a trailing
 * literal when it cannot. The scalar inline range is 0..64 in operands 128..192. */
void glsl_emit_s_cmp_ge_u32_imm(glsl_code_t *c, uint32_t sreg, uint32_t imm) {
    if (imm <= 64u) {
        glsl_emit_sopc(c, GLSL_SOPC_CMP_GE_U32, sreg, GLSL_SRC_INLINE_ZERO + imm);
    } else {
        glsl_emit_sopc(c, GLSL_SOPC_CMP_GE_U32, sreg, GLSL_SRC_LITERAL);
        put(c, imm);
    }
}

/* `s_add_u32 sN, sN, 1` - the trip counter's step. From `s_add_u32 s20, s20, 1` =
 * 0x80148114. */
void glsl_emit_s_inc_u32(glsl_code_t *c, uint32_t sreg) {
    glsl_emit_sop2(c, GLSL_SOP2_ADD_U32, sreg, sreg, GLSL_SRC_INLINE_ZERO + 1u);
}

/* `s_mov_b32 exec_lo, 0` - no lane writes anything after this. The export still runs
 * and still carries `done`, which is what retires the wave. */
void glsl_emit_exec_clear(glsl_code_t *c) {
    glsl_emit_sop1(c, GLSL_SOP1_MOV_B32, GLSL_SREG_EXEC_LO, GLSL_SRC_INLINE_ZERO);
}

void glsl_emit_nop(glsl_code_t *c) {
    /* 0xbf800000, which is what every unused patch slot in the payload already holds.
     */
    put(c, 0xbf800000u);
}

void glsl_emit_endpgm(glsl_code_t *c) {
    /* The word that ends every hand-written shader in this repository, and what clang
     * assembles `s_endpgm` to. */
    put(c, 0xbf810000u);
}

/* -------------------------------------------------------------------------
 * `mat4 * vec4`, which is what a vertex shader is mostly made of
 * ------------------------------------------------------------------------- */

/* Emits `dst[0..3] = m * v`, column-major, with `m` a block of sixteen consecutive
 * VGPRs and `v` a block of four.
 *
 * GL stores a matrix as columns laid end to end, so `m[0..3]` is the first column and
 * the product is `col0*v.x + col1*v.y + col2*v.z + col3*v.w`.
 *
 * `dst` must not overlap `v`, because the first component of the result is written
 * before the last component of the vector is read. Overlapping `m` is fine.
 */
void glsl_emit_mat_mul_vec_cr(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v,
                              uint32_t cols, uint32_t rows) {
    /* The first column multiplies, the rest accumulate, so there is no zeroing pass.
     * Element `row` of the result takes `m[col][row]` for every column, `rows` apart in
     * the register run. `matCxR * vecC` is a `vecR`. */
    for (uint32_t row = 0; row < rows; row++) {
        glsl_emit_mul_f32(c, dst + row, m + row, v + 0u);
    }
    for (uint32_t col = 1; col < cols; col++) {
        for (uint32_t row = 0; row < rows; row++) {
            glsl_emit_fmac_f32(c, dst + row, m + col * rows + row, v + col);
        }
    }
}

void glsl_emit_mat_mul_vec(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v,
                           uint32_t n) {
    glsl_emit_mat_mul_vec_cr(c, dst, m, v, n, n);
}

void glsl_emit_mat4_mul_vec4(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v) {
    glsl_emit_mat_mul_vec(c, dst, m, v, 4u);
}

/* `v * m`, the product with the transpose: component `i` of the result is the dot of
 * `v` with column `i`. A column is contiguous - column `i` starts at `m + i*rows` - so
 * each component is a multiply and `rows-1` accumulates over consecutive registers. */
void glsl_emit_vec_mul_mat_cr(glsl_code_t *c, uint32_t dst, uint32_t v, uint32_t m,
                              uint32_t cols, uint32_t rows) {
    /* `vecR * matCxR` is a `vecC`: a row vector matches the rows, and the result has
     * one entry per column. */
    for (uint32_t i = 0; i < cols; i++) {
        glsl_emit_mul_f32(c, dst + i, v + 0u, m + i * rows);
        for (uint32_t row = 1; row < rows; row++) {
            glsl_emit_fmac_f32(c, dst + i, v + row, m + i * rows + row);
        }
    }
}

void glsl_emit_vec_mul_mat(glsl_code_t *c, uint32_t dst, uint32_t v, uint32_t m,
                           uint32_t n) {
    glsl_emit_vec_mul_mat_cr(c, dst, v, m, n, n);
}
