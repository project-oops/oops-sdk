# Changelog

oops-sdk publishes **no artifact**. It is consumed as a sibling checkout by the payloads in
the collection, which link the archive they built from it - so there is no version and no
release: the commit a consumer was built against is the only version that means anything.

Entries are grouped **Added / Changed / Fixed**, newest first.

Nothing has shipped yet - this is the initial commit.

## [unreleased] - as of 2026-09-03

### Added

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
