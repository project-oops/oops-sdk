# oops-gl: the road to OpenGL 1.x and 2.x

What is done, what is left, and **what each remaining piece actually costs** - which is the part
that is expensive to work out twice. D008 set the goal; this is the map.

Counted, not estimated: **187 declared entry points** as of 2026-09-17.

## The gap, measured

Everything below this line used to be a list of what I remembered was missing. On 2026-09-17 it
was replaced with a diff: the entry points Mesa's `include/GL/gl.h` declares (GL 1.0-1.3, 455 of
them) against the ones `include/GL/gl.h` here declares.

| | 2026-09-17, morning | 2026-09-17, evening |
|---|---|---|
| Present in both | **161** | **350** |
| Missing | **294** | **108** |

The morning column is what this document described for most of its life. The evening one is after
a day of filling it in, and the remaining 108 are no longer a mixture:

- **101** are families this deliberately does not have, and mostly should not: the imaging subset
  (colour tables, convolution, histogram, minmax), evaluators and the `glMap*` surface grid,
  selection and feedback mode, the accumulation buffer, colour-index mode, polygon and line
  stipple, `glPixelMap`/`glPixelTransfer`.
- **7** are genuinely wanted and genuinely not done: `glTexImage3D`, `glTexSubImage3D`,
  `glCopyTexSubImage3D`, and `glFogf`/`glFogfv`/`glFogi`/`glFogiv`.

**Not all of the 101 are worth having.** The imaging subset, evaluators, selection/feedback, the
accumulation buffer and colour-index mode were already unusual when this hardware's ancestors were
new. They are listed so the gap is honest, not because they are planned.

> **The counts come from a diff, not a memory**, and the method is worth stating because the
> numbers move: `grep -oE '\bgl[A-Z][A-Za-z0-9_]*'` over Mesa's `include/GL/gl.h` and over this
> one, sorted and compared. Mesa's side is 458 names by that measure; 24 names exist only here -
> the context calls, the GL 1.5 buffer objects and the GL 2.0 shader surface - and are not part
> of either column.

## An absent entry point is not absent

**`-Wl,--unresolved-symbols=ignore-all` is in the payload link.** A program calling a GL entry
point this does not declare therefore links cleanly and calls **address zero** at run time: a
`SIGSEGV` at `rip: 0x0000000000000000`, with no build diagnostic and nothing in the log naming the
symbol. That is not a theoretical reading - it is the crash signature that was familiar all day
from a different cause.

So D009's "an absent feature is an absent symbol" does not hold its usual meaning here. Omitting an
entry point does not produce a clean failure; it produces the worst kind. Where a refusal is
*conformant* - one texture unit, an empty compressed-format set - this now declares the entry point
and refuses, which is strictly better than a crash and is what the specification asks for anyway.

**Whether to extend that to features which are simply unimplemented - 3D textures and fog - is an
open decision and is deliberately not taken here.** Declaring them and returning `GL_INVALID_ENUM`
would turn a null jump into a readable error; leaving them out keeps D009 literal. The evidence is
recorded so the choice can be made rather than drifted into.

## Done

