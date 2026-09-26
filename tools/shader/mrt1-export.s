.text
// Exporting the same fragment to both colour targets: what a draw under
// glDrawBuffer(GL_FRONT_AND_BACK) needs on the console path.
//
// The single-target export already in the tree is `exp mrt0, v4, v5, v6, v7 done vm`
// (0xf800180f, 0x07060504). Two targets is the same export twice, to mrt0 and mrt1, with
// `done` on the last one only - a wave may raise done once, and it marks the final export.
// The colour is the same in both: GL writes the same fragment to both buffers.
//
// The receiving side is CB_TARGET_MASK and CB_SHADER_MASK 0xff and SPI_SHADER_COL_FORMAT 0x99;
// one target is masks 0xf and format 0x9.
exp mrt0, v4, v5, v6, v7 vm
exp mrt1, v4, v5, v6, v7 done vm
s_endpgm
// Cross-check against the word already in the tree: the single-target export, which must
// assemble to 0xf800180f / 0x07060504 exactly as gl_context.c writes it.
exp mrt0, v4, v5, v6, v7 done vm
s_endpgm
