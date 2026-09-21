/*
 * **A second texture unit on the console** - measured 2026-09-20, on.
 *
 * The software rasteriser applies every unit `glActiveTexture` names. The hardware path sampled
 * one: a draw with a texture bound above GL_TEXTURE0 left it out and said so once in the log, so
 * a multitextured scene came out on the console missing its second layer - a lightmap, a detail
 * map, a decal. That was the largest single difference a port saw between the two paths.
 *
 * # What settles it
 *
 * `REQ-20260920T0745Z-9a41` (re-filing half of `-8b1c`; sweep `20260920-103636`,
 * `166-agc/primitive-draw-param4`) answers both halves with controls that could have come out
 * the same and did not:
 *
 * - **The fourth parameter carries its own value.** Attributes 0-2 were given `(1, 0, 0, 1)` and
 *   attribute 3 `(0.25, 0.5, 0.75, 1)`. The three-parameter control, `SPI_PS_INPUT_CNTL_3` 0x0,
 *   printed `0xff0000ff` - red, the value the unrouted slot reads. The four-parameter arm,
 *   `SPI_VS_OUT_CONFIG` 0x6, `SPI_PS_IN_CONTROL` 0x4, `SPI_PS_INPUT_CNTL_3` 0x3, printed
 *   `0xffbf8040` - attribute 3's own constant. `-8b1c` could not tell these apart because its
 *   control printed the arm's answer.
 * - **Two descriptor pairs are sampled in one pixel shader.** `arm2-two-samples` binds images at
 *   `0x2009000` and `0x2009100` with their samplers and returns `0xff00ffff`, yellow: red plus
 *   green. A shader exporting a constant would print that too, so the same fixture ran a
 *   single-sample control, which returned `0xff0000ff` - red. One sample is red, two are yellow.
 *
 * **And the registers are the ones this library already loads.** The check reports
 * `desc1-sgpr 0x14` and `samp1-sgpr 0x1c`: s20 and s28, which is where `tools/shader/tex-prolog2.s`
 * loads the second pair from. That was chosen here as the next free range and turns out to be
 * what the measurement used.
 *
 * # What is still one unit
 *
 * **There are no units above GL_TEXTURE1 anywhere**, and this said they were "the software
 * rasteriser's alone" until 2026-09-21. `OOPS_GL_MAX_TEXTURE_UNITS` is 2, every per-unit array
 * in the library is that size, `GL_MAX_TEXTURE_UNITS` reports 2, and
 * `glActiveTexture(GL_TEXTURE2)` is refused with `GL_INVALID_ENUM` by `gl_mt_unit` - in the
 * software rasteriser exactly as here. GL 1.3's minimum is two, so that is conformant. The log
 * line `gl_draw.c` emits is about **unit 1 when unit 0 has no texture**, which this path cannot
 * apply because its second stage combines against the first's result and there is no first.
 *
 * A third unit is a whole-library change, and its console half needs a fifth parameter export,
 * which has never been measured on this part (`REQ-20260921T1210Z-4f16`).
 *
 * GL 1.4's crossbar - `GL_TEXTURE0` named as a source from unit 1's environment - reads zero,
 * because unit 0's combine stage has already overwritten v4..v7 with its result; that is the
 * rule a unit applying no texture follows here and in Mesa.
 */
#ifndef OOPS_GL_MULTITEX_H
#define OOPS_GL_MULTITEX_H

/* 1 since 2026-09-20: REQ-9a41's rows, above. */
#define OOPS_GL_MULTITEX_MEASURED 1

#endif
