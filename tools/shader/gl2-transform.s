// The arithmetic a GLSL 2.0 vertex shader's `mvp * vec4(pos, 1.0)` becomes.
//
// Ground truth for the code generator's encodings. Nothing here ships as a literal: what ships
// is an encoder, and these words are what the encoder's tests assert against. A wrong encoding
// cannot fail loudly, so the encoder is checked against a real assembler rather than itself.
//
//   clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c tools/shader/gl2-transform.s -o /tmp/t.o
//   objdump -s -j .text /tmp/t.o
//
// Wave32, so masks are vcc_lo / exec_lo. See the README beside this file.

.text
.globl gl2_transform
gl2_transform:

// One column of a mat4 times one component of the vector, accumulated. A 4x4 by vec4 is
// sixteen of these; the generator emits them from the AST rather than unrolling by hand.
//
// v0..v2  = pos.xyz as it arrives
// v4..v7  = the accumulating result
// v8..v11 = a matrix column loaded from the uniform buffer

// result = column0 * pos.x
v_mul_f32 v4, v8, v0
v_mul_f32 v5, v9, v0
v_mul_f32 v6, v10, v0
v_mul_f32 v7, v11, v0

// result += column1 * pos.y   - fused, which is what a GPU multiply-add is
v_fmac_f32 v4, v8, v1
v_fmac_f32 v5, v9, v1

// The w column is added with an implicit 1.0, so it is a plain add rather than a multiply.
v_add_f32 v4, v8, v4

// Subtraction, not a negated add: VOP2 subtracts vsrc1 from src0, so the operand order is part
// of the meaning and getting it backwards computes b - a silently.
v_sub_f32 v4, v8, v9

// Unary minus, as the generator emits it: zero minus the operand, with the zero as an inline
// constant rather than a register the allocator would have to find.
v_sub_f32 v5, 0, v8

// Moving a constant into a lane: the inline constant 1.0, the inline constant 0, and a 32-bit
// literal. The three encode differently and are the set most easily confused.
v_mov_b32 v12, 1.0
v_mov_b32 v15, 0
v_mov_b32 v13, 0x3e800000

// Copying one lane to another, which is how a varying reaches its export register.
v_mov_b32 v14, v4

s_endpgm
