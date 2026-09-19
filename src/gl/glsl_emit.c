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
void glsl_emit_mat4_mul_vec4(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v) {
    /* The first column multiplies, the rest accumulate: one instruction per element either
     * way, and no separate zeroing pass. */
    for (uint32_t row = 0; row < 4u; row++) {
        glsl_emit_mul_f32(c, dst + row, m + row, v + 0u);
    }
    for (uint32_t col = 1; col < 4u; col++) {
        for (uint32_t row = 0; row < 4u; row++) {
            glsl_emit_fmac_f32(c, dst + row, m + col * 4u + row, v + col);
        }
    }
}
