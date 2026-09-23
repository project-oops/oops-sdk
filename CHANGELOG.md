# Changelog

oops-sdk publishes **no artifact**. It is consumed as a sibling checkout by the payloads in
the collection, which link the archive they built from it - so there is no version and no
release: the commit a consumer was built against is the only version that means anything.

Entries are grouped **Added / Changed / Fixed**, newest first.

Nothing has shipped yet - this is the initial commit.

## [unreleased] - as of 2026-09-03

### Added

- **`gl_PointCoord`** (2026-09-23). It was refused with "point sprites are not implemented", and
  they were - `cap_point_sprite`, `GL_COORD_REPLACE`, `GL_POINT_SPRITE_COORD_ORIGIN` and the
  corner expansion have all been here since 2026-09-20, and gl1-probe's `point-sprite` passes on
  hardware. The refusal outlived its reason, which is the one failure mode a refusal that names
  its cause is supposed to prevent.

  It is texture coordinate 0's interpolant, which is the hardware's own arrangement rather than a
  shortcut: the part substitutes the sprite coordinate for a chosen interpolant with
  `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX`, and the expansion already writes it into that slot for
  `GL_COORD_REPLACE`. A program that reads it generates the coordinate without
  `glEnable(GL_POINT_SPRITE)`, since `gl_PointCoord` is defined for any point and a GLSL program
  has no `GL_COORD_REPLACE` to set.

  **Two restrictions, both link errors with a sentence.** Not beside `gl_TexCoord`, because they
  are one slot. And not beside a vertex shader, because `gl_draw_point_square` expands the point
  into its square *before* the vertex stage, sizing it in object space through the inverse MVP -
  all four corners carry the same attributes, so a vertex shader collapses them and the sprite is
  never drawn. Hardware expands points after the vertex stage; doing that here is what would lift
  the second restriction.

  Measured rather than asserted: gl2-probe's `point-coord` paints `vec4(gl_PointCoord, 0, 1)`
  into a 64-pixel point and checks all four quadrants, so a constant, a swapped pair or an
  inverted axis fails.

### Fixed

- **`glReadPixels` of the second colour target read raw memory, not the GPU's answer**
  (2026-09-23). The end of every submission has the CP copy the render target into a CPU-cached
  buffer, and `gl_color_read_source` hands that copy back only for the buffer it is tagged as
  being of. Only `ctx->framebuffer` was ever copied - and under `GL_FRONT_AND_BACK`
  `gl_draw_targets` puts the back in `framebuffer` and the front in `fb_also`, so **the front
  was never the tagged buffer and every read of it fell through to the surface pointer**.

  That is not a stale frame with a name on it. It is the CPU's view of memory the GPU has just
  written through its own caches, so what comes back is neither the old value nor the new one
  reliably - which is exactly the signature gl1-probe's `front-and-back` has been reporting since
  it was written: a blue byte of `0x14`, `0x56`, `0xb9`, `0xd3`, `0x4e` across five runs, drifting
  between runs and holding still within one, in **both** targets at once, while the same pixel
  read through `glGetFrameReadback` came back the colour GL asks for.

  **Four checks across two suites were reading the instrument rather than the result**, and the
  reading of them had already reached the pixel shader's second export - which LLVM assembles to
  the words oops-gl emits, byte for byte, so that hunt would have found nothing. What settled it
  was gl2-probe's `two-draw-buffers`, which drives the same path from a shader whose output it
  chooses and printed the two read paths disagreeing about one pixel in one row.

  A second copy and a second tag, allocated the first time a program binds a second target -
  in `gl_draw_targets` rather than `gl_front_buffer`, because the latter is skipped entirely on
  the scanout path, which is the console. Both tags are dropped when the CPU writes into the
  colour buffer, because the CPU path writes `fb_also` too. Pinned by
  `test_pm4_gl_both_colour_targets_are_copied_back`: two `DMA_DATA` packets behind the one fence
  wait, a tag naming each, and a read of either buffer taking its own copy.

- **A discarded fragment kept its depth, because the depth block was never told the shader could
  kill** (2026-09-22). `DB_SHADER_CONTROL.KILL_ENABLE` - bit 6 of context register `0x203` - was
  never set, by any path. Clearing `exec` stops the shader writing; it does not stop the depth
  block, which under early Z has already tested, written and retired the pixel on the
  understanding that the shader cannot change the answer. The discarded fragment therefore kept
  its colour *and* its depth, and the next draw behind it was rejected by a depth value that
  should never have been written.

  **The bug was diagnosable only because two checks disagreed.** gl2-probe's `discard` has failed
  on hardware since it first ran; the reading was "discard does not work". In the same run on
  2026-09-22 the new `discard-in-loop` **passed**, and it discards too - the difference being
  that it draws with no depth test, so there is no early Z to retire anything and the export's
  mask is the only thing deciding. One check saying discard works and one saying it does not is
  what located the register.

  Set now from the shader's AST, and from the fixed-function state for GL 1.x - the alpha test
  and the polygon stipple both clear `exec` and had the identical bug, unmeasured because no GL
  1.x check put a kill and a depth test in the same draw. `gl1-probe`'s new `alpha-test-depth`
  does. `Z_ORDER` stays at `EARLY_Z_THEN_LATE_Z`: radeonsi sets the kill bit from `uses_discard`
  and leaves the order alone (`si_state_shaders.cpp:1711`, and case 1 of the table at `:1730`).

  Two smaller things fell out of it. `glsl_unit_mentions(fs, "discard")` - the obvious way to
  ask - walks the **identifiers**, and `discard` is a keyword, so it looks in the one place the
  answer cannot be and returns false every time; `glsl_unit_discards` reads the node kind
  instead. And `DB_SHADER_CONTROL` shared a cache with `SPI_SHADER_Z_FORMAT`, on the grounds
  that only a depth-exporting shader moved either - so a program that discards and writes no
  depth changed the register while the format stood still, and the emission keyed on the format
  would have sent neither. The caches are separate, and
  `test_pm4_gl_a_discarding_shader_sets_kill_enable` covers exactly that case. Neither register
  had any test before this; that is how it stayed quiet.

  Fixed in code and in the words; **not yet confirmed on a console**.

- **The second colour target had no blend control** (2026-09-21). Blending on this part is per
  MRT - `CB_BLEND1_CONTROL` is `0x028784` (Mesa `src/amd/common/amdgfxregs.h:12802`), context
  offset `0x1e1`, and radeonsi writes the whole run as `R_028780_CB_BLEND0_CONTROL + i * 4`
  (`si_state.c:420`) - and only `0x1e0` was ever emitted. A draw under
  `glDrawBuffer(GL_FRONT_AND_BACK)` therefore blended into the back and **replaced** in the
  front. That is exactly what gl1-probe's `front-and-back` measured on hardware twice: `saw`
  `0xff00ffff`, the cyan GL asks for, from the back, and the wrong colour from the front. Both
  controls now go out in one two-dword packet, MRT1 carrying the same state when it is bound and
  zero when it is not; `test_pm4_gl_front_buffer_targets` pins both directions. Fixed in code,
  **not yet confirmed on a console**.
  The hour before finding it went into the tiling of the second target, because
  `gl_draw_targets`' comment still said the hardware had one colour target and left the second
  buffer out with a log line - untrue since MRT1 landed the day before. That comment now
  describes what the function does and says what it used to claim.

- **A pixel rectangle the CPU wrote was read back as what was there before** (2026-09-21).
  `glGetFrameReadbackSampled` handed out `ctx->readback` - the CP's copy **as of the last
  submit** - without asking whether it was still current. `glDrawPixels`, `glBitmap`,
  `glCopyPixels` and `glAccum` write the colour buffer with the CPU and build no command stream,
  so no submit follows and nothing re-copies the buffer; the caller got the frame as it was
  before its own pixels. Six of gl1-probe's eight hardware failures were this one thing.
  The accessor now asks `gl_color_read_source`, the same question glReadPixels already asked -
  which is why `read-pixels` passed beside the six the whole time, and the tell that should have
  been read as a discriminator rather than as corroboration.

- **A drain for the CPU's colour writes** (2026-09-20), `gl_color_cpu_drain`: a `clflush` over
  the span written and an `sfence`, at the end of every CPU pixel operation and at every flush.
  On the scanout path the colour buffer is write-combined display memory, and a WC store is not
  ordered against a later load, so the CP's DMA in `gl_hw_flush` can read one that has not
  drained.
  **This was written believing it was the fix for the six above, and it was not**: the console
  returned all eight failing pixels byte for byte unchanged with it in place, which is what
  ruled the memory ordering out and sent the search to the accessor. It stays because the hazard
  is real; the comments around it say plainly that it fixed nothing that was measured, and
  `REQ-20260920T2230Z-7c31` is withdrawn.

- **The four-parameter vertex shader was never written into the payload** (2026-09-20).
  `gl_vs_build_param4` was written, unit-tested against a scratch buffer, and wired into the
  offset a draw selects when it needs four parameters - and `glContextCreate` never called it,
  where it calls `gl_vs_build_param3` on the line above. **The first draw with two texture units
  on a console jumped to `0xb00` and executed whatever the allocation held**: `ILLEGAL_INST` on
  two waves at one PC, `GPU_FAULT_WAVEFRONT_ERROR_ASYNC`, a GPU reset, and the probe's remaining
  checks unrun.
  A definition nothing calls links and tests clean. `app.mk`'s undefined-symbol check catches
  the opposite case - a call with no definition - and there is nothing on this side that catches
  this one; it took a console, a `payload-va` line and one subtraction.
  **With the line added, `multitexture` passes on hardware**, and with it `vs-param4.s`,
  `tex-prolog2.s` and unit 1's combine stage, none of which had ever executed on the part.

### Added

- **`texture1D`, `texture1DProj`, `shadow1D` and `shadow1DProj`** (2026-09-23), which completes
  the set: **every texture lookup a fragment shader may call is now generated.**

  These were refused the day before with "not a different kind of thing, only an unwired one -
  so it is refused rather than written blind". That was the wrong call, and the fact that
  settles it was in this repository rather than on a console: `glTexImage1D` stores `width, 1`,
  one row, and `gl_state.c`'s descriptor builder special-cases only 3D and cube - so a
  `GL_TEXTURE_1D` gets **TYPE 9, the 2D descriptor**.

  That makes the lowering determined rather than guessable. A 1D lookup is a **2D** sample with
  a zero beside the coordinate; sampling `dim:SQ_RSRC_IMG_1D` would tell the hardware something
  its own descriptor does not say. The zero has to be written rather than left - the sampler
  reads a `t` either way - and the test checks it is there rather than whatever the allocator
  held.

  The projective divide was corrected while here. It divided as many components as the *address*
  takes; it now divides as many as sit ahead of the divisor, which is the reference's own rule
  and the only one right for every form at once - one component for a 1D, two for a 2D, three
  for a volume, and a shadow's reference along with them.

  What is refused now is the explicit-level family, `texture2DLod` and its kin - and by the
  front end rather than here, which is correct: in GLSL 1.10 they exist only in a vertex shader.

- **`shadow2D` and `shadow2DProj`** (2026-09-23), which compare rather than return a texel -
  the last GLSL lookup this back end was missing above one dimension.

  **The comparison is the sampler's, not the shader's.** `DEPTH_COMPARE_FUNC` in the sampler's
  word 0 already carries `GL_TEXTURE_COMPARE_FUNC`, so what the shader adds is the reference and
  `image_sample_c`, the form that hands it over. One instruction, from
  `tools/shader/tex-shadow.s`: 0xf0a00108, the opcode and a `dmask` of 1 being the whole
  difference from the plain sample - a comparison returns one value where a texel returns four.

  **The reference is the first address register, ahead of s and t**, and that is measured rather
  than read off the ISA: obSCEne's `-b4e1` reports the VADDR range as `v[2:4] with ref_z in v2
  at position 0`. Getting it the other way round would sample at the reference and compare
  against a texture coordinate, which is a picture rather than an error - so the tests check
  both a passing and a failing comparison, because with `s` below the stored depth the passing
  case looks right either way and only the failing one gives it away.

  The reference is clamped to [0, 1] before the comparison (GL 1.4, 3.8.14), low end first as
  softpipe does it.

  **`GL_DEPTH_TEXTURE_MODE` is where this stops being exact.** The single value GL spreads by
  that parameter, and it is a per-texture choice made after the shader is compiled. The compiled
  path emits the `GL_LUMINANCE` spread, `(v, v, v, 1)`, which is its default; a texture asking
  for `GL_INTENSITY` or `GL_ALPHA` gets luminance and one log line from the draw path, rather
  than a colour nobody can account for.

- **`texture3D` and `texture3DProj`** (2026-09-23), which needed no new instruction at all: a
  volume takes its three coordinates straight through, and `image_sample` already carried a dim.
  That is exactly what separates it from the cube it sits one bit away from in the encoding -
  no face selection and no reduction. The projective form divides all three by `w`, where the 2D
  one divides two.

  **The descriptor was measured a year's worth of context ago and I nearly asked for it again.**
  obSCEne's `-6c80` sampled a 4x4x2 volume on this part with `TYPE 0xa` and the last slice in
  `desc-word-4`, against a 2D control reading the same memory, and reported the texel. The
  sampler record now carries the `GLSL_IMG_DIM_*` the lookup will use rather than a cube flag,
  so the three lookups are one code path with the dim as data.

  What is still refused is the shadow forms, and for a reason that is not "unmeasured" either:
  the hardware side of `image_sample_c` with a compare function was measured in the same sweep.
  They are different in kind - a comparison against a reference rather than a texel - and the
  shader side is simply not written.

- **`textureCube` in a compiled shader** (2026-09-23). **The descriptor half has been there
  since the fixed-function path learnt cube maps**: the six faces upload as one array, the
  descriptor carries TYPE 0xb, and obSCEne measured a cube sampled on this part and reported the
  face its texel came from (`-6c80`). What was missing was that a `samplerCube` never got a
  descriptor set, so the GL 2.0 path had nowhere to put it.

  The face selection is four instructions and not arithmetic: `v_cubeid_f32` names the face,
  `v_cubesc_f32` and `v_cubetc_f32` give the place on it, `v_cubema_f32` gives twice the major
  axis, and the shader divides and biases by a half to land in [0, 1]. All four read x, y and z
  at once, which is why they are VOP3 - **the first VOP3 this back end emits**, and the reason
  it needed an encoder at all. Words from `tools/shader/tex-cube.s`, assembled: the four share a
  second dword and differ only in the opcode, which is exactly where an off-by-one would hide,
  so the encoder test pins all eight.

  `|ma|` is `max(ma, -ma)` rather than VOP3's absolute-value modifier, so nothing beyond the
  four opcodes needed verifying. The direction is not normalised and must not be: scaling all
  three components leaves the face and the place on it alone, which is why a direction works as
  a coordinate at all - there is a test that samples `(1, 0.5, 0)` and `(2, 1, 0)` and requires
  the same answer.

  A `samplerCube` sampled through `texture2D` would hand two address registers where the
  hardware reads three. The back end refuses it; the semantic stage gets there first, so no
  shader can carry one that far.

- **`asin`, `acos`, `atan` and `refract`** (2026-09-23), which were refused on the grounds that
  "a polynomial of unmeasured accuracy is not generated in their place".

  **That objection was to a polynomial chosen here, and the one now emitted is not.**
  `oops_atan2f` in `src/math/math.c` already carries a minimax cubic and a reduction to [0, 1],
  and the software rasteriser answers every `atan` in this SDK through it. The compiled path
  emits those coefficients and that reduction, so the two paths compute one function rather than
  two that agree - the same discipline as `m * m` matching `glsl_exec.c`'s loop. `asin` is
  `atan2(x, sqrt(1 - x*x))` over a clamped argument and `acos` is `pi/2` minus it, which is how
  the reference defines both.

  The quadrant fixups are selects rather than branches, so it is one straight run per component.
  `x == 0` needs no case of its own - it falls out of the reduction as `a = 0`, and `|y| > |x|`
  and the sign of `y` finish it - and only `x` and `y` both zero needs a guard, because that
  divide is `0/0`.

  **Measured rather than asserted**: the tests compare against `oops_atan2f` itself across the
  reduction's seam and all four quadrants, to **1e-5**, and that tolerance is the reciprocal's.
  There is no divide instruction here, so one true divide becomes `v_rcp_f32` and a multiply,
  good to a unit in the last place; everything else is identical, so that is the whole of the
  difference.

  `refract` was refused for "a square root of a value that may be negative and a select on it".
  Both arms are computed and the sign of `k` selects, so the square root of a negative is
  produced and discarded - a `v_cndmask` moves a register rather than evaluating anything, and
  the NaN leaves with the arm it belongs to. Total internal reflection returns the zero vector,
  which is the specification's wording and what a shader leans on to darken a grazing angle.

- **`texture2DProj`** (2026-09-23), which is `texture2D` with a divide in front of it and needed
  no new encoding. The coordinate is divided by its **last component**, and which component that
  is depends on the form rather than the vector's width: the `vec4` form divides by `w` and
  ignores `z`, where the `vec3` form divides by `z`. There is a test with `99.0` sitting in `z`
  for exactly that.

  The divide answers **zero** on a zero divisor rather than an infinity. The language calls it
  undefined and `glsl_exec.c` picks zero, so the compiled path picks zero too - the two agreeing
  is worth a compare and a select. Ordinary `/` here does not guard that way, so the guard is
  written at the lookup rather than borrowed from it.

- **Square-matrix arithmetic, all of it** (2026-09-23): `m * m`, a matrix with a scalar either
  way round, two matrices componentwise, and the built-ins `matrixCompMult`, `transpose` and
  `outerProduct`. Only `m * v` and `v * m` were generated before.

  **None of it needed an instruction that was not already here**, and column-major storage is
  why. `m * m` is n of the matrix-vector products, one per column of the right operand, because
  a column of the right operand is already a run of n registers and needs no gather - which is
  the same shape `glsl_exec.c` computes, so the two paths agree by construction rather than by
  arithmetic that happens to match. Everything else falls through to the componentwise path that
  was there all along; the change was to stop refusing it.

  `[]` now indexes a matrix's column and a vector's component as well as an array's element -
  the same arithmetic at three strides, and `m[c][r]` is two of them composed. The base is
  evaluated rather than looked up, so a computed matrix can be indexed and `m[1].x = …` is still
  a place rather than a temporary.

  What a matrix still cannot do is meet a vector that is not its width. `m4 * v3` used to fall
  into the same refusal as `m * m`; now it has its own, because with the blanket refusal gone it
  would otherwise fall through to componentwise and compute something that is not a product at
  all. `inverse` and `determinant` remain refused - both are real arithmetic rather than a
  shuffle.

- **An early `return` ends the function and nothing else** (2026-09-23). A guard clause -
  `if (x > 1.0) return 0.0;` and then the real body - is the shape, and it was refused because
  the mask would have to be carried through every statement after it. That mask is the one
  `break` already carries: the value goes into the caller's result under the exec the lanes have
  at that point, the lanes come out of every `if` and loop **inside the function**, and `exec` is
  cleared so the rest of the body writes nothing for them.

  **What it must not touch is anything around the call.** The `if` the call sits in, the loop it
  sits in, and the function's own entry mask all keep the lane, because returning ends the
  function and not the statement the call was part of - so the depths are recorded at the call
  and counted from there rather than from zero. The mask is restored **before** the `out`/`inout`
  copy-back, or a lane that returned early would leave the caller's variable holding what it had.

  A body with no early return in it takes no mask at all, so every shader that had none emits
  exactly the words it did before. A value-returning function must still end in
  `return <expr>;` - GLSL requires every path to return, and the trailing one catches the lanes
  no earlier return took. A `return` in `main` is still refused, and now says why: there is no
  call to hand the lanes back at, and the export that retires the wave runs after the body.

- **Local arrays** (2026-09-23), as a run of registers - `float w[4]`, `vec3 v[2]`, element `k`
  at `base + k * width`. There is no addressable memory behind one, so the index has to be known
  when the shader is compiled.

  **That is less restrictive than it sounds, and the reason is the unrolled loop.** Its counter
  holds a different constant in each copy of the body, so `for (int i = 0; i < 4; i++) total +=
  w[i];` resolves element by element - which is how the pattern is actually written, and the
  case that makes an array in a register file useful rather than merely legal. `const_of` learnt
  to resolve that one variable, and deliberately only that one: "this variable holds a constant"
  stops being true the moment something assigns to it, and the counter is the only variable this
  generator can answer for, because a body that assigns to it is refused before anything is
  emitted.

  An index the shader computes at run time is refused. The alternatives are a chain of selects
  costing the whole array per access - which silently changes what a shader costs - or scratch
  memory this back end has not got.

- **`while` and `do`-`while` say why they are refused** (2026-09-23), which they previously did
  by falling into a message claiming loops need "a branch and a label mechanism it has not got".
  That stopped being true when `for` started branching. The real reason is the trip guard: a
  branched loop is bounded by the trip count the compiler worked out from the initialiser, bound
  and step, and `while` has none of the three to count from. A ceiling picked out of the air
  would end a legitimate loop early and quietly, which is worse than refusing. The message names
  the `for`-with-`break` rewrite, which is the same loop and is bounded.

- **Loops branch, with `break` and `continue`** (2026-09-22) - the first backward jump this back
  end emits. Everything else in it is straight-line: an `if` narrows the exec mask and runs both
  arms, which is cheaper than a jump as well as simpler. Going round again is the one thing a
  mask cannot express.

  A loop is still **unrolled** where the trip count allows, because a constant counter folds and
  costs nothing. When it does not - more trips than the unroller writes out, or a `break` or
  `continue` in the body - the loop takes a real backward branch instead, and carries three
  scalar masks: the lanes still going round, the mask to restore on the way out, and a **trip
  guard**. `break` takes lanes out of the first, `continue` leaves it alone and lets the top of
  the next trip reload `exec` from it, and that one difference is the whole of the two
  statements. `exec` is reloaded before the *step* as well, which is what makes a `continue`
  mean "skip the rest of the body" rather than "stop counting" - a lane whose counter stopped
  would sit on the same value for every remaining trip.

  **The guard is the reason this is safe to emit at all.** A backward branch is the only
  construct here whose failure mode is worse than a wrong pixel: a condition that never goes
  false does not draw badly, it does not finish, and the part goes with it. So a branched loop
  carries a counter that ends it after the number of trips the compiler counted, whatever the
  lanes are doing. Nothing a shader can write makes it fire - the trip count is known when the
  shader is compiled, a body that moves its own counter is refused, and a loop whose bound is
  not constant never gets this far - and it is there for a bug in the generator rather than a
  bug in the shader. `test_gl2_a_branched_loop_carries_its_trip_guard` checks it ships in the
  words, because no value test can show something that never happens.

  The words come from `tools/shader/branch.s`, assembled: `s_branch` back over one instruction
  is `0xbf82fffe`, and the three forward jumps to one label are `+4`, `+3`, `+2`. **`simm16`
  counts from the instruction after the branch**, so a jump back over one is -2 and not -1;
  off by one lands mid-loop, which is a hang rather than a fault. Both sides of the scalar
  inline boundary are pinned too - `s_cmp_ge_u32 s20, 64` is one word and 65 is two, and taking
  the inline path for 65 would compare against the inline constant *-4.0*.

  A `discard` inside a loop now comes out of the loop's masks as well as the enclosing `if`s'.
  Without that the loop hands the lane straight back at the top of the next trip and it reaches
  the export alive - the same resurrection an `if` would do, one construct further out.

  The simulator in `test_gl2.c` executes branches, and runs shaders under an instruction budget:
  a loop that does not terminate is the one failure with no wrong value to assert on, so it is
  made into a failing test at a line number rather than a test run that never returns.

- **`&&` and `||` stop early when the right side does something** (2026-09-22). With a pure
  right operand both sides are computed and `min`/`max` combines them, which is two instructions
  and no mask; the language's guarantee is only observable when the right side assigns. It now
  runs under a narrowed `exec` - the lanes the left operand has not already decided - and the
  result, which starts as the left operand, is overwritten under that same mask. A lane that
  skipped keeps `false` for `&&` and `true` for `||`, which is what those are. No branch and no
  combine. `^^` is untouched: GLSL gives it no short-circuit, so a right side that assigns is
  correct there rather than a problem, and it was being refused for a rule that does not apply
  to it.

- **Integer comparisons** (2026-09-22), which are the float comparisons of the same registers.
  An `int` in this back end is a float kept whole by a truncation after every operation, so
  `i > 5` and `float(i) > 5.0` are the same two values in the same two places and
  `v_cmp_gt_f32` is the instruction for both. `==` is exact for the same reason, and more
  reliably than for floats: whole numbers up to 2^24 have one representation each. Past 2^24 the
  representation stops being exact, which is a limit this back end's integers already have
  everywhere - `i + 1` is a float add there too.

  Together with the loops above, this is what `gl2-probe`'s `control-flow` and `short-circuit`
  needed: both were compiled refusals (`GL_INVALID_OPERATION`) on hardware while the software
  reference ran them. `test_gl2_the_probes_control_flow_shaders_compile_and_run` compiles and
  runs those two sources exactly as the probe writes them.

- **A loop whose body assigns its own counter is refused** (2026-09-22), in both lowerings. The
  trip count is worked out when the shader is compiled; `i = i + 2` in the body makes that count
  wrong without failing, and the unrolled path was already silently wrong about it - each copy
  bakes its own constant, so the assignment is overwritten at the top of the next one.

- **A GL 2.0 fragment shader compiles to gfx1030** (2026-09-21), and the draw path binds it.
  `src/gl/glsl_ps.c`: the varyings interpolated, the body from `glsl_gen.c`, the colour exported.
  **No console has executed one** - obSCEne's `REQ-20260921T1615Z-4e77` is the gating
  measurement, and nothing downstream of it is worth building until it answers.

  **Only the fragment stage is compiled, and that is the architecture rather than a shortcut.**
  oops-gl's hardware vertex shader is a passthrough: the CPU builds each vertex already in clip
  space and the shader loads it and exports it - `exp pos0 v2, v3, v4, v5` on values it did not
  touch. So a GL 2.0 vertex shader runs where every other vertex computation here runs, on the
  CPU in `glsl_exec.c`, writing the same vertex the fixed-function path writes. The fragment
  stage is the one with no CPU standing in for it on the console, which is why it is the half
  that needs a compiler.

  **Every encoding came out of clang.** `tools/shader/gl2-fragment.s` is a *table* rather than a
  program - what has to be pinned is each instruction's encoding, because the words a compiler
  emits depend on the GLSL it was given - and `test_gl2_pixel_shader_encodings_match_the_assembler`
  asserts the encoder reproduces it. A wrong encoding in a hand-written shader is wrong once; in
  a compiler it is wrong in every shader it ever emits. The interpolation fields were pinned
  across four channels and a high attribute rather than inferred from one example, which would
  not have separated the attribute from its channel. The cross-check that the pipeline is right
  rather than self-consistent is `s_and_b32 exec_lo, exec_lo, vcc_lo`: the same word is already
  in the tree behind `glAlphaFunc` and the polygon stipple.

  What is deliberately *not* generated: uniforms, texture sampling, control flow. Each is
  refused with a sentence naming it, rather than emitted as something plausible.

  Two hardware facts the wiring leans on rather than changes. **RSRC1 is untouched** - the
  frame's stage table already reserves 136 VGPRs for the pixel stage, and the compiler refuses a
  shader needing more, so the register that says how much of the file to allocate stays at its
  measured value. And the parameter registers are `gl_hw_emit_param_count`'s, which obSCEne
  already measured for two, three and four: a program's varyings are counted into that same
  configuration rather than a new one.

- **A context has the entry points its version defines and no others** (2026-09-21).
  `glContextSetVersion` changed the string `glGetString` answers and nothing else - it said so in
  its own log line - so every call in the library was reachable from every context, and a program
  written for GL 1.1 could call `glCreateShader`.

  On a desktop driver that discipline comes from the linker: an entry point a context does not
  have is not exported, and a program calling it fails to load. Everything here is compiled into
  one archive, so the equivalent is a runtime check, and the answer is the specification's for a
  call that is not in the context: **GL_INVALID_OPERATION, and the call does nothing**. A
  function returning a value returns its failure value - 0 for a name, -1 for a location,
  GL_FALSE for a predicate. An **enumerant** a later version added is GL_INVALID_ENUM instead,
  which is the different thing it is: "I have never heard of this" rather than "not from here",
  and the query leaves its destination alone.

  **The gate is on GL 2.0's entry points, completely, and not within GL 1.x** - and that second
  half is a decision rather than a shortcut. Every 1.2 through 1.5 feature here is *also*
  advertised in `glGetString(GL_EXTENSIONS)`: `GL_ARB_multitexture`,
  `GL_ARB_vertex_buffer_object`, `GL_EXT_fog_coord`, `GL_ARB_window_pos` and the rest. An
  extension is available to a context whatever its core version - that is what an extension is -
  so a GL 1.1 context here genuinely has buffer objects through `glBindBufferARB`, and refusing
  `glBindBuffer` beside it would be a rule about spelling rather than about capability. GL 2.0
  is the opposite case: nothing advertises the programmable pipeline as an extension, so the
  claim is the only door to it.

  The default stays 1.1, so no GL 1.x program changes. **2.0 will not become the default**: the
  opt-in is what keeps a GL 1.x program out of a pipeline it never asked for, which is the whole
  point. `gl2-cube`, `gl2-probe` and the unit suite each gained the line that claims it, and
  `gl2-probe`'s `version-gating` narrows the context mid-run to check the refusal still bites -
  and widens it again, because a claim is a property of the context and not a one-way switch.

- **GLSL 1.20, which is what a `#version 120` shader needs** (2026-09-21). The front end took
  1.10 and refused everything else by number; Craft and most shaders written after about 2006
  declare 1.20, and hit that refusal on line one.

  **The rule that matters is implicit conversion**: 1.20 converts `int` to `float` and `ivecN`
  to `vecN`, and 1.10 converts nothing. So `pos * 2` and `clamp(v, 0, 1)` are shaders in one and
  errors in the other - which is how shader authors write, and is the whole of why a 1.20 shader
  compiled as 1.10 stops at its first line of arithmetic.

  It goes **one way only**: `float f = 1;` is legal in 1.20 and `int i = 1.0;` is not, in
  either. The rule lives in `glsl_type_accepts`, which every assignment, initialiser, argument
  and return goes through, so none of them can disagree about it; binary operators widen once at
  the top of `binary_type` so every rule below sees a pair that already agrees.

  **The conversion has to arrive at the value too.** The interpreter took a binary expression's
  type from its left operand, so `2 * 0.25` would have been an integer expression and the answer
  truncated to 0 - a black channel where a half-lit one was meant. A float operand now widens an
  integer result, guarded on the result being integer-based so a matrix and a transform's vector
  are untouched.

  With it: `invariant` and `centroid`, consumed and recorded nowhere because neither has
  anything to change here - there is one code path per stage so invariance already holds, and
  there is no multisample buffer so every sample is already at the pixel centre. `invariant
  gl_Position;` on its own parses as the restatement it is and produces no node. `transpose` and
  `outerProduct`, over square matrices; 1.20's non-square `mat2x3` and its relatives are not
  implemented and there is no shape for a non-square transpose to return.

  `glGetString(GL_SHADING_LANGUAGE_VERSION)` answers **1.20**, the highest dialect the front end
  takes - a ceiling, not a mode: a shader saying `#version 110` is still held to 1.10's rules.
  `glContextSetVersion` takes 2.1 for the same reason, having refused it for exactly as long as
  that was untrue.

- **A `gl_` name that is real GLSL and is missing here is named, with the reason**
  (2026-09-21). `gl_PointCoord` needs point sprites, which are not drawn; `gl_LightSource[]`,
  `gl_Fog`, `gl_FrontMaterial` and `gl_DepthRange` are structs, and there is no struct type.
  They used to be "use of an undeclared name", which reads as a typo and sends an author to
  check their spelling instead of their expectations. `gl_VertexID` and `gl_ClipDistance` are
  named too, with the version that brought them.

- **A GL 1.x program pays nothing for GL 2.0 being in the library** (2026-09-21). Every vertex
  carries sixteen generic attribute slots and filling them is real work per vertex, for values a
  fixed-function draw never reads. The fetch and the immediate-mode latch now skip the whole
  block until the context has made a shader or a program, so the cost arrives with the first
  shader and not before.

