# oops-gl: the road to OpenGL 1.x and 2.x

What is done, what is left, and **what each remaining piece actually costs** - which is the part
that is expensive to work out twice. D008 set the goal; this is the map.

Counted, not estimated: **187 declared entry points** as of 2026-09-17.

## The gap, measured

Everything below this line used to be a list of what I remembered was missing. On 2026-09-17 it
was replaced with a diff: the entry points Mesa's `include/GL/gl.h` declares (GL 1.0-1.3, 455 of
them) against the ones `include/GL/gl.h` here declares.

| | |
|---|---|
| Present in both | **161** |
| Missing | **294** |

That number is much less alarming than it looks, and saying why is the point of measuring it:

- **~160** are further *spellings* of things that exist - `glColor3b`, `glVertex4s`,
  `glTexCoord3i` and the rest of the type-and-arity grid. Cheap whenever they are wanted.
- **34** are `*ARB` aliases of the multitexture calls.
- The remaining **~100** are genuinely distinct features, and they cluster: the imaging subset
  (colour tables, convolution, histogram, minmax), evaluators and the `glMap*` surface grid,
  selection and feedback mode, the accumulation buffer, colour-index mode, `glBitmap` /
  `glDrawPixels` / `glCopyPixels`, raster position, polygon and line stipple, texture coordinate
  generation, user clip planes, 1D and 3D textures, compressed textures, multitexture, fog and
  stencil.

**Not all of those are worth having.** The imaging subset, evaluators, selection/feedback, the
accumulation buffer and colour-index mode are features that were already unusual when this
hardware's ancestors were new. They are listed so the gap is honest, not because they are
planned.

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

## Left in 1.x, with what it costs

### Cheap and additive - no hardware risk

Nothing in this group touches a register or a shader, so the cost is writing it.

Not much, and that is the point of having measured. What is left in this group is the tail of
families already here rather than anything structural.

**Note the ~100 "genuinely distinct features" in the gap above are mostly not in this group.**
Per D009 an absent feature is an absent symbol, so `glFog*`, `glStencil*`, `glAccum*`, the
evaluators, selection and feedback, the stipples, `glBitmap` and `glTexGen*` are not entry
points waiting to be written - they are features waiting to be decided on, and most of them
should stay absent.

### Not cheap, despite looking it

**`glTexImage1D`** was on the cheap list and should not have been. A 1D texture is not a
height-1 2D texture as far as the API is concerned: `GL_TEXTURE_1D` is **its own binding
point**, so `glBindTexture(GL_TEXTURE_1D, n)` and `glBindTexture(GL_TEXTURE_2D, m)` are both
live at once and `glEnable(GL_TEXTURE_1D)` is a separate switch that 2D overrides. Uploading a
1D image into the 2D slot would give a texture that stores correctly and **never samples**,
which is the silent-success failure this port refuses everywhere else.

The real cost is a second binding point in the context, a second enable, and the priority rule
between them in the sampler path and in the hardware descriptor build. That is a day's work,
not a forwarder - so it sits here until it is worth spending.


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

**Stencil** (`glStencilFunc`, `glStencilOp`, `glStencilMask`) has the same shape one level down:
the depth buffer is `Z_32_FLOAT` with no stencil plane, so it needs a different depth format and
a reallocated buffer, and `DB_Z_INFO` is pinned by that test.

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

### Costed, waiting on one measurement

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

**What stops it being written today** is that this pipeline runs NGG in *passthrough*
(`VGT_SHADER_STAGES_EN = 0x02002000`). The clipper sits in the primitive assembler, downstream
of the geometry engine, so on Mesa's model passthrough should not affect it - but passthrough has
already produced one surprise here, obSCEne having measured that it will not assemble points or
lines at all. Writing this against the assumption and finding out on hardware is the failure this
project keeps not having. Filed as `REQ-...-8c4d`.

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
| Type checking | yes - the language is the language |
| Instruction encoding | mostly: `clang -target amdgcn-amd-amdhsa -mcpu=gfx1030` assembles, so generated words can be checked against a disassembler without a console |
| Anything about the shader *interface* | **no** - varyings are parameter exports, and the export count is fixed at two until `REQ-...-3a91` is measured |

So the back end is gated on the same measurement fog, stencil and multitexture are. The front
end is not, and none of it is wasted whichever way that measurement goes.

Nothing in 1.x is wasted when it arrives: a GL 2.0 context still has to answer every 1.x call.

## How shader words get written here

Not from memory. `tools/shader/` holds the source, it is assembled for gfx1030, and the words go
into the C with the instruction beside them. A wrong encoding cannot fail loudly - it assembles
into the payload, the hardware does something else, and the frame is wrong rather than the build
stopped. See `tools/shader/README.md`.
