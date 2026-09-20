# Shader source for the words that ship

`oops-gl` emits RDNA2 bytecode as literal `uint32_t` words. That is the right shape for a
measuring instrument - the stream is readable as evidence, and there is no compiler between a GL
call and the packet - and it has one hazard: **a wrong instruction encoding cannot fail loudly.**
It assembles into the payload, the hardware does something else, and the result is a frame that
is wrong rather than a build that stops.

So the words are not written from memory. Each `.s` file here is assembled, and the object read
back, and the resulting words are what go into the source with the instruction in the comment
beside them:

```bash
clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c tools/shader/alpha-test.s -o /tmp/a.o
objdump -s -j .text /tmp/a.o
```

`clang` is in the `oops-builder` WSL distribution; there is no `llvm-mc` there, but the AMDGPU
target is built in, which is all this needs.

Two of the results agree with words that were already in the tree, which is the cross-check that
the pipeline is right rather than merely self-consistent: `s_endpgm` assembles to `0xbf810000`,
and the 32-bit-literal source marker `0xff` appears in the same position as in the canary load
the untextured pixel shader has always done.

## Wave32

These shaders run wave32 - `VGT_SHADER_STAGES_EN` sets `GS_W32` and `VS_W32` - so the mask
registers are `vcc_lo` and `exec_lo`. A wave64 build would need `vcc` and `exec`, and the
encodings would differ; assembling for the wrong width is exactly the mistake this directory
exists to prevent.

## Files

- `alpha-test.s` - the comparisons and the lane kill behind `glAlphaFunc`. The words are checked
  against this file by `test_gl_alpha_test_patches_both_shaders`.
- `tex-env.s` - the texture environment's per-channel words: multiply, add, and the move that
  keeps the fragment's own value. The three multiplies were already in the textured shader and
  assembled to the same words. Checked by `test_pm4_gl_tex_env_reaches_the_combine_slot`.
- `combine.s` - the general texture combine (`GL_BLEND`, RGBA `GL_DECAL`, `GL_COMBINE`). Those
  are generated programs rather than fixed words, so this file holds one line for each
  instruction form and operand kind the generator's encoder emits - `v_mov_b32` from a VGPR or a
  literal, the six VOP2 operations with VGPR and inline-constant sources, and `s_branch` - and
  `test_pm4_gl_combine_encoder_matches_the_assembler` checks the encoder against every line. The
  field layout the encoder uses is what these words show; nothing it emits is outside them.
- `tex-prolog.s` - the textured pixel shader's sampling: whole-quad mode (`s_wqm_b32`, the live
  mask kept in s16 and restored after), q interpolated from the texture parameter's `w` and
  divided out, and `image_sample` with the level of detail the hardware derives - replacing the
  `image_sample_lz` that sampled at level zero. The file also assembles that `_lz` form and the
  canary's exec restore, which came out as the words already in the tree. Checked by
  `test_pm4_gl_textured_shader_samples_with_lod_and_divides_q`.
- `fog.s` - fog's twelve words: the factor interpolated from the texture parameter's `z`, and
  the blend of red, green and blue towards the fog colour, which is three literals. The
  placeholder colour is 0.3 because 0.5, 0 and 1 have inline encodings - the assembler uses
  them, leaving no literal word to patch. The file also assembles the textured shader's
  `attr1.x` interpolation, which came out as the word already in the tree. Checked by
  `test_pm4_gl_fog_reaches_both_shaders_and_the_vertex`.
- `vs-param3.s` - the vertex shader for a draw with a third interpolant: the two-parameter NGG
  program with a 64-byte vertex, a fourth vec4 loaded from offset 48, and `exp param2`. The
  program is written out whole, and every instruction it shares with the two-parameter shader
  assembled to the word `gl_context.c` already writes. The four address literals are
  placeholders (`0x12345678` and so on) that `gl_vs_build_param3` overwrites at words 19, 21,
  36 and 38. Checked by `test_pm4_gl_param3_vertex_shader_is_the_assembled_one`.
- `colour-sum.s` - the colour sum after texturing: the secondary colour interpolated from
  `attr2.x`, `.y` and `.z` and added to the combined colour with `v_add_f32_e64 ... clamp`,
  twelve words. The file also assembles fog's `attr1.z` interpolation, which came out as the
  word already in the tree. Checked by `test_pm4_gl_colour_sum_takes_the_third_parameter`.
- `mrt1-export.s` - the two exports a draw into both colour buffers ends with, `exp mrt0 ... vm`
  then `exp mrt1 ... done vm`: `done` marks a wave's last export, so the single-target word could
  not simply be written twice. Its third instruction is the single-target export already in the
  tree, which assembled to the same word. Checked by `test_pm4_gl_front_buffer_targets`.