- **OpenGL 2.0 runs on the software reference** (2026-09-21). `glUseProgram` draws: the entry
  points exist, `glLinkProgram` builds a program's interface, and the software path runs the
  vertex shader per vertex and the fragment shader per fragment. `gl2-probe` measures 41 checks
  against it and `gl2-cube` draws a cube through the two shaders it has been carrying since it
  could only preprocess them.

  Five files and about ninety entry points. `gl_shader.c` is the object model - shaders and
  programs in **one name space**, as the specification requires, and deferred deletion with both
  of its observable halves: `glIsShader` answers false from the moment the flag is set while
  `glGetShaderiv(GL_DELETE_STATUS)` still answers, and an implementation with only one of them
  passes half the tests that exist for this. `glsl_builtin.c` resolves GLSL 1.10's built-in
  functions **by rule** rather than by symbol, because every one of them is overloaded over
  genType and a symbol table holds one signature per name; the asymmetries are kept rather than
  tidied, so `min(genType, float)` is legal and `min(float, genType)` is not. `glsl_link.c`
  compiles and links, taking **references** to the compiled units rather than copying them - the
  specification lets a shader be deleted the moment a program has linked it, and a copy would
  cost the whole node arena per stage. `glsl_exec.c` is the reference execution.

  **A derivative is the expression re-evaluated against the neighbouring pixel's interpolants.**
  `dFdx`, `fwidth` and a mipmapped lookup's level of detail all need to know how a value changes
  across the screen; hardware takes that from the other pixels of a quad, and the rasteriser
  hands the fragment stage the interpolated block three times - at the pixel, one right and one
  down. It is exact for anything linear in the varyings and is the same finite difference the
  hardware takes for anything else, and a program whose fragment shader needs none of it is
  still evaluated once.

  **The console refuses a draw with a program bound**, and logs once. There is no GL 2.0 back
  end; running the fixed-function instruments in its place would put a picture on screen that no
  part of the program asked for, from a call that reported success. `gl2-cube` and `gl2-probe`
  are `check-only` for the same reason and say so in their Makefiles.

- **The rest of GL 2.0, which is not about shaders** (2026-09-21): separate stencil state for
  the two faces (`glStencilFuncSeparate`, `glStencilOpSeparate`, `glStencilMaskSeparate`), a
  blend equation per channel group (`glBlendEquationSeparate`), and `glDrawBuffers`.

  **The three GL 1.x calls are now the `GL_FRONT_AND_BACK` case of the separate ones**, which is
  how the specification defines them from 2.0 onwards - one implementation rather than two that
  have to agree, and a GL 1.x program keeps working because setting both faces to the same thing
  is what it always did. The face is resolved once per triangle, not per fragment, because that
  is what it is a property of. Which face a triangle is comes from its own winding - the same
  sign culling reads - so the single-pass stencil shadow volume works: increment where a front
  face passes and decrement where a back one does, in one draw rather than two.

  `CB_BLEND0_CONTROL` has always carried `ALPHA_COMB_FCN` in its own field and this wrote the
  colour's equation into both, because GL 1.x had only one to write. `gl_blend_reads_constant`
  needed the same correction: with separate equations a GL_MIN colour and a GL_FUNC_ADD alpha
  still reads the constant, and testing only the colour's equation would have left the register
  unwritten.

  `glDrawBuffers` on a window-system framebuffer means **the same fragment colour to each named
  buffer**, not a different one per buffer - true multiple render targets are framebuffer objects
  and GL 3.0. A name covering more than one buffer, and a buffer named twice, are both
  GL_INVALID_OPERATION rather than a silent union.

- **`gl_FrontFacing` comes from the triangle's own winding** (2026-09-21). It was taking its
  value from `prim_polygon_back`, which is two-sided *lighting*'s flag and is set only while
  GL_LIGHTING and GL_LIGHT_MODEL_TWO_SIDE are both on - so every GL 2.0 fragment, which is every
  fragment with lighting off, was told it faced forward. Found by `gl2-probe`'s `front-facing`
  on its first run.

- **GLSL arrays** (2026-09-21): `uniform vec4 palette[4]`, `gl_TexCoord[]` and `gl_FragData[]`.
  A symbol carries a length, and **an element keeps the array's element type** - indexing a
  `vec4` array used to read as indexing a vec4 and produce a float, which then failed several
  lines away with a message about the wrong thing. A constant index past the end is a compile
  error, as the specification says, rather than a read of whatever follows.

- **The parser reads through the preprocessor** (2026-09-21). They were two passes over the same
  text, so `#version 110` reached the grammar as a stray `#` and a `#define` was an undeclared
  identifier several lines from where it was written. `glsl_parser_init_pp` puts the
  preprocessor in front of the lexer at the one place a token enters the parser, and consumes the
  directives before the first real token - so a caller knows the version before parsing a line
  of the shader, and refuses a language it does not implement rather than compiling it as 1.10.

- **`GL_POLYGON_SMOOTH` on the console** (2026-09-21), the last feature the roadmap listed as
  needing a shader change. It gave two reasons this could not be done - a coverage of three edge
  fades is a different shape of slot from one distance, and the outer half of every fade falls on
  pixels the hardware rasteriser never raises - and both were true and neither was structural.
  The coverage slot holds either form now, twenty-eight words rather than sixteen, with the alpha
  test and export moved up in both shaders. And the CPU **widens the triangle** by a pixel about
  its incenter, which moves every edge outward by the same distance - the trick that already
  turns a point and a line into a quad - so the fragments exist and the shader's kill removes
  what the widening added beyond the fade.
  The three distances need no per-vertex geometry: the distance to edge *i* is `λ_i·h_i`, so
  vertex *j* carries its own height in component *j* and zero in the others. An edge that is not
  antialiased - a diagonal the polygon was triangulated along - carries 1.0 at every vertex and
  fades nothing. Each rides as `d*w` with `w` beside it and is divided per fragment, because
  `v_interp` is perspective-correct and a distance to a line is not; that is the identity the
  textured prolog already uses for `q`. `tools/shader/coverage-poly.s`, whose four cross-check
  lines assembled to words already in the tree.
  **Two texture units is now the only primitive case still aliased**, for points, lines and
  polygons alike. gl1-probe's `polygon-smooth` **passed on hardware the first time it ran**,
  82/85.

- **A ring of texture descriptor slots, so a frame with many textures submits once**
  (2026-09-21). There was one slot, and every textured draw handed the shader its address - so a
  second texture bound later in the frame would have overwritten the first, and the way that was
  kept correct was to **submit the frame** on any descriptor change. One texture a frame cost
  nothing. A scene with twenty materials submitted twenty times and waited on a fence each time,
  and glut-demo flips in ten to twelve milliseconds with one texture, so that is the difference
  between a port that runs and one that does not. It is the first thing a real port meets, which
  is why it went ahead of the remaining conformance work.
  **No shader changed.** The descriptor table's address was already per-draw user data, and both
  texture prologs load their pairs at offsets *relative* to it, so pointing the base at a
  different slot is the whole mechanism. Slot 0 is the original table at `0x900`, so a frame that
  never changes texture emits the stream it always did and the gl-cube oracle record is
  untouched; slots 1-63 live at `0x1800`, clear of the textured pixel shader that ends at
  `0x1500`. The ring wraps into a submission, as the vertex ring does, and a **border colour**
  still submits on a change whatever the ring has room for - `TA_BC_BASE_ADDR` is a frame
  register rather than something a slot carries.
  Three existing tests changed the behaviour they pin, which is the honest measure of the
  change: a mip-chain test that submitted four times now takes four slots in one stream and
  still finds slot 0 holding what the first draw was given, and a volume-and-cube test that
  counted one triangle after a texture change now counts three.
  `test_pm4_gl_descriptor_ring_wraps_into_a_submission` covers the boundary and asserts the last
  slot lands inside the payload.
  **gl1-probe cannot measure this and the attempt is recorded rather than dropped**: the suite
  submitted 193 times before the ring and 194 after, for one more check, and identically per
  check. 83 of its 85 checks read a pixel back and a readback submits, so what it counts is its
  own measuring rather than its texture changes. The mechanism is host-measured; the benefit
  needs a program that draws many textures a frame and reads nothing back.

- **A volume's mip chain: the layout is written, the console still reads the base level**
  (2026-09-21). `gl_tex_chain_levels` refused a volume because the chain layout was
  two-dimensional. `gl_tex_chain_layout_3d` halves all three axes and the copy walks slices at
  `pitch * height`, so that reason is gone - and gl1-probe's new `volume-mipmap` **failed on
  hardware**, `saw 0xffff0000`, red, level 0, with the descriptor carrying `LAST_LEVEL` 1 and
  `MAX_MIP` 1. So a volume still samples its base level there and still says so in the log; the
  refusal in `gl_tex_chain_levels` is now a measured one rather than a structural one, with the
  layout and its test kept for when the answer arrives.
  **I wrote that both halves of the layout were "measured, not reasoned" before running it, and
  that was wrong.** `texture-3d` measures the slice stride *within* level 0 and `mipmap-levels`
  measures the level placement of a *2D* chain; neither measures where a *level* of a *volume*
  begins, which is the extrapolation the part rejected. `REQ-20260921T1300Z-9b73` asks for it.
  The host reference did catch the first version of the check, which scaled the coordinate by 8
  and magnified instead of minifying.

- **A textured smooth point or line is antialiased on the console** (2026-09-21). It was drawn
  aliased because the offset from the primitive's centre rides in the texture-coordinate
  parameter and a textured draw reads all four of its components. It now rides in the **second
  texture unit's** parameter, which a one-unit draw has spare, and such a draw escalates to four
  parameters for it - eighty bytes a vertex and the four-parameter vertex shader, both of which a
  two-unit draw already uses. **This was unblocked by a bug fix rather than by a design:** the
  four-parameter shader had never executed until `gl_vs_build_param4`'s missing call was found
  the same morning.
  The textured pixel shader gained a coverage slot at 258, between fog and the alpha test, where
  the untextured one keeps its own; the alpha test and the export moved up sixteen. A branch over
  the slot is no smoothing, so a frame that never smooths runs the program it always ran.
  `tools/shader/coverage-tex.s` is `coverage.s` with the six interpolations' ATTR field moved
  from 1 to 3 - `+0x800` each - and its seven cross-check lines assembled to words already in the
  tree, which is what makes "only the ATTR field moved" checked rather than asserted.
  gl1-probe's new **`smooth-textured` passed on hardware the first time it ran**, 80/83. Two
  texture units is now the only primitive case still aliased: the fourth parameter is the second
  texture's coordinate then, and only its `z` is spare.

- **The GPU payload's address is logged** (2026-09-20). A GPU fault reports a program counter
  and nothing else - `PC=0x0000000201390B04 ILLEGAL_INST`, one line per wave - and every shader
  oops-gl runs lives at a fixed offset inside one allocation, so the base is the entire
  difference between that number and a line of a `.s` file. The first draw with two texture
  units faulted on 2026-09-20 and the log could not say which shader the address fell in.
  `payload-va` now sits beside the init lines.

### Changed

- **The GL roadmap records what the console actually does** (2026-09-20). Its claims about the
  hardware path were written from the host rasteriser and from register measurements. **The
  suite now runs to the end on a console: `gl1-probe: 74/82 passed on hardware`**, and every
  check in it has a verdict. The new section gives each of the eight failures the pixel it left
  behind, groups six of them as one shape - the CPU writing colour into the render target - and
  names the eleven things that had never drawn a frame there and now pass.
  Three comments that the run falsified went with it: two saying no draw reaches the second
  texture unit's slots, and one saying the fog words had never run on a console.

### Fixed

- **Four things `include/GL/gl.h` told a porter that stopped being true** (2026-09-20). The
  header is what someone reads to decide whether to work around a feature, so a caveat that
  outlived its cause costs real work:
  - **The polygon stipple** said the hardware path "does not yet" apply it. It has since the
    console got the fragment's position in the pixel shader.
  - **`GL_COMBINE`** said it was combined by the software rasteriser. Both paths combine it; the
    console writes the combiner into a slot as instructions.
  - **Cube maps** said they were sampled by the software rasteriser. Both paths sample them.
  - **Antialiasing** said the hardware draws every smooth primitive aliased. It draws smooth
    points and lines properly; what stays aliased is a *textured* smooth primitive and
    `GL_POLYGON_SMOOTH`, and the header now names both and says why.
  Three internal comments went the same way: the second texture unit's two slots still said no
  draw set them, which stopped being true when the multitexture gate opened, and the sample slot
  said it had three forms when it has five. Found by reading the public headers for claims about
  the console rather than by anything failing - which is the only way this class shows up.

- **The extension enum aliases broke a hosted title, and one of them had the wrong type**
  (2026-09-20, found the same day they were added). The `_ARB` and `_EXT` spellings added with
  the depth-texture, 3D-texture and occlusion-query extensions were defined in terms of the core
  names - `GL_DEPTH_COMPONENT16_ARB` as `GL_DEPTH_COMPONENT16`. A hosted title includes this
  header **and** Mesa's own `GL/glext.h`, which defines the same enums as literals, and a macro
  redefined with a *different* token sequence is a diagnostic under `-Werror` even when the value
  is identical. Both oops-mesa probes stopped compiling. They are literals now, which is what the
  `GL_TEXTUREn_ARB` block in the same file has always been and why that one never broke.
  And `glTexImage3DEXT`'s `internalformat` is a **`GLenum`**, not the `GLint` the core call
  takes: the extension predates GL 1.2 and declares it that way, so a program written against
  the extension passes one. Meeting a real `GL/glext.h` is what said so - which is the argument
  for building the hosted probes after any change to this header, not just the freestanding ones.

### Added

- **A robustness sweep over everything added on 2026-09-20**, given what a sloppy port gives it
  rather than what the code was written against: a volume with a negative depth, a cube map whose
  faces disagree, the comparison state set on a colour texture, every way a program gets the
  occlusion query order wrong - ending one that never began, reading a result while it runs,
  beginning a second, **deleting the active one** - and the font asked for bytes it has no glyph
  for. On a console each of those is a fault rather than a diagnostic, so what they have to do is
  refuse. All of it already did, which is the answer worth having.
  **One deviation came out of it, and is now gone.** A zero-sized texture image was refused with
  `GL_INVALID_VALUE`; GL says a zero size means no image at all, which is how a program releases
  a level it no longer wants. It now does exactly that - the level's storage is freed and the
  level left absent, which `gl_tex_level_view` already reported as "not there", so the texture
  becomes incomplete if that level was needed and a draw with it is untextured. A negative size
  is still an error.
  **It was written up as a documented deviation first**, on the grounds that making a zero-sized
  level exist would reach into completeness, the samplers and the chain layout. Looking again,
  that was the wrong shape: a zero-sized level does not *exist*, it is released - and releasing
  one is a path the code already had for every other reason. Documenting a difference that could
  be removed in twenty lines is not the same as deciding it should stay.

- **`make checks`, and `tools/header-check` under it** (2026-09-20), written the same day two
  header bugs got through everything else.
  **The mistakes it catches are not wrong values.** They are macros and declarations that only
  conflict when two sets of GL headers meet, which happens in exactly one place: a hosted title
  includes this SDK's `<GL/gl.h>` **and** Mesa's `<GL/glext.h>`, because it drives both. A test
  that includes only our header cannot see them at all. So `header-check` compiles a translation
  unit twice - once with the SDK's headers alone, once with `glext.h` after them - and the second
  is what would have caught `GL_TEXTURE_3D_EXT` defined as a name rather than a literal, and
  `glTexImage3DEXT` declared with the core call's `GLint` where the extension has a `GLenum`.
  Confirmed by putting the first one back and watching it fail.
  The `glext.h` half needs a sibling `oops-mesa` checkout. Without one it is **skipped with a
  line saying which half did not run and what is therefore unchecked** - a check that looks like
  it ran and did not is worse than none.
  **And the alias values are now compile-time assertions** in `tests/unit/test_gl.c`. Writing
  them as the core names could not be wrong and turned out to be unbuildable; writing them as
  literals builds everywhere and can be mistyped. A `_Static_assert` per alias removes the risk
  the change introduced, and stops the build rather than waiting for the one call that uses the
  enum to return `GL_INVALID_ENUM` somewhere far from the typo.
  `make checks` runs this and `libc-check` together. Both are out of `make test` because they
  need the target compiler, and `make test` should still run on a machine without it.

- **The reported GL version is the caller's to state** (2026-09-20). `glGetString(GL_VERSION)`
  answered `"1.1 oops-gl fixed-function subset"` unconditionally - the honest class of what is
  implemented everywhere, and deliberately conservative. The cost of that is a port written
  against a later 1.x, which checks the badge before calling something this library *does* have -
  1.2's 3D textures, 1.3's multitexture, 1.4's secondary colour, 1.5's buffer objects - and
  refuses to run when it reads lower.
  `glContextSetVersion(1, 4)` makes the string begin `"1.4"`. **It changes nothing else**: no call
  becomes implemented, the suffix still reads `subset`, and a log line records that the program
  asked. That is the claim in its right place - a program asserting what it targets, rather than
  this library asserting a conformance it has not got, which is the mistake the old
  `"OpenGL 1.3 oops-gl 2.0"` string made twice over. `major` must be 1 and `minor` at most 5;
  anything else is `GL_INVALID_VALUE` and the version is left as it was, not half-set.
  `glContextGetVersion` reads it back, and `OOPS_GL_DEFAULT_VERSION_MINOR` moves the default for
  a build serving ports that all expect the same one.

- **`sscanf`, because that is how a model file is read** (2026-09-20). An OBJ loader is a `fgets`
  and an `sscanf("%f %f %f")`; so is an MTL loader, and so is every level format anyone wrote by
  hand. `sscanf` and `vsscanf`, converting `%d %i %u %o %x %X %p`, the float forms, `%s %c %n %%`
  and `%[...]` scansets, with the `hh h l ll L z j t` modifiers, a field width and `*` to skip a
  field (`src/system/scanf.c`).
  **It is checked against the host's own library, not against a list of expectations.** The tests
  build on the host, where `<stdio.h>` is the real thing, so the same inputs and formats go to
  both and the return value and every converted value are compared. That found **three**
  differences, and all three were in what *fails* rather than in what converts - which is the
  half nobody writes a case for:
  - `"1e"` with `%f` is a **matching failure**, not the number one. C takes the longest sequence
    that could begin a valid number - "1e" can, since "1e5" is one - and then fails when that
    sequence is not itself valid. This library wound back and returned 1, which reads as the
    friendlier answer and is the wrong one.
  - `"0x"` with `%f`, and with `%x` or `%i`, is the same failure: once those two characters are
    seen C has committed to a hexadecimal item, so a missing digit fails the conversion rather
    than falling back to reading the "0" and leaving the "x".
  - The **hexadecimal float** form - `%f` over `"0x10"` is sixteen - was missing entirely.
  The difference that matters most is the one the first hand-written test got wrong in the other
  direction: a *matching* failure returns 0 and an *input* failure returns `EOF`, and a loader's
  read loop turns on exactly that - 0 means skip this line, `EOF` means stop.
  **`fscanf` and `scanf` are not here.** Both need to put a character back when a conversion
  reads one too many, and this SDK's file handles have no pushback.

- **The C library gained the parts a GL 1.x port actually reaches for** (2026-09-20), which is
  not the same set as "what a C library has".
  **`qsort` and `bsearch`.** A fixed-function pipeline blends in the order the triangles arrive,
  so there is no order-independent transparency: anything see-through is sorted back to front by
  the program, every frame, and sorted with this. It is a median-of-three quicksort, insertion
  sorting under sixteen, recursing into the **smaller** partition only - which bounds the stack
  at log2(n) frames rather than n, and the input that would otherwise reach n is a *sorted* one,
  which is exactly what a scene hands it on the frame after it sorted.
  **The partition stops both scans on an element equal to the pivot**, and that is not a detail.
  Walking the ascending scan over equals puts a run of identical elements entirely on one side,
  so an all-equal array partitions into n-1 and 0 every time - the quadratic case the
  median-of-three was chosen to avoid. The first version did that, and the test found it by
  counting comparisons on an all-equal array rather than by checking the output, which was
  correctly sorted throughout.
  **`strtok`, `strtok_r`, `strdup`, `strspn`, `strcspn`, `strpbrk` and `strerror`.** An OBJ, MTL
  or level loader is built out of those, and without them the loader is the part of the port
  that gets rewritten. `strtok` keeps its state in a static, as C says it does, and terminates
  its token in the caller's buffer, as C says it does - including the sharp edge where a string
  literal is a write to read-only memory.
  **`strtoul`, `strtof`, `llabs`, `div`, `ldiv`**; and from `<math.h>` `log2`, `copysign`,
  `modf`, `ldexp`, `frexp` in both widths, and `isnan`, `isinf`, `isfinite`, `signbit`,
  `isnormal` as macros over the compiler's builtins - so `if (isnan(x))` compiles, which until
  now was a compile error rather than a link one and so the *first* thing a port had to edit.
  **The algorithms live in `src/system/freestd.c`, not in `src/system/libc.c`.** That file is
  target-only, because a host build's real C library owns those names - so an algorithm written
  there could never be run by a test. `libc.c` keeps the promise its own header makes: every
  function in it is a name change and nothing more.

- **Smooth points and lines are antialiased on the console** (2026-09-20), the last of the six
  gl1-probe checks written to fail on hardware.
  **The roadmap said this needed the fragment's position in the pixel shader. It does not.** GL's
  coverage for a smooth point is `r + 1/2` minus the distance from its centre, and for a smooth
  line `w/2 + 1/2` minus the distance across it - and the *offset* those distances are taken from
  is linear across the quad the CPU already widens the primitive into. So the CPU writes each
  corner's offset into the vertex and the interpolator carries it in; nothing needs to know where
  the fragment is. A line writes zero in the second component, which makes `sqrt(x*x + y*y)` the
  absolute across distance and **one shader form serve both kinds**.
  Fifteen words in the untextured pixel shader, between fog and the alpha test where GL applies
  coverage and where the software rasteriser applies it (`tools/shader/coverage.s`). It kills the
  fragments the primitive misses entirely, as the software path drops them, so the depth buffer
  takes no write from a pixel outside the disc. Its two cross-check instructions - this shader's
  own interpolation of the red channel and the lane kill the alpha test ends with - assembled to
  words already in the tree. The alpha test's slot and the export moved up to make room, as they
  did for fog.
  **Two cases stay aliased and earn the log line, which is reworded to say which.** A *textured*
  smooth primitive, because the parameter the offset rides in is the texture coordinate and there
  is no other spare interpolant. And `GL_POLYGON_SMOOTH`, whose coverage is the product of three
  edge fades rather than one distance.
  **The console's smooth line does not fade its end caps.** The software rasteriser does, from
  how far along the segment a pixel is - which one interpolated distance cannot say. Rather than
  extend the quad a pixel past each end and have those pixels come out fully covered, making the
  line a pixel too long, the quad stops at the ends. GL does not require the fade.

- **A bitmap font, so `glutBitmapCharacter` works** (2026-09-20). Most GLUT code that draws
  anything draws text too - a frame counter, a key legend - and draws it with this call, so a
  port that cannot make it had to have its text rewritten, which is the one thing this library
  exists to avoid. `glutBitmapCharacter`, `glutBitmapString`, `glutBitmapWidth`,
  `glutBitmapLength` and `glutBitmapHeight`, over `glBitmap`.
  **The glyphs are drawn here, not taken from X11.** `GLUT_BITMAP_8_BY_13` and
  `GLUT_BITMAP_9_BY_15` are the X11 `fixed` fonts and every GLUT ships their data as a table;
  copying one would be taking someone else's font. These are a 5x7 box with two descender rows,
  and they are not those shapes and do not pretend to be - what they keep is what a program
  depends on: the advance (8 and 9), the cell height (13 and 15), the baseline, and every
  character from space to `~`.
  **Each glyph is a picture in the source**, nine rows of `#` and `.` top row first, because a
  font is data that is wrong in one glyph and looks right everywhere - a hex table can only be
  tested one character at a time, and this one can be read. The conversion to bitmap bytes is
  then the part that can be wrong, and `test_gl_bitmap_font_draws_what_it_is_a_picture_of` checks
  it by drawing an `F` - asymmetric both ways, so a vertical flip and a horizontal mirror each
  break it differently - plus the baseline through a descender, the advance, and that every
  printable character has ink while the space has none. Flipping the row order in the converter
  fails that test, which is how it was confirmed to discriminate.
  **The proportional fonts are absent**, and a program using `GLUT_BITMAP_HELVETICA_18` still
  fails to link. Offering a fixed-width font under a proportional name would return the wrong
  `glutBitmapWidth` and break the layout of anything that measures before it draws.
  `glutStrokeCharacter` is absent for the same reason - its glyphs are line segments.

- **The GLUT surface a port actually calls** (2026-09-20): the Platonic solids, the one window's
  own calls, and the `glutGet` queries this SDK can answer exactly.
  **The solids are derived, not transcribed.** `glutSolidTetrahedron`, `glutSolidOctahedron`,
  `glutSolidIcosahedron` and `glutSolidDodecahedron` with their wire twins, at the radii GLUT's
  manual documents - 1 for two of them, sqrt(3) for the other two. Every GLUT ships these as
  literal vertex and face tables; copying one would be taking another implementation's data, and
  writing one out by hand is the kind of work that is wrong in one entry and looks right
  everywhere. So the vertices come out of the definitions - alternate corners of a cube, the unit
  axes, three golden rectangles - and the faces are found from the vertices: for the triangular
  solids any three that are pairwise an edge apart, and for the dodecahedron the five furthest
  along each of its face normals, which are the icosahedron's vertices because the two are duals.
  **The first version of that was wrong and the test caught it.** There are two icosahedra in
  those rectangles, mirror images of each other - (0, ±1, ±φ) and (0, ±φ, ±1) - and only one is
  the dual of the dodecahedron as its vertices are written. The other put five vertices from
  three different faces into each pentagon, which `test_gl_platonic_solids_are_the_solids_they_
  claim` failed on the planarity check: a set of five furthest along a direction that does not lie
  in one plane is not a face. That test reads the solids back through `GL_FEEDBACK` and checks
  the face counts, the radii, **Euler's formula on the edges that came back** rather than on a
  count assumed from the table, the winding, and that the pentagons are flat.
  **The window calls are here because each is exact.** `glutGetWindow`, `glutSetWindow`,
  `glutSetWindowTitle`, `glutSetIconTitle`, `glutFullScreen`, `glutSetCursor` and
  `glutVisibilityFunc` - the window is the display and permanently full screen, there is one of
  it, there is no cursor, and it is visible for as long as the program runs, so the callback is
  called once with `GLUT_VISIBLE` when the main loop starts. `glutReshapeWindow`,
  `glutPositionWindow` and `glutWarpPointer` are **not** here and still fail to link, because
  they cannot be honoured - which is the same line `glutGet` is answered along: the queries that
  have an exact answer here got one (the screen size, the window's position, the channel sizes,
  the accumulation buffer's sixteen bits, `GLUT_WINDOW_CURSOR` answering `GLUT_CURSOR_NONE`
  whatever was asked for) and the rest are absent rather than guessed.

- **Occlusion queries count the GPU's own samples on the console** (2026-09-20). They held the
  CPU's fragments alone there, with `GL_QUERY_COUNTER_BITS` 0 to say the number carried no
  information; it is 32 on both paths now.
  Two `EVENT_WRITE ZPASS_DONE` packets bracket the query - event type 21, `EVENT_INDEX` 1, the
  dword `0x00000115` - each dumping every render backend's counter, begin at the slot and end
  eight bytes on, and the result is the sum of the differences with bit 63 masked off as the
  hardware's valid marker. `REQ-20260919T2048Z-4d19` measured every part of that: sixteen
  backends, `enabled-rb-mask 0xffff`, and a sum of `0x200` against a draw of exactly 512 pixels.
  **Four of its sixteen slots carried a zero or half difference**, which is why the answer is the
  sum and never one slot - the work simply does not reach every backend.
  **The counters were already running.** `DB_COUNT_CONTROL`'s `ZPASS_ENABLE` is bits [8,11]
  (`gfx103.json:12552-12565`), and the measured depth-block recipe this library has emitted since
  the beginning is `0x11000100` - that field set. What a query adds is `PERFECT_ZPASS_COUNTS` and
  `DISABLE_CONSERVATIVE_ZPASS_COUNTS`, the `0x11000106` `-4d19` measured exact against, and it
  adds them as a register write of their own after the depth surface is bound rather than inside
  the depth block - so a frame with no query emits the stream it always did, which the gl-cube
  oracle record pins byte for byte.
  **Counting never starts before a depth surface is bound.** `REQ-20260919T1600Z-e3a7` set
  `ZPASS_ENABLE` with no depth target and the depth block stalled before the pixel shader ran:
  the canary unwritten, the fence never hit, the GPU wedged. So the arming waits for the first
  draw that binds one, and a query whose draws never test depth keeps the CPU's count and says so
  once in the log - `GL_SAMPLES_PASSED` with the depth test off is legal and common, and the
  alternative is a hang.
  `GL_ARB_occlusion_query` joins **both** extension lists, and its eight entry points -
  `glBeginQueryARB` and the rest - arrive with it, the extension predating GL 1.5.

- **Depth textures and GL 1.4's shadow comparison are sampled on the console** (2026-09-20).
  Three things had to agree and now do.
  **The image format.** A depth texel here is one 32-bit float, and every descriptor this library
  writes carried `8_8_8_8_UNORM`, which would have read four bytes of colour. It is
  `GFX10_FORMAT_32_FLOAT` for a depth texture - 22 against 56, `gfx10-rsrc.json:27` and `:61`,
  the second being the value already in the tree, which is what says the table being read is the
  right one. Both are four bytes a texel, so no pitch, chain layout or slice stride moved.
  **The comparison is the sampler's.** `SQ_IMG_SAMP_WORD0`'s `DEPTH_COMPARE_FUNC` has carried
  `GL_TEXTURE_COMPARE_FUNC` since `-6c80` measured a pass and a fail either side of the stored
  depth. What the shader adds is the reference - r/q, clamped to [0, 1] as GL 1.4 3.8.14 and
  softpipe both clamp it - handed over by `image_sample_c` as the first address register.
  **One value comes back, not four**, so `dmask` is `0x1` and three moves spread it across
  `v4..v7` as `GL_DEPTH_TEXTURE_MODE` says: (v, v, v, 1) luminance, (v, v, v, v) intensity,
  (0, 0, 0, v) alpha - that last writing `v7` before it zeroes `v4`. The roadmap had expected the
  depth mode to be the descriptor's destination swizzle; doing it in the shader keeps it clear of
  the border colour, which is a *stored depth* the comparison still runs against and so goes into
  the table as the border colour's red in all four rather than expanded by the mode. Expanding it
  there would have zeroed red under `GL_ALPHA` - the one channel a 32_FLOAT image reads.
  `tools/shader/tex-shadow.s`, whose four cross-check instructions - the plain 2D sample, the
  prolog's divide of s by q, its `attr1.x` interpolation and `s_waitcnt vmcnt(0)` - assembled to
  words already in the tree. `GL_ARB_depth_texture` and `GL_ARB_shadow` join **both** extension
  lists, with the `_ARB` spellings of their enums; neither adds an entry point, a depth texture
  being `glTexImage2D` with a `GL_DEPTH_COMPONENT` internal format and the comparison being
  `glTexParameteri`.
  **One value in it is documentation rather than measurement**: which address register
  `image_sample_c` reads the reference from. `-6c80`'s two arms prove it is read from whichever
  register they varied - their texture was a uniform 0.5 and only the reference changed between
  the passing and the failing row - but not its index, so `REQ-20260920T1340Z-b4e1` asks for that
  and for the descriptor word those arms did not print.

- **3D textures are sampled on the console** (2026-09-20), which completes the two targets that
  read a third texture coordinate. The volume's slices were already laid out one after another by
  the upload and the descriptor already carried `TYPE 0xa` with the last slice in `WORD4`; what
  was missing was the sample. The slot the cube map introduced now holds a third form,
  `tools/shader/tex-3d.s`: r interpolated from the third parameter's `w`, multiplied by the
  `1 / q` the prolog left in `v12`, the divided s and t copied up beside it - the address
  registers have to be consecutive and `v4` is the texel's own - and `image_sample` with
  `dim:SQ_RSRC_IMG_3D`. Seven words. Three cross-check instructions assembled to words already in
  the tree: the 2D sample it replaces, the prolog's own divide of s by q, and its `attr1.x`
  interpolation.
  **The divide is the whole difference from the cube map's sample.** A volume's (s, t, r) is a
  position GL 1.2 divides by q like the other two; a cube map's is a direction, which scaling
  leaves unchanged, so that one interpolates all three components fresh instead. Both read r from
  the same place, so a volume-textured draw also exports at least three parameters.
  `GL_EXT_texture3D` joins **both** extension lists, and its two entry points - `glTexImage3DEXT`
  and `glTexSubImage3DEXT` - arrive with it, along with the `_EXT` spellings of its enums. A list
  entry whose entry points are missing is the promise this library refuses to make, and a program
  of that era calls those names rather than GL 1.2's.
  **Level 0 only.** A volume's mip levels halve depth as well, and the chain layout here is a 2D
  one, so `LAST_LEVEL` stays 0 and a minifying filter reads the base level on the console while
  the software rasteriser reads the chain. A draw whose filter would have used one says so once
  in the log - which is what the line a 3D-textured draw used to earn now means. gl1-probe's
  `texture-3d` is expected to pass on the console.

