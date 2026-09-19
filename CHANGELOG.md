# Changelog

oops-sdk publishes **no artifact**. It is consumed as a sibling checkout by the payloads in
the collection, which link the archive they built from it - so there is no version and no
release: the commit a consumer was built against is the only version that means anything.

Entries are grouped **Added / Changed / Fixed**, newest first.

Nothing has shipped yet - this is the initial commit.

## [unreleased] - as of 2026-09-03

### Added

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
  on the console until it is. The likely route does not need the third parameter export `-7c40`
  confirmed: the vertex buffer's texture coordinate is a `vec4` whose `z` is unused and which the
  vertex shader already exports whole, so a per-vertex fog factor computed on the CPU can ride there
  and only the pixel shaders change.
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
