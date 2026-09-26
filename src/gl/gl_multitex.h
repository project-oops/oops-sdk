/*
 * A second texture unit on the console.
 *
 * The hardware path applies both GL texture units, as the software rasteriser does. The
 * pixel shader imports a fourth parameter (`SPI_VS_OUT_CONFIG` 0x6, `SPI_PS_IN_CONTROL`
 * 0x4, `SPI_PS_INPUT_CNTL_3` 0x3) and samples a second descriptor pair from s20 and
 * s28, which is where `tools/shader/tex-prolog2.s` loads it; obSCEne's
 * `166-agc/primitive-draw-param4` measured both against controls that differ.
 *
 * `OOPS_GL_MAX_TEXTURE_UNITS` is 2, GL 1.3's minimum, and
 * `glActiveTexture(GL_TEXTURE2)` is refused with `GL_INVALID_ENUM` by `gl_mt_unit` on
 * both paths. A third unit needs a fifth parameter export on the console, which retires
 * with the value intact (docs/hardware/agc-blend-and-export-fw1240.md, section 4).
 * `gl_draw.c` logs unit 1 bound while unit 0 has no texture: the second stage combines
 * against the first's result, and there is none.
 *
 * GL 1.4's crossbar - `GL_TEXTURE0` named as a source from unit 1's environment - reads
 * zero, because unit 0's combine stage has already overwritten v4..v7 with its result;
 * that is the rule a unit applying no texture follows here and in Mesa.
 */
#ifndef OOPS_GL_MULTITEX_H
#define OOPS_GL_MULTITEX_H

/* The second unit is measured on hardware; see above. */
#define OOPS_GL_MULTITEX_MEASURED 1

#endif