- **Cube maps are sampled on the console** (2026-09-20). With the six faces uploaded as one array
  and the descriptor carrying `TYPE 0xb`, the pixel shader gained the sample: a new slot holding
  either the two-word 2D `image_sample` the prolog always ended with, or the twenty-four words a
  cube needs.
  **A cube map is not a sample with an extra coordinate.** The texture coordinate is a
  *direction*, and the sampler wants the face it points at and the place on that face - which
  RDNA2 computes in four instructions of its own, `V_CUBEID_F32`, `V_CUBESC_F32`, `V_CUBETC_F32`
  and `V_CUBEMA_F32`. The shader divides the two coordinates by twice the major axis, biases them
  by a half, and hands the sampler `(u, v, face)`. `tools/shader/tex-cube.s`, whose two
  cross-check instructions - the 2D sample it replaces and the prolog's own `attr1.x`
  interpolation - assembled to the words already in the tree.
  **The direction is interpolated again rather than reused**: the prolog divides s and t by q
  before the slot runs, which is right for a 2D sample and wrong for a direction. Dividing all
  three components would leave a direction unchanged; dividing two of them does not. The third
  component is `attr2.w`, where the vertex carries unit 0's r - so a cube-textured draw exports
  at least three parameters, which the draw path now forces independently of the colour sum's
  use of the same export.
  `GL_ARB_texture_cube_map` joins **both** extension lists. It adds no entry point of its own,
  only targets, enums and the `GL_NORMAL_MAP` and `GL_REFLECTION_MAP` generation modes, and every
  one of those is kept on both paths now. An incomplete cube map is still drawn untextured, which
  is what GL does with one anyway.

- **A cube map's six faces upload as one array** (2026-09-20). The faces arrive one at a time,
  each its own tight-packed image in process memory, because that is what the software rasteriser
  samples - so a cube map had no hardware image at all and its descriptor described nothing.
  `gl_tex_cube_upload` builds the array the hardware wants: face f at slice f in GL's order
  (+X, -X, +Y, -Y, +Z, -Z), each slice laid out as a 2D image with the same 256-byte row pitch
  every other image here has. It runs from `gl_tex_hw_prepare`, when a draw needs the image,
  rather than as each face arrives - six faces would otherwise allocate and copy six times and
  hold a half-built cube in between - and only when all six are present and square, an incomplete
  cube map being one GL does not sample at all.
  The descriptor takes the face size from the array rather than from the object, whose own image
  fields stay empty for a cube map; a new face marks it stale.
  **The slice stride is derived, not measured, and that is said where it is used.** `-6c80`
  sampled a 3D image and a cube on the part, but each arm reported one texel and the 3D arm's 2D
  control reported the same one, so any stride is consistent with those rows. Consecutive slices
  of `pitch * height` is what addrlib computes for a linear array and what this library's own 3D
  upload already writes. If it is wrong, face 0 is right and the other five are wrong - a picture
  nobody would attribute to a stride - so `REQ-20260920T1050Z-5d7c` asks for slices that differ
  from each other, and `test_pm4_gl_cube_faces_upload_as_one_array` pins the current one so a
  corrected stride fails there first.
  **Nothing samples it yet**: a cube-textured draw is still dropped with its log line, because
  the pixel shader has no cube variant - `dim:SQ_RSRC_IMG_CUBE` and a direction for coordinates.
  This is the storage half.

- **The descriptors for 3D images, cube maps and depth comparison** (2026-09-20).
  `REQ-20260920T0745Z-6c80` (sweep `20260920-103636`) sampled all three on the part with texels a
  failed sample could not produce, and reported every descriptor and sampler word. Those words
  are now what `gl_pack_descriptors` produces and what a test asserts - not a reading of the
  register tables, the words the hardware actually sampled with:
  - `TYPE` is the target's: 9 for 2D, `0xa` for 3D, `0xb` for a cube map. The check's 3D arm and
    its 2D control describe the *same memory*, so that field alone is the difference between a
    green texel and nothing.
  - `WORD4` is **the last slice for a 3D image and a cube map**, not a row pitch - `0x1` for a
    two-slice volume, `0x5` for six faces. Writing the pitch there would describe a volume one
    row wide.
  - `CLAMP_Z` carries `GL_TEXTURE_WRAP_R` for a 3D image, which is the whole difference between
    the check's `0x92` and its 2D control's `0x12`.
  - `DEPTH_COMPARE_FUNC` carries `GL_TEXTURE_COMPARE_FUNC` when the mode asks for it. The check
    put one reference either side of the stored depth under `LEQUAL` and got a pass and a fail,
    which is what says the field works rather than the sample returning nothing.
  **And `glTexParameteri` now repacks for the compare mode and function.** Both returned early -
  correctly, while they were state for the software sampler alone and the hardware drew a depth
  texture untextured. Now that the sampler carries the comparison, a descriptor that did not
  follow them would compare against whatever the field last held.
  **A cube map still has no hardware image**: its six faces live on the CPU and the object's own
  image fields stay empty, so there is nothing for a descriptor to point at and a cube-textured
  draw is still dropped with a log line. The test asserts that gap rather than asserting words
  for an image that does not exist, which would pass and mean nothing. `-6c80` settles the
  hardware half, so what remains is uploading the faces as one array.

- **The second texture unit is on: the console applies two** (2026-09-20). Everything for it was
  written and gated; `REQ-20260920T0745Z-9a41` (sweep `20260920-103636`) opened the gate with the
  controls `-8b1c` lacked:
  - **The fourth parameter carries its own value.** Attributes 0-2 were `(1, 0, 0, 1)` and
    attribute 3 `(0.25, 0.5, 0.75, 1)`. The three-parameter control printed red `0xff0000ff`; the
    four-parameter arm printed `0xffbf8040`, attribute 3's own constant. `-8b1c` could not tell
    these apart, because its control printed the arm's answer.
  - **Two descriptor pairs are sampled in one pixel shader.** The two-sample arm binds images at
    `0x2009000` and `0x2009100` and returns yellow; the *same fixture* with a single sample
    returns red, which is what rules out a shader exporting a constant.
  The check reports `desc1-sgpr 0x14` and `samp1-sgpr 0x1c` - s20 and s28, exactly where
  `tools/shader/tex-prolog2.s` loads the second pair from, chosen here independently as the next
  free range.
  `GL_ARB_multitexture` joins the console's extension list, so a port reading it there takes its
  multitexture path and gets the second layer. **A texture on unit 1 with unit 0 untextured is
  still left out** - this path's second stage combines against the first's result and there is no
  first - and the log line says a unit is dropped without claiming which ones are safe, because
  "above GL_TEXTURE1" would have been wrong in exactly that case. `ctx->hw_multitex` moved to the
  context defaults from the hardware-init block: it is a property of the build, and a host test
  needs it true without a queue to talk to.

- **`docs/PORTING.md`, and `<time.h>`** (2026-09-20). The guide is for someone holding a GL 1.x
  program: what of GL, GLU, GLUT and the C library is here, what is absent, the entry point a
  payload needs, and the differences that will bite - `glutMainLoop` returning, the pad arriving
  as keys, `printf` going to the kernel log, `time()` not being a wall clock, the version badge.
  It gives the absent list its own table and says why each entry **fails to link** rather than
  stubbing: a stub that draws nothing is a bug found on a console, a link error is one found on
  a desk.
  Its sharpest section is *The one that will get you*: a payload link ignores unresolved symbols,
  so a clean build is not evidence, and the `nm -u` line that is. glut-demo's eleven silently
  undefined functions are written down there as the worked example, because a porter who has not
  seen that failure will not believe it until it happens to them.
  `<time.h>` is `time()` and `clock()` over the monotonic clock, and says out loud that `time()`
  counts from the payload's start rather than the epoch - right for `srand(time(NULL))` and for
  elapsed measurement, wrong for a date, which is why `localtime` and `strftime` are absent
  rather than approximate.

- **`<stdio.h>`, `<ctype.h>` and `<assert.h>`: a port loads its own files now** (2026-09-20). A
  port loads a texture, a model, a level, a config - with `fopen` and `fgets`, parsed with
  `isspace` and `atof`, reporting with `printf`. All of it sits on what this SDK already had
  (`oops_fs_open`, `oops_klog`, `oops_vsnprintf`) under names nothing being ported calls.
  **`stdout` and `stderr` are the kernel log**, buffered to a line so a line arrives as a line
  and tagged `stdout`/`stderr`, because there is no terminal - a `printf` from a port lands where
  every other diagnostic in this collection does. A line longer than the buffer is flushed in
  pieces rather than truncated: a split diagnostic beats a cut one. `FILE` is a descriptor and
  three flags; `fopen`'s mode string maps to the filesystem's flags including `+`. `<ctype.h>` is
  the C locale's and says so - a port needing more than ASCII needs more than that header, and
  should find out from the header rather than from a mis-parsed file. A failed `assert` writes
  its expression, file and line to the log before ending the payload, the log being the only
  place anyone will look.
  **`tools/libc-check` grew a self-test**, because a checker that cannot fail says nothing when
  it passes - it links a unit calling a function that exists nowhere and requires that name to
  come back, before it trusts its own report about the real ones.

- **The C library a port expects, and a check that it is really there** (2026-09-20). The SDK has
  had the functions for a long time - as `oops_sqrtf`, `obs_strlen`, `oops_malloc`. Nothing being
  ported calls them by those names: it writes `#include <math.h>` and `sqrtf`. `include/libc`
  now holds `<math.h>`, `<string.h>` and `<stdlib.h>` under the names a port uses, and
  `src/system/libc.c` implements them over what was already here. It is on the **target** include
  path only; the host build must keep the real C library, or a host test including `<string.h>`
  would get a freestanding one.
  **`tools/libc-check` is the part that matters.** A payload link passes
  `--unresolved-symbols=ignore-all` so the platform can resolve its own `sce*` imports at load,
  which means a missing `sqrtf` links silently and faults on the console - the most expensive
  place to find it. The tool calls every name the headers declare and fails if the linked object
  leaves any undefined. That trap is not hypothetical: glut-demo was built with `-Werror` against
  these headers before `libc.c` was in any source list, **linked clean, and left eleven standard
  functions undefined**. The check caught it; nothing else would have until hardware.
  So `libc.c` and `math.c` joined `CORE_SDK_SRCS` in oops-apps' `app.mk`: every payload gets them
  whether it lists them or not, because leaving it to each app to remember is the wrong way round
  when forgetting links clean.
  The double forms are the float ones widened, `asinf`/`acosf` come from `atan2` and clamp at the
  ends, `rand` is the usual linear congruential generator and says in its header that it is not
  for anything that must be unguessable, and `exit`/`abort` go through the platform's own exit
  because a payload has no process to return through.

- **oops-glut: enough of GLUT that a program written against it builds and runs** (2026-09-20).
  Most GL 1.x code in the world does not open a window or read input itself - it calls
  `glutCreateWindow`, registers callbacks and hands control to `glutMainLoop`. Without that,
  every port begins by rewriting the one part of the program that has nothing to do with what it
  draws. `include/GL/glut.h` and `src/gl/glut.c` are the subset those programs use, over this
  SDK's own display, input and timing: one window, the callbacks (display, reshape, idle,
  keyboard and its up twin, special, mouse, motion, passive motion, timers),
  `glutPostRedisplay`, `glutSwapBuffers`, `glutGet`, `glutGetModifiers`, and the solids over the
  GLU quadrics.
  **`glutMainLoop` returns**, through freeglut's `glutLeaveMainLoop`, because GLUT's contract -
  that it never returns and the program lives in its callbacks - leaves a console program with no
  way to stop. The keyboard arrives as HID usage codes and is mapped only where
  `glutKeyboardFunc` and `glutSpecialFunc` can express it; a key neither can carry is dropped
  rather than delivered as a plausible wrong character, and shifted punctuation is left alone
  because those rows differ by layout. The mouse is relative and GLUT's callbacks are absolute,
  so a cursor is kept here, clamped, and starts centred.
  **The pad arrives as keys**, which is not GLUT's idea at all: a console often has no keyboard,
  so without it many ports run and cannot be controlled. The d-pad is the arrow specials, cross
  and circle are `\r` and escape, and the option button leaves the loop. `glutOopsPadKeys(0)`
  turns it off. **Not here:** subwindows, menus, overlays, the font, game mode, and the teapot -
  306 control points this does not carry, so a program that wants one fails to link rather than
  drawing a sphere and hoping.

- **A two-unit draw, end to end and gated off** (2026-09-20). The three pieces are wired
  together: a draw with a texture on unit 1 *and* on unit 0 binds the four-parameter vertex
  shader, sets `SPI_VS_OUT_CONFIG` 0x6, `SPI_PS_IN_CONTROL` 0x4 and `SPI_PS_INPUT_CNTL_3` 0x3
  (0x194, Mesa `gfx103.json:4203`) - the registers `-8b1c`'s arm retired with - writes an 80-byte
  vertex carrying unit 1's coordinate at offset 64, fills the second descriptor pair one stride
  along the table, and turns on both shader slots. `gl_hw_emit_param_count` carries a count now
  rather than a flag, since there are three interfaces to switch between.
  **`ctx->hw_multitex` is `OOPS_GL_MULTITEX_MEASURED` in every real context**, so no draw takes
  this path on a console. A test may set it, and `test_pm4_gl_two_unit_draw_binds_the_fourth_
  parameter` does - which is how the whole path is checked before the measurement lands, the way
  the scanout path's `hw_rx` was. It also checks the way back: dropping unit 1 rebinds the
  two-parameter shader, zeroes `SPI_PS_INPUT_CNTL_3` and returns both slots to branches, because
  a stale slot would sample a descriptor pair the draw no longer writes.
  The log line about units above `GL_TEXTURE0` now says `GL_TEXTURE1` when the gate is open, so
  it cannot claim a unit is dropped that is not.

- **The second combine stage, and a latent bug in the first** (2026-09-20). The combine encoder
  took unit 0's registers as given: the texel in `v4..v7`, everything else in `v8..v11`. It now
  takes a `gl_ps_stage_t` - which register holds this unit's texel, which holds `GL_PREVIOUS`,
  and whose environment state to read - so unit 1's stage is unit 0's with `{28, 4, 1}` in place
  of `{4, 8, 0}`. `gl_ps_env_word`'s four hard-coded word tables became generated words for the
  same reason; the assembler-pinned tests confirm unit 0's output is unchanged, which is what
  makes the refactor safe to believe.
  **The bug it surfaced:** `gl_tex_env_as_combine` encoded the incoming fragment colour as
  `GL_PRIMARY_COLOR`. GL 1.3's table 3.18 says the fixed environment functions combine the texel
  with the colour *the unit before left* - `GL_PREVIOUS`. At unit 0 the two are the same thing,
  which is why this was harmless while one unit was applied. At unit 1 it is the difference
  between a lightmap modulating the base map and a lightmap modulating the vertex colour with the
  base map thrown away - the single most common multitexture setup there is. Corrected in all
  five places. Nothing shipped changes: the software rasteriser threads the incoming colour
  correctly and always has, and the hardware path has no second unit yet, so this was reachable
  only by the code being written for it.
  The second slot is 64 words after the first; off, it is a branch over itself, so a draw that
  drops to one unit stops combining a texel it no longer samples.

- **The second texture unit's sample slot** (2026-09-20). The sampling half of the multitexture
  gap: unit 1's coordinate interpolated from the fourth parameter the four-parameter vertex
  shader exports, its own image and sampler loaded from the second pair of the descriptor table
  (`+0x40` and `+0x60`, now named `OOPS_GL_DESC_UNIT_STRIDE` rather than being four bare `0x900`s
  and an assumption), and the texel in `v16..v19`. `tools/shader/tex-prolog2.s`, whose three
  cross-check instructions - unit 0's own descriptor loads and sample - assembled to the words
  the tree already writes.
  **The texel goes to `v28..v31`, and that is not free choice**: `gl_ps_combine_program` gathers
  the general combine form's arguments into `v16..v27`, so a texel parked there would be
  overwritten by unit 0's *own* combine whenever its environment is `GL_COMBINE`, `GL_BLEND` or
  `GL_DECAL` of an RGBA texture - correct under `GL_MODULATE` and wrong under the modes that need
  a program, which is the worst shape a bug can have. The pixel shader's
  `SPI_SHADER_PGM_RSRC1` is `0x000c0010`, VGPRS `0x10`, which is 136 registers in wave32.
  **It sits before the prolog's exec restore**, so both samples are taken inside whole-quad mode.
  A sample outside it has no helper pixels, so its implicit derivatives and its level of detail
  are wrong along every quad edge - a seam of the wrong mip level rather than a missing picture,
  which is why the test pins the placement and not just the words. Hoisting both samples above
  both combines costs nothing: a sample depends on its coordinate, not on the combine before it,
  and GL's order is kept by the combine slots. Off, the slot is a branch over itself rather than
  twenty `s_nop`s, because every fragment of every textured draw runs through it.
  The textured pixel shader **moved to 0x1000 with 320 words** to make room for this slot and the
  second combine stage still to come; growing in place would have run into the three-parameter
  vertex shader at 0x700. `gl_ps_flush_shaders` flushes the two shader ranges separately rather
  than one span covering the gap between them.
  **No draw sets it**, and the second combine stage is not written - `gl_multitex.h` holds the
  gate and `REQ-20260920T0745Z-9a41` asks for the descriptor rows `-8b1c` did not report.

- **The four-parameter vertex shader, towards a second texture unit on the console**
  (2026-09-20). The software rasteriser applies every unit `glActiveTexture` names; the hardware
  path samples one and logs a line, so a multitextured scene comes out on the console missing its
  second layer - the largest single difference a port sees between the two paths.
  `gl_vs_build_param4` (`tools/shader/vs-param4.s`) carries the second unit's coordinate in an
  80-byte vertex exported as `param3`. That stride is not a shift, so the lane's offset is
  lane * 64 plus lane * 16 - a program keeping the 64-byte shift would read every vertex but the
  first from the wrong place, which is what the test pins. Every instruction shared with the
  three-parameter program assembled to the word already in the tree.
  **No draw runs it yet.** obSCEne's `-8b1c` showed the four-parameter interface retires on the
  part with both canaries - so it is legal, where the packed form of the *third* parameter hung
  the GPU - but not that the value arrives: its three-parameter control printed the same pixel as
  the arm, and its two-sample arm reported no descriptor words at all. `gl_multitex.h` holds the
  gate and the reasoning, as `gl_rx.h` did for the scanout path; `REQ-20260920T0745Z-9a41` asks
  for the controls that could differ.

- **The scanout path is on: oops-gl draws into the display's buffers as a title does**
  (2026-09-20). `OOPS_GL_RX_MEASURED` was 0 because one link was unmeasured - whether the colour
  block, told `COLOR_SW_MODE` 27, writes the layout VideoOut scans. Two obSCEne results settle
  both halves of it, and `tools/rx-check` reads them rather than the prose around them:
  - **Inside a block**, `-2d7f` (re-filing `-7e21`, sweep `20260920-085011`) dumped all 65,536
    bytes of a 128x128 render, linear and tiled. Detiled through the display tiler's own vectors,
    **every one of the 16,384 pixels equals the linear control**; read as rows only 10,408 do, so
    the picture can tell the two layouts apart rather than agreeing because it cannot.
  - **Across blocks**, `-4b19` dumped a 256x256 render straddling four blocks. There is no linear
    control at that extent, so `rx-check --shape` detiles under **every** block order and keeps
    those giving one run of drawn pixels per row with no gap between drawn rows. Exactly one
    survives - the row-major order this library assumes - and its 24,512 pixels are the number the
    check itself reported, which is the triangle's area from its own geometry rows.
  The gap rule exists because the first pass kept two orders: swapping whole rows of blocks moves
  image rows without splitting any, so "one run a row" cannot see it. Both verdicts are tracked as
  `tools/rx-check/rx_check_7e21.txt` and `rx_check_4b19.txt`.
  **What is still not settled** is recorded in `gl_rx.h`: both tiled arms matched, so the rows
  cannot separate `0x08c6c000` from `0x0dc6c000`. Arm 1's value is taken because it is the smaller
  change from the linear value already drawing correctly, not because the other was refused.
  `include/agc/tiler.h` no longer calls the block order a computation.

- **The GLU quadrics** (2026-09-20). `gluNewQuadric`, `gluDeleteQuadric`, `gluQuadricDrawStyle`,
  `gluQuadricNormals`, `gluQuadricOrientation`, `gluQuadricTexture`, `gluQuadricCallback`,
  `gluSphere`, `gluCylinder`, `gluDisk` and `gluPartialDisk` - the ball, tube and ring half the
  tutorials in the world are built from, and so half the code being ported. They emit ordinary
  `glVertex` calls, so a quadric lights, textures, compiles into a display list and reaches the
  hardware path like typed-out geometry.
  **Two conventions are the specification's and are pinned by a test**, because neither is
  visible in a port's own source when it goes wrong: `s` runs 0 at the +y axis, 0.25 at +x, 0.5
  at -y (a sphere whose `s` runs the other way looks plausible until the label on it reads
  backwards), and GLU_OUTSIDE winds the surface counter-clockwise seen from outside, so it
  survives the default `glCullFace` setup - a sphere wound the wrong way disappears entirely.
  The test reads the geometry back through `GL_FEEDBACK` and checks both, in both orientations.
  The poles are triangle fans and the bands between them quad strips, as GLU does it: a band
  reaching a pole would be quads with two corners in the same place. **The winding test found
  that** - eight of sixty-four faces had no outward direction to check, being degenerate.
  A cone's normals lean with its slope rather than standing out as a cylinder's would, and every
  normal is unit length. The tessellator and NURBS are still absent.

- **The GLU functions a port actually calls** (2026-09-20). `src/gl/gl_glu.c` adds
  `gluBuild2DMipmaps`, `gluBuild1DMipmaps`, `gluScaleImage`, `gluProject`, `gluUnProject` and
  `gluGetString` beside the five that were already there. GLU is not part of GL, but a port that
  cannot find it does not build, and every texture-loading path written before
  `GL_GENERATE_MIPMAP` (1.4) calls `gluBuild2DMipmaps`.
  The filter is a box, as SGI's GLU is: an output pixel is the average of the input pixels its
  footprint covers, and magnification replicates. **Each mipmap level is built from the level
  above**, not from the original - a chain sampled from the original looks right at level 1 and
  wrong further down, which the test pins by requiring the 1x1 level of a 4x4 image to be the
  mean of all sixteen texels. The builders scale to powers of two, upload through a known unpack
  state and restore the caller's. `gluScaleImage` reads through `GL_UNPACK_*` and writes through
  `GL_PACK_*`, as the specification says, and converts between types as it goes.
  `gluProject`/`gluUnProject` are exact inverses through a perspective projection; both answer
  GL_FALSE rather than dividing by zero on the eye plane or by a singular matrix. GLU's errors
  are its own return codes and reach `glGetError` nowhere; `gluErrorString` names them now.
  The quadrics, the tessellator and NURBS are still absent - a program needing them fails to
  link, which is a better answer than a stub that draws nothing.

- **The polygon stipple is applied on the console** (2026-09-20). It was a software-rasteriser
  feature: a stippled polygon came out solid on the hardware path, and gl1-probe's
  `polygon-stipple` was written expecting that failure. RDNA2 has no stipple hardware, so both
  pixel shaders carry a sixteen-word discard between the canary block and the interpolation -
  `tools/shader/polygon-stipple.s`, whose cross-check word is one the shaders already had. It
  reads the fragment's window position from `v2` and `v3`, which arrive because a stippled draw
  sets `SPI_PS_INPUT_ENA` and `SPI_PS_INPUT_ADDR` to `0x302`; that those two VGPRs carry `POS_X`
  and `POS_Y` is obSCEne's `REQ-20260919T2258Z-c7d4`, measured against a control that had them
  undefined. The slot sits before whole-quad mode, so the mask the sample restores is the one
  the discard leaves, and a discarded fragment stays discarded while its quad's helpers still
  run for the derivatives.
  The mask is written to the payload at `OOPS_GL_STIPPLE_OFFSET` **in the form the shader wants**
  rather than the form GL stores: row `i` is `polygon_stipple[(height - 1 - i) & 31]` with its
  bits reversed, so the lookup is one load and one shift instead of the rotation and reversal the
  software path does per fragment. `test_pm4_gl_polygon_stipple_discards_in_the_shader` asserts
  the two forms select the same pixels across a 32x32 window - a dropped rotation would stipple
  correctly but upside down, which a screenshot does not show.
  Both shaders' slots moved up sixteen words for it; the textured shader now has 176 words (to
  0x6c0) and the untextured one moved to 0x200 with 128, since 64 at 0x300 left three spare once
  the stipple slot and the second export had taken theirs. A mid-frame `glPolygonStipple`
  submits the draws built against the old mask first, as a mid-frame texture change already did:
  one table serves every draw of a frame.

- **A draw into both colour buffers reaches both on the console** (2026-09-20). `glDrawBuffer`
  with `GL_FRONT_AND_BACK` or `GL_LEFT` wrote the back only there and logged a line saying so;
  the front kept whatever the CPU's clears and pixel rectangles had put in it, so the two drifted
  apart exactly where a draw had been. The draw now binds `CB_COLOR1_BASE` / `_BASE_EXT` /
  `_VIEW` / `_INFO` / `_ATTRIB` / `_DCC_CONTROL` / `_ATTRIB2` / `_ATTRIB3` to `fb_also` (offsets
  from Mesa `src/amd/registers/gfx103.json`), sets `CB_TARGET_MASK` and `CB_SHADER_MASK` to
  `0xff` and `SPI_SHADER_COL_FORMAT` to `0x99`, and ends both pixel shaders with
  `exp mrt0 ... vm` / `exp mrt1 ... done vm` - `done` marks a wave's last export, so the
  single-target word could not simply be repeated. The registers are obSCEne's
  `REQ-20260919T2258Z-3f62` (sweep `20260920-082906`), the shader words are
  `tools/shader/mrt1-export.s`, whose cross-check instruction re-assembles the single-target
  export already in the tree. `gl_ps_patch_export` runs on every draw, like the colour sum's
  slot and for the same reason: a draw after `glDrawBuffer(GL_BACK)` must stop writing a buffer
  GL no longer names, and an unbound second target has `CB_COLOR1_INFO` zeroed as well as being
  dropped from both masks. `test_pm4_gl_front_buffer_targets` checks both directions - the
  registers and the export words - and gl1-probe's `front-and-back` is expected to pass on
  hardware now rather than to fail.

- **`tools/rx-check` reads the block order out of a dump instead of assuming it** (2026-09-20).
  Past one 64 KiB block the tool's addressing rests on the blocks running row-major across the
  surface, which is what addrlib computes and what no hardware row has shown. For any extent
  larger than one block it now prints a block map - which 64 KiB block of the dump holds which
  128x128 block of the picture - and says whether that is row-major, not row-major, or
  undecided because a block holds no drawn pixel of its own. It decides nothing: the pixel
  comparison already fails an arm whose blocks are out of order. It says *how*, which is the
  difference between a re-file and a fix. The self-test gained a four-block round (256x256, the
  extent `-4b19` asks for) whose second arm has the same 64 KiB pieces transposed; the round
  requires that arm to fail and the map to name the transposition.
  `include/agc/tiler.h` now records which half of the scanout layout has a hardware anchor
  (the vectors inside a tile, obSCEne `-a91a`) and which is still a computation (the order of
  the tiles), and `docs/GL_ROADMAP.md` no longer calls the whole of it hardware-verified.

- **60 FPS locked presentation: cached scratch buffer & sequential-write RDNA2 CPU tiler**
  (2026-09-18). Implemented in `src/agc/agc_display.c`, `src/agc/agc_tiler.c`, and `src/input/keyboard.c`.
  - **Cached anonymous memory for linear scratch buffer**: `linear_scratch_fb` is now allocated in CPU-cached virtual memory (`oops_malloc` backed by anonymous `SYS_mmap`), with a fallback to direct memory. Eliminates uncached GDDR6 bus reads across Infinity Fabric that caused ~500 ms frame latencies on 1080p full-surface reads, reducing software 2D canvas draw time from 23.3 ms to 7.3 ms.
  - **Sequential-write optimized CPU macro-tiler**: Precomputed the closed-form inverse permutation tables (`s_inv_lx` and `s_inv_ly`) during `init_tiler_lut` using `agc_detile_pixel`. `agc_tile_surface` now writes destination macro-tiles sequentially (`tile_dest[0..16383]`), allowing the CPU's Write-Combining buffers to burst-combine stores and stream directly into GDDR6 at full bus bandwidth without cache line thrashing. Full 1080p display tiling time dropped from 508 ms down to 8.7 ms (58x speedup), achieving rock-solid 60.0 FPS presentation.
  - **Non-blocking flip submission**: Presentation now issues non-blocking `sceVideoOutSubmitFlip` directly, removing the blocking 500 ms kernel event queue timeouts.
  - **Keyboard process focus & multi-handle polling**: Added `sceKeyboardSetProcessPrivilege(1)` and `sceKeyboardSetProcessFocus(1)` during keyboard initialization so the process maintains input focus. Polling queries `sceKeyboardReadState` on every frame for immediate key responsiveness, zero double-tap loss, and smooth hold-to-repeat.

- **`OOPS_BUTTON_PS` / `OOPS_BUTTON_HOME` (bit 16), `scePadSetProcessPrivilege(1)` support, and native Switcher lifecycle primitives**
  (2026-09-18). Declared in `oops/input.h` and `oops/system.h`, defined in `src/input/input.c` and `src/system/system.c`.
  - Controller pad privilege (`scePadSetProcessPrivilege(1)`): Enables receipt of bit 16 (`0x10000`) for the PS / Home button, allowing root-tier applications like SeaShell to handle the PS button rather than the OS compositor swallowing it.
  - User ID resolution & handle fallback: `try_resolve_user_id()` initializes `sceUserServiceInitialize` with priority `{256}`, and `oops_input_poll()` falls back to `scePadGetHandle(user, 0, port)` with state recovery on disconnect.
  - Native Switcher lifecycle primitives: Added `oops_system_get_running_app_title_id()`, `oops_system_is_app_suspended()`, and `oops_system_kill_app()`, binding `sceSystemServiceGetMainAppTitleId`, `sceSystemServiceIsAppSuspended`, and `sceSystemServiceKillApp` / `sceSystemServiceGetAppIdOfBigApp`. Updated `oops_system_launch_app()` to pass `checkAppSystemVer = 2` (`SkipSystemUpdateCheck`).

- **`oops_system_park_until_closed()`: how a title finishes, on a platform where it may not**
  (2026-09-17). Declared in `oops/system.h`, defined in `src/system/system.c`. Never returns.

  obSCEne measured every candidate on retail firmware 12.40 and the answer is architectural:
  process lifecycle belongs to `SceShellCore` and **no userland call ends a `big-app` process**
  (`REQ-20260917T1450Z-2e71`, check `017-posix/process-exit-candidates`, sweep
  `20260917-160206`). `exit`, `_Exit`, `sceKernelExit` and every shell-level kill are absent.
  `_exit` is present at `0x8000003f0` and raises `SIGSYS`, because it reaches FreeBSD's syscall 1
  and a big-app container's credentials do not permit it. And returning from the entry point
  faults at `rip: 0x0`, because the dynamic linker transfers control with no caller frame.

  So every title in the collection had been taking a crash report at the end of a *successful*
  run. The conforming pattern is to print the final line and idle while the host closes the app
  (`pros close <ID>`), which produces no coredump, no crash report and no hung GPU ring. This is
  that loop, with the measurement written up at the declaration.

  **It binds `sceKernelUsleep` itself rather than calling `oops_time_sleep_ms`,** which is worth
  a sentence because the tidier version is a latent defect. Four titles in oops-apps link
  `src/system/system.c` without `src/time/time.c`, and a title's link ignores unresolved symbols
  rather than failing - so calling into `time.c` from here buys those four a symbol that resolves
  nowhere and traps the first time it is reached. A duplicated weak binding is the cheaper of the
  two, and `time.c` binds the same symbol the same way.

  On a host build it traps rather than parking: a host process has a real lifecycle, and a
  selftest that idles forever is a hang rather than a result.

  `sceSystemServiceNavigateToGoHome` is the alternative the same resolution names - return the
  display to the home screen and idle - and is already bound weakly in `system.c`. It is
  deliberately not what this does, because a probe's caller usually wants the rendered output
  left on screen to sample.