- `tex-cube.s` - sampling a cube map: the texture coordinate taken as a direction and turned into
  a face and a place on it by RDNA2's own `V_CUBEID_F32`, `V_CUBESC_F32`, `V_CUBETC_F32` and
  `V_CUBEMA_F32`, then sampled with `dim:SQ_RSRC_IMG_CUBE`. Twenty-four words, including its own
  `image_sample`. It also assembles the 2D sample it replaces and the prolog's `attr1.x`
  interpolation, which came out as the words already in the tree. Checked by
  `test_pm4_gl_volume_and_cube_sample_on_hardware`.
- `tex-3d.s` - sampling a volume: r interpolated from the third parameter's `w`, divided by the
  `1 / q` the prolog left in `v12`, and sampled with `dim:SQ_RSRC_IMG_3D`. Seven words. **The
  divide is the difference from `tex-cube.s`**, which interpolates all three components fresh
  because scaling a direction leaves it unchanged and dividing two of three does not. It also
  assembles the 2D sample it replaces, the prolog's own divide of s by q, and its `attr1.x`
  interpolation - all three came out as words already in the tree. Checked by
  `test_pm4_gl_volume_and_cube_sample_on_hardware`.
- `coverage.s` - antialiasing's coverage in the untextured pixel shader: the fragment's alpha
  weighted by how much of the pixel a smooth point or line covers, and the fragments it misses
  killed. Fifteen words. **The geometry is interpolated rather than computed** - the offset from
  the primitive's centre is linear across the quad the CPU widened it into, so the CPU writes it
  at the corners and this reads it back; a line puts zero in the second component, which makes
  one form serve both kinds. Its two cross-check instructions - the untextured shader's own
  interpolation of the red channel and the lane kill the alpha test ends with - came out as words
  already in the tree. Checked by
  `test_pm4_gl_smooth_points_and_lines_carry_their_coverage`.
- `tex-shadow.s` - sampling a depth texture, with GL 1.4's comparison and without. `dmask:0x1`,
  because a depth texel is one float and a comparison's result is one value, and moves that
  spread it across `v4..v7` as `GL_DEPTH_TEXTURE_MODE` says - three forms, of which `GL_ALPHA`'s
  has to write `v7` before it zeroes `v4`. The comparison itself is the sampler's
  (`DEPTH_COMPARE_FUNC`); what this adds is `image_sample_c` and the reference, r/q clamped to
  [0, 1], as the **first** address register. Four cross-check instructions came out as words
  already in the tree: the plain 2D sample, the prolog's divide of s by q, its `attr1.x`
  interpolation, and `s_waitcnt vmcnt(0)`. Checked by
  `test_pm4_gl_depth_texture_samples_and_compares_on_hardware`.
- `tex-prolog2.s` - the second texture unit's sample: unit 1's coordinate interpolated from the
  fourth parameter, its own descriptor pair loaded from `+0x40` and `+0x60` of the table, and the
  texel left in `v28..v31` - clear of `v16..v27`, where the general combine form gathers its
  arguments, so unit 0's own combine cannot overwrite it. Seventeen words, placed **before** the
  prolog's exec restore so that
  both samples are taken in whole-quad mode. Its three cross-check instructions are unit 0's own
  descriptor loads and sample, which assembled to the words already in the tree. **No draw sets
  it** - see `src/gl/gl_multitex.h`. Checked by
  `test_pm4_gl_second_unit_samples_inside_whole_quad_mode`.
- `vs-param4.s` - the vertex shader for a draw with two texture units: the three-parameter
  program with an 80-byte vertex and a fifth vec4 exported as `param3`. The 80-byte stride is not
  a shift, so the lane's offset is lane * 64 plus lane * 16. Every instruction it shares with the
  three-parameter program assembled to the word already in the tree. **No draw runs it** - see
  `src/gl/gl_multitex.h`. Checked by `test_pm4_gl_param4_vertex_shader_is_the_assembled_one`.
- `polygon-stipple.s` - the polygon stipple's discard: the fragment's window position converted
  from `v2` and `v3`, a row of the mask loaded with `global_load_dword`'s saddr form, and the
  lanes whose bit is clear taken out of `exec_lo`. Sixteen words, two of them the table's address
  as placeholder literals that `gl_ps_patch_stipple` overwrites. It also assembles the wait both
  shaders do after their canary store and `s_endpgm`, which came out as the words already in the
  tree. Checked by `test_pm4_gl_polygon_stipple_discards_in_the_shader`.
