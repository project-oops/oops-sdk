.text
// Exporting the same fragment to both colour targets (since 2026-09-20): what a draw under
// glDrawBuffer(GL_FRONT_AND_BACK) needs on the console path, where the GPU had one colour
// target and the front buffer was reached only by clears and pixel rectangles.
//
// The single-target export already in the tree is `exp mrt0, v4, v5, v6, v7 done vm`
// (0xf800180f, 0x07060504). Two targets is the same export twice, to mrt0 and mrt1, with
// `done` on the last one only - a wave may raise done once, and it marks the final export.
// The colour is the same in both: GL writes the same fragment to both buffers.
//
// obSCEne's REQ-20260919T2258Z-3f62 measured the receiving side on hardware
// (sweep 20260920-082906, 166-agc/mrt-dual-target): with CB_TARGET_MASK and CB_SHADER_MASK
// 0xff and SPI_SHADER_COL_FORMAT 0x99, target 0 took 512 red pixels and target 1 took 512
// green ones in a single draw, while the one-target control (masks 0xf, format 0x9) left
// target 1 at its fill. So both halves are known: the registers from that check, these two
// instructions from here.
exp mrt0, v4, v5, v6, v7 vm
exp mrt1, v4, v5, v6, v7 done vm
s_endpgm
// Cross-check against the word already in the tree: the single-target export, which must
// assemble to 0xf800180f / 0x07060504 exactly as gl_context.c writes it.
exp mrt0, v4, v5, v6, v7 done vm
s_endpgm