| Feature | Notes |
|---|---|
| Immediate mode, vertex arrays | `glBegin`/`glEnd`, `glDrawArrays`, `glDrawElements` |
| One 2D texture unit | upload, sub-image, parameters, environment |
| Lighting and materials | eight lights, `glColorMaterial` |
| Matrix stacks | model-view, projection, texture |
| Blend, depth, cull, scissor, colour mask | |
| **Display lists** | records calls, not effects; array draws refuse to compile |
| **`glTexSubImage2D`, `glPixelStorei`** | unpack alignment and row length |
| **`glDepthRange`** | `(far-near)/2`, `(far+near)/2` into the viewport registers |
| **`glReadPixels`, `GL_PACK_ALIGNMENT`** | flips to GL's bottom-left origin |
| **`glPolygonOffset`** | registers and scaling cited from Mesa |
| **`glAlphaFunc`, `GL_ALPHA_TEST`** | a discard patched into both pixel shaders |
| **`glCopyTexSubImage2D`** | reads through `glReadPixels`, so it inherits the flip |
| **`glPushAttrib` / `glPopAttrib`** | saves all, restores what the mask names; unsupported groups refused |
| **`glPushClientAttrib` / `glPopClientAttrib`** | its own stack, so an inner client pop cannot take an outer server frame |
| **`glRect*`** (8 forms) | the spec's four-vertex polygon, drawn as `GL_QUADS` - for four convex vertices, the same two triangles |
| **`glCopyTexImage2D`** | allocates, then copies through `glCopyTexSubImage2D`, so one piece of code owns the flip |
| **The other spellings** | `glVertex`/`glColor`/`glTexCoord`/`glNormal` in `d`, `i`, `ub` and their `v` forms; `glTranslated`, `glRotated`, `glScaled`, `glLoadMatrixd`, `glMultMatrixd`. Each converts and forwards to its `f` sibling, so an attribute has one implementation |
| **The query family** | the limits (`GL_MAX_LIGHTS`, the stack depths, `GL_MAX_TEXTURE_SIZE`), the stack positions and the state; `glGetDoublev` added; an unanswerable `pname` now **refused** rather than leaving the caller's buffer as it found it |
| **The per-object queries** | `glGetTexParameter*`, `glGetTexLevelParameter*`, `glGetLight*`, `glGetMaterial*`, `glGetPointerv` - each answering from the field its own setter writes |
| **`glGetTexImage`** | the bound texture read back. **Does not flip** - a texture has no window origin, unlike `glReadPixels` |
| **`glInterleavedArrays`** | all fourteen formats; offsets checked against Mesa's `_mesa_get_interleaved_layout` rather than derived |
| **Buffer objects (GL 1.5)** | `glGenBuffers`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glDeleteBuffers`, `glIsBuffer`, `glGetBufferParameteriv`; both binding points, and `glDrawElements` reads its indices from one. An array keeps the buffer's **name**, resolved at draw time, so `glBufferData` may reallocate under it |
| **`glArrayElement`** | one vertex out of the enabled arrays, inside `glBegin`/`glEnd`; a disabled array falls back to the current attribute |
| **`glDrawRangeElements`** | the range is a hint this ignores, which the specification allows; `start > end` is still refused |
| **Integer lighting forms** | `glLighti*`, `glMateriali*`, `glLightModeli*`. **A colour converts by range, not by cast** - `INT_TO_FLOAT` per Mesa, so `GL_AMBIENT` with `INT_MAX` is 1.0 while a position is a plain cast |
| **Transpose matrices** | `glLoadTransposeMatrix{f,d}`, `glMultTransposeMatrix{f,d}` |
| **Residency** | `glAreTexturesResident`, `glPrioritizeTextures` - nothing is ever evicted, so everything that exists is resident and an unknown name is an error rather than a "no" |
| **The rest of the type-and-arity grid** | every remaining `glVertex`/`glColor`/`glTexCoord`/`glNormal` spelling. **Integer colours and normals normalise; positions and texture coordinates do not** - conversions from Mesa's `macros.h`, and normals use the same macros as colours rather than a plain cast. `glTexCoord4`'s `q` is a projective divide, which widened the current texture coordinate to four components |
| **Multitexture, one unit** | `glMultiTexCoord*`, `glActiveTexture`, `glClientActiveTexture` and their `*ARB` spellings. `GL_MAX_TEXTURE_UNITS` reports 1 and a unit above `GL_TEXTURE0` is `GL_INVALID_ENUM`, which is what the specification requires of a one-unit implementation |
| **Texture coordinate generation** | `glTexGen*`, `glGetTexGen*`, the four enables. Object-linear, eye-linear and sphere map; the cube-map modes refused. `GL_EYE_PLANE` is stored through the inverse modelview of the moment it was set, which needed a full 4x4 `mat4_invert` |
| **User clip planes** | `glClipPlane`, `glGetClipPlane`, `GL_CLIP_PLANE0..5`. Two registers and no shader change; **three coordinate spaces**, object in, eye stored, clip in the register |
| **Stencil (software)** | `glStencilFunc`, `glStencilOp`, `glStencilMask`, `glClearStencil`, and `GL_STENCIL_BUFFER_BIT` on the attribute stack. The hardware surface is outstanding - see below |
| **Raster position and pixel operations** | all 24 `glRasterPos*` spellings, `glDrawPixels`, `glBitmap`, `glCopyPixels`, `glPixelZoom`. An invalid raster position draws **nothing**; `glBitmap` moves the position even when it draws nothing |
| **1D textures** | `glTexImage1D`, `glTexSubImage1D`, `glCopyTexImage1D`, `glCopyTexSubImage1D`. **Its own binding point**, its own enable and its own default texture, with 2D winning when both are enabled |
| **Compressed textures** | the seven entry points, with **no formats**: `GL_NUM_COMPRESSED_TEXTURE_FORMATS` is 0 and every upload is refused, which is what the specification says an implementation with an empty format set does |

## Left in 1.x, with what it costs

### Cheap and additive - no hardware risk

Nothing in this group touched a register or a shader, so the cost was writing it.

**This group is now empty**: the type-and-arity grid, multitexture on one unit, texture
generation, the raster position family, 1D textures and the compressed-texture refusals all
landed on 2026-09-17 and have moved to **Done** above.

> The paragraph that stood here said the ~100 genuinely distinct features were "waiting to be
> decided on, and most of them should stay absent", and listed `glStencil*`, `glBitmap` and
> `glTexGen*` among them. Four of those were decided the other way and are implemented; the
> judgement about the imaging subset, evaluators, selection/feedback, the accumulation buffer and
> colour-index mode stands. The list was a snapshot of what had not been costed yet, not a ruling,
> and reading it as one is how a gap stays a gap.

### Not cheap, despite looking it - and now done

**`glTexImage1D`** was on the cheap list and should not have been. A 1D texture is not a
height-1 2D texture as far as the API is concerned: `GL_TEXTURE_1D` is **its own binding
point**, so `glBindTexture(GL_TEXTURE_1D, n)` and `glBindTexture(GL_TEXTURE_2D, m)` are both
live at once and `glEnable(GL_TEXTURE_1D)` is a separate switch that 2D overrides. Uploading a
1D image into the 2D slot would give a texture that stores correctly and **never samples**,
which is the silent-success failure this port refuses everywhere else.

The cost was as described - a second binding point, a second enable, and the priority rule in the
sampler path and the descriptor build - and it was spent on 2026-09-17. The upload, sub-upload and
copy bodies turned out to be target-agnostic already, so they became shared helpers and only the
public `*2D` entry points validate.

It also uncovered something this paragraph did not predict: **the default 2D texture was object 1,
and `glGenTextures` counts up from 1.** A program that generated a texture and then uploaded with
nothing bound wrote into its own. Each target now has a reserved default id above anything counting
reaches.


### Needs a shader-interface change - real risk to the oracle frame

**`glFog*`**. RDNA2 has no fixed-function fog, so it is a per-pixel blend towards the fog colour
in the pixel shader, like the alpha test. Unlike the alpha test it needs something the shader
does not currently receive: **the fragment's depth**.

`SPI_PS_INPUT_ENA` and `SPI_PS_INPUT_ADDR` are both `0x2` - `PERSP_CENTER_ENA` and nothing else -
so the pixel shader gets the two barycentrics in `v0`/`v1` and no position at all. Getting depth
means one of:

1. Enabling `POS_Z` in `SPI_PS_INPUT_ENA`, which **adds VGPRs after the barycentrics** and so
   renumbers everything above them. The colour currently lives in `v4`..`v7` and would collide.
2. Having the vertex shader export an extra parameter and interpolating it, which changes
   `SPI_VS_OUT_CONFIG`, `SPI_PS_IN_CONTROL` and the parameter-cache allocation with it.

Either way the shader interface moves - and **the shaders here are hand-written binaries**, so
moving it means rewriting them, not recompiling them.

`test_pm4_gl_honours_the_gl_cube_oracle_record` now pins `SPI_VS_OUT_CONFIG`,
`SPI_PS_IN_CONTROL`, `SPI_PS_INPUT_ENA` and the two `SPI_PS_INPUT_CNTL` slots, so a change to
any of them is a build failure rather than a bad frame. **That test failing is the point**, not
an obstacle: it means the recorded frame has to be re-recorded on hardware, which needs a
console.

> This document previously claimed those registers were already pinned. On 2026-09-17 that was
> checked and they were not - by this test or any other. The safety net described here did not
> exist until the assertions were added; a shader-interface change would have passed the whole
> suite and gone wrong only on hardware. `DB_Z_INFO` and the colour-surface registers *were*
> pinned throughout, as the stencil paragraph below says.

**Stencil** (`glStencilFunc`, `glStencilOp`, `glStencilMask`, `glClearStencil`) **is implemented
in software as of 2026-09-17** - the test, all six operations, the write mask, and the ordering
against the alpha and depth tests. Only the hardware path is outstanding.

> This paragraph used to say stencil "has the same shape one level down: the depth buffer is
> `Z_32_FLOAT` with no stencil plane, so it needs a different depth format and a reallocated
> buffer, and `DB_Z_INFO` is pinned by that test." **That was wrong on this part.** Stencil is a
> *separate surface* with its own registers - `DB_STENCIL_INFO` at context offset `0x011`,
> `DB_STENCIL_READ_BASE` at `0x013`, `DB_STENCIL_WRITE_BASE` at `0x015`, `DB_STENCILREFMASK` at
> `0x10C` (`mesa/src/amd/registers/gfx103.json`) - so `Z_32_FLOAT` having no stencil plane costs
> nothing and `DB_Z_INFO` does not have to move. The depth block already writes every one of
> those registers, zeroed. What is missing is a surface and the `DB_STENCIL_INFO` value that
> turns it on, which is obSCEne `REQ-20260917T1845Z-3d5b`.

**Multitexture** (`glActiveTexture`, `glClientActiveTexture`, `glMultiTexCoord*`) is the same
blocker again, and this is what it costs, measured rather than guessed:

- A second texture unit needs a **second interpolated texture coordinate set**, which is a third
  parameter export. `SPI_VS_OUT_CONFIG` goes from `VS_EXPORT_COUNT=1` to `2`,
  `SPI_PS_IN_CONTROL` from `NUM_INTERP=2` to `3`, and `SPI_PS_INPUT_CNTL_2` appears.
- The vertex shader binary has to export it and the pixel shader binary has to sample twice and
  combine per unit 1's texture environment. Both are hand-written.
- The descriptor table already has room - `gl_draw.c` copies one 32-byte image descriptor and
  one 16-byte sampler into it, and the pixel shader takes the table in `s[0:1]`.

So the API surface is large (the measurement above counts ~50 `glMultiTexCoord` spellings and 34
`*ARB` aliases) but the *real* work is two shader rewrites and a re-record - the same console
dependency as fog. It is not the "next big software item" it looked like from the entry-point
count alone.

### Costed, waiting on one measurement - clip planes since implemented

> **Implemented 2026-09-17.** The costing below held: two registers and no shader change. Both
> paths are written - the software rasteriser interpolates a signed distance per plane and
> discards per fragment, ahead of the depth test - and gl1-probe has a `clip-plane` check, so the
> next hardware run answers on **oops-gl's own stage**. That matters because the question below
> stayed open through three obSCEne measurements, every one of which rebuilt the stage by hand
> with the passthrough `VGT_SHADER_STAGES_EN` rather than the `0x00c12010` this programmes. The
> register offsets and the third coordinate space the hardware wants are in the CHANGELOG entry.

**User clip planes** (`glClipPlane`, `glGetClipPlane`, `GL_CLIP_PLANE0..5`) looked like a
vertex-stage rewrite and are not. Mesa's `si_emit_clip_regs`
(`src/gallium/drivers/radeonsi/si_state.c`) takes the fixed-function path whenever the vertex
shader exports no clip distances - which is exactly this case, the shaders here being
hand-written and exporting position plus two parameters:

```c
if (!vs->selector->info.has_clip_outputs && !vs->info.clipdist_mask) {
    ucp_mask = SI_USER_CLIP_PLANE_MASK & rs->clip_plane_enable;
}
```

So it is two registers and no shader change:

| Register | Byte address | Context offset | What |
|---|---|---|---|
| `PA_CL_UCP_0_X` | `0x0285BC` | `0x16F` | six planes, four floats each, 24 consecutive registers |
| `PA_CL_CLIP_CNTL` | `0x028810` | `0x204` | `UCP_ENA_0..5` in bits 0..5 |

Offsets and field positions from `oops-mesa/mesa/src/amd/registers/gfx103.json` - the `gfx103`
family file, not a general one.

**What stops it being written today**, corrected on 2026-09-17. This paragraph used to say the
pipeline runs NGG in *passthrough* (`VGT_SHADER_STAGES_EN = 0x02002000`) and reasoned from there.
**It does not**: `gl_draw.c:657` programmes `0x00c12010`, which is non-passthrough NGG. The same
confusion - obSCEne's fixture uses the passthrough value, oops-gl does not - has already cost two
probe resolutions that measured the wrong stage, so it is written down here rather than left as
something to rediscover.

What is actually known, from `REQ-...-8c4d` re-filed as `REQ-...-b2c7` and resolved on
2026-09-17: writing `PA_CL_UCP_0_X` and setting `UCP_ENA_0` does **not** stall the geometry
engine - all three arms produced byte-identical hardware outcomes. But all three were identical
*failures*: nothing drew in any of them, because the console fired a `BIG_APP` power transition
whose ShellUI preemption ran past 363 ms against the probe's 200 ms canary timeout. So the
negative is sound and the positive is still unmeasured, and **whether user clip planes clip is
open**. Re-asked as `REQ-...-c3f1`, which is the same probe with a timeout that outlasts a
power transition.

### Measured shut

**Points and lines**, and therefore **`glPolygonMode`**'s wireframe. Not a gap - a measured
negative. obSCEne submitted both on retail hardware across five sweeps, including one using
**oops-gl's own `VGT_SHADER_STAGES_EN` value** (`0x00c12010`, sweep `20260917-010918`,
`REQ-...5a3e`), and every run recorded `fence-hit 0` and `pixel-hit 0`: the pipe stops rather
than the primitives drawing wrongly. The same path with three vertices retires and draws, so the
geometry engine wants three vertices per primitive.

They need a stage this does not build. That is a real piece of work and not a register value.

## 2.0, and why it is last

OpenGL 2.0 is GLSL: `glCreateShader`, `glShaderSource`, `glCompileShader`, `glLinkProgram`,
`glUseProgram`, uniforms and vertex attributes - and behind them a front end turning GLSL source
into the RDNA2 bytecode this emits by hand. That is the largest single piece of work in this
repository and the reason D007 originally pointed at Mesa.

**Started, from the only end that can be finished today.** `src/gl/glsl_lex.c` is the GLSL 1.10
lexer: pure text handling, so it is testable to the same standard as everything else here. What
sits behind it is not, and the staging is worth being explicit about:

| Stage | Can be finished now? |
|---|---|
| Lexer | **done** - text in, tokens out, nothing hardware-shaped about it |
| Expression parser and AST | **done** - the full 1.10 precedence ladder, arena-allocated, indices not pointers |
| Declarations and statements | **done** - blocks, selection, iteration, jumps, declarators, functions, translation unit |
| Preprocessor | **done** for what 1.10 shaders use - `#version`, object-like macros, `#ifdef` nesting. `#if`, function-like macros and `#extension` refused by name |
| `struct`, and the type-name ambiguity | not started - `starts_declaration` decides on one token today, which a user-defined type name will break |
| Types, scopes, expression checking | **done** - operator rules, swizzles, constructors, shadowing |
| Statement checking, function signatures, l-values | **done** - the front end is complete for what 1.10 shaders use |
| `struct`, arrays beyond a size, built-in functions | not started; all software, none of it blocked |
| Code generation | **blocked, and no longer on obSCEne.** `REQ-...-f9d3` resolved on 2026-09-17: on oops-gl's own non-passthrough NGG stage (`0x00c12010`) a synthetic probe's inline vertex assembly never launches a wavefront (`canary-vs 0xaaaaaaaa`) without the ring descriptors and user-SGPR bindings oops-gl configures - so the question cannot be asked in isolation and re-filing it would ask the same thing a third time. Three parameter exports now have to be measured **from a payload of ours on a console**, which makes gl2-cube's first hardware run the gate rather than a probe sweep |
| Instruction encoding | **started** - `src/gl/glsl_emit.c`. VOP1/VOP2 formats, the constant forms, and `mat4 * vec4`, every field verified against clang's own output |
| Instruction selection from the AST | **started** - `src/gl/glsl_gen.c`. A value is consecutive VGPRs, one per component, `mat4` column-major; a bump allocator with a per-statement mark; `+ - *` component-wise, scalar broadcast either way round, `mat4 * vec4`, unary minus, swizzle reads, `vecN`/`matN` constructors (and `matN(s)` fills the **diagonal**, which is a different rule from `vecN(s)`), assignment, declarations with initialisers, blocks. Everything else - `/`, integer arithmetic, comparisons, calls, swizzle writes, compound assignment - sets `error` and emits nothing, because an instruction whose encoding has not been read out of an assembler is not guessed |
| Anything about the shader *interface* | **no** - varyings are parameter exports, and the export count is fixed at two. `REQ-...-3a91` and its re-file `REQ-...-f9d3` both came back unable to answer it from a synthetic probe, so the measurement has to come from one of our own payloads |

So the back end is gated on the same measurement fog, stencil and multitexture are. The front
end is not, and none of it is wasted whichever way that measurement goes.

Nothing in 1.x is wasted when it arrives: a GL 2.0 context still has to answer every 1.x call.

## How shader words get written here

Not from memory. `tools/shader/` holds the source, it is assembled for gfx1030, and the words go
into the C with the instruction beside them. A wrong encoding cannot fail loudly - it assembles
into the payload, the hardware does something else, and the frame is wrong rather than the build
stopped. See `tools/shader/README.md`.