- **The extension entry points a port calls, and an extension list that is no longer empty**
  (2026-09-19). A homebrew port written for the OpenGL of that era reads
  `glGetString(GL_EXTENSIONS)` and then calls *that extension's* names, not the core spellings
  which arrived in versions it does not assume. The list was empty, by a rule worth keeping: an
  extension is a promise about its own entry points, and only the core ones existed.
  - **Those entry points exist now** (gl.h's compatibility section): all eleven of
    `GL_ARB_vertex_buffer_object`, the seventeen of `GL_EXT_secondary_color`, five of
    `GL_EXT_fog_coord`, sixteen of `GL_ARB_window_pos`, four of `GL_ARB_transpose_matrix`,
    `GL_EXT_draw_range_elements`, `GL_EXT_multi_draw_arrays`, `GL_EXT_blend_color`,
    `GL_EXT_blend_minmax`, and the point parameters under both suffixes. Each is the core
    function under its published name, beside it in the source.
  - **The list names those**, and the extensions that are state this library already keeps:
    the texture environment's add, combine and dot3 under both suffixes, mirrored repeat,
    border and edge clamp, the LOD bias, `GL_EXT_bgra`, stencil wrap, rescale normal and
    separate specular colour.
  - **It is the path's list.** `GL_ARB_multitexture` is on it for the software rasteriser and
    off on the console, where a draw samples one unit: a port reading it there takes its
    single-texture path and draws correctly, rather than losing a layer silently. It joins the
    console's list when a draw samples two units.
  - **Not listed:** cube maps, 3D textures, depth textures, shadow comparison and occlusion
    queries, which work in software and not on the console.
  - `test_gl_extension_entry_points_are_the_core_ones` calls the new names and checks each
    against the core behaviour; `test_gl_strings_are_honest_and_parseable` checks the list's
    shape, what is on it, and what is not.

- **A third interpolant on the console, and the colour sum after texturing through it**
  (2026-09-19; written, not yet run on one). Until now a textured draw's secondary colour
  (lighting's separate specular term, or `GL_COLOR_SUM`'s) joined the primary per vertex, and
  the texture modulated it. The third parameter is measured:
  `REQ-20260919T1745Z-9c3e`'s unpacked form carried a value to the pixel shader byte for byte.
  That form is `SPI_VS_OUT_CONFIG` 0x4, `SPI_PS_IN_CONTROL` 0x3, `SPI_PS_INPUT_CNTL_2` 0x2. Its
  packed form hung the GPU, and it is not used.
  - **The vertex shader:** `gl_vs_build_param3` (`tools/shader/vs-param3.s`, assembled, at
    payload 0x700) is the two-parameter program with a 64-byte vertex and `exp param2`. The
    third vec4 is `{secondary r, g, b, unit 0's r}`.
  - **The switch:** `gl_hw_emit_param_count` points the NGG program at it and sets the three
    registers, only on a change. A frame without such a draw, gl-cube's included, is the
    stream it was.
  - **The pixel shader:** its colour-sum slot (`gl_ps_patch_sum`, `tools/shader/colour-sum.s`)
    adds the secondary colour after the combine and before fog, clamped. Fog, the alpha test
    and the export moved from 108, 120 and 124 to 120, 132 and 136.
  - **The vertex buffer** is filled by a byte cursor (`hw_vbo_cursor`), since a triangle is 144
    bytes or 192. Its capacity is the old 450 triangles of 144, so two-parameter streams submit
    where they did. The cache flush after the vertices now covers every line a triangle
    touches.
  - **Untextured draws** keep the per-vertex sum.
  - **Tests:** `test_pm4_gl_param3_vertex_shader_is_the_assembled_one`, and
    `test_pm4_gl_colour_sum_takes_the_third_parameter`, which covers the switch, the slot, the
    wide vertex, the switch back, and the slot emptying when the sum is switched off after a
    draw that summed. That last case was a bug in the first draft.
  - **Command budget:** `OOPS_GL_DCB_DRAW_MAX_DW` is 224, adding the switch's 21 dwords.

- **The CPU drawing calls take the GPU's tiled layout** (2026-09-19). `oops_surface_t` gains
  `layout`: `OOPS_SURFACE_LINEAR`, which is 0, so every existing initializer keeps its
  meaning; or `OOPS_SURFACE_RX`, the 64KB_R_X swizzle of the AGC scanout buffers.
  - **Every `oops_draw_*` call** - fills, blends, gradients, lines, circles, text, and blits in
    either direction between layouts - addresses a pixel through one index.
    - Linear keeps its row pointers.
    - Tiled uses two 128-entry tables built from `agc_tile_pixel`, because the swizzle is an
      XOR of one term per coordinate bit.
  - **`oops_display_get_surface` moved from `draw.c` into `display.c`,** because which buffer
    the next flip shows is the display's to know. After a renderer calls the new
    `oops_display_use_scanout`, it is the next scanout buffer, tiled. So a CPU overlay on a GL
    frame drawn in place, such as gl1-cube's HUD, lands in what is flipped, with no change to
    the app.
  - **`draw.c` no longer reaches the display,** so the app selftests that stubbed display
    calls for it no longer need to.
  - **Tests:** `test_draw_rx_layout_matches_linear` draws one scene through every call onto
    a 200 x 150 linear surface and a tiled one. The tiled one must match pixel for pixel,
    with the padding untouched. Swapping the x and y tables makes 24,770 pixels differ.
  - **Initializers:** the SDK's own tests now name the field.
  - **Cost:** on a scanout buffer a blended call reads write-combined memory, which is slow for
    the CPU.

- **oops-gl's scanout path: written, and off until measured** (2026-09-19). A title on this
  console draws straight into the buffers the display scans out. Those buffers are
  `WC_GARLIC` memory in the GPU's 64KB_R_X swizzle. oops-gl draws a linear buffer instead,
  and the display re-tiles it on the CPU at every flip. The pieces of the scanout path:
  - **The display**, for a renderer that draws its scanout buffers itself:
    `oops_display_scanout_layout`, `oops_display_scanout` (the next buffer or the shown
    one), `oops_display_wait_scanout` and `oops_display_flip_scanout` (flip as drawn), on
    AGC and GNM. The wait is the flip-status poll that `042d236` removed from every flip,
    now asked for only by a renderer that needs it.
  - **`agc_tile_pixel`**, the display tiler's forward direction for one pixel. It is checked
    against `agc_detile_pixel` and `agc_tile_surface` for all 16,384 pixels of a block.
  - **oops-gl:** `src/gl/gl_rx.h` holds the one unmeasured value (`CB_COLOR0_ATTRIB3`,
    `COLOR_SW_MODE` 27) and `OOPS_GL_RX_MEASURED`.
    - On the scanout path the back is the next scanout buffer and the front the one on
      screen, and a swap flips in place.
    - Every CPU colour access goes through `gl_color_index`: pixel rectangles, reads,
      copies, the accumulation buffer, and the CP's copy, which `glGetFrameReadback` detiles.
    - Whole clears and the readback copy are sized to the padded blocks.
  - **`tools/rx-check`** reads `REQ-20260919T1927Z-7e21`'s rows. It detiles each arm's dump,
    compares it with a linear control, and names the `ATTRIB3` value that draws the
    display's layout. Its self-test proves it on synthetic rows in obSCEne's format. It
    also parses the real 2026-09-16 rows, which show that run drew one pixel into a
    **linear** target, despite `REQ-20260916T1250Z-6e0f`'s result claiming a 64KB_R_X
    measurement.
  - **Off on the console** until 7e21's rows pass rx-check. `test_pm4_gl_scanout_path_targets`
    covers the path on the host, and a wrong swizzle fails it. gl1-cube's HUD follows the path
    with no change to the app, through the tiled CPU drawing below.
  - **Corrected comments:** `gl_zs_tiling.h` said depth's vectors were the display's with x
    and y exchanged; they share only the upper four bits of each coordinate. Two comments
    called `agc_draw.c`'s `0x08c6c000` a "measured" compositor value, and no log records it.

- **Front-buffer rendering** (2026-09-19): GL 1.0's `glDrawBuffer(GL_FRONT)` and
  `glReadBuffer(GL_FRONT)`, refused until now because the display handed oops-gl its back
  buffer only. Every name a double-buffered mono visual has is accepted, as Mesa's
  `main/buffers.c:145-170` and `:209-226` map them. `GL_LEFT` and `GL_FRONT_AND_BACK` draw
  into both buffers and read the front.
  - **The front** is a surface of oops-gl's own, allocated the first time a program names it
    and filled with the picture on screen.
  - **Writes and reads:** every write reaches the buffers `glDrawBuffer` names, each blended
    and masked against its own pixel. Every read takes the one `glReadBuffer` names.
  - **Presenting:** `glFlush` and `glFinish` put a drawn-into front on screen, and
    `glSwapBuffers` makes the front the picture it presented.
  - **Two new display calls** make that possible. `oops_display_present` puts a caller's linear
    image on screen without touching the framebuffer: AGC tiles and flips it, and GNM, whose
    framebuffer is a scanout buffer, copies it into the one on screen.
    `oops_display_read_shown` returns what is on screen. `agc_gpu_tile` now takes its source.
  - **Left on the console:** a *draw* into both buffers reaches only the back, with one log
    line. A second colour target and an `exp mrt1` would be needed to reach both. Clears and
    pixel rectangles reach both there.
  - **Also fixed:** `glContextDestroy` now frees the stencil surface, which it had leaked on
    the console.

  `test_gl_front_buffer` and `test_pm4_gl_front_buffer_targets` cover it; gl1-probe's
  `front-buffer` and `front-and-back` are the console measurements.

- **Depth and stencil pixel operations on the console** (2026-09-19). They are written but have
  not yet run on one. The console's depth and stencil surfaces are the GPU's, laid out 64KB_Z_X,
  and every CPU operation on them was refused with `GL_INVALID_OPERATION`: `glReadPixels`,
  `glDrawPixels` and `glCopyPixels` of depth or stencil, and a depth texture copied from the
  frame. A pixel rectangle's own depth and stencil tests were left out. All of them now address
  the surfaces as the DB lays them out. `tools/zs-tiling` builds Mesa's addrlib and computes
  the mode's swizzle vectors under the part's identity and the `GB_ADDR_CONFIG` oops-mesa
  derived (`0x4`, 16 pipes). It then checks every pixel of a three-by-two-block surface against
  addrlib, and none disagree for depth or stencil; an eight-pipe control disagrees, as it must.
  `src/gl/gl_zs_tiling.h` carries the vectors, and `gl_zs_depth_ptr`/`gl_zs_stencil_ptr` in
  `gl_internal.h` are the one way any of this code reaches a depth or stencil pixel.
  Coherence rests on the flush every pixel operation already made, and no HTILE is bound. The
  frame's closing `RELEASE_MEM` writes the DB caches back and invalidates GL2 (`GCR` `0x603`),
  so the CPU reads what the GPU drew and the next frame reads what the CPU wrote.
  `gl_depth_buffer_cpu` and `gl_stencil_buffer_cpu`, the functions that refused, are gone.
  `test_pm4_gl_zs_tiling_is_a_permutation` checks that each block maps one to one, with pinned
  offsets. `test_pm4_gl_stencil_reaches_its_registers` now round-trips a stencil and a depth
  value through the tiled addressing. gl1-probe's new `depth-readback` and `stencil-readback`
  are the console measurement.

- **The stencil test on the console** (2026-09-19) - written, not yet run on one; obSCEne could
  not measure it (`REQ-20260917T1845Z-3d5b`, not-possible from its fixture), so gl1-probe's
  `stencil` is the measurement. The console had no stencil buffer: stencil-tested draws drew
  unmasked. Now the stencil buffer there is a GPU surface, STENCIL_8 at the depth surface's
  64KB_Z_X swizzle and padded to 8-bit 64 KiB blocks (256 x 256, addrlib's
  `ComputeThinBlockDimension`; it was sized to depth's 128-pixel padding while it was CPU memory,
  which a tiled 8-bit surface would overrun). A frame that tests stencil makes it live -
  `DB_STENCIL_INFO` `0x20000181`, the measured `0x20000180` with `FORMAT` `STENCIL_8`, and the four
  bases - and each stencil-tested draw sets `DB_DEPTH_CONTROL`'s `STENCIL_ENABLE` and
  `STENCILFUNC`, `DB_STENCIL_CONTROL` and `DB_STENCILREFMASK`/`_BF`, as radeonsi does. A frame
  without stencil emits none of it, so gl-cube's stream is unchanged. A whole stencil clear is a
  fill in the command stream; a boxed or masked one is drawn with `GL_REPLACE` through the write
  mask. The stencil pixel operations were refused on the console at first, with one log line, as
  the depth ones were; they had read and written a CPU buffer the GPU never tested. Both work
  there since the evening (**Depth and stencil pixel operations on the console**, above).
  `test_pm4_gl_stencil_reaches_its_registers`.

- **A second texture unit** (2026-09-19) - GL 1.3 requires at least two ("must be at least two",
  section 2.6; table 6.29), and oops-gl had one: `GL_MAX_TEXTURE_UNITS` answered 1 and
  `GL_TEXTURE1` was refused everywhere, which the roadmap had wrongly called conformant. It
  answers 2 now. Each unit has its own server state (`gl_tex_unit_t` - target enables and
  bindings, environment and combiner, LOD bias, texture matrix stack, coordinate generation),
  selected by `glActiveTexture`; its own current, raster and array texture coordinate
  (`glMultiTexCoord*` and the client-active unit's `glTexCoordPointer`); and its coordinate in the
  vertex. The software rasteriser samples every applying unit, then runs each unit's environment
  on what the one before left: `GL_PREVIOUS` is that, `GL_PRIMARY_COLOR` the fragment's own, and
  GL 1.4's `GL_TEXTUREn` unit n's texel - zero for a unit applying none, as Mesa reads it
  (`main/ff_fragment_shader.c:760-762`). Pixel rectangles use every unit's raster coordinate.
  `GL_TEXTURE_BIT` saves every unit and `GL_ACTIVE_TEXTURE` (table 6.20); `GL_CURRENT_BIT` every
  current coordinate; the client vertex-array bit every array and `GL_CLIENT_ACTIVE_TEXTURE`.
  Lists record `glMultiTexCoord` with its unit, and an array element compiles to one per unit.
  Deleting a texture unbinds it from every unit (Mesa `main/texobj.c:1381`). Feedback reports
  unit 0's coordinate and the evaluators feed unit 0, as Mesa's do
  (`state_tracker/st_cb_feedback.c:114-118`, `vbo/vbo_exec_eval.c:88-92`). `GL_TEXTURE1` to
  `GL_TEXTURE31` are declared (Mesa `include/GL/gl.h:1710-1740`). **The console applies unit 0
  only** - a second coordinate is a pixel-shader interface change - and logs once when a draw uses
  more. `test_gl_multitexture_state_is_per_unit`, `test_gl_two_texture_units_draw` (which fails
  when unit 1 reads unit 0's coordinate - tried) and
  `test_pm4_gl_second_unit_is_left_out_on_hardware`; gl1-probe's `multitexture`.

- **`GL_BLEND`, RGBA `GL_DECAL` and `GL_COMBINE` on the console** (2026-09-19) - written, not yet
  run on one. They modulated there, with a log line, because the textured pixel shader's combine
  was four words - one per channel - and these are programs. The shader moved from payload
  offset 0x200 to the free 0x400 (128 words; the fog slot had left it 59 of 64) and the combine
  slot grew to sixty-four words: the four-word forms stay where they suffice, followed by an
  `s_branch` over the rest of the slot, and the three are generated - arguments, function, scale,
  clamp, as the software rasteriser's `gl_tex_combine` and Mesa's `emit_texenv` do, with
  `GL_BLEND` and `GL_DECAL` restated as combiner settings first (`calculate_derived_texenv`). The
  words come from an encoder in `gl_internal.h` whose every instruction form is checked against
  the new `tools/shader/combine.s`, assembled; `test_pm4_gl_combine_programs_compute_what_software_does`
  runs every generated program through a reader for those forms and holds it to the software
  path's `gl_tex_env_apply`, on all five modes over all six base formats and 32 `GL_COMBINE`
  settings (and fails when a constant in the generator is wrong - tried). The short `GL_ADD` form
  gained its clamp, which only the colour buffer's conversion applied before - too late for fog
  and the alpha test. gl1-probe's `combine` and new `tex-env-blend-decal` are the measurement.

- **Fog on the console** (2026-09-19) - written, not yet run on one. RDNA2 has no fixed-function
  fog and oops-gl's pixel shaders had none, so the hardware path drew every fogged primitive
  unfogged, silently. Now the per-vertex factor the software path already computes - eye distance
  or GL 1.4's fog coordinate, clamped - goes into the vertex's texture coordinate `z`, which the
  vertex shader already exported unused, and both pixel shaders interpolate it and blend red,
  green and blue towards the fog colour in a twelve-word slot before the alpha test
  (`gl_ps_patch_fog`), the colour as three literals patched in place. Every word is from the new
  `tools/shader/fog.s`, assembled for gfx1030 and read back. **The shader interface does not
  move**: the registers `test_pm4_gl_honours_the_gl_cube_oracle_record` pins are unchanged, and
  gl-cube, which uses no fog, runs twelve `s_nop` more. The alpha test's slots moved up twelve
  words to make room. `test_pm4_gl_fog_reaches_both_shaders_and_the_vertex` checks the words and
  the factors; gl1-probe's `fog` and `fog-coord` are the measurement.

- **Vertex arrays read as their type, and checked** (2026-09-19). Every array was read as
  floats - a colour array as unsigned bytes too - whatever type its pointer named, and no pointer
  call checked anything, so `glVertexPointer(2, GL_SHORT, ...)` drew from reinterpreted bits. Each
  array now takes the types GL 1.1 lists for it and is read as that type (`gl_array_comp`):
  positions and texture coordinates as values, colours and normals normalised as `glColor*` and
  `glNormal*` of that type convert them. A type outside the list is `GL_INVALID_ENUM`, a size
  outside the range or a negative stride `GL_INVALID_VALUE`, each leaving the array as it was
  (Mesa, `main/varray.c:918`, `:1180-1193`, `:1250-1262`, `:1330-1350`, `:1615-1629`). Elements are
  copied out, so an unaligned stride is safe.
  - **Colours are clamped to [0, 1] before rasterising** (GL 1.x, 2.14.9). Only lit ones were:
    `glColor3f(-1, ...)` - or a `GL_BYTE` colour array's -128 - wrapped to full intensity in the
    framebuffer's bytes.
- **`glWindowPos`, all sixteen** (2026-09-19) - GL 1.4's raster position in window coordinates,
  as Mesa's `window_pos3f` sets it (`main/rastpos.c`): always valid, no transform or clip test, z
  clamped and put through the depth range, w 1, the colour and texture coordinate the current
  ones unlit and ungenerated, the raster distance 0. A hit in `GL_SELECT`; compiled into lists.
- **Secondary colour and `GL_COLOR_SUM`** (2026-09-19) - GL 1.4's `glSecondaryColor3*`, all
  sixteen spellings, normalised as `glColor`'s are (Mesa `vbo/vbo_attrib_tmp.h:3258-3316`), and
  `glSecondaryColorPointer`: any of the eight types, 3 or 4 components as Mesa accepts them
  (`main/varray.c:1536-1553`), with its client state, queries and pointer. Unlit, `GL_COLOR_SUM`
  adds the vertex's secondary colour, clamped, after texturing and before fog; lit, lighting's
  secondary colour (the separate specular term, or zero) takes over and the sum runs regardless,
  Mesa's rule (`main/ff_fragment_shader.c:69-78`, now `gl_color_sum_on`). Flat shading takes the
  provoking vertex's; stippled and expanded lines interpolate it; array draws compiled into a list
  record it. Saved with `GL_CURRENT_BIT`, the enable with `GL_FOG_BIT` and `GL_ENABLE_BIT` - where
  Mesa's `attrib.c` saves it and never restores it - and the array with
  `GL_CLIENT_VERTEX_ARRAY_BIT`. The hardware path sums per vertex, as it did for separate
  specular; textured, it logs once.
  - **The client arrays start at their GL sizes and types.** The zeroed context answered 0 for
    `GL_VERTEX_ARRAY_SIZE` and every other size and type until a pointer call; they start at
    Mesa's `init_default_vao_state` values now (`main/varray.c:4127-4150`).
- **Fog coordinates** (2026-09-19) - GL 1.4's `glFogCoordf`, `glFogCoordd`, their vector forms and
  `glFogCoordPointer` (`GL_FLOAT` or `GL_DOUBLE`, Mesa `main/varray.c:1408`), and
  `GL_FOG_COORD_SRC`: under `GL_FOG_COORD` fog reads each vertex's coordinate, as given, in place
  of the eye distance (Mesa `main/ffvertex_prog.c:1062-1064`), and the raster distance of
  `glRasterPos` and `glWindowPos` is the current coordinate (`main/rastpos.c:475-479`,
  `:730-733`). The GL 1.4 and GL 1.5 enum names are both defined. Lists record it, from arrays
  too; `GL_CURRENT_BIT`, `GL_FOG_BIT` and `GL_CLIENT_VERTEX_ARRAY_BIT` save it - the source where
  Mesa's pop does not. Software only on the console, with the rest of fog.
  - **`GL_FOG_INDEX` is kept** rather than refused: colour-index fog is state in an RGBA context,
    queryable and saved with `GL_FOG_BIT`, as Mesa has it (`main/fog.c:136-142`).
- **Point parameters and multi-draw** (2026-09-19), the last GL 1.4 entry points - all 47 are
  declared now. `glPointParameter{f,i}{,v}` with Mesa's checks and initial values
  (`main/points.c:117-195`, `:211-219`): every point's size clamped to `GL_POINT_SIZE_MIN` and
  `GL_POINT_SIZE_MAX` - unattenuated too, as Mesa's rasterizer state clamps it - and divided first
  by `sqrt(a + b d + c d^2)` under `GL_POINT_DISTANCE_ATTENUATION`, d the eye distance. The point
  is expanded on the CPU, so this holds on the console as well. The fade threshold is state only
  (it fades multisampled points; there is no multisample buffer). Compiled into lists and saved
  with `GL_POINT_BIT` (Mesa `main/attrib.c:936-939`). `glMultiDrawArrays` and
  `glMultiDrawElements` validate every count before drawing anything, as Mesa does
  (`main/draw.c:530-547`, `:303-320`), then issue each non-empty draw - which is also how a list
  compiles them.
- **Texture LOD bias, `GL_GENERATE_MIPMAP`, `GL_INCR_WRAP`/`GL_DECR_WRAP`** (2026-09-19), GL
  1.4's value-level features but one (depth textures).
  - `GL_TEXTURE_LOD_BIAS` through `glTexParameter` and through `glTexEnv`'s
    `GL_TEXTURE_FILTER_CONTROL` target (Mesa `main/texenv.c:448-460`), queryable both ways, their
    sum clamped to `GL_MAX_TEXTURE_LOD_BIAS` (14, Mesa's) and added to the level of detail before
    `GL_TEXTURE_MIN_LOD`/`MAX_LOD`. The hardware draw writes the sum into the sampler's
    `LOD_BIAS` field (`SQ_IMG_SAMP_WORD2` bits 0-13, signed with eight fraction bits, as radeonsi encodes
    it for GFX10, `ac_descriptors.c:144-145`) as it copies the descriptor into the slot - never
    into the texture's own descriptor, since half of it is context state - and a zero bias adds
    no bits, so gl-cube's stream is unchanged. Derived, not yet measured.
  - `GL_GENERATE_MIPMAP` (and `GL_GENERATE_MIPMAP_HINT`): a change to the base level - an
    upload, a sub-image, a copy - rebuilds the levels above it up to `GL_TEXTURE_MAX_LEVEL`
    (Mesa's condition, `main/teximage.c:2889-2897`), one cube face at a time. A 2x2 box filter
    (2x2x2 for a volume) on the CPU, rounded half to even as Mesa's software path rounds; a row-
    by-row copy generates once, at its end. The levels land where uploaded ones do, so the
    hardware's mip chain carries them.
  - `GL_INCR_WRAP` and `GL_DECR_WRAP`, which the stencil test refused; they wrap round the eight
    bits where `GL_INCR` and `GL_DECR` saturate.
- **Depth textures and the shadow comparison** (2026-09-19) - GL 1.4's last feature, which makes
  GL 1.4 complete in software. `GL_DEPTH_COMPONENT` and `GL_DEPTH_COMPONENT16/24/32` internal
  formats on 1D and 2D targets (Mesa `main/teximage.c:1744-1790`); depth data for them and only
  for them (`:1795-1832`); stored as 32-bit floats in the four bytes a colour texel takes, and read
  back, copied from the depth buffer (`glCopyTexImage`/`glCopyTexSubImage` into a depth texture
  read depth) and mipmapped as depths. `GL_TEXTURE_COMPARE_MODE`, `GL_TEXTURE_COMPARE_FUNC` (all
  eight, GL 1.5's set) and `GL_DEPTH_TEXTURE_MODE`, with `GL_TEXTURE_BIT`: each texel compared
  against r clamped to [0, 1] before filtering, so `GL_LINEAR` gives percentage-closer filtering,
  and the result read as luminance, intensity or alpha. On the console a depth-textured draw is
  untextured, with one log line - a float descriptor and a comparison sample are the hardware half.
  `GL_TEXTURE_COMPARE_MODE` was the "unkept parameter" example in the refusal test; a GL 3.3
  swizzle is now.
- **GL 1.5's mapping, read back and occlusion queries** (2026-09-19) - its last 12 entry points,
  so every core GL 1.x entry point is now declared (499 in all).
  - `glMapBuffer`, `glUnmapBuffer`, `glGetBufferPointerv` and `glGetBufferSubData`, with
    `GL_BUFFER_ACCESS` and `GL_BUFFER_MAPPED`, checked in Mesa's order (`main/bufferobj.c:2671-2687`,
    `:3059-3069`, `:3270-3286`, `:3876-3897`). The store is process memory, so the mapped pointer
    is the store. A mapped buffer cannot be mapped again, updated, read back or drawn from
    (`GL_INVALID_OPERATION`, `gl_draw_sources_mapped`); `glBufferData` and a delete unmap it.
    `glBufferData` accepts all nine usages - it refused the `_READ` and `_COPY` ones.
  - Occlusion queries on `GL_SAMPLES_PASSED` with Mesa's rules (`main/queryobj.c`): a name is a
    query object once begun, `glBeginQuery` makes a new one in a compatibility context, one
    active query at a time, begin and end compiled into lists. The software rasteriser counts
    every fragment past the alpha, stencil and depth tests (in `gl_fragment_tail`, so pixel
    rectangles too); the result is exact and always available. On the console
    `GL_QUERY_COUNTER_BITS` is 0, with one log line - GL 1.5's way of saying the count is not
    informative - until the GPU's samples are counted through `ZPASS_DONE`, whose render-backend
    layout is filed with obSCEne as `REQ-20260919T1600Z-e3a7`.

- **`GL_COLOR_MATERIAL` writes the material** (2026-09-19). The current colour was substituted
  for the tracked properties while lighting and nowhere else: `glGetMaterial` read back the
  material as last set, and when the enable went off the material was that value again instead
  of the last colour it had tracked. It is written through now, as Mesa's
  `_mesa_update_color_material` does (`main/light.c:759-772`) and when Mesa does it - each colour
  change, the enable going on, `glColorMaterial` while on (`vbo/vbo_exec_api.c:237-240`,
  `main/enable.c:568-577`, `main/light.c:800-803`) - and `glMaterial` leaves a tracked property
  alone while the enable is on (`vbo/vbo_exec_api.c:590-598`). gl-cube, which tracks with it, lights
  the same pixels.

- **Antialiasing, in software** (2026-09-19) - `GL_POINT_SMOOTH`, `GL_LINE_SMOOTH` and
  `GL_POLYGON_SMOOTH`, refused before. The software rasteriser multiplies each fragment's alpha,
  after fog and before the alpha test, by the fraction of the pixel the primitive covers:
  - a smooth point is a disc of the unrounded size and a smooth line a rectangle of the unrounded
    width, their quads grown by a pixel so the partly covered pixels are drawn; a stipple's dashes
    keep crisp ends;
  - a smooth polygon fades across its own edges only - `gl_draw_polygon_tri`'s boundary bits -
    so the diagonals its triangles share draw no seam. A corner the diagonal leaves comes out half
    covered where a quarter is right, the approximation of fading triangle by triangle.
  Smooth sizes step by an eighth; `GL_POINT_SIZE_GRANULARITY` and `GL_LINE_WIDTH_GRANULARITY`
  (GL 1.2's `GL_SMOOTH_*`) report it, where they reported 1. The enables are on
  `GL_ENABLE_BIT` and the point, line and polygon bits. The hardware draws all three aliased
  and logs once.

- **`GL_COMBINE`, in software** (2026-09-19) - GL 1.3's texture combiner, with `GL_DOT3_RGB` and
  `GL_DOT3_RGBA`. It was refused with `GL_INVALID_ENUM`.
  - State: the colour and alpha functions, three sources and operands each, and the two scales,
    with GL 1.3's defaults (table 6.19); queryable, saved by `GL_TEXTURE_BIT`, compiled into
    lists.
  - Checked as Mesa's `set_combiner_*` check them (`main/texenv.c:107-370`): DOT3 a colour
    function only, the alpha operands only alpha ones, the sources GL names (`GL_TEXTURE0` among
    them, the one unit under GL 1.4's crossbar), each an enum error otherwise; a scale not 1, 2 or
    4 a value error.
  - Computed as Mesa's fixed-function program computes it (`main/ff_fragment_shader.c:563-735`):
    each function per component, DOT3 as `(2a - 1).(2b - 1)` into colour (and alpha, for
    `GL_DOT3_RGBA`), scaled, then clamped.
  - `glTexEnvf` went through `glTexEnvi` and truncated - a scale of 1.5 would have been 1. It is
    its own path, and the texture environment has one setter for every form.
  - On the hardware a combined draw modulates and logs once, as `GL_BLEND` does: the pixel
    shader's four combine words cannot hold it.

- **Cube maps, in software** (2026-09-19) - GL 1.3's `GL_TEXTURE_CUBE_MAP`.
  - Its binding, enable (outranking 3D, 2D and 1D, GL 1.3 3.8.15), default texture, and
    `GL_TEXTURE_BINDING_CUBE_MAP` and `GL_MAX_CUBE_MAP_TEXTURE_SIZE` (1024: the six faces live on
    the CPU, in a table of levels each cube-map object allocates when a face is first given).
  - The six face targets in `glTexImage2D`, `glTexSubImage2D`, `glCopyTexImage2D`,
    `glCopyTexSubImage2D`, `glGetTexImage` and `glGetTexLevelParameter`; a face must be square
    (Mesa, `main/teximage.c:1079-1100`). `GL_PROXY_TEXTURE_CUBE_MAP`. The object target and the
    image targets are kept apart: `glBindTexture` and `glTexParameter` take `GL_TEXTURE_CUBE_MAP`
    and refuse a face; the image calls take a face and refuse `GL_TEXTURE_CUBE_MAP`.
  - Cube completeness - six faces of one size and internal format, each mipmap complete under a
    mipmap filter (`main/texobj.c:800-830`).
  - The software sampler looks up by direction as softpipe's `convert_cube` does
    (`gallium/drivers/softpipe/sp_tex_sample.c:3220-3291`), and measures the level of detail
    with the neighbouring pixels projected onto the centre's face.
  - `GL_REFLECTION_MAP` and `GL_NORMAL_MAP` generation for s, t and r (q refused, as Mesa's
    `main/texgen.c:113-121`); refused before.
  - Texture generation's eye-space normal is now lighting's (`gl_eye_normal`): through the
    inverse-transpose, and unit length only under `GL_NORMALIZE` or `GL_RESCALE_NORMAL`. Sphere
    mapping used the modelview's own 3x3 and always normalised.
  - On the hardware a cube-mapped draw is untextured and logs once, as a 3D-textured one does.

- **Two-sided lighting** (2026-09-19). `GL_LIGHT_MODEL_TWO_SIDE` was refused, on the reasoning
  that lighting runs per vertex before a primitive's facing exists - but the triangle stage has
  all three vertices when it lights them. It now winds the triangle in normalised device
  coordinates first (the sign culling reads, under `glFrontFace`) and lights a polygon facing
  away with the back material and its normal reversed (`gl_compute_lighting_side`). An outline
  or corner drawn by `glPolygonMode` takes its polygon's side; `GL_LINES` and `GL_POINTS` are lit
  from the front. Both paths, the lighting being CPU work on both. Queryable, and on
  `GL_LIGHTING_BIT`.
  - `glColorMaterial`'s face was ignored: `GL_BACK` changed the front material, which one-sided
    lighting lit with. Each side now tracks the current colour only if the face names it. The
    face and property are checked (`GL_INVALID_ENUM`, as Mesa's `_mesa_ColorMaterial`,
    `main/light.c:776`), answered by `GL_COLOR_MATERIAL_FACE` and `_PARAMETER`, and saved by
    `GL_LIGHTING_BIT` - none of which they were.

- **GL 1.2's separate specular colour and `GL_RESCALE_NORMAL`** (2026-09-19).
  - `GL_LIGHT_MODEL_COLOR_CONTROL` accepts `GL_SINGLE_COLOR` and `GL_SEPARATE_SPECULAR_COLOR`
    (anything else `GL_INVALID_ENUM`) and is queryable and on `GL_LIGHTING_BIT`. Kept apart, the
    specular term is a secondary colour: `gl_compute_lighting2` returns it, flat shading holds it,
    and the software rasteriser interpolates it and adds it after the texture environment and
    before fog. The hardware path has one colour interpolant, so there it joins the primary
    colour per vertex - right untextured but where the sum saturates, wrong under a texture,
    which the log says once; see GL_ROADMAP.md, "Needs a shader change".
  - `GL_RESCALE_NORMAL` - an enable, on `GL_ENABLE_BIT` and `GL_TRANSFORM_BIT` - scales the
    eye-space normal by the inverse modelview's third-row length, Mesa's factor
    (`main/light.c:1090-1103`).
  - **Normals were always normalised**, `GL_NORMALIZE` or not: a program's own normal lengths
    never reached the lighting, as they do in every GL. They do now; gl-cube enables
    `GL_NORMALIZE`, so its frame is unchanged.
  - **A shininess of 0 lit no specular at all** - the term was skipped for it, and 0 is the
    default. `(n.h)^0` is 1, a full highlight wherever `n.h` is positive.
  - `GL_LIGHT_MODEL_LOCAL_VIEWER` was neither answered by `glGet` nor saved by
    `GL_LIGHTING_BIT`; it is both.

- **GL 1.2's texture LOD parameters** (2026-09-19) - `GL_TEXTURE_BASE_LEVEL`,
  `GL_TEXTURE_MAX_LEVEL`, `GL_TEXTURE_MIN_LOD` and `GL_TEXTURE_MAX_LOD`, all refused before.
  - Completeness counts from the base level to `min(p, MAX_LEVEL)` (GL 1.2, 3.8.8; Mesa,
    `main/texobj.c:729-760`): setting the maximum level to the last level uploaded is how a program
    keeps a short chain complete, and that now works. A base level with no image is incomplete,
    and so is a mipmapped texture whose maximum level is below its base.
  - The software sampler clamps the level of detail to [MIN_LOD, MAX_LOD] and counts levels, and
    measures the level of detail, from the base level. The environment and the border colour use
    the base level's format.
  - The hardware chain is built from the base level, cut at the maximum level, and a base level
    other than 0 is a chain even of one level - the descriptor's own image is level 0's. The
    sampler's MIN_LOD and MAX_LOD carry the clamp in 4.8 fixed point, as radeonsi encodes it
    before GFX12 (`ac_descriptors.c:139-140`); the default maximum stays the field's 0xfff, so
    every descriptor that existed before - gl-cube's included - is unchanged.
  - `glPushAttrib(GL_TEXTURE_BIT)` restored the bindings and nothing about the textures bound:
    a routine that pushed, clamped the caller's texture and popped left it clamped. The bound
    textures' parameters are saved and restored now, as Mesa's `copy_texture_attribs` does
    (`main/attrib.c:251-275`).

- **Texture parameters checked and complete but for the LOD ones; border colour and the wrap
  modes GL has** (2026-09-19).
  - `glTexParameter` stored any value for a wrap mode or filter; one GL does not have is now
    `GL_INVALID_ENUM` and changes nothing (Mesa, `main/texparam.c`, `set_tex_parameteri`).
  - `GL_TEXTURE_PRIORITY` and `GL_TEXTURE_BORDER_COLOR` are kept, clamped to [0, 1] as Mesa keeps
    them without float textures, and `GL_TEXTURE_RESIDENT` is answered. `glPrioritizeTextures`
    validated its priorities and dropped them; they are kept.
  - `glTexParameterf` went through `glTexParameteri`, truncating - a priority of 0.5 was 0 - and
    lists compiled it as the integer form. Each form now records and converts as its own: an
    integer border colour by range (`INT_TO_FLOAT`), and the scalar forms refuse the border colour
    as Mesa does.
  - Wrap modes: `GL_CLAMP_TO_BORDER` (1.3) and `GL_MIRRORED_REPEAT` (1.4) added, and `GL_CLAMP` is
    GL's - the coordinate clamped to [0, 1] and the border reached by a linear filter - where it
    was `GL_CLAMP_TO_EDGE`. A border colour is expanded by the base format, as texels are.
  - On the hardware, CLAMP_X/Y follow radeonsi's `si_tex_wrap` (`si_state.c:1927`), which makes
    `GL_CLAMP` the half-border clamp it was not, and the border colour type its
    `si_translate_border_color` (`si_state.c:4010-4074`): transparent black, opaque black and
    opaque white built in, anything else from a one-entry table at payload + 0xa00 that every
    frame now points `TA_BC_BASE_ADDR` at (`gfx103.json:2932-2943`; radeonsi sets it in its GFX10
    preamble, `ac_cmdbuf.c:529-530`). That is two more context registers in every frame's
    preamble, gl-cube's included; its pixels are unchanged.
  - `glGetTexEnviv(GL_TEXTURE_ENV_COLOR)` multiplied by `2147483647.0f`, which is 2^31 as a float,
    so a channel of 1.0 overflowed and read back as -2147483648. It converts in double, as Mesa's
    `FLOAT_TO_INT`.

- **Texture internal formats, the environment by base format, and proxy textures**
  (2026-09-19). `internalformat` was ignored: every texture was RGBA, so a `GL_ALPHA` texture
  under `GL_MODULATE` painted its black colour over the fragment's, a `GL_RGB` one under
  `GL_REPLACE` threw away the fragment's alpha, and `GL_INTENSITY` did not exist. Now:
  - **formats**: every base and sized GL 1.1 format, 1 to 4, and the GL 1.3 generic compressed
    formats (stored uncompressed), per Mesa's `_mesa_base_tex_format` (`main/glformats.c:2408`);
    anything else is `GL_INVALID_VALUE` (`main/teximage.c:1953`), and `glCopyTexImage*` refuses 1
    to 4 as `GL_INVALID_ENUM`. Texels are still RGBA8, each held as its base format expands it;
  - **queries**: `GL_TEXTURE_INTERNAL_FORMAT` answers the format as named (it answered `GL_RGBA`
    for everything), a generic compressed one as its base; `GL_TEXTURE_RED_SIZE` through
    `GL_TEXTURE_INTENSITY_SIZE` answer 8 or 0. `glGetTexImage` reports luminance and intensity in
    red alone (`main/texgetimage.c:289-301`). A mip chain of mixed formats is incomplete
    (`main/texobj.c:881`);
  - **the environment**: `gl_tex_env_apply` does Mesa's `calculate_derived_texenv`
    (`main/texstate.c:173`) for all five modes and six base formats. `GL_BLEND` was drawn as
    `GL_MODULATE`; it now mixes towards `GL_TEXTURE_ENV_COLOR`. A mode other than the five is
    refused - `GL_COMBINE` included - where it used to be stored and drawn as modulate;
  - **the hardware combine** is chosen per draw from the mode and the bound texture's base format,
    from `s_nop`, `v_mov_b32`, `v_mul_f32` and `v_add_f32` words assembled from the new
    `tools/shader/tex-env.s` (the three multiplies already in the shader came out identical). That
    brings `GL_ADD` and RGB `GL_DECAL` to the console. `GL_BLEND` and RGBA `GL_DECAL` do not fit
    four words and modulate there, with one log line;
  - **proxies**: `GL_PROXY_TEXTURE_1D`, `_2D`, `_3D` - sized and reported, never allocated, never
    compiled into a list (`main/dlist.c:4163`);
  - `glTexImage2D` refuses a non-zero border, as the 1D and 3D uploads already did.

- **Every pixel type and every `glPixelStorei` parameter** (2026-09-19). Images were read as
  `GL_UNSIGNED_BYTE` only, and `glPixelStorei` kept alignment, the unpack row length and the image
  heights while refusing the rest. Now:
  - **types**: `GL_BYTE`, `GL_SHORT`, `GL_INT` and their unsigned forms, `GL_FLOAT`, and the
    twelve GL 1.2 packed types (`GL_UNSIGNED_BYTE_3_3_2` to `GL_UNSIGNED_INT_2_10_10_10_REV`),
    both reading and writing. A packed type puts the format's first component in its most
    significant bits and `_REV` in its least, checked against Mesa's `formats.csv`; a packed
    type with a format it does not fit is `GL_INVALID_OPERATION`, per `glformats.c`;
  - **formats**: `GL_RED`, `GL_GREEN` and `GL_BLUE` join the rest;
  - **store**: the row, pixel and image skips, the pack row length and pack image height,
    `GL_*_SWAP_BYTES` and `GL_*_LSB_FIRST`, each queryable and saved by
    `GL_CLIENT_PIXEL_STORE_BIT`.

  A new `src/gl/gl_pixel.c` walks client memory for every image entry point - `glTexImage*`,
  `glTexSubImage*`, `glDrawPixels`, `glBitmap`, `glPolygonStipple`, `glReadPixels`,
  `glGetTexImage` - where each had its own loop before, so they now agree by construction. A
  display list takes the pixels the store selected when it compiles, packed tight in native byte
  order, and replays them under neutral store state. Signed components convert by the
  specification's `(2c + 1) / (2^b - 1)`; packing back, a tie goes toward zero as Mesa's
  `FLOAT_TO_SHORT` does (`main/macros.h:83`), so 0.0 reads back as 0 rather than -1. Textures are
  still stored RGBA8, so a float or 16-bit upload keeps eight bits a channel.

- **3D textures, in software** (2026-09-19) - `glTexImage3D`, `glTexSubImage3D` and
  `glCopyTexSubImage3D`, the last core GL 1.0-1.3 entry points: all 422 of Mesa's are declared,
  and the 33 missing are the optional imaging subset and one vendor extension.

  `GL_TEXTURE_3D` has its own binding, enable and default texture, and beats 2D and 1D when
  several are enabled; `GL_TEXTURE_WRAP_R`, `GL_TEXTURE_DEPTH`, `GL_TEXTURE_BINDING_3D` and
  `GL_MAX_3D_TEXTURE_SIZE` (256 - the volume lives on the CPU, and 256 on a side is 64 MB) are
  answered; `GL_UNPACK_IMAGE_HEIGHT` and `GL_PACK_IMAGE_HEIGHT` place a caller's slices. The
  upload, sub-upload and copy helpers take a depth and a z offset, so a volume is stored as its
  slices laid out as 2D images are; mip levels halve depth too, and completeness counts it.
  Compiled into lists with the volume packed at compile time (the recorder's argument limit went
  from 8 to 10 for `glTexSubImage3D`).

  The vertex now carries the texture coordinate's r, through generation and the texture matrix,
  and the software sampler filters in three dimensions - GL_LINEAR is eight texels, with r's rate
  of change in the level of detail. **The hardware path does not sample them yet**: that needs a
  3D descriptor, r carried to the pixel shader and a three-dimensional sample - the shader
  interface change fog and the polygon stipple wait on. Until then a 3D-textured draw on the
  console is drawn untextured and logs one line (`gl_log_line`, new, the library's way to the
  kernel log), rather than handing a 2D descriptor the first slice; gl1-probe's `texture-3d` is
  expected to fail there.

  Three tests had used `GL_TEXTURE_3D` or `GL_TEXTURE_WRAP_R` as their example of something
  absent; they use `GL_TEXTURE_CUBE_MAP` and `GL_TEXTURE_MIN_LOD` now.
- **The accumulation buffer** (2026-09-19) - `glAccum` with `GL_ACCUM`, `GL_LOAD`, `GL_RETURN`,
  `GL_MULT` and `GL_ADD`; `glClearAccum`; `glClear`'s `GL_ACCUM_BUFFER_BIT`;
  `GL_ACCUM_CLEAR_VALUE` and `GL_ACCUM_{RED,GREEN,BLUE,ALPHA}_BITS`; and `GL_ACCUM_BUFFER_BIT` on
  the attribute stack, the last GL 1.x group it could not save. 419 of Mesa's 455 GL 1.0-1.3
  entry points are declared; the three core ones left are the 3D-texture calls.

  Signed 16-bit channels on the CPU, as Mesa's `MESA_FORMAT_RGBA_SNORM16` accumulation buffer
  (`main/accum.c`), allocated the first time it is used - 16 MB at 1080p, which a program that
  never accumulates does not pay. Reads go through `glReadPixels`' flush and readback and the
  return is a CPU write like `glDrawPixels`', so **it works on the hardware path with no register
  or shader involved**. Every operation and the clear keep to the scissor box when the test is
  on; `GL_RETURN` goes through the colour mask and draws nothing in selection or feedback.
  Products round, and sums clamp to the buffer's range: Mesa truncates and lets its GLshort
  arithmetic wrap, so an overflowing `GL_ACCUM` came back as a large negative value.

  Alongside it, the buffer sizes are answered for the first time: `GL_RED_BITS` through
  `GL_ALPHA_BITS` (8) and `GL_DEPTH_BITS` (32, the float depth buffer both paths keep).
- **Line and polygon stipple** (2026-09-19) - `glLineStipple`, `glPolygonStipple` and
  `glGetPolygonStipple`, `GL_LINE_STIPPLE` and `GL_POLYGON_STIPPLE`, the pattern and repeat
  queries, and `GL_POLYGON_STIPPLE_BIT` on the attribute stack (with each enable in its own group
  too: the line's in `GL_LINE_BIT`, the polygon's in `GL_POLYGON_BIT`). 417 of Mesa's 455 GL
  1.0-1.3 entry points are declared; the 5 core ones left are 3D textures and the accumulation
  buffer.

  **A stippled line is cut into its dashes before it is widened**: GL's fragment count runs
  along the line's major axis, one per pixel, and keeps fragment s when bit (s / factor) mod 16
  of the pattern is set - so the segment is split at those pixel steps, each run of kept
  fragments becomes its own quad, and the colour, texture coordinate and normal at each cut are
  interpolated perspective-correctly. The count carries across a strip's segments and restarts
  for each separate line and each outlined polygon - the same places feedback reports
  `GL_LINE_RESET_TOKEN`. The dashes are ordinary quads, so the hardware draws them; gl1-probe's
  `line-stipple` should pass on a console.

  **The polygon stipple is software-only for now.** The rasteriser drops a filled polygon's
  fragment whose window position finds a 0 in the 32x32 mask - unpacked like a bitmap, bottom row
  first, row words as Mesa packs them (`main/pack.c`) - and leaves lines, points and outlines
  alone. The hardware path needs a discard in the pixel shader with the fragment's position,
  which is the shader-interface change fog waits on; `polygon-stipple` is expected to fail on the
  console until it lands, as `fog` is.
- **Pixel transfer and pixel maps** (2026-09-19) - `glPixelTransfer{f,i}`,
  `glPixelMap{fv,uiv,usv}` and `glGetPixelMap{fv,uiv,usv}`, with every transfer parameter and map
  size answered by `glGet`, `GL_MAX_PIXEL_MAP_TABLE` (256, Mesa's), and `GL_PIXEL_MODE_BIT` on the
  attribute stack - which also saves the pixel zoom. 414 of Mesa's 455 GL 1.0-1.3 entry points are
  declared; the 8 core ones left are the stipples, 3D textures and the accumulation buffer.

  Each colour component of a pixel rectangle is scaled and biased, then with `GL_MAP_COLOR`
  replaced from its `GL_PIXEL_MAP_x_TO_x` table, then clamped - Mesa's order
  (`main/pixeltransfer.c`) - on `glDrawPixels`, `glCopyPixels`, the texture uploads and
  `glReadPixels`. **A texture copy is transferred once**: it reads through `glReadPixels` and
  then uploads, and the upload would have applied it again. `glGetTexImage` is not transferred,
  as in Mesa. The colour-index and stencil maps, the index shift and offset, `GL_MAP_STENCIL` and
  the depth scale and bias are kept, compiled, saved and queried, and act on no pixels: the
  index, stencil and depth formats they apply to are refused everywhere here.

  Two places this follows the specification over Mesa (`main/pixel.c`): `GL_PIXEL_MAP_I_TO_I`
  must be a power of two in size like the other index-looked-up maps, which Mesa's range check
  skips; and `glGetPixelMapuiv`/`usv` read an index map back as the integers it was given, where
  Mesa normalises `I_TO_I` and copies `S_TO_S`'s float bits.
- **Selection and feedback** (2026-09-19), in a new `src/gl/gl_select.c` - `glRenderMode`,
  `glSelectBuffer`, `glFeedbackBuffer`, `glInitNames`, `glLoadName`, `glPushName`, `glPopName`
  and `glPassThrough`, with `GL_RENDER_MODE`, the name-stack and buffer queries and both buffer
  pointers; and **`gluPickMatrix`** in the GLU, derived from the viewport transform. `GL_SELECT`
  is how a GL 1.x program picks with the mouse, and feedback is how one exports vectors. 406 of
  Mesa's 455 GL 1.0-1.3 entry points are declared now; 16 core ones remain.

  Primitive assembly hands each polygon triangle, line and point to this instead of the
  rasteriser - **before** a line or point is widened into triangles, so the geometry reported
  is GL's own - and every way of drawing goes through that one point. The semantics are Mesa's
  (`main/feedback.c`, `state_tracker/st_cb_feedback.c`), and what is not obvious:
  - **It clips geometrically**, in clip space against the view volume and every enabled user
    plane - Sutherland-Hodgman for a polygon, entry and exit parameters for a line - carrying
    colour and texture coordinate along. The drawing path never needed to, since it rejects per
    fragment; a hit's depth range and a feedback polygon are defined after clipping.
  - A culled polygon is no hit; `glPolygonMode` decides whether a polygon is reported as a
    polygon, its sides or its corners; colours are lit and flat-shaded as drawing does them.
  - Nothing reaches the framebuffer, **`glClear` included** - a pick pass that begins with the
    program's usual clear would otherwise wipe the frame it picks in.
  - Hit depths are scaled to 2^32-1 in double. Mesa multiplies in float, where a depth of 1.0
    rounds to 2^32 and does not fit an unsigned int.
  - A refused `glRenderMode` changes nothing: `GL_SELECT` with no selection buffer is
    `GL_INVALID_OPERATION` and the mode stays. Mesa raises the error and switches anyway.
  - `GL_LINE_RESET_TOKEN` marks the first line of each independent line, strip, loop and
    outlined polygon, where GL resets the stipple.
  - Feedback texture coordinates are `(s/q, t/q, 0, 1)`: the vertex keeps its coordinate
    divided, which is the same point projectively and loses only `r`, which no texture here reads.
- **Evaluators** (2026-09-19), in a new `src/gl/gl_eval.c` - all 23 entry points: `glMap1{f,d}`,
  `glMap2{f,d}`, `glGetMap{f,d,i}v`, the eight `glEvalCoord*`, `glMapGrid{1,2}{f,d}`,
  `glEvalPoint{1,2}` and `glEvalMesh{1,2}`, with the eighteen `GL_MAP*` enables,
  `GL_AUTO_NORMAL`, `GL_MAX_EVAL_ORDER` (30, Mesa's), the grid queries and `GL_EVAL_BIT`. This is
  what `glutSolidTeapot` draws through, and the family was the largest core GL 1.x gap left
  (398 of Mesa's 455 GL 1.0-1.3 entry points are now declared, and 24 core ones remain).

  **CPU arithmetic ending in ordinary `glVertex` calls**: de Casteljau's construction, which also
  gives the derivatives `GL_AUTO_NORMAL` needs, feeding the immediate-mode path - so an
  evaluated patch is lit, textured, clipped, compiled into lists and sent to the hardware exactly
  as the same vertices typed out would be. What is GL's rather than obvious, each checked against
  Mesa (`main/eval.c`, `vbo/vbo_exec_eval.c`, `vbo/vbo_exec_api.c`, `main/draw.c`):
  - **Evaluation never changes the current colour, normal or texture coordinate.** Each
    evaluated value stands in for one vertex and the current one is put back.
  - The highest enabled texture-coordinate map wins, `_VERTEX_4` wins over `_VERTEX_3` and is
    projected, and with no vertex map enabled nothing is issued. Colour-index maps are stored and
    queried and evaluate to nothing in this RGBA context.
  - `GL_AUTO_NORMAL` takes the derivatives with respect to the domain values themselves, so a
    domain given backwards turns the normal round with the surface. **Mesa differs here**: it
    differentiates in the 0..1 parameter and so ignores a reversed domain.
  - `glEvalMesh2(GL_FILL)` is quad strips, as the specification writes it. Mesa draws triangle
    strips, which fill the same but outline each quad's diagonal under `glPolygonMode(GL_LINE)`.
  - Grid points are computed from their index, not accumulated, and the last one is the domain's
    end exactly - so adjacent patches agree on their shared edge to the bit.
  - A compiled `glMap` keeps its control points packed at compile time. One whose stride was too
    short fails when the list runs, as the call itself would have; Mesa's list replays it
    successfully.
  - `glEvalMesh` inside `glBegin` is `GL_INVALID_OPERATION`, refused before it could throw the
    open primitive's vertices away.
- **Mip chains on the hardware** (2026-09-19) - written from addrlib and Mesa, not yet seen on a
  console. A texture that is complete, a power of two, and filtered through its mipmaps is copied
  into GPU memory as a chain laid out **exactly as addrlib lays out a linear GFX10 surface**:
  smallest level first and the base level last - the opposite of the obvious order - each level
  `ceil(w / 2^i)` by `ceil(h / 2^i)` with its rows padded to 256 bytes
  (`amd/addrlib/src/gfx10/gfx10addrlib.cpp:5082-5104`, `GetMipSize` at `gfx10addrlib.h:367-383`).
  The descriptor points at the chain's start with `LAST_LEVEL` and `MAX_MIP` at its last level and
  no custom pitch, and the sampler's `MIP_FILTER` is POINT or LINEAR, as radeonsi fills them
  (`common/ac_descriptors.c:528-551`, `gallium/drivers/radeonsi/si_state.c:1950-1961`; field
  positions from `registers/gfx10-rsrc.json`). The chain is rebuilt when a level changes, and a
  texture that does not read mipmaps gets byte-for-byte the descriptors it always had.

  **Power of two only**, because GL halves a level by floor and addrlib by ceil, and the two
  agree only there; GL 1.x requires power-of-two textures, and one that is not keeps sampling its
  base level. gl1-probe's `mipmap-levels` now draws an 8x8 chain across 4x4 pixels and needs the
  second level's colour - the hardware's answer to whether the layout is right.
- **Mipmap levels, texture completeness, and a software sampler that follows the filters**
  (2026-09-19).

  **`level` was ignored by every texture upload** (`(void)level`), so a program uploading its mip
  chain one level at a time - as the Quake engines do - wrote each smaller image over the base
  one and ended with the texture as its own 1x1 level: one flat colour where the texture should
  be. Each level is now its own image (`mips` in the texture object), and `glTexSubImage*`,
  `glCopyTexImage*`, `glCopyTexSubImage*`, `glGetTexImage` and `glGetTexLevelParameter*` all
  address the level they are given; a level beyond the chain, or larger than its level can be,
  is `GL_INVALID_VALUE`.

  **Completeness**, by the specification's rule: a texture whose minification filter reads
  mipmaps needs every level down to 1x1 at exactly the halved size, and **an incomplete texture
  draws as if texturing were off**. That is what every GL does, and why a program that never
  sets a non-mipmap filter draws untextured everywhere; this used to sample it anyway. Two SDK
  tests relied on that and now set the filter, as a real program must.

  **The software sampler is the specification's**: nearest as `floor(s * width)` (it was
  `round(s * (width - 1))`, which is not GL's), bilinear with texel centres at half-integers,
  the magnification/minification switch at GL's `c`, and `*_MIPMAP_NEAREST` /
  `*_MIPMAP_LINEAR` from a per-pixel level of detail taken from the screen-space derivatives of
  the texture coordinate. The hardware still samples only the base level - its descriptor's
  LAST_LEVEL is 0, which is what it has always had - so a minified texture aliases on the
  console; putting the chain in GPU memory needs its layout measured first.

  gl1-probe has a `mipmap-levels` check for the part the console does see: the base image kept
  and an incomplete texture drawn untextured.
- **Colour-index state, multisample state, dithering and every hint** (2026-09-19). The ten
  `glIndex*` spellings, `glClearIndex`, `glIndexMask`, `glIndexPointer` with `GL_INDEX_ARRAY`,
  and `GL_INDEX_LOGIC_OP`; `glSampleCoverage` with `GL_MULTISAMPLE`,
  `GL_SAMPLE_ALPHA_TO_COVERAGE`, `GL_SAMPLE_ALPHA_TO_ONE` and `GL_SAMPLE_COVERAGE`; `GL_DITHER`;
  and the point-smooth, line-smooth, polygon-smooth, fog and texture-compression hints.

  **All of it is state that changes no pixel, and that is correct rather than partial.** This
  is an RGBA context - `GL_INDEX_MODE` answers false - and in one the specification keeps
  colour-index state and draws nothing with it; with no multisample buffer (`GL_SAMPLE_BUFFERS`
  0) it gives the multisample state no effect; a hint is a preference an implementation may
  ignore; and the colour block rounds rather than dithers, which is also what Mesa's hardware
  drivers make of `GL_DITHER`. What mattered was that these were **refused**: `GL_DITHER` and
  `GL_MULTISAMPLE` are on by default in every GL context, and the `glDisable(GL_DITHER)` and
  `glHint(GL_FOG_HINT, ...)` a great deal of 1.x start-up code carries were errors here. The
  index is a plain number, not an intensity, so every spelling converts by cast (Mesa
  `vbo/vbo_attrib_tmp.h:1978-1990`, `:2705-2761`); the defaults are Mesa's.

  With them: **`glPushAttrib` accepts `GL_POINT_BIT`, `GL_LINE_BIT`, `GL_HINT_BIT` and
  `GL_MULTISAMPLE_BIT`** - line width, point size and the hints existed and could not be saved,
  so a `glPushAttrib(GL_LINE_BIT)` around a wireframe overlay was refused and its pop took a
  frame it never pushed. **`GL_ALL_ATTRIB_BITS` is the Khronos `0xFFFFFFFF`**; this header had
  GL 1.0's `0x000FFFFF`, and a program built against a standard header was refused outright.
  Both are accepted as "all".

  And **point size and line width are queryable**, with their ranges: `GL_POINT_SIZE`,
  `GL_LINE_WIDTH`, the four `*_RANGE`s and the granularities were declared and answered by
  nothing. They are drawn by the specification's aliased rule now - rounded, at least 1, at most
  256 - where the raw float used to be the quad's width, so `glLineWidth(0.5)` could draw nothing.
- **`glPolygonMode` and edge flags** (2026-09-19): `glPolygonMode`, `glEdgeFlag`, `glEdgeFlagv`,
  `glEdgeFlagPointer` with `GL_EDGE_FLAG_ARRAY`, `GL_POLYGON_OFFSET_LINE` and
  `GL_POLYGON_OFFSET_POINT`, the queries, and both attribute stacks. Wireframe and point-cloud
  views of a mesh are the everyday use.

  **An outline is the primitive's own edges.** A quad reaches the triangle stage as two triangles
  and a polygon as a fan, and drawing each triangle's three edges would draw the diagonals
  between them - a quad with five sides. Each triangle therefore carries which of its edges
  belong to the primitive, and edge flags (which the specification applies to separate
  triangles, quads and polygons only) choose among those. In point mode the vertices that start
  a boundary edge are drawn, which puts each corner down exactly once however the primitive was
  split. The lines and points go through the same screen-space expansion `GL_LINES` and
  `GL_POINTS` use, so nothing new is asked of the hardware. **Culling is decided for the polygon
  before its mode**, as the specification orders it - a culled back face draws no outline.

  To carry all of that, **primitive assembly is now one function** (`gl_assemble`), given a
  fetcher per caller. `glEnd`, `glDrawArrays` and `glDrawElements` each had their own copy of
  the switch - the refusal test's comment records the time they disagreed about which modes drew.

  Three things underneath it were wrong before it existed, and are fixed with it:
  - **Culling applied to lines and points.** The quad a line becomes winds like any triangle, so
    with `GL_CULL_FACE` on, every line whose quad happened to wind backwards vanished - in
    software and, through `PA_SU_SC_MODE_CNTL`'s cull bits, on the hardware.
  - **`GL_POLYGON_OFFSET_FILL` applied to lines and points** the same way; each offset enable now
    covers only its own kind.
  - **A flat-shaded quad came out in two colours, and a polygon in the wrong one.** The flat
    colour was each triangle's last vertex, which is right for triangles, strips and fans; a quad
    takes its fourth vertex (its first triangle does not contain it) and a polygon its first. The
    specification's table of provoking vertices is followed exactly now, lighting included.

  `glIsEnabled` now answers the client arrays (`GL_VERTEX_ARRAY` and the rest), and
  `glGetIntegerv` each array's size, type, stride and buffer binding - all of which were refused
  as unknown. `glInterleavedArrays` disables the edge-flag array, as Mesa's does
  (`main/varray.c:2960`). gl1-probe has a `polygon-mode` check for the console.
- **Display lists record every call the specification compiles** (2026-09-19). They recorded 21
  operations - the vertex attributes, the enables, the matrix-stack calls and a handful of others
  - and every other call **ran at compile time** under `GL_COMPILE`. So a `glMaterialfv` inside a
  list changed the material while the list was being built and was absent when it was called: a
  program giving each object its own material in its own list drew every object in the material
  set last. That is the gears demo, and the shape of a great deal of GL 1.x code. `glLoadMatrix`,
  `glMultMatrix` and `glBlendFunc` even had replay arms that nothing ever recorded.

  Now about 75 entry points record - lighting and materials, blend and logic op, depth, stencil,
  fog, the texture environment and generation, texture parameters and uploads, clip planes,
  viewport and scissor, clears, `glOrtho`/`glFrustum`, the attribute stack, raster position,
  `glBitmap`, `glDrawPixels`, `glCopyPixels`, `glListBase`, `glCallLists` - at the one place each
  family's spellings converge. Only what the specification says executes immediately does:
  queries, gen/delete, the client-side state, `glPixelStore`, `glReadPixels`, buffer objects.
  What a list keeps follows the specification and Mesa's `main/dlist.c`:

  - **Pointer arguments are copied when compiled** - a matrix, a light's position, a list of
    names - and a `*v` call copies exactly as many values as its `pname` takes, so a
    `glMaterialfv(GL_SHININESS, &one)` never reads past the one float it was given.
  - **Images are unpacked through the compile-time `glPixelStorei` state** into tight rows and
    replayed under default unpacking, so a list does not depend on what the pixel-store state has
    become by the time it runs. `glBitmap` likewise - bitmap fonts in lists are most of what it
    is used for.
  - **The vertex-array draws compile**, as the `glBegin`/element/`glEnd` sequence they are
    defined to be, with the arrays read now. They were refused with `GL_INVALID_OPERATION`.
  - **`glCallLists` applies the list base current when it runs.** It used to be compiled as
    individual `glCallList(base + name)` calls with the base of compile time.
  - **`GL_COMPILE_AND_EXECUTE` records each call once.** The recorder appends the command and
    then executes *that command* with recording suspended - so a recorded entry point that calls
    another in its body (`glCopyTexImage2D` does) records once, and a list called while another
    is compiled this way is not copied into it. Both recorded twice before.
  - **A compiled call's error is raised when it runs**, as the specification has it, because the
    replay goes through the same public entry points.

  The storage changed with it: each list's commands and data now grow from the SDK heap, as a
  texture's image or a buffer object's store does, instead of a fixed 4096 commands per list. The
  fixed arrays cost 32 MiB of context whether or not a list was ever made, capped one list below
  1400 triangles, and could not hold an image at all. The GL and PM4 suites run clean under
  AddressSanitizer and UndefinedBehaviorSanitizer with leak checking.
- **`glLogicOp` and `glBlendColor`** (2026-09-19), with `GL_COLOR_LOGIC_OP`, `GL_LOGIC_OP_MODE`,
  `GL_BLEND_COLOR`, the four constant-colour blend factors, and both on the attribute stack under
  `GL_COLOR_BUFFER_BIT` (the enable under `GL_ENABLE_BIT` too). Both paths are written.

  **The logic op is radeonsi's recipe** (`gallium/drivers/radeonsi/si_state.c:341`, `:365-368`,
  `:545-549`): `CB_COLOR_CONTROL`'s eight-bit `ROP3` holds the four-bit mode twice, `GL_COPY`
  counts as off, and `DISABLE_DUAL_QUAD` is set with it on every RB+ part - which is every
  GFX10_3 one (`amd/common/ac_gpu_info.c:1117`, `:1122-1125`). Off, the register is exactly the
  `0x00cc0010` the frame table always carried, so gl-cube's stream does not change.

  **The mode is not `opcode - GL_CLEAR`.** GL's enum is a truth table with its bits in the other
  order - `GL_COPY` is `0x1503`, the table value 12 - so the mapping is Mesa's
  `color_logicop_mapping` (`main/blend.c:835-852`) rather than arithmetic. `GL_XOR` is 6 either
  way, which is why the tests use `GL_AND_REVERSE` and `GL_INVERT`, where the two readings differ.
  A logic op replaces blending rather than following it, as Mesa's state tracker has it
  (`state_tracker/st_atom_blend.c:269-273`). `GL_INDEX_LOGIC_OP` stays refused with the rest of
  colour-index mode.

  **The blend constant is not in the frame's register table.** `CB_BLEND_RED..ALPHA` (context
  offset `0x105`, `gfx103.json`) go out at the first draw of a frame that reads a constant factor
  and again after each `glBlendColor` - once per value, not per draw - so a program that never
  uses one emits exactly the stream it did before. `glBlendColor` clamps to [0, 1] as Mesa does
  (`main/blend.c:788-791`), and `BLEND_CONSTANT_COLOR` and friends are 13, 14, 19 and 20 in
  `BlendOp`, not contiguous with the rest.

  gl1-probe has `logic-op` and `blend-constant` checks for the console.
- **Fog, in software** (2026-09-19): `glFogf`, `glFogi`, `glFogfv`, `glFogiv`, `GL_FOG` on the
  enables, the fog queries, and `GL_FOG_BIT` on the attribute stack - which was refused until now,
  on the correct grounds that there was no fog state to save. The attribute test's own rule
  applied: *the list of what cannot be saved has to shrink as features land.*

  The three modes are the specification's - `GL_LINEAR` between a start and an end, `GL_EXP` and
  `GL_EXP2` by density - from the **eye-space distance** to the fragment. Not the window depth,
  which would make fog change with `glDepthRange`, and not the object-space distance, which would
  ignore the modelview. The factor is computed per vertex and **interpolated per fragment**; folding
  fog into the vertex colour instead is exact only when the factor is constant across a primitive.

  **Fog sits after texturing and before the alpha test, and it does not touch alpha.** Fogging alpha
  too would make a fully fogged fragment fail an alpha test it passes unfogged, so turning fog on
  would silently change what a cut-out texture keeps. The test checks exactly that.

  `gl_exp` joins `gl_sqrt` and `gl_pow` in `gl_matrix.c` rather than calling `oops_expf`: the host
  self-tests of gl1-probe and gl1-cube compile the GL sources without `math.c`, and reaching into it
  would break two builds with nothing wrong with them. `GL_FOG_INDEX` is refused - it belongs to
  colour-index mode. An integer fog colour normalises; the integer distances are plain casts.

  **The hardware path is not written yet**, and gl1-probe has a `fog` check that is expected to fail
  on the console until it is. The likely route does not need a third parameter export: the vertex
  buffer's texture coordinate is a `vec4` whose `z` is unused and which the vertex shader already
  exports whole, so a per-vertex fog factor computed on the CPU can ride there and only the pixel
  shaders change. (What `-7c40`'s sweep does show about a third export - that it draws, not that
  its values arrive intact - is in `docs/GL_ROADMAP.md`, with the log's real arm names.)
- **Points, lines, polygons and quad strips draw** (2026-09-17): `GL_POINTS`, `GL_LINES`,
  `GL_LINE_STRIP`, `GL_LINE_LOOP`, `GL_POLYGON` and `GL_QUAD_STRIP`, plus `glPointSize` and
  `glLineWidth`. All six were refused with `GL_INVALID_ENUM` until now.

  **The measurement that shut them was read one step too far.** obSCEne submitted a one-vertex
  point and a two-vertex line on retail hardware across five sweeps - one on oops-gl's own
  `VGT_SHADER_STAGES_EN` - and every run recorded `fence-hit 0`: the pipe stops. That is a fact
  about the *native* primitive. It is not a fact about drawing a line, because a line of a given
  width is a quad and a point is a square, and this file already turns `GL_QUADS` into triangles -
  which the same sweeps showed retiring and drawing. The conclusion in the roadmap, "they need a
  stage this does not build", followed only if you insisted on native primitives.

  **The width is in pixels, so the expansion happens after projection.** Each endpoint goes to
  normalised device coordinates, the perpendicular is taken *in pixels* through the viewport scale
  - taking it in NDC gives a line that is the wrong width on any viewport that is not square - and
  the corners come back to object space through the inverse of the combined matrix, because the
  pipeline applies that matrix again. `mat4_invert` was already there for the clip planes.

  Expanded corners keep their endpoint's depth and `w`, so a wide line does not sink through what
  it crosses. A vertex behind the eye is dropped rather than expanded, because clipping a line
  properly means splitting it at the near plane and that clipper does not exist yet.

  **All three primitive-assembly switches had to agree** - immediate mode, `glDrawArrays` and
  `glDrawElements`. A mode accepted by `glBegin` and unhandled by the array path draws nothing and
  raises nothing, which is the silent-success failure this port refuses. gl1-probe gains a
  `points-and-lines` check, which is where the narrower reading of the measurement gets tested on
  the console rather than argued about.
- **Compressed textures: seven entry points, no formats** (2026-09-17). The specification allows
  the set of compressed formats to be empty, and this one is:
  `GL_NUM_COMPRESSED_TEXTURE_FORMATS` reports 0, `GL_COMPRESSED_TEXTURE_FORMATS` writes nothing,
  every `glCompressedTexImage*` and `glCompressedTexSubImage*` is `GL_INVALID_ENUM`, and
  `glGetCompressedTexImage` is `GL_INVALID_OPERATION` because the texture is not compressed -
  a different answer from the format refusals, and the right one.

  **These are conformant, not stubs.** A program using compressed textures queries the count
  first, and 0 is what sends it down its uncompressed path. One that does not query gets an error
  it can read. The alternative - leaving the entry points out under "an absent feature is an
  absent symbol" (D009) - is not neutral here: the payload links with
  `-Wl,--unresolved-symbols=ignore-all`, so an absent entry point is a call to address zero with
  no diagnostic. **That tension is worth deciding deliberately for the features still missing**
  (3D textures, fog), and it is recorded here rather than settled unilaterally.
- **One-dimensional textures** (2026-09-17): `glTexImage1D`, `glTexSubImage1D`,
  `glCopyTexImage1D`, `glCopyTexSubImage1D`, `GL_TEXTURE_1D` on the enables and on
  `glBindTexture`/`glTexParameter`/`glGetTexImage`, and `GL_TEXTURE_BINDING_1D`.

  **A 1D texture is stored as a height-1 2D one - what is *not* shared is the binding.**
  `GL_TEXTURE_1D` has its own binding point, its own enable and its own default texture; both
  bindings are live at once and **2D wins when both are enabled**. An implementation that treated
  1D as a shape of 2D would hand a program its 2D texture where it asked for its 1D one, and would
  pass any test that only ever uses one of them - so the test keeps both bound with different
  contents and checks which a draw samples.

  An object belongs to the target it was first bound to; rebinding it elsewhere is
  `GL_INVALID_OPERATION`, not a silent reinterpretation. A non-zero `border` is refused rather
  than ignored.

  **This fixed a pre-existing bug on the way.** The default 2D texture - the object a `glTex*`
  call acts on when nothing is bound - was object **1**, and `glGenTextures` counts up from 1. A
  program that generated a texture and then uploaded with nothing bound wrote into its own. Each
  target now has a reserved default id above anything counting will reach, which also stops a 1D
  upload from overwriting the default 2D image.

  The 2D upload, sub-upload and framebuffer-copy bodies were already target-agnostic and are now
  shared helpers that take the target; only the public `*2D` entry points validate, and they still
  refuse `GL_TEXTURE_1D`.
- **Raster position, glDrawPixels, glBitmap and glCopyPixels** (2026-09-17): 28 entry points in a
  new `src/gl/gl_raster.c`. The raster position is a vertex that is never drawn - it goes through
  the whole transform and what comes out is kept in window coordinates, with the colour and
  texture coordinate latched alongside it.

  **Three behaviours here are invisible in a "did it draw something" check and each breaks a real
  program:**

  - **An invalid raster position draws nothing.** If the point clips, every later `glDrawPixels`
    and `glBitmap` is a no-op until a valid one is set. The tempting alternative - clamp to the
    nearest edge and draw anyway - puts an image where the program never asked for it; a program
    scrolling text off the side would get it piled against the edge instead of disappearing.
  - **`glBitmap` moves the raster position**, whatever it drew, including for a null or empty
    bitmap. That is what lays out a string, and a null one is how a space is drawn. Leaving it
    out draws every glyph on top of the first.
  - **The colour is latched at `glRasterPos`, not read at draw time.** Changing `glColor`
    afterwards must not change what a later `glBitmap` draws.

  Bitmap rows are most-significant-bit first and padded to the unpack alignment, which needed the
  row-stride helper split so a row already measured in bytes can use it - one bit per pixel does
  not divide into a pixel size.

  These write the colour buffer from the CPU, so they flush a built frame first, the same rule
  texture storage and the shader payload follow. The first version transformed through a stale
  combined matrix: `gl_update_mvp` is lazy and only the draw path had been asking for it.

  **A new source file has to be registered in each app that lists GL sources, not just in the
  SDK.** `gl1-probe` and `gl1-cube` name the `src/gl/*.c` files they link one by one, and the
  host link caught the omission - but the *target* link would not have. It carries
  `-Wl,--unresolved-symbols=ignore-all`, so on the console `glDrawPixels` would have been a call
  to address zero: the same silent crash the type-and-arity grid was filled to prevent, arriving
  by a different route.
- **Stencil, in software** (2026-09-17): `glStencilFunc`, `glStencilOp`, `glStencilMask`,
  `glClearStencil`, `GL_STENCIL_TEST`, `glClear(GL_STENCIL_BUFFER_BIT)`, the eight `glGetIntegerv`
  queries, and `GL_STENCIL_BUFFER_BIT` on the attribute stack - which until now was **refused**,
  on the correct grounds that there was no stencil state to save.

  **Two orderings that go wrong silently, and each has its own assertion:**

  - The buffer is written **even when the stencil test fails**. That is the whole point of the
    feature - `GL_INCR` or `GL_REPLACE` on the fail path is how a mask gets built in the first
    place - and skipping it would make every shadow-volume and outline technique quietly do
    nothing.
  - **The alpha test comes first.** A fragment alpha discards must not reach the stencil buffer at
    all, not even the fail operation. That forced the depth result to be carried past the alpha
    test rather than acted on where it is computed; with stencil off the old fast path is
    untouched.

  The write mask applies to the **result**, not the operand: `GL_INVERT` under a mask of `0x0f`
  inverts eight bits and writes back four. It does not gate `glClear`, which GL says ignores it.
  `GL_INCR`/`GL_DECR` saturate; GL 1.4's wrapping forms are refused rather than aliased onto them.

  **The roadmap was wrong about what this cost**, and is corrected: it said stencil needed a
  different depth format and a reallocated buffer because `Z_32_FLOAT` has no stencil plane, with
  `DB_Z_INFO` pinned by the oracle test. On GFX10.3 stencil is a *separate surface* with its own
  registers, and the depth block already writes every one of them, zeroed. `DB_Z_INFO` does not
  move. What is left is a surface and the one `DB_STENCIL_INFO` value that turns it on - obSCEne
  `REQ-20260917T1845Z-3d5b`, filed rather than guessed.
- **User clip planes** (2026-09-17): `glClipPlane`, `glGetClipPlane`, `GL_CLIP_PLANE0..5` on the
  enables, `GL_MAX_CLIP_PLANES` reporting 6, and the state saved under `GL_TRANSFORM_BIT`. Six is
  both the specification's minimum and exactly what the hardware has - `PA_CL_CLIP_CNTL` carries
  `UCP_ENA_0..5` and there is no seventh bit.

  Two registers and no shader change, because the fixed-function clipper applies whenever the
  vertex shader exports no clip distances, which these hand-written shaders do not:
  `PA_CL_UCP_0_X` at context offset `0x16F` (24 consecutive registers, six planes of four floats)
  and `PA_CL_CLIP_CNTL` at `0x204`, both verified against
  `mesa/src/amd/registers/gfx103.json`.

  **Three spaces, and each one matters.** The plane arrives in object coordinates; it is stored in
  eye coordinates through the inverse modelview of the moment `glClipPlane` was called, which is
  what makes it stay put while the modelview moves afterwards; and the register takes it in *clip*
  space - "Clip-Space Plane = Eye-Space Plane * Projection Matrix", `mesa/src/mesa/main/clip.c:40-51`.
  Mesa passes the eye-space form only when a vertex shader writes a clip vertex
  (`st_atom_clip.c:52-55`). Skipping the projection transform passes an identity-projection test
  and fails everywhere else, so the test checks both.

  The software rasteriser interpolates a signed distance per plane and discards per fragment,
  ahead of the depth test - a clipped fragment must leave the depth buffer alone, or it hides
  geometry that is genuinely visible. That is cheaper and simpler than a geometric clipper that
  turns one triangle into several and has to re-derive colours, texture coordinates and winding
  for each piece, and the visible result is the same.

  gl1-probe gains a `clip-plane` check (35 now). obSCEne has measured clip planes three times and
  every run reconstructed the stage by hand with the *passthrough* `VGT_SHADER_STAGES_EN`
  (`0x02002000`) rather than the `0x00c12010` oops-gl programmes; the probe draws through the real
  path, so the next hardware run answers the question those three could not.
- **Texture coordinate generation** (2026-09-17): `glTexGen{i,f,d}{,v}` and `glGetTexGen{i,f,d}v`,
  with `GL_TEXTURE_GEN_S/T/R/Q` on the enables and the state saved under `GL_TEXTURE_BIT`.
  `GL_OBJECT_LINEAR`, `GL_EYE_LINEAR` and `GL_SPHERE_MAP` generate; the GL 1.3 cube-map modes
  `GL_NORMAL_MAP` and `GL_REFLECTION_MAP` are **refused**, because there is no cube map behind
  them and a program handed sphere mapping when it asked for reflection mapping draws a wrong
  picture with `GL_NO_ERROR` throughout.

  Generation happens at vertex assembly, where the object coordinates and the object-space normal
  are both in hand, and it applies to the vertex-array path as well - it is a property of the
  coordinate, not of how the vertex arrived, so it overrides a texture-coordinate array exactly
  as it overrides `glTexCoord`.

  **`GL_EYE_PLANE` is stored multiplied by the inverse modelview of the moment it was set**, which
  is the whole difference between eye-linear and object-linear: the plane stays where it was put
  while the modelview moves afterwards. That needed a full 4x4 inverse (`mat4_invert`) - the
  existing normal matrix is only the inverse-transpose of the upper 3x3 and cannot carry the
  translation a plane equation needs. A singular matrix is refused rather than filled with
  infinities. The test moves the modelview between specifying the plane and drawing, because an
  implementation that just stores the caller's numbers behaves identically to object-linear and
  passes any test that does not.

  Only `s` and `t` reach the sampler: the texture unit is 2D, so generated `r` and `q` are visible
  to `glGetTexGen` and go no further. Same limit `glTexCoord4` has.
- **The type-and-arity grid, and multitexture on one unit** (2026-09-17): 128 entry points, taking
  the GL 1.0-1.3 surface from 161 of Mesa's 455 to 289. Sixty are the rest of the
  `glVertex`/`glColor`/`glTexCoord`/`glNormal` grid; sixty-eight are `glMultiTexCoord*`,
  `glActiveTexture`, `glClientActiveTexture` and their `*ARB` spellings.

  **These are not cosmetic.** The payload links with `-Wl,--unresolved-symbols=ignore-all`, so a
  program calling a spelling that does not exist links cleanly and calls **address zero** at run
  time - a `SIGSEGV` at `rip: 0x0000000000000000`, with no build diagnostic and nothing in the log
  naming the symbol. An absent spelling was not a missing convenience, it was a crash with the
  evidence removed.

  Two behaviours in here are worth more than the count:

  - **Integer colours and normals are normalised; positions and texture coordinates are not.**
    `glVertex3i(1,2,3)` is the point `(1,2,3)`, but `glColor3i(1,2,3)` is indistinguishable from
    black. The conversions are Mesa's (`macros.h:49-104`), and normals use the *same* macros as
    colours (`vbo/vbo_attrib_tmp.h:2770-2795`), not a plain cast. A consequence that looks like a
    bug: the signed mapping is `(2c+1)/(2^b - 1)`, so **zero does not map to zero** -
    `glNormal3b(127,0,0)` is `(1.0, 1/255, 1/255)`. That is what puts both `-128` and `127`
    exactly on `-1` and `1`, and it is what Mesa produces.
  - **`q` is a projective divide, not a fourth coordinate to ignore.** `glTexCoord4f` now divides,
    which needed the current texture coordinate widening from two components to four. What it does
    not do is carry `q` through the rasteriser and divide per fragment, so a primitive whose
    vertices carry different `q` interpolates already-divided coordinates - exact wherever `q` is
    constant, an affine approximation where it is not.

  **Multitexture reports one unit and means it**: `GL_MAX_TEXTURE_UNITS` is 1 and every call naming
  a unit above `GL_TEXTURE0` is refused with `GL_INVALID_ENUM`, which is what the specification
  requires of a one-unit implementation. Accepting `GL_TEXTURE1` and quietly applying unit 0's
  coordinate would draw a plausible, wrong picture under `GL_NO_ERROR` throughout. A second unit
  needs a third parameter export, which is an unmeasured hardware fact - obSCEne
  `REQ-20260917T1652Z-7c40` - and when it arrives the range check is the only thing that moves.
- **Instruction selection for the GL 2.0 back end** (2026-09-17): `src/gl/glsl_gen.c`, the stage
  between the semantic checker and the encoder. A value occupies **consecutive VGPRs, one per
  component** - a `float` is one, a `vec4` four, a `mat4` sixteen column-major, which is how
  `glsl_emit_mat4_mul_vec4` reads one - with a bump allocator that marks and rolls back per
  statement, and exhaustion reported rather than wrapped. Generates `+ - *` component-wise,
  scalar-with-vector either way round (the two are the same meaning and **not** the same
  encoding, since VOP2 has one biased source and one bare one), `mat4 * vec4`, unary minus,
  swizzle reads in all three vocabularies, `vecN`/`matN` constructors, assignment, declarations
  with initialisers, and blocks.

  **`matN(s)` fills the diagonal where `vecN(s)` fills everything**, which is the rule most worth
  getting right here: a `mat4` of ones transforms every vertex to the same point, so the mistake
  is a black screen on hardware and nothing at all on the host. It has its own assertion.

  Everything else sets `error` and emits nothing - `/`, integer and bool arithmetic, comparisons,
  function calls, matrix arithmetic beyond `mat4 * vec4`, writing through a swizzle, and the
  compound assignments. Each of those parses and type-checks, so a generator that emitted
  *something* would produce a shader that runs and computes the wrong thing with nothing on the
  host to show for it. Division is refused rather than emitted as reciprocal-then-multiply, whose
  precision nobody here has measured (D009).
- **`v_sub_f32` and the inline zero in the GLSL encoder** (2026-09-17): opcode 4, read back from
  `clang`'s own output for `v_sub_f32 v4, v8, v9` = `0x08081308`, with `tools/shader/gl2-transform.s`
  extended to carry it. Opcode 5 is `v_subrev_f32` with the operands the other way round, so an
  off-by-one computes the negation of what was asked - which is why the test asserts the exact
  word. `glsl_emit_neg_f32` is zero minus the operand with the zero inline (`128`, from
  `v_mov_b32 v15, 0` = `0x7e1e0280`), so unary minus costs one instruction and no register, and
  `glsl_emit_mov_imm` now knows that constant too.
- **`oops_display_try_gpu_tiler(disp)`** (2026-09-17): the public opt-in for the AGC backend's
  compute tiler, which has existed since that backend was written and which **no app had ever
  been able to reach** - `agc_display_try_gpu_tiler()` was declared in the internal header and
  called by nothing, and there was no `oops_display_*` wrapper, so `gpu_accelerated` was 0 in
  every app on every frame and `oops_display_is_gpu_accelerated()` could only ever answer 0.
  Every flip has therefore been converting the linear render target into the display-tiled
  scanout surface on the CPU: a full read of write-combined video memory and a scattered write
  back, 2,073,600 words each way at 1920x1080, neither side cached. The opt-in dispatches the
  shader once, compares it against the CPU tiler byte for byte, and only then lets flips use it;
  a mismatch, a queue that will not create, or a dispatch that stops retiring all fall back
  rather than present a wrong buffer. It must be called before the first flip and refuses
  afterwards. gl1-cube reaches it behind an `/app0/gputile` control file, so what the CPU tiler
  costs can be measured as the difference between two runs before anything changes by default.
- **`glGetFrameReadbackSampled(line_stride)` in oops-gl** (2026-09-17): the frame readback with
  only every `line_stride`-th cache line invalidated. The existing `glGetFrameReadback()` flushes
  every line, which on a 1920x1080 target is 129,600 `clflush` per call, and gl1-cube was paying
  that on every frame of every run to feed a HUD field that samples one word in 64. The full form
  is now that call with a stride of 1 and is unchanged for a caller that reads the whole frame.
  **A caller that reads a line it did not ask to be invalidated gets whatever the CPU had cached**
  - for a still frame, the previous frame's pixels - and the header says so.
- **`glSetHardwarePrelude(words, count)` in oops-gl** (2026-09-14): words every later frame's
  command stream opens with, ahead of oops-gl's own state. An experiment hook, unvalidated on
  purpose, so another driver's preamble can be put in front of this one and measured; oops-mesa
  used it for its route measurement (oops-mesa#D003, worklog 002).
- **JIT and Dynamic Executable Memory Subsystem (`oops/jit.h`).** Clean-room APIs
  (`oops_jit_alloc`, `oops_jit_free`, `oops_jit_flush_icache`, `oops_jit_is_available`)
  providing progressive fallback across Sony shared memory dual-mapping (`rx_addr` / `rw_addr`),
  relaxed W^X `mprotect` execution under `kstuff-lite`, and host testing environments. (D006)
- **A repository for what target-side payloads share.** Display, input, audio, direct
  memory, system, time, threads and sockets behind one set of headers, built as a static
  archive a payload links. obSCEne is the first consumer.
- **Two display backends and a host one.** `agc` for Prospero-generation hardware, `gnm` for
  Orbis-generation, and an in-memory buffer so the interface can be exercised with no
  hardware in the room.
- **`oops-sdk.mk`**, so a consumer says where this repository is and gets the include flags,
  the archive path and the sources from one include.

### Added

- **oops-gl is growing to OpenGL 1.x and 2.x** (2026-09-17, D008), superseding D007's
  instrument-only scope at the operator's direction. What survives from D007: the refusal
  discipline (a call whose behaviour is unknown is refused, not approximated),
  `glIsHardwareAccelerated` staying measured, and oops-mesa still being upstream's GL at 3.3.
  What changes: a missing entry point is a gap to fill rather than a boundary to point at.
  **1.x first**, because 2.0 means GLSL and a compiler, which is the largest single piece of
  work here and the reason D007 pointed at Mesa.
- **The GL 2.0 back end started** (2026-09-17): `src/gl/glsl_emit.c`, RDNA2 instruction
  encoding. The first piece of GL 2.0 that produces machine code rather than a tree.
  - **Every field position was read out of a real assembler**, never written from memory.
    `tools/shader/gl2-transform.s` holds the source; the tests assert the encoder reproduces the
    exact words `clang -target amdgcn-amd-amdhsa -mcpu=gfx1030` produced from it. This matters
    more than usual because **a wrong instruction encoding cannot fail loudly** - it assembles
    into the payload, the hardware decodes it as something else, and the frame is wrong rather
    than the build stopping. Asserting against words the encoder itself produced would prove
    only self-consistency.
  - The independent cross-check is `s_endpgm` = `0xbf810000`: clang produces it, and it is the
    word every hand-written shader already in this repository ends with.
  - **A VGPR source operand is biased by 256.** Encoding the bare number names an SGPR instead,
    which runs and computes rubbish rather than faulting.
  - `glsl_emit_mat4_mul_vec4` is **column-major**, because GL lays a matrix out as four columns
    end to end. Treating the block as rows transposes it, and a transposed model-view matrix
    still draws a cube - just the wrong way round, which is a bug that survives a screenshot.
    The test decodes the register numbers back out of the emitted words rather than trusting
    the shape.
  - Buffer overflow is recorded, never wrapped: a truncated shader is a valid instruction
    stream that stops in the middle, which the GPU will happily execute.
- **`glBlendEquation` was stored and used by nothing** (2026-09-17). It set a field the
  attribute stack saved, `glGetIntegerv` reported and `glContextCreate` defaulted - and neither
  the rasteriser nor the hardware register emission ever read it.
  `glBlendEquation(GL_FUNC_SUBTRACT)` returned clean and added.
  - **A worse shape than the three before it**: the alpha test, depth range and polygon offset
    at least worked on the hardware path. This worked nowhere.
  - Implemented for `GL_FUNC_ADD`, `GL_FUNC_SUBTRACT`, `GL_FUNC_REVERSE_SUBTRACT`, `GL_MIN` and
    `GL_MAX`. **`GL_MIN` and `GL_MAX` ignore the blend factors entirely**, which is what an
    implementation treating them as another sign gets wrong.
- **`GL_LIGHT_MODEL_TWO_SIDE` is refused rather than accepted** (2026-09-17). It set a field
  nothing read, so the call returned clean and two-sided lighting never happened. Doing it
  properly needs a primitive's *facing*, which is not known where lighting is computed per
  vertex - so it is refused, and the dead field is gone.
  - A unit test asserted this call returned `GL_NO_ERROR`, which **encoded the bug as a
    behaviour**. Updated; the assertion now expects the refusal.
- **`glDepthRange` and `glPolygonOffset` now exist on the software rasteriser too**
  (2026-09-17), found by widening `gl1-probe` from 15 checks to 25.
  - Both were **hardware-path only**. `glDepthRange` wrote `PA_CL_VPORT_ZSCALE`/`ZOFFSET` while
    the rasteriser hardcoded the 0..1 mapping; `glPolygonOffset` wrote `PA_SU_POLY_OFFSET_*`
    while the rasteriser knew nothing of it. Either way the same program drew one picture on the
    console and a different one on the host, with no error anywhere.
  - That is now **three** of these found by the same method - the alpha test was the first. The
    pattern is a feature implemented where the registers are and nowhere else, and running one
    suite against both paths is the only thing that sees it.
  - The polygon offset is computed **per triangle, not per fragment**: the specification's
    `factor * m + units * r` has both terms constant across a primitive, which is what makes the
    offset a plane shift rather than a warp. `m` is the larger of `|dz/dx|` and `|dz/dy|` from
    the plane through the three screen vertices, so it sits after the signed area that is its
    denominator.
- **The alpha test now exists on the software rasteriser too** (2026-09-17). `glAlphaFunc` was
  implemented by patching a discard into the pixel shader, which is the *hardware* path - so the
  same program drew one picture on the console and a different one on the host, with no error
  anywhere. `gl1-probe` found it on its first run, which is exactly what running one suite
  against both paths is for.
  - **The depth write is deferred past the test.** GL's fixed-function order is alpha test, then
    depth, so a discarded fragment must leave the depth buffer alone - otherwise everything
    behind it is hidden by something that is not visible, which is the classic symptom of a
    half-implemented alpha test. The rasteriser wrote depth before the colour stage, so the
    write is now held until the fragment is known to survive.
- **GLSL statement checking, function signatures and l-values** (2026-09-17), completing the
  front end. `glsl_check_unit` walks a whole shader.
  - **Two passes over the top level**, so a function may call one defined later in the file.
    Otherwise a shader compiles or not depending on the order somebody wrote its functions in.
  - **L-values are not a shape question.** A swizzle that repeats a component - `v.xx = ...` -
    cannot be written, because that is two values for one place; and a `uniform`, `attribute` or
    `const` is read-only, which needs the storage qualifier rather than the shape of the
    expression. Both mutation-tested.
  - Conditions must be `bool`: GLSL does not take "non-zero is true" from C. `break` and
    `continue` outside a loop are refused, which the parser cannot do because it has no idea
    where it is.
  - A `for` init declares into the loop's own scope, so two loops in a row do not collide and
    the counter does not leak out.
  - **Two bugs fixed, both found by a test rather than by reading:**
    - `parse_compound` spliced over a declarator chain. `float a, b;` is two DECL nodes on the
      same `sibling` field the statement list uses, so the next statement overwrote the second
      declarator and `b` silently never existed. The translation unit had the same hazard and
      had already been fixed; the compound had not. Only a *use* of `b` notices, which is why it
      survived until now.
    - Function parameters were being put in a scope *outside* the body's, so a local merely
      shadowed a parameter instead of colliding with it. GLSL puts them in one scope -
      `float f(float x){ float x; }` is a redefinition, which is what glslang reports - and a
      comment in this file already claimed that was the behaviour while the code did the
      opposite.
- **GLSL types, scopes and expression checking** (2026-09-17): `src/gl/glsl_sema.c`. The grammar
  is happy with `vec3 + mat4` and with `.xyzw` on a `vec2`; deciding those are wrong happens
  here, and it is where a shader compiler earns most of its diagnostics.
  - **GLSL's operators are not C's**, and each difference is mutation-tested because a wrong
    answer still looks plausible:
    - **No implicit conversion between int and float.** `1 + 1.0` is an error in 1.10. Adding
      C's usual arithmetic conversions picks a type the author did not write.
    - **A scalar against a vector is component-wise and keeps the vector type**, in either
      order - `vec3 * float` and `float * vec3` are both `vec3`.
    - **`mat * vec` is a transform, not a component-wise multiply.** `mat4 * vec4` is a `vec4`;
      `mat4 * vec3` is an error. A component-wise implementation accepts the second and produces
      a plausible, wrong type.
    - Ordering (`<`, `>`) is scalars only, because it has no single answer on a vector - which
      is why GLSL has `lessThan()`.
  - **Swizzles may not mix the `xyzw`, `rgba` and `stpq` vocabularies**, and a component past
    the end of the operand is refused. `v.xg` and `v3.w` are the typos this catches; the second
    would otherwise read whatever sits after the vector.
  - **Constructors count components, not arguments.** `vec4(v3, 1.0)` is two arguments and four
    components and is legal; counting arguments rejects the idiom every vertex shader uses.
  - Scopes: an inner declaration shadows an outer one and is dropped when its block closes;
    redeclaration is an error only in the *same* scope. Lookup runs backwards so the inner name
    wins.
- **The GLSL preprocessor** (2026-09-17): `src/gl/glsl_pp.c`, a **token filter** rather than a
  text-to-text pass, so nothing allocates a rewritten source and every token keeps pointing at
  the original text for diagnostics. `#version`, object-like `#define`/`#undef`,
  `#ifdef`/`#ifndef`/`#else`/`#endif`, `#error`.
  - **Skipping still parses the directives.** Inside a false branch the tokens go and the
    conditionals do not, or `#ifdef A` / `#ifdef B` / `#endif` / `#endif` pairs with the wrong
    one and surfaces hundreds of lines later as a brace mismatch.
  - **A directive reads one token past its own line**, because the lexer emits no newlines and
    a directive's end is a change of line number. That token is parked and served next; dropping
    it loses the first token after every directive, which is what the first version did.
  - **When the self-expansion flag is cleared is the whole of the difficulty.** Clearing it as
    the expansion queue drains is one read too early - that read yields the token which would
    re-trigger the expansion, so `#define A A` loops forever. Clearing it on return to the lexer
    is right, and makes mutual recursion (`#define A B`, `#define B A`) terminate as well.
    Mutation-tested: the wrong lifetime hangs rather than fails.
  - Function-like macros, `#if`, `#extension`, `#pragma` and `#line` are **refused by name**. A
    skipped `#extension` compiles a shader that asked for something it did not get; a skipped
    `#if` takes the wrong branch. A directive inside a dark branch is ignored entirely, refusals
    included - a `#pragma` in a branch that is not compiled has not been asked for.
- **GLSL declarations and statements** (2026-09-17), completing the grammar: compound blocks,
  `if`/`else`, `while`, `do`-`while`, `for`, the jumps, declarations with initialisers and array
  sizes, function definitions and prototypes, and a translation unit.
  - **A prototype has no body, which is not an empty body.** `c` absent means prototype; an
    empty `{}` is a compound node with no statements. The two read differently and a later stage
    needs to tell them apart.
  - **A declarator list and the translation unit both chain through `sibling`**, so the unit has
    to walk to the tail of a declaration rather than assume the node it just received is the
    last. Getting that wrong drops everything after the first `float a, b;` in the file -
    mutation-tested, because it is silent.
  - A mutation making the declarator list parse initialisers with the comma operator **survived
    the two-declarator test**: the first declarator's initialiser is parsed at a different call
    site, so only three declarators can expose it. Test widened, mutation then caught.
  - The preprocessor is refused by name rather than skipped, because skipping would silently
    ignore a `#version` and compile something the program did not write.
  - The dangling `else` binds to the nearest `if`, and that is **structural rather than a
    decision**: recursive descent has the inner `if` consume the `else` before the outer frame
    sees it. Noted in the source, because it looks like a condition someone could get wrong and
    an attempted mutation there had nothing to bite on.
- **The GLSL expression parser** (2026-09-17): `src/gl/glsl_parse.c`, recursive descent over
  GLSL 1.10's full precedence ladder, building an AST in a caller-supplied arena. Nodes refer to
  each other by **index, not pointer** - indices survive the arena moving, and a stale one is a
  bounds check rather than a wild read, which matters where nothing catches the alternative.
  - **Associativity is asserted, not assumed.** Getting it backwards still parses every program
    and computes a different answer: `a-b-c` must be `(a-b)-c` and `a=b=c` must be `a=(b=c)`.
    The tests render the tree fully parenthesised so the shape is readable in the expected
    string rather than inferred from node counts.
  - **The bitwise levels are present although GLSL 1.10 barely uses them**, because leaving a
    level out of the ladder does not raise an error - it silently reassociates everything
    around it.
  - **A comma inside an argument list is not the sequence operator.** Parsing the list with the
    full expression parser makes `f(a,b)` a one-argument call whose argument is `(a,b)`, which
    type-checks differently and is very hard to see. Mutation-tested.
  - A dropped precedence level turns out to be caught by `-Wunused-function` before any test
    runs, which is a nicer failure than the one the tests would give.
- **The GLSL front end started** (2026-09-17): `src/gl/glsl_lex.c`, a GLSL 1.10 lexer. GL 2.0 is
  a compiler, and this is the stage that can be finished today - pure text handling, testable to
  the same standard as everything else, and needed by any version of GL 2.0 that ever lands.
  The back end is gated on the same shader-interface measurement fog and multitexture are
  (`REQ-...-3a91`), because varyings are parameter exports.
  - It owns no memory and never stops: an error is a token, so a compiler built on this can
    report several problems in one pass rather than the first and nothing else.
  - **Maximal munch and whole-word keywords** are the two things a hand-written lexer gets
    wrong. `>=` is one token; `floatx` is an identifier and not `float` followed by `x`. The
    second is the nastier: the program still lexes, and the syntax error points at something
    that was never wrong. Both are mutation-tested.
  - The unterminated-block-comment check turned out to be **the scan loop's only exit**, not
    just a diagnostic - `advance` returns '\0' without moving once the source is exhausted, so
    removing it hangs the build rather than failing it. The comment says so now.
  - GLSL 1.10's reserved words are recognised and named, so a program using `goto` is told which
    word it may not use instead of meeting a syntax error downstream.
- **`glGetString(GL_EXTENSIONS)` was advertising an extension that is not here** (2026-09-17).
  It returned `GL_EXT_vertex_array`. The arrays are here - but an extension string is a promise
  about *that extension's entry points*, and `glVertexPointerEXT`, `glDrawArraysEXT` and
  `glArrayElementEXT` do not exist; only the core spellings do. It now returns an empty list,
  which is the true statement that no extension's own entry points are provided.
  - This is the same reasoning that kept `GL_ARB_vertex_buffer_object` out when the buffer
    objects landed the day before - applied then to a new claim, and missed on the claim already
    in the file.
- **D009: an absent feature is an absent symbol, not a function that refuses** (2026-09-17).
  Practice since D007, written down now because the wrong choice looks kinder. A shim that
  declares `glPolygonStipple` and refuses it produces a program that links, runs and draws the
  wrong picture - almost nothing checks `glGetError` on a state setter, so the refusal is a
  message nobody reads. Omitting it produces a link error at build time naming the symbol. The
  line is not "is the feature supported" but "does this entry point exist for any other reason":
  `glEnable` has to exist to enable depth testing, so it is the right place to say fog does not;
  `glFogf` has no other job.
- **`glTexEnvfv` was refusing nothing** (2026-09-17). Given a target or `pname` it did not keep
  it returned clean having done nothing, while its own sibling `glTexEnvi` refused the same
  arguments. Two spellings of one call disagreeing about what is an error is worse than either
  answer: a program that set the environment through the vector form believed it had. Fixed, and
  `glTexEnviv`, `glGetTexEnvfv`, `glGetTexEnviv` added alongside - the integer colour converting
  by range, as a light's does.
- **`glHint`, `glDrawBuffer`, `glReadBuffer`** (2026-09-17), all three of them mostly refusals.
  A hint is advisory and ignoring one is allowed, but **naming a hint for a feature that does
  not exist is not the same thing**: `GL_FOG_HINT`, `GL_POINT_SMOOTH_HINT` and
  `GL_LINE_SMOOTH_HINT` are refused because this draws none of those, and only
  `GL_PERSPECTIVE_CORRECTION_HINT` is kept and reported. There is one surface, so `GL_BACK` is
  accepted and everything else refused - `GL_NONE` above all, which would have a program believe
  it had switched drawing off.
- **User clip planes costed** (2026-09-17). They looked like a vertex-stage rewrite and are not:
  Mesa's `si_emit_clip_regs` takes the fixed-function path whenever the vertex shader exports no
  clip distances, which is this case, so it is `PA_CL_UCP_*` and `UCP_ENA_0..5` in
  `PA_CL_CLIP_CNTL` and no shader change. Offsets cited from `src/amd/registers/gfx103.json`.
  Not written yet: the doubt is whether the clipper is reached under NGG passthrough, and
  passthrough has already produced one surprise here. Filed as `REQ-...-8c4d`.
- **The shader interface is now actually pinned** (2026-09-17). `docs/GL_ROADMAP.md` said the
  gl-cube oracle test asserted `SPI_VS_OUT_CONFIG`, `SPI_PS_IN_CONTROL`, `SPI_PS_INPUT_ENA` and
  the `SPI_PS_INPUT_CNTL` slots, and used that as the reason fog and multitexture are blocked -
  "that test failing is the point". **It did not assert any of them**, and neither did anything
  else: the safety net the document described was not there, so moving the shader interface
  would have passed the whole suite and gone wrong only on hardware, which is the one place
  nothing here can check. The assertions were added and the document corrected. `DB_Z_INFO` and
  the colour-surface registers were pinned all along, so the stencil half of the claim held.
  - It matters because the shaders are hand-written binaries: an extra interpolated input
    renumbers the VGPRs the pixel shader already reads, so the registers and the shader code
    have to move together or not at all.
- **Multitexture costed** (2026-09-17). Large API surface, small real core - and the core needs
  a third parameter export, which is the same shader-interface change fog needs and the same
  console dependency. Recorded in the roadmap with the specific register deltas so it is not
  re-derived.
- **The gap is measured rather than remembered** (2026-09-17). `docs/GL_ROADMAP.md` used to
  list what I remembered was missing; it now carries a diff of Mesa's declared entry points
  against this header. 161 present, 294 missing - of which ~160 are further spellings of things
  that exist, 34 are `*ARB` aliases, and ~100 are genuinely distinct features. The measurement
  found several cheap gaps that had not been on the remembered list, including everything below.
- **`glArrayElement`** and **`glDrawRangeElements`** (2026-09-17). The first is the bridge
  between the two ways of feeding geometry - arrays for the data, immediate mode for the
  assembly - and a disabled array falls back to the current attribute, which is the half that
  is easy to lose. The second ignores its range hint, which the specification allows, but still
  refuses `start > end`.
- **The integer lighting spellings** (2026-09-17): `glLighti*`, `glMateriali*`,
  `glLightModeli*`. **A colour does not convert the way a scalar does** - an integer colour
  component is mapped across the whole signed range onto [-1, 1], so `GL_AMBIENT` with `INT_MAX`
  is 1.0, while a position or an attenuation is an ordinary cast. Casting a colour would put
  every integer colour astronomically out of range and light the scene white. The rule and the
  constant are Mesa's `INT_TO_FLOAT`, applied to exactly these pnames in
  `src/mesa/main/light.c` and `src/mesa/vbo/vbo_attrib_tmp.h` - it is not something to recall
  from memory.
- **`glLoadTransposeMatrix{f,d}` / `glMultTransposeMatrix{f,d}`**, **`glTexParameter{i,f}v`**,
  **`glPixelStoref`**, **`glAreTexturesResident`**, **`glPrioritizeTextures`** (2026-09-17).
  Nothing here is ever evicted, so every texture that exists is resident - and a name that was
  never generated is an error rather than a "no", because a program asking about a texture it
  does not own has a bug that reporting non-residency would hide.
- **Buffer objects** (2026-09-17), the GL 1.5 arrays: `glGenBuffers`, `glBindBuffer`,
  `glBufferData`, `glBufferSubData`, `glDeleteBuffers`, `glIsBuffer`,
  `glGetBufferParameteriv`, both binding points, and `glDrawElements` reading its indices from
  one. Nearly all non-trivial GL code written since about 2003 draws this way.
  - **An array keeps the buffer's *name*, and the address is worked out at draw time.**
    Resolving it at `glVertexPointer` time is simpler and works right up until a program calls
    `glBufferData` again - which is an ordinary thing to do every frame - and then reads
    through a freed pointer. The test forces a reallocation between the two and makes the
    second allocation far larger, so a captured address cannot pass by landing in the same
    place.
  - **`pointer` becomes a byte offset while a buffer is bound, and offset zero arrives as
    NULL.** The array reader's `pointer != NULL` guards meant "no array bound", so a buffer
    whose data starts at offset 0 - the common case - would have drawn the default attribute
    silently. Each array's base now comes from one `gl_array_base` call.
  - The storage is ordinary process memory. Everything reading it is the CPU-side array
    reader, and the hardware path copies the vertices it assembles into its own allocation
    afterwards regardless. A GPU-resident buffer would save that copy: an optimisation, not a
    correctness question.
  - `glContextDestroy` releases it. Nothing else would have.
- **`glInterleavedArrays`** (2026-09-17), all fourteen formats. It is the pointer calls a
  program would otherwise write by hand, so there is nothing new for the draw path to read.
  - **The arrays a format does not name are disabled, not left alone** - otherwise a program
    switching to `GL_C3F_V3F` with a normal array already on keeps reading normals through a
    pointer nothing set for that buffer.
  - The offsets and default strides were **checked against Mesa's own table**
    (`src/mesa/main/varray.c`, `_mesa_get_interleaved_layout`) rather than derived from the
    specification's prose; all fourteen agree.
- **`glGetTexImage`** (2026-09-17). Shares `glReadPixels`' format conversion but **not its
  flip**: GL turns the framebuffer over because the window origin is the bottom-left corner, and
  a texture has no window. Reusing the readback loop would have been the tidy-looking mistake.
- **The per-object queries** (2026-09-17): `glGetTexParameter*`, `glGetTexLevelParameter*`,
  `glGetLight*`, `glGetMaterial*`, `glGetPointerv`. Each answers from the field its own setter
  writes.
  - `GL_TEXTURE_INTERNAL_FORMAT` reports `GL_RGBA` - **what is stored, not what was asked for**.
    Every upload is converted, so echoing the caller's `internalformat` back would have a
    program size its readback for three bytes a texel against an image holding four.
  - `glGetMaterialfv(GL_FRONT_AND_BACK, ...)` is refused although `glMaterialfv` accepts it:
    setting both at once is meaningful, reading "both" has no single answer.
- **The query family answers, and refuses** (2026-09-17). `glGetIntegerv`, `glGetFloatv` and
  `glGetBooleanv` answered a handful of pnames and **silently ignored the rest** - leaving the
  caller's buffer holding whatever it held before, with no error. A program reading
  `GL_MAX_LIGHTS` got back stack garbage that looked like an answer.
  - The implementation limits, the stack positions and the rest of the state are answered; an
    unknown `pname` is refused with `GL_INVALID_ENUM` and the buffer is left untouched.
  - **`glGetDoublev`** added, going through `glGetFloatv` rather than repeating the table.
  - A width table decides how many elements each query writes. Getting this wrong is not a
    wrong answer but a **buffer overrun**: the caller sized its `GLint[1]` from the pname, and
    only for the pnames nobody tested. Every single-valued query in the test has a guard
    element after it.
  - **`glIsEnabled` was answering from a shorter list than `glEnable` accepts**, so
    `GL_ALPHA_TEST` and `GL_POLYGON_OFFSET_FILL` read back as off immediately after being
    switched on. Both added, and an unknown cap is now refused rather than answered `GL_FALSE`
    - "not enabled" and "no such thing" are different answers.
  - **`glGetBooleanv` forwarded everything to `glIsEnabled`**, which reported
    `GL_COLOR_WRITEMASK` as all four channels off while all four were on.
- **The internal capacities moved out of the `GL_` namespace** (2026-09-17). They were spelled
  `GL_MAX_LIGHTS`, `GL_MAX_ATTRIB_STACK_DEPTH` and so on - *the names of the enumerants a
  program passes to `glGetIntegerv` to ask for those very numbers*. Holding the names internally
  made the enums impossible to define, so the queries above could not be written at all. They
  are now `OOPS_GL_LIGHT_COUNT`, `OOPS_GL_ATTRIB_STACK_CAPACITY` and so on: a size this port
  chose is `OOPS_GL_`, a number the specification assigned is `GL_`.
- **`glPushClientAttrib` / `glPopClientAttrib`** (2026-09-17), the client half of the attribute
  stack: the array pointers and the pixel-store modes, which live in this process rather than in
  a register.
  - **A second stack, not a second mask on the first one.** A library that brackets its array
    setup with a client push sits inside a caller that may have bracketed itself with
    `glPushAttrib`; sharing one stack would make the inner pop take the outer frame and hand the
    caller back whatever the library was holding. The test asserts the independence directly, by
    interleaving the two pushes and checking both depths.
  - The pointer and its enable are restored together, because `gl_client_array_t` holds both -
    restoring one without the other leaves an array switched on pointing at something never set
    for it, which is a dangling read rather than a wrong picture.
- **`glRect*`** (2026-09-17), all eight forms. The specification defines it as a four-vertex
  `GL_POLYGON`, which is not a primitive this accepts - but `GL_POLYGON` triangulates a convex
  polygon as a fan from vertex 0, and for four vertices that *is* the `GL_QUADS` split, so a
  rectangle makes the substitution exact rather than approximate.
  - Written on top of `glBegin`/`glVertex`/`glEnd` rather than reaching into the immediate-mode
    buffer, which is what makes it compile into a display list correctly.
  - The fixture in the test is deliberately not square and not centred: a rectangle symmetric
    about x=y passes against an implementation that swaps the axes, which is the shape of two
    escapes already caught in this file.
- **`glCopyTexImage2D`** (2026-09-17), the allocating half of the copy pair. Sizes the texture
  from the window rectangle and then copies into it through `glCopyTexSubImage2D`, so exactly one
  piece of code decides which way up a framebuffer copy lands. A non-zero `border` is refused
  rather than allocated a pixel short.
  - The allocation is checked against the **texture**, not against `glGetError`: GL errors are
    sticky, so an error left over from an earlier call would otherwise abort a copy that had
    every right to run.
- **`glPushAttrib` / `glPopAttrib`** (2026-09-17). Older GL code brackets state changes with
  these, and their absence does not merely render differently - **state leaks out of the routine
  that set it**, and everything drawn afterwards is wrong in a way that looks like a bug
  somewhere else.
  - A push saves all of the state and records the mask; the pop restores only the groups the
    mask names. Saving selectively would be cheaper and much easier to get subtly wrong.
  - **An enable belongs to more than one bit**: `GL_ENABLE_BIT` carries all of them, and each
    buffer bit also carries the enable for its own feature, so `GL_DEPTH_BUFFER_BIT` alone
    restores the depth-test enable. Getting that wrong gives a pop that puts back a depth
    function while leaving the test off, so the narrow masks are tested on their own rather than
    only through `GL_ALL_ATTRIB_BITS`, which would hide it.
  - A mask naming state this subset does not have - stencil, fog, accumulation buffer,
    evaluators, stipple, hints, pixel mode, points, lines - is refused, because a push that
    silently saved nothing is the leak this exists to prevent. `GL_ALL_ATTRIB_BITS` is narrowed
    to what exists instead, since a program saying "all" is asking for whatever there is.
  - The alpha test lives in the shader rather than in a register the next frame re-emits, so a
    pop rewrites it.
- **`glCopyTexSubImage2D`** (2026-09-17) - render-to-texture, the 1.x way. Reads through
  `glReadPixels` rather than repeating it, so it inherits the y flip; a second flip written by
  hand would be a second chance to get it wrong. The test proves that sharing by doing the copy
  and a read-then-upload of the same rectangle and comparing the two textures.
- **`docs/GL_ROADMAP.md`** (2026-09-17) - what is done, what is left, and what each remaining
  piece costs. The costs are the part worth writing down: fog and stencil both need changes to
  the shader interface that the gl-cube oracle record pins, so they cannot land without
  re-recording that frame on hardware, and points and lines are measured shut rather than
  merely missing.
- **`glAlphaFunc` and `GL_ALPHA_TEST`** (2026-09-17), the first feature here that needed new
  shader instructions. RDNA2 has no fixed-function alpha test - it was removed after the
  fixed-function era - so this is a discard written into both pixel shaders: a literal load, a
  comparison, and a mask update that kills the lanes that failed.
  - **The encodings are assembled, not remembered.** `tools/shader/alpha-test.s` holds the
    source, `clang -target amdgcn-amd-amdhsa -mcpu=gfx1030` produces the words, and each one
    sits in the switch beside the instruction it came from. This matters more here than for a
    register: a wrong encoding cannot fail loudly - it assembles into the payload, the hardware
    does something else, and the result is a wrong frame rather than a stopped build. Two
    results cross-check against words already in the tree: `s_endpgm` came out `0xbf810000`, and
    the 32-bit-literal marker `0xff` sits where the untextured shader's own canary load has it.
  - Four words are reserved in each shader and **patched in place** rather than rebuilt, because
    the payload is laid out and cache-flushed once at context creation.
  - `GL_NEVER` kills every lane instead of comparing; `GL_ALWAYS` writes no instructions at all
    rather than a comparison that always passes; a function outside the eight is refused and
    leaves the shader as it was. Both shaders are patched together, so a textured draw and an
    untextured one cannot disagree.
- **`glPolygonOffset`** and `GL_POLYGON_OFFSET_FILL` (2026-09-17). Every value here is cited
  rather than recalled, because register offsets and field semantics are exactly what this
  collection has been burned by before:
  - The five registers come from Mesa's generated `src/amd/common/amdgfxregs.h` -
    `R_028B7C..R_028B8C` - converted to context dwords as `(byte - 0x28000) / 4`. The
    conversion was checked against a known answer first: `R_028B78_PA_SU_POLY_OFFSET_DB_FMT_CNTL`
    gives `0x2de`, which is the register oops-gl already had at that offset.
  - The enable bits (11, 12 and 13 of `PA_SU_SC_MODE_CNTL`) come from the same header. All three
    are set together, because GL has one enable for filled polygons and setting front without
    back would offset half a mesh.
  - **The factor is scaled by 16 and the units are not**, from radeonsi's `si_state.c`, which
    multiplies units by 4, 2 or 1 for 16-, 24- and 32-bit z-buffers. Which of those applies was
    not assumed: oops-gl's existing `DB_FMT_CNTL` of `0x1e9` decodes to
    `NEG_NUM_DB_BITS(-23) | DB_IS_FLOAT_FMT(1)`, which is Mesa's 32-bit float case, so the
    units go in unscaled. The test uses different values for factor and units precisely so a
    build applying one scaling to both fails it.
- **`glReadPixels`**, and `GL_PACK_ALIGNMENT` alongside it (2026-09-17). Reads the command
  processor's cached copy of the render target when a frame has been confirmed, and the
  framebuffer otherwise.
  - **It flips the rows.** GL's window origin is the bottom-left corner and this framebuffer's
    row 0 is the top, so GL row 0 is the *last* row in memory. This is the detail that produces
    a plausible wrong answer - an unflipped read is a vertically mirrored image, which still
    looks like a screenshot - so the test fills each row with its own value and reads the
    corners, and removing the flip fails it.
  - Pixels outside the window read **zero rather than whatever followed the framebuffer**. The
    specification leaves them undefined and undefined may be zero; handing back adjacent memory
    would also be permitted and is the kind of plausible garbage this refuses to produce.
- **`glDepthRange`** (2026-09-17). `PA_CL_VPORT_ZSCALE` and `ZOFFSET` were literal `0.5f`
  constants; they are now `(far - near) / 2` and `(far + near) / 2`. The derivation was checked
  against a known-good point rather than assumed: the default range 0..1 gives exactly the two
  constants the gl-cube oracle frame was recorded with, and
  `test_pm4_gl_honours_the_gl_cube_oracle_record` now asserts that, so a change to this
  arithmetic cannot silently move every depth-tested frame away from the record. A reversed
  range (`near` above `far`) is accepted rather than sorted - it reverses the depth buffer,
  which is a technique.
- **`glTexSubImage2D` and `glPixelStorei`** (2026-09-17), and the texture upload path made
  honest on the way. It accepted any `format`, converted only `GL_RGBA` and `GL_RGB`, and left
  the texture holding **whatever the allocation contained** for everything else - uninitialised
  memory, sampled, with `glGetError` reporting nothing. Formats are now checked before anything
  is allocated, and `GL_BGRA`, `GL_BGR`, `GL_LUMINANCE`, `GL_LUMINANCE_ALPHA` and `GL_ALPHA`
  convert alongside the two that already did. A type other than `GL_UNSIGNED_BYTE` is refused
  rather than read as bytes.
  - `glPixelStorei` carries `GL_UNPACK_ALIGNMENT` and `GL_UNPACK_ROW_LENGTH`, defaulting to the
    specification's 4 and 0 - **set explicitly in `glContextCreate`**, because a zeroed context
    would read as alignment 1 and silently misplace every row after the first for the many
    callers that never call `glPixelStorei` at all.
  - A sub-image outside the texture is `GL_INVALID_VALUE`, not a clamp and not an overrun.
- **Fixed on the way: `tex->pitch` was only set on the target build.** It stayed zero on a host
  build, which was harmless for exactly as long as nothing read it - and `glTexSubImage2D` reads
  it to find a row, so every row landed on top of row zero. Caught by the first test to depend
  on it, and now set with `width` and `height` rather than inside one branch of an `#ifdef`.
- **Display lists** (`glNewList`, `glEndList`, `glCallList`, `glCallLists`, `glGenLists`,
  `glDeleteLists`, `glIsList`, `glListBase`) - the first of the 1.1 gaps, and the one GL 1.x
  programs lean on hardest. A list records the calls made while it is open and replays them:
  **the call, not its effect**, so a list compiled before a texture is bound and executed after
  it draws with the later texture, as the specification requires.
  - `GL_COMPILE` records without drawing; `GL_COMPILE_AND_EXECUTE` does both. Watched failing:
    making `GL_COMPILE` also execute is caught by the test that checks the framebuffer is still
    clear *between* compiling and calling - the check that tells a list which drew at the wrong
    time from one that worked.
  - **The vertex-array draws cannot be compiled and say so** (`GL_INVALID_OPERATION`). Their
    semantics need the client pointers dereferenced at compile time; storing the pointer to read
    on replay would draw whatever the array holds then, which is a different picture from the one
    that was compiled. Refused rather than silently deferred.
  - A list calling itself is bounded (`GL_STACK_OVERFLOW`) rather than overflowing the stack, and
    calling an *undefined* list is ignored rather than an error - the specification is explicit,
    and it is what lets a list reference one compiled later.

### Changed

- **oops-gl refuses what it does not implement, instead of quietly doing nothing**
  (2026-09-16). Callers that relied on the old silence will now see `glGetError()` report
  `GL_INVALID_ENUM` or `GL_INVALID_VALUE` where they previously saw `GL_NO_ERROR`. Nothing in
  the collection relied on it - `gl-cube` uses only supported enums and its selftest is
  unchanged - but it is a visible behaviour change, so it is listed here rather than under
  Fixed. What now refuses:
  - **Primitive modes.** Everything reaches the hardware as triangles, so `GL_TRIANGLES`,
    `GL_TRIANGLE_STRIP`, `GL_TRIANGLE_FAN` and `GL_QUADS` draw and the rest refuse.
    `GL_POINTS`, `GL_LINES`, `GL_LINE_STRIP`, `GL_LINE_LOOP`, `GL_QUAD_STRIP` and `GL_POLYGON`
    used to reach a `default: break;` in three separate switches - no geometry, no error.
    **The refusal of points and lines is now measured rather than cautious**: obSCEne
    submitted both on retail hardware across four sweeps on 2026-09-16, a point as
    `m0 = 0x1001` (one vertex) and a line as `m0 = 0x1002` (two), and every run recorded
    `fence-hit 0` and `pixel-hit 0` - the end-of-pipe fence kept its sentinel, so the pipe
    stopped rather than the primitives drawing wrongly. The same passthrough path with
    `m0 = 0x1003` (three vertices) retires and draws. One and two vertices stall, three do
    not. **The stall is measured; the reason for it is not** - obSCEne reads it as GFX10 NGG
    passthrough being hardwired for triangles, but no sweep records a register showing that
    fixture is in passthrough, and the two streams disagree about `VGT_SHADER_STAGES_EN`
    (`0x2d5`): the fixture uses `0x02002000`, oops-gl uses `0x00c12010` with `PRIMGEN_EN` set.
    The refusal stands on the measurement rather than the explanation, and the open question
    is obSCEne `REQ-20260916T2223Z-b6d4`.
  - **Capabilities.** `glEnable`/`glDisable` of something outside the eight this subset keeps
    (plus `GL_LIGHT0..7`) is `GL_INVALID_ENUM`. A dropped `glEnable(GL_STENCIL_TEST)` rendered
    wrong with nothing anywhere to say why.
  - **Matrix modes.** An unrecognised `glMatrixMode` used to leave the previous mode selected,
    so every matrix call afterwards silently edited a different stack.
  - **Client arrays**, and **`glDrawArrays`/`glDrawElements` with a negative count**
    (`GL_INVALID_VALUE`; a zero count stays a legal draw of nothing, which shared the same
    silent return).
  - **Texture parameters, texture-environment parameters and material properties.**
    `glTexParameteri` validated its target and not its `pname`, so a real GL parameter this
    subset does not keep - `GL_TEXTURE_WRAP_R`, an LOD bias - was dropped and the caller went
    on believing the texture was configured. `glTexEnvi` and `glMaterialfv` had the same hole,
    and `glMaterialfv` did not check `face` at all.

  `src/gl/gl_state.c` now has no silent `default:` arms left. The six remaining in
  `gl_draw.c` and `gl_matrix.c` are unreachable by construction, because the entry points
  above them validate first.
- **oops-gl is scoped in writing, and it is not an OpenGL version** (2026-09-16, D007). It is a
  fixed-function instrument: OpenGL 1.1-class, plus individual later calls added when an oracle
  program needs one. Applications use oops-mesa, which provides OpenGL 3.3 Core and GLSL 3.30.
- **oops-gl's tested surface went from 64 of its 86 entry points to all 86** (2026-09-16).
  Every refusal above was watched rejecting before being trusted, and the ones that matter most
  had no test at all: `glIsHardwareAccelerated` - the badge that has already overclaimed once -
  the matrix builders, and the vector forwarders.

  **Two of those tests passed against the bug they were written for, and both failures were the
  same mistake.** A `glVertex3fv` test used a triangle symmetric about `x = y` and checked one
  pixel, so a transposed forwarder drew a different triangle that covered that pixel in the same
  flat colour. A `glNormal3fv` test lit the face from `(0, 0, 1)`, so the shading depended on the
  normal's z alone and a transposed normal lit identically. In both cases the *test setup* was
  symmetric in exactly the axes the bug swapped. They now use an asymmetric triangle compared
  over the whole framebuffer, and a light along x, and both fail on their mutation as they
  should. Worth remembering when writing the next forwarder test: a transposition test whose
  fixture is symmetric is not a test.

- **Three of oops-gl's register constants stopped being unmeasured defaults** (2026-09-16).
  `PA_SU_POINT_SIZE`, `PA_SU_POINT_MINMAX` and `PA_SU_LINE_CNTL` were values nobody had
  varied, and since everything here draws as triangles they had never affected a pixel.
  obSCEne's `166-agc/primitive-draw` (sweep `20260916-223136`) put a submission through this
  hardware whose fence retired, and decoding its command stream shows all three set to
  byte-identical values. Cross-checked rather than measured, and the comments say which.

- **The gl-cube oracle record's facts are now assertions, not prose** (2026-09-16).
  `docs/hardware/agc-gl-cube-oracle-fw1240.md` holds the register facts that made a frame draw
  on retail firmware 12.40, and it is the reason this subsystem exists (D007) - but every one
  of them was documented and checked nowhere, so a register could be edited and the document
  would go on describing a frame the code no longer produces.
  `test_pm4_gl_honours_the_gl_cube_oracle_record` builds oops-gl's real command stream on the
  host and asserts all six against it: the `CB_COLOR0_ATTRIB2` field order, `CB_COLOR0_INFO`
  `COMP_SWAP=ALT`, a negative `PA_CL_VPORT_YSCALE`, `VGT_GS_OUT_PRIM_TYPE` = TRISTRIP,
  `DB_Z_INFO` `SW_MODE` 24 with no HTILE, and `CB_COLOR0_ATTRIB3` `COLOR_SW_MODE` = LINEAR.
  The display is 640x480 on purpose, so the extent fields cannot be transposed without it
  failing - which is the bug the record itself says was measured and fixed once already.
  Watched failing on a transposed **call site**, which the existing macro-level test cannot
  see, and on the compositor's tiled `ATTRIB3` value being copied in.

  The same decode gave `VGT_GS_OUT_PRIM_TYPE` a **measured negative** to sit beside the
  positive one in the gl-cube oracle: that stream sets it to `0` (POINTLIST), supplies three
  vertices, and its 64x64 target comes back with 4,095 background pixels and exactly one red
  one. The triangle became a point, its fence retired, and oops-gl's `2` (TRISTRIP) is the
  right value for the reason the oracle already gave.

### Fixed

- **The AGC display's render target was heap memory the GPU could not use** (2026-09-19).
  `042d236` allocated `linear_scratch_fb` with `oops_malloc` to make CPU access cached. A large
  `oops_malloc` is an anonymous `mmap` with no GPU access asked for, returned 24 bytes past its
  page, behind the heap's block header. The GPU reads and writes this buffer: the compute
  tiler reads it, oops-gl drew into it at `CB_COLOR0_BASE` in 256-byte units, and the CP copies
  it. So every oops-gl draw on a console either faulted or landed 6 pixels early over the
  header. No console run since the change had shown which. It is `oops_mem_alloc(...,
  64 KiB, OOPS_MEM_WB_ONION)` now, freed with `oops_mem_free`. That is the "cached Onion memory
  (type 0)" the allocation's own comment described: CPU-cached, GPU-mapped and aligned.
  Whether the colour block writes Onion correctly is `REQ-20260919T1811Z-b52e`'s question.
  oops-gl's scanout path, above, stops drawing into this buffer altogether.

- **A read after a pixel rectangle read the pixels from before it, on the console**
  (2026-09-19). `glReadPixels` and the accumulation buffer read the CP's CPU-cached copy of
  the render target. The copy is made at the end of every submission, and nothing marked it
  stale when the CPU wrote into the target. So a `glDrawPixels`, `glBitmap`, `glCopyPixels`
  or `glAccum(GL_RETURN)` followed by a read, with no draw in between, read the frame as it
  was before the write. The copy now records which buffer it is of (`readback_of`), and every
  CPU write into a colour buffer clears that (`gl_raster_sync`). A read then takes the buffer
  itself, through `gl_color_read_source`. `test_pm4_gl_front_buffer_targets`.

- **State the specification lists that no query answered, and enums nothing declared**
  (2026-09-19), found by auditing oops-gl against GL 1.5's state tables and Mesa's GL 1.0-1.5
  enums rather than against its entry points. Every table name `glGet`/`glIsEnabled` names now
  answers through every getter: ten were neither declared nor answered (`GL_TEXTURE_STACK_DEPTH`,
  `GL_LIST_INDEX`, `GL_LIST_MODE`, `GL_MAX_LIST_NESTING`, `GL_AUX_BUFFERS`, `GL_DOUBLEBUFFER`,
  `GL_STEREO`, `GL_SUBPIXEL_BITS`, `GL_CURRENT_RASTER_INDEX`, `GL_MAX_ELEMENTS_VERTICES`/`_INDICES`
  - Mesa's answers where they are constants), and `glGetIntegerv` refused eleven float states it
  now reaches through `glGetFloatv`, the normalised ones mapped linearly as GL 1.5's 6.1.2 and
  Mesa's `TYPE_FLOATN` rows say. GL 1.3's `GL_TRANSPOSE_MODELVIEW_MATRIX`,
  `_PROJECTION_MATRIX` and `_TEXTURE_MATRIX` answer. **`glCallLists` takes all ten of GL 1.0's
  name types** - it took the three unsigned ones - decoded as Mesa decodes them
  (`main/dlist.c:13481-13575`), `GL_2_BYTES` to `GL_4_BYTES` big-endian. **`glDrawBuffer`
  accepts `GL_NONE`** (colour writes off, on the console through `CB_TARGET_MASK`) and
  `GL_BACK_LEFT`, `glReadBuffer` `GL_BACK_LEFT`; the right and auxiliary buffers, which this
  visual lacks, are `GL_INVALID_OPERATION`, and `GL_DRAW_BUFFER`/`GL_READ_BUFFER` answer what
  was set, saved with the colour-buffer and pixel-mode groups. **`GL_CURRENT_BIT` saves the raster
  position**, its colour, texture coordinates, distance and validity, as GL 1.3's table 6.5 and
  Mesa's push do. And declared: the buffer names, `GL_2_BYTES`..`GL_4_BYTES`, GL 1.5's
  `GL_SRC0_RGB`..`GL_SRC2_ALPHA`, GL 1.0's `GL_TEXTURE_COMPONENTS`, `GL_ALL_CLIENT_ATTRIB_BITS`,
  and the `GL_TEXTUREn_ARB`/`GL_*_TEXTURE_ARB` enums beside the `_ARB` entry points.
  `test_gl_state_table_audit_findings`. Front-buffer rendering stays refused - see the roadmap.
- **The console sampled every texture at level zero** (2026-09-19) - written, not yet run on one.
  The textured pixel shader's sample was `image_sample_lz`, which takes no level of detail: the
  mip chain laid out for the GPU the same day could never be read past its base, a minified
  texture used the magnification filter, and GL 1.4's LOD bias in the sampler had nothing to bias.
  It is `image_sample` now, in whole-quad mode so the helper pixels of each quad supply the
  implicit derivatives - the live pixels' mask kept and restored after the sample, as Mesa's ACO
  does (`aco_insert_exec_mask.cpp:61-97`, `:150-163`), so the combine, fog, alpha test and export
  see exactly the pixels they did. **And q is divided per fragment there too**: the vertex carries
  s, t and q undivided, q in the texture parameter's spare `w`, and the shader divides after
  interpolating. Words from the new `tools/shader/tex-prolog.s`, which also reassembles the
  replaced `_lz` word as a cross-check; the shader is built by `gl_ps_build_textured`, which the
  host can call, and `test_pm4_gl_textured_shader_samples_with_lod_and_divides_q` pins it. gl-cube
  (linear filters, no mipmaps, q 1) draws the same picture. gl1-probe's `mipmap-levels`,
  `lod-bias` and `projective-texture` are the measurement.
- **An enabled 3D texture or cube map textured a drawn clear, and `GL_ENABLE_BIT` left the
  texture generation enables out** (2026-09-19), both found moving the texture state into
  per-unit form for the second unit GL 1.3 requires. A scissored or masked `glClear` is drawn as a
  quad with every other per-fragment effect set aside - but only the 1D and 2D enables were set
  aside, so with `GL_TEXTURE_3D` or `GL_TEXTURE_CUBE_MAP` on, the clear colour was modulated by
  the texture. And `glPushAttrib(GL_ENABLE_BIT)` did not save `GL_TEXTURE_GEN_S` and the rest,
  which GL 1.3's table 6.20 puts in the enable group as well as the texture group (Mesa,
  `main/attrib.c:189-192`). `test_gl_drawn_clear_and_enable_bit_cover_every_texture_enable`.
- **A texture coordinate's q was divided at the vertex** (2026-09-19). `glTexCoord4`, texture
  generation with a q plane and a projective texture matrix all give q, and GL divides s, t and r
  by it per fragment, after interpolation. oops-gl divided at the vertex and interpolated the
  quotients, which agrees only while q is the same at every vertex of a primitive - so a
  projected texture (a spotlight's image, a shadow map's coordinates) slid across its surface
  instead of staying put. The vertex now keeps all four components undivided and the software
  rasteriser divides per fragment, level of detail included; feedback reports all four, as Mesa
  does (`main/feedback.c:136-139`), rather than `(s/q, t/q, 0, 1)`. The console path still divides
  per vertex until its pixel shader takes q - `docs/GL_ROADMAP.md`, **Needs a shader change**.
  `test_gl_projective_texcoords_divide_per_fragment` and gl1-probe's `projective-texture` hold it.
- **Pixel centres on a shared edge were drawn by both triangles** (2026-09-19). The software
  rasteriser took a centre exactly on an edge as inside every triangle it bounded, so the
  diagonal of every quad - and every other shared edge a centre fell on - was drawn twice:
  blended twice, stencil-incremented twice, and counted twice by an occlusion query (a 4x4 quad
  counted 20 samples, which is how it was found). GL requires a shared edge to produce each
  fragment once. A top-left tie rule now gives each such centre to exactly one triangle. Coverage
  changes only for exact-edge centres: a width-1 line lying between two rows now fills one row,
  not both, and a one-pixel quad on a pixel corner fills one pixel, not four. gl1-cube's picture
  is unchanged.
- **Depth and stencil pixel rectangles were refused** (2026-09-19). `glReadPixels` and
  `glDrawPixels` of `GL_DEPTH_COMPONENT` and `GL_STENCIL_INDEX`, and `glCopyPixels` of `GL_DEPTH`
  and `GL_STENCIL`, are GL 1.0 and answered `GL_INVALID_ENUM` - so reading the depth under the
  cursor to unproject a click failed outright. Each works now, with its pixel transfer
  (`GL_DEPTH_SCALE`/`BIAS`; `GL_INDEX_SHIFT`/`OFFSET` and `GL_MAP_STENCIL`, Mesa's order): a drawn
  depth pixel is a fragment at its own z, through the depth test; a stencil index goes straight
  into the stencil buffer through the scissor and write mask. On the hardware path the depth
  operations answered `GL_INVALID_OPERATION` with one log line, the depth surface being the GPU's
  and tiled. The stencil ones joined them when the console's stencil test made the stencil
  surface the GPU's too. Both reach the tiled surfaces there since the same evening
  (**Depth and stencil pixel operations on the console**).
- **Colour-index images and the `GL_BITMAP` type were refused** (2026-09-19). GL 1.0 takes
  `GL_COLOR_INDEX` images in an RGBA context: `glDrawPixels` and the texture uploads convert each
  index through the index shift and offset and the `GL_PIXEL_MAP_I_TO_R/G/B/A` maps, as Mesa does
  (`main/pack.c:1500-1548`), with no RGBA scale or bias after. They are never read back -
  `glReadPixels` answers `GL_INVALID_OPERATION`, `glGetTexImage` `GL_INVALID_ENUM`. `GL_BITMAP` -
  an index a bit, for colour and stencil indices only - draws, uploads, reads stencil back and
  compiles into lists as glBitmap's bits do. A stencil read's transfer was also applied while the
  transfer was suspended, which it no longer is.
- **Pixel rectangles skipped every fragment operation** (2026-09-19). `glDrawPixels`, `glBitmap`
  and `glCopyPixels` wrote their pixels straight into the colour buffer, so a bitmap label ignored
  the scissor, a drawn sprite did not blend, and `glColorMask`, the alpha, stencil and depth
  tests, the logic op, texturing and fog never touched them. Each pixel is now a fragment (GL 1.x
  3.6.4-3.8): at the raster position's z, textured with the one texel its raster texture
  coordinate samples, fogged by the raster distance, and then through the triangle rasteriser's
  own per-fragment tail - one copy, `gl_fragment_tail`, which both now call. The sampler, blend
  factors and stencil operations are compiled for the target too (about 18 KB), because there the
  rectangle is the CPU's; the depth and stencil tests are left out on that path, with one log line,
  since the depth surface is the GPU's and tiled. Found alongside:
  - **`glCopyPixels` smeared overlapping copies.** It read and wrote pixel by pixel, so a copy
    one row up or one column right read back what it had just written. The whole source is read
    first now, as GL's read-then-draw definition requires, and the copy is zoomed like
    `glDrawPixels` (it ignored `glPixelZoom`).
  - **The zoom follows GL's rule**: a pixel covers the window columns whose centres fall in
    `[xr + zx n, xr + zx (n + 1))`. The raster position was truncated and whole-pixel steps
    taken, which put a mirrored image one column over, and a zoom of 0 drew at a zoom of 1.
  - **`glBitmap` lands at floor(raster - origin)**, and a negative raster colour is black
    rather than full intensity.
- **`glClear` ignored the scissor box and every write mask, on both paths** (2026-09-19). GL
  clears only inside the scissor box and only through `glColorMask`, `glDepthMask` and
  `glStencilMask`; this filled the whole surface regardless, so a program clearing one viewport
  of a split screen wiped the others and one clearing with a channel masked lost that channel.
  The source even said the stencil write mask does not gate a clear, and a test pinned it - both
  wrong: Mesa clears a masked stencil buffer through a quad whose stencil writemask is the
  program's (`state_tracker/st_cb_clear.c`, `clear_with_quad`). Now:
  - the depth mask drops the depth clear;
  - the stencil clear, CPU memory on both paths, keeps to the box and goes through the mask;
  - colour and depth through a partial scissor box or a colour mask are **drawn** - one quad at
    the clear colour and depth, through the ordinary pipeline, with depth test `GL_ALWAYS` and
    blending, logic op, alpha test, texturing, lighting, fog, stencil test, culling, offset,
    stipple and clip planes set aside and put back. On the hardware the scissor registers and
    `CB_TARGET_MASK` the draw path already emits do the rest, where the DMA fill of the whole
    allocation could not; on the host it is the software rasteriser's scissor and mask, so the
    two paths clear identically.
  - An unscissored, unmasked clear is still the fill, which leaves gl-cube's recorded frame as it
    was. `glClear` inside `glBegin` is `GL_INVALID_OPERATION`, and a bit naming no buffer is
    `GL_INVALID_VALUE` - both were ignored.

  Two tests had the old behaviour built in: the stencil test's "does not gate a clear" and the
  logic-op test, whose masked draw followed a clear made under the same mask. The drawn clear is
  unmeasured on a console; gl1-probe's `scissored-clear` measures it.
- **`glReadPixels` into `GL_LUMINANCE` read the red component alone** (2026-09-19). GL reads
  luminance from a colour buffer as R + G + B, clamped - Mesa's `main/readpix.c:484` and
  `pack.c:1297` - so pure blue read as 0 here and as 1 anywhere else.
- **The texture matrix was never applied** (2026-09-19). `glMatrixMode(GL_TEXTURE)` had a stack
  that pushed, popped, loaded and answered `GL_TEXTURE_MATRIX` - and no vertex ever went through
  it, so a program that scrolled or scaled a texture that way drew it unmoved, and a projected
  texture (eye-linear generation, then a projection in the texture matrix) came out as the raw
  planes. Every texture coordinate now goes through it after generation, on both paths, since
  the coordinate reaching the hardware is computed here. gl1-probe's `texture-matrix` checks it
  on the console.
- **The raster position was not processed as a vertex** (2026-09-19). Its colour was the current
  colour copied raw - GL lights it when lighting is on - and its texture coordinate skipped
  generation and the texture matrix; both follow `glVertex` now (Mesa `main/rastpos.c`). Its w,
  which `GL_CURRENT_RASTER_POSITION` reports, held 1/w; it is the clip w, as Mesa keeps it.
- **`glGetPointerv(GL_INDEX_ARRAY_POINTER)` was refused** (2026-09-19), though the index array
  had had a pointer since colour-index state landed.
- **`glGetIntegerv`, `glGetFloatv` and `glGetDoublev` refused every enable** (2026-09-19). GL
  answers each capability `glIsEnabled` knows through every `glGet` - `glGetIntegerv(GL_DEPTH_TEST)`
  is 1 or 0 - and only `glGetBooleanv` did; the other three raised `GL_INVALID_ENUM` and left the
  caller's buffer as it was. They now fall back to `glIsEnabled` before refusing.
- **A texture parameter changed mid-frame re-sampled the frame's earlier draws** (2026-09-19).
  Every textured draw loads its descriptors into one slot, and the draws run at the flush - so
  the slot was reloaded only for a *different* texture, and a program that changed one texture's
  wrap mode or filter between two draws of a frame had both drawn with the second setting on the
  hardware. The slot is now compared by content, and any change submits the draws that read the
  old contents first. (`desc_dirty` had been set for this and never read.)
- **The software rasteriser interpolated in screen space** (2026-09-19): colours, texture
  coordinates, fog and clip distances all used the affine barycentric weights, which is right
  for window depth and wrong for anything linear in clip space - so a textured floor in
  perspective swam on the host and not on the console, whose interpolators have always
  corrected, and the comment on the perspective hint claiming otherwise was false. Every
  attribute but depth is now weighted by its vertex's 1/w and renormalised. Under an orthographic
  projection nothing changes; gl1-cube's lit-pixel count moved from 86833 to 89903.
- **Texture 0 was never sampled** (2026-09-19). With nothing bound, an upload went to the
  target's default texture - GL 1.x's texture 0, a real texture - and the draw then asked for
  "the bound texture", found 0, and drew untextured. A program in the GL 1.0 style, which calls
  `glTexImage2D` and never `glBindTexture`, drew nothing textured at all.
- **`glGetTexImage` wrote past the end of the caller's buffer**, the same way `glReadPixels` did
  (below): it zeroed `stride * height` bytes, the last row's alignment padding included.
- **`glCopyTexImage1D` consumed the program's error** (2026-09-19). It checked its own allocation
  with `glGetError()`, which clears the flag - so an error it raised was never reported, and an
  older unrelated one aborted the copy. The 2D version's comment already warned of exactly this.
- **`glDeleteTextures` reset only the 2D binding** (2026-09-19). Deleting a bound 1D texture left
  `GL_TEXTURE_BINDING_1D` naming a texture that no longer existed.
- **`glReadPixels` wrote past the end of the caller's buffer** (2026-09-19). It zeroed
  `stride * height` bytes before filling them, and the stride includes the pack alignment's
  padding - so the *last* row's padding was written too, which the caller never allocated: a 1x1
  `GL_RGB` read at the default alignment of 4 wrote four bytes into three. It also clobbered the
  padding between rows, which GL leaves alone. Found by running the suite under AddressSanitizer,
  in the file's own `test_gl_read_pixels_formats_and_bounds`; only each row's pixels are written
  now.
- **The PM4 test fixtures handed `gl_hw_flush` a one-word fence and a one-word canary**
  (2026-09-19), and every flush wrote the GPU clock 8 bytes past the first and seven canary words
  past the second - on the stack, in one of them. The real context allocates a page for each, so
  the library was right and the tests were overwriting their own frames; AddressSanitizer found it
  the same way.
- **A frame of more than 450 triangles drew wrongly on hardware** (2026-09-19). Each triangle's
  vertices go into a slot of a 450-slot ring in the vertex buffer, and the draw that reads them
  runs only when the stream is submitted - but the slot was `triangles_drawn % 450` with nothing
  at the wrap. The 451st triangle of a frame overwrote the first one's vertices before the GPU had
  read them, so the frame drew triangle 451 twice and triangle 1 never, and so on round the ring.
  gl-cube's twelve triangles and gl1-probe's handful never came near it; any real scene does. A
  full ring is now submitted before it is reused - the same step a texture change already takes,
  and the render target persists across it. gl1-probe's new `many-triangles` check draws 600 in
  one frame and counts the first one's pixels.
- **The command buffer could be closed past its end** (2026-09-19). The per-draw room check
  reserved 160 dwords and reserved nothing for the 46 dwords `gl_hw_flush` appends to close a
  stream - two `RELEASE_MEM`s, the `WAIT_REG_MEM`, the readback copy and the padding. The clear
  path reserved 32 for up to 14 of its own. Both now reserve the trailer explicitly
  (`OOPS_GL_DCB_TRAILER_DW`), and the worst case of one draw is itemised where it is checked:
  162 dwords, held as `OOPS_GL_DCB_DRAW_MAX_DW` with slack.
- **`glDepthRange` inside a frame did nothing on hardware until the next frame** (2026-09-19).
  `PA_CL_VPORT_ZSCALE`/`ZOFFSET` were written only by the frame's register table, on the stated
  grounds that "the new range is in force from the next frame" - which is wrong for the way depth
  range is used: narrow it, draw one thing, widen it again, all in one frame (a view weapon drawn
  in front of the world is the classic case). The range is now flagged and re-emitted before the
  next draw. `glPopAttrib(GL_VIEWPORT_BIT)` had the same hole for the viewport *and* the range - it
  set neither flag, while the scissor arm beside it always did. gl1-probe's existing `depth-range`
  check could not see any of it: it samples between its two halves, and a sample submits the
  frame. The new `depth-range-in-frame` check does not.
- **The blend factors defaulted to alpha blending** (2026-09-19). GL's defaults are `GL_ONE` and
  `GL_ZERO` (Mesa `main/blend.c:1148-1151`); these were `GL_SRC_ALPHA` / `GL_ONE_MINUS_SRC_ALPHA`,
  so a program that enabled `GL_BLEND` without calling `glBlendFunc` got a blend here and an
  overwrite everywhere else, and `glGetIntegerv(GL_BLEND_SRC)` was wrong before any call. The one
  test that asserted the old default now asserts GL's.
- **`glBlendFunc`, `glBlendFuncSeparate` and `glBlendEquation` accepted anything** (2026-09-19).
  An enum that was not a factor was stored, reported back, and blended as the old default by
  `gl_blend_op`'s fallback. They now refuse with `GL_INVALID_ENUM` against Mesa's lists
  (`main/blend.c:49-118`, `:435-447`), including `GL_SRC_ALPHA_SATURATE` as a *destination*
  factor, which needs an extension this does not have.
- **`GL_SRC_ALPHA_SATURATE` scaled alpha in the software rasteriser** (2026-09-19). Its alpha
  factor is 1 - softpipe says so in as many words (`sp_quad_blend.c:467-470`) and the colour block
  does it itself - but the software path applied `min(As, 1 - Ad)` to all four channels.
- **The q coordinate was divided at the wrong place** (2026-09-19). `glTexCoord4` divided s, t and
  r by q as it stored them, so `glGetFloatv(GL_CURRENT_TEXTURE_COORDS)` answered s/q where GL
  answers s - and a q produced by **texture generation** was never divided at all, so eye-linear
  projective texturing came out as if q were 1. The current coordinate is now stored as given and
  the divide happens once, at the vertex, after generation (`gl_vertex_texcoord`). A size-4
  texture-coordinate array's q is divided too, where it used to be dropped.
- **Texture generation replaced an array's coordinates with glTexCoord's** (2026-09-19).
  Generating only s kept "what glTexCoord left" for t - even for a vertex drawn from arrays, whose
  t is the array's. Generation now starts from the vertex's own coordinate however it arrived.
- **Queries that answered less than the state held** (2026-09-19).
  `glGetFloatv(GL_CURRENT_TEXTURE_COORDS)` answered the constants 0 and 1 for r and q, from when
  only s and t were tracked. `glGetIntegerv` refused `GL_COLOR_CLEAR_VALUE`, `GL_CURRENT_COLOR` and
  `GL_FOG_COLOR` outright; it now maps them as Mesa does (`FLOAT_TO_INT`, `main/macros.h:111`).
  And `glGetDoublev` - which copies exactly `gl_query_element_count` values - copied one component
  of `GL_FOG_COLOR` and of the three current-raster vectors, leaving three of the caller's four as
  they were.
- **`glPopAttrib` left the 1D texture enable and binding alone** (2026-09-19). The attribute entry
  had carried both fields since 1D textures landed; nothing filled or read them.

- **Shader state was rewritten under draws that had not run yet** (2026-09-17). The pixel shader
  payload is one buffer shared by every draw in a frame, and on hardware a draw is *built* when
  it is issued and executes at the flush. Patching the payload therefore changed the program
  already-built draws would run - and the last thing a program does with a piece of shader state
  is switch it off:

  ```c
  glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f);
  glRectf(...);              // alpha 0.25 - must be discarded
  glRectf(...);              // alpha 0.75 - must survive
  glDisable(GL_ALPHA_TEST);  // <- patches both back to s_nop, before either has run
  ```

  Nothing was discarded, because by the time the GPU read the program there was no test in it.
  That is gl1-probe's `alpha-test` failing on hardware while passing on the host, where a draw is
  finished when it returns - and it is the same shape as the texture storage freed under a built
  frame, fixed earlier the same day. `gl_ps_sync_payload_edit` submits the frame before a patch
  slot is rewritten, and **only when the words actually change**, so an unconditional flush does
  not split a frame on every `glPopAttrib` or on the `glDisable` of a test that was never on.
- **`glTexEnvi` never reached the hardware** (2026-09-17). `tex_env_mode` was read only by the
  software rasteriser, so the textured shader multiplied the texel by the interpolated colour
  whatever the mode said. That is invisible whenever the colour is white - which is why gl-cube
  never showed it and gl1-probe's `tex-env-modes` did, drawing one white texel under `GL_REPLACE`
  and `GL_MODULATE` with a dark red colour and requiring the two to differ.

  The mode is now four instructions patched into the textured shader's combine slot.
  `GL_MODULATE` is the multiply it is assembled with; `GL_REPLACE` is four `s_nop`, because the
  texel already sits in the registers the export reads and RGBA replace is exactly `C = Cs,
  A = As`. **`GL_ADD` and `GL_DECAL` fall back to modulate rather than to something plausible**:
  add needs `v_add_f32` on three channels and decal is a lerp by the texel's alpha, three
  instructions per channel against a four-word slot. Neither encoding has been read back from an
  assembler, and an instruction word here is measured rather than remembered (`tools/shader/`),
  so they wait for that. The software rasteriser still honours all four.
- **Every textured draw in a frame shared one descriptor slot** (2026-09-17). The image and
  sampler descriptors are copied to a single address and each draw hands the shader that same
  address, so a second texture bound later in the frame overwrote the first and, at the flush,
  every textured draw sampled whichever texture was bound last. No probe check had caught it
  because each one uses a single texture, but any scene with two would have rendered the wrong
  one. The frame is now submitted when a *different* texture is bound - rebinding the same one,
  which a display list does constantly, still costs nothing. A ring of slots like the vertex
  buffer's would avoid the flush and is the change to make if it ever shows up in a profile.
- **Blending was bypassed in the colour target, not just misconfigured** (2026-09-17).
  `CB_COLOR0_INFO` carried `BLEND_BYPASS` (bit 16) set, inherited from the measured
  primitive-draw recipe. The bypass is read ahead of the blender, so it made
  `CB_BLEND0_CONTROL` irrelevant however correct it was - the entry below fixed that register
  and gl1-probe came back **26/34 for the third time, unchanged**, which is what sent the search
  past it. Mesa derives the two bits together and never emits this combination:
  `mesa/src/amd/common/ac_descriptors.c:1426-1438` sets `blend_clamp` for NORM/SRGB types and
  `blend_bypass` only for UINT/SINT or the `8_24`/`24_8`/`X24_8_32_FLOAT` formats, clearing
  `blend_clamp` when it does. An `8_8_8_8` UNORM target gets clamp set and bypass clear; ours
  had **both**, which that code cannot produce.

  The gl-cube oracle record pinned all 32 bits of this register while its stated record covered
  only `COMP_SWAP`, so it failed on the change. It now asserts `COMP_SWAP`, `BLEND_BYPASS` and
  `BLEND_CLAMP` by name *and* the whole word - more facts, not fewer, and a regression that says
  which bit moved instead of printing two integers.
- **`glScissor` never reached the hardware** (2026-09-17). All four scissor rectangles were
  patched to the render target's extent and `cap_scissor_test` was read only by the software
  rasteriser, so the box was silently ignored on the console - the same shape of bug as the
  viewport, found the same way, by a probe check that passed on the host and failed on hardware.
  `PA_SC_VPORT_SCISSOR_0_TL/_BR` (`0x094`/`0x095`, offset `0x028250` per
  `mesa/src/amd/registers/gfx103.json`) now carry GL's box, flipped against the target height
  because GL measures from the bottom-left, and clamped to the target because the fields are
  unsigned and a box hanging off the left would otherwise wrap. A change after the frame opened
  re-emits before the next draw, as the viewport does. The screen, window and generic rectangles
  are surface bounds and stay at the full extent.
- **Texture rows were written at the image width and sampled at a wider pitch** (2026-09-17).
  A linear image's rows now sit at a 256-byte pitch - 64 pixels - and the descriptor carries
  that pitch when it exceeds the width (`SQ_IMG_RSRC_WORD4`: `DEPTH` bits 0-12, `PITCH_MSB`
  bit 13, both as `pitch - 1`, per `mesa/src/amd/registers/gfx10-rsrc.json:401-406` and
  `ac_descriptors.c:711-712`).

  The note this replaces recorded a 2026-09-14 measurement that the pitch comes from the width,
  and closed with "widths that are not a multiple of 64 pixels are unmeasured". **That caveat
  was the answer.** At 64 pixels the row *is* 256 bytes, so a texture that wide cannot tell the
  two apart - and the texture that measurement used was 64 wide, as gl-cube's still is. gl1-probe
  supplies the missing width: its textures are 2x2, and its hardware results split exactly where
  a too-narrow pitch predicts - `texture-wrap` passes while `texture-2d`, `tex-env-modes`,
  `copy-tex` and `tex-sub-image` fail, and `texture-wrap` is the only one of the five that holds
  `t` constant and so never samples a row past row 0.

  Rounding up cannot disturb what already works: at any width that is a multiple of 64 the pitch
  equals the width, the descriptor field stays inert, and the bytes land exactly where they did.
  The software sampler indexed rows by width and now uses the pitch. Whether the hardware's
  *default* pitch is this value, and whether word 4 is honoured at all, comes out of addrlib and
  is not readable from Mesa's source - obSCEne `REQ-20260917T1605Z-8b12` asks the hardware.
- **Texture storage was only zeroed when no pixels were supplied** (2026-09-17). An upload writes
  `width` pixels into a row `pitch` wide, so the padding between them was left as the allocator
  returned it - uninitialised heap on the host, whatever GARLIC last held on the target. Both
  branches now zero unconditionally before unpacking, and the target flushes the zeroing to
  write-combined memory whether or not an upload followed.
  `test_gl_copy_tex_sub_image_matches_a_read_then_upload` caught this within one run of widening
  the pitch: two textures holding identical images compared unequal in the gap between rows.
- **The GPU was told the right blend factors and never told to blend** (2026-09-17).
  `CB_BLEND0_CONTROL` was the constant `0x00002504` whenever `GL_BLEND` was enabled, so
  `glBlendFunc` and `glBlendEquation` reached the software rasteriser and never the hardware.
  Decoding that constant against the field layout says more than "it was a constant": its two
  colour factors were **right** - `COLOR_SRCBLEND` 4 and `COLOR_DESTBLEND` 5, exactly GL's
  default `SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA` - while **bit 30, `ENABLE`, was clear**, and bit 13
  was set where the register has no field at all. The colour block had the right recipe and no
  instruction to use it.

  Now computed from the GL state by `gl_compute_cb_blend_control()`, with every field position
  and both enums taken from `oops-mesa/mesa/src/amd/registers/gfx103.json` and its `gfx10.json`
  base, which agree. `GL_MIN` and `GL_MAX` hold the factor fields at `BLEND_ONE`, since the
  equation ignores them and leaving whatever `glBlendFunc` last said would be noise.
  `SEPARATE_ALPHA_BLEND` is always set because oops-gl tracks a separate alpha pair.

  Two mutation checks guard it: clearing `ENABLE` reproduces the original bug and fails, and
  swapping `COMB_SRC_MINUS_DST` with `COMB_DST_MINUS_SRC` - GL's `FUNC_SUBTRACT` against
  `FUNC_REVERSE_SUBTRACT`, which negate each other - fails. The gl-cube oracle is unaffected: it
  never enables `GL_BLEND`, so the register stays zero and the recorded stream is unchanged.
- **`glViewport` did nothing on the hardware path** (2026-09-17). `PA_CL_VPORT_XSCALE`,
  `XOFFSET`, `YSCALE` and `YOFFSET` were computed from the render target's width and height, so
  the GPU mapped NDC across the whole surface whatever the viewport said. The software rasteriser
  has always honoured it, so the two paths disagreed - and **nothing on the host could see it**,
  because gl1-cube sets the viewport to exactly the framebuffer size, which is the one case where
  the hardcoded values are correct.

  gl1-probe's first full hardware run is what found it, and the shape of the result named the
  cause: every check that sampled a pixel and compared it to an expected colour failed, while the
  two that compare one frame against another - `array-paths` and `type-variants` - passed,
  because both frames were displaced identically. Pure query checks passed too.

  The four registers now come from `gl_compute_vport()`, whose formulas **generalise the previous
  constants rather than replacing them**: a viewport covering the whole target reduces to `w/2`,
  `w/2`, `-h/2`, `h/2`, exactly what was there, so the gl-cube oracle frame is unchanged. A
  viewport set *after* a frame's registers are written raises `hw_vport_dirty` and the next draw
  re-emits the four as one packet; a frame whose viewport was already current emits nothing extra,
  which keeps gl-cube's recorded stream byte for byte.
- **Deleting a texture freed GPU memory a built frame still pointed at, and took the GPU down**
  (2026-09-17). The hardware path is deferred: `glDrawArrays` writes the texture's *address* into
  the command buffer and returns, and nothing executes until the flush. `glDeleteTextures` -
  and `glTexImage2D` re-specifying an existing texture - freed the GARLIC storage immediately, so
  a program that drew, deleted, then flushed handed the sampler unmapped pages.

  **The specification is on the caller's side here**: deleting a texture still in use is legal,
  and the implementation owes the storage a lifetime long enough for the draws that name it.
  gl1-probe's texture check does exactly that, and the console answered:

  ```
  GPU Protection fault. client:TCP(8) access:Read permission:0x3
  reason: Unmapped page access, Protection fault addr(VA): 0x0000000203190000
  504 wavefronts ... XNACK_ERROR MEMVIOL
  ```

  `TCP` is the texture cache. The GPU was reset and the user interface restarted, and every check
  after the ninth was lost. Both release sites now flush an active frame before freeing, which
  costs a submission only when a program changes texture storage mid-frame. A deferred-free list
  keyed on the fence would cost less and is worth having when something needs it.

  **This is invisible on the host** and always will be: the software rasteriser draws at the call
  rather than at a flush, so there is no window in which to free anything. gl1-probe's
  `tex-delete-in-frame` check passes on the host whatever the implementation does; its whole
  value is on a console.
- **`glReadPixels` did not flush, so on hardware it read the frame before last** (2026-09-17).
  The specification requires it to reflect everything issued before it, and on the hardware path
  that is not free: drawing builds a command stream and returns, so without a flush the call read
  a target the GPU had not been told to draw into yet - reporting the previous frame, or, before
  any frame, the buffer as the display left it. It also made the readback copy it prefers one
  frame stale, since that copy is filled by the very submission it was not waiting for.
  `glCopyTexSubImage2D` and `glCopyTexImage2D` read through `glReadPixels`, so both inherited it.
  On the host the flush costs nothing beyond closing an open `glBegin`, which the specification
  also wants.
- **oops-gl kept the wrong error: the last one, where GL says the first** (2026-09-16). The
  specification is explicit that once the error flag is set nothing further is recorded until
  `glGetError()` reads and clears it. All 26 sites assigned the field directly, so a caller
  making several calls before one `glGetError()` was shown the most recent failure and never the
  one that started it - which points at the wrong call, and pointing at the right call is the
  only thing an error code is for. They now go through one helper that keeps the first.
- **`glDrawElements` read past the end of an index array on an unrecognised type**
  (2026-09-16). The index reader selects 16-bit and 8-bit explicitly and treats *everything
  else* as 32-bit, so a type it did not recognise did not merely return nonsense - it read four
  bytes per index out of an array the caller had sized for one. The type is now checked before
  the reader runs.
- **`glGetString(GL_VERSION)` claimed a version this is not, in a form nothing could parse**
  (2026-09-16). It read `"OpenGL 1.3 oops-gl 2.0"`: the wrong number, and with a word in front
  of it, so the conventional `atof()` on that string returned `0.0` rather than any version at
  all. It now reads `"1.1 oops-gl fixed-function subset"`, which begins with the version as the
  specification requires. `GL_RENDERER` no longer carries a vendor brand (conventions §2), and
  an unrecognised name returns `NULL` with `GL_INVALID_ENUM` rather than an empty string that
  reads as a real answer of "none".
- **The README described a repository that did not exist.** It documented a `lib/` directory
  holding a vendor tiler archive, a `.cpp` source that is a `.c`, one subsystem of eight, and
  two integration variables - `OOPS_SDK_INC` and `OOPS_SDK_LDFLAGS` - that the makefile
  helper does not define. Make does not warn on an undefined variable, so following the
  README produced empty include and link flags **silently**.
- **`.gitignore` carried a `!lib/*.a` negation** whose only effect would have been to let a
  vendor archive be committed. Removed, along with the documented slot for one.
