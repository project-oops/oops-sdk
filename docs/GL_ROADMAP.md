# oops-gl: the road to OpenGL 1.x and 2.x

What is done, what is left, and **what each remaining piece actually costs** - which is the part
that is expensive to work out twice. D008 set the goal; this is the map.

Counted, not estimated: **499 declared entry points** as of 2026-09-19 - 422 of the 455 GL
1.0-1.3 entry points Mesa's `include/GL/gl.h` declares (the 33 left are the optional imaging
subset and one vendor extension), every one of GL 1.4's 47 and GL 1.5's 19, and the context and
hardware-status calls. **Every core GL 1.x entry point is declared.**

## GL 1.4 and 1.5, measured

The count below is GL 1.0-1.3, the functions Mesa's `include/GL/gl.h` declares. **GL 1.4 and 1.5
are GL 1.x too**, and their functions are in Mesa's `include/GL/glext.h`, in the
`GL_VERSION_1_4` and `GL_VERSION_1_5` blocks. Counted against this header on 2026-09-19, prototypes
by name:

| | Declared | Missing |
|---|---|---|
| GL 1.4 (47) | **all 47** - `glBlendColor`, `glBlendEquation`, `glBlendFuncSeparate`; `glWindowPos*` (16), `glSecondaryColor*` (17), `glFogCoord*` (5), `glPointParameter*` (4), `glMultiDrawArrays` and `glMultiDrawElements`, all 2026-09-19 | none |
| GL 1.5 (19) | **all 19** - the buffer objects, and the occlusion queries (8), `glMapBuffer`, `glUnmapBuffer`, `glGetBufferSubData` and `glGetBufferPointerv`, 2026-09-19 | none |

The values they bring, which a count of functions cannot see, are all in as of 2026-09-19:
`GL_MIRRORED_REPEAT` and the texture crossbar with the 1.3 work, then `GL_COLOR_SUM` with the
secondary colour, `GL_FOG_COORD_SRC` with the fog coordinate, point size attenuation with the
point parameters, `GL_GENERATE_MIPMAP`, the texture LOD bias, `GL_INCR_WRAP`/`GL_DECR_WRAP`, and
depth textures with the shadow comparison - see **Done** - and GL 1.5's nine buffer usages,
mapping and occlusion queries. **GL 1.4 and 1.5 are complete in software** - and since the
evening of 2026-09-19 so is what they inherit from 1.3: a second texture unit, which GL 1.3
requires and oops-gl lacked when this paragraph first said "complete" (the gap table's first
row). **Their hardware halves landed on 2026-09-20** - the second texture unit, cube maps, 3D
textures, depth textures with the shadow comparison, and occlusion queries, which count the
GPU's own samples there rather than reporting `GL_QUERY_COUNTER_BITS` 0. What is left on the
console is a smooth *textured* primitive, `GL_POLYGON_SMOOTH`, and a volume's mip chain; all
three are under **Needs a shader change**.

## What the console actually does - 2026-09-20, the first rows

Everything above this section was written from the host software rasteriser and from obSCEne's
register measurements. On 2026-09-20 gl1-probe ran on a console, and after four runs - the first
three of which each found a fault in the instrument rather than in the GL - the suite ran to the
end and printed its own verdict:

```
gl1-probe: 74/82 passed on hardware
```

**Every check in the suite now has a verdict from real hardware.** That is the first time any of
these claims has been answerable rather than argued, and it is the number this document should
be read against.

**What newly works there.** Ten checks had never drawn a frame on a console before these runs and
all ten pass: `cube-map`, `texture-3d`, `shadow-compare`, `occlusion-query`, `combine`, the
depth and stencil readbacks, `smooth`, `multitexture` and `tex-delete-in-frame`. Three are worth
naming.

- **`stencil`** this document called "written, not yet seen" since 2026-09-19, and the depth and
  stencil surfaces reach the CPU through computed 64KB_Z_X swizzle vectors - the part that had
  no right to be taken on trust.
- **`smooth`** is the antialiasing coverage shader. Smooth points and lines are right on the
  part, so what is left under **Needs a shader change** is the *textured* smooth primitive and
  `GL_POLYGON_SMOOTH`, not the coverage arithmetic.
- **`multitexture`** is the largest of them: a red base map under a blue `GL_ADD` second unit,
  magenta. `vs-param4.s`, `tex-prolog2.s` and unit 1's combine stage all ran for the first time
  in that one draw, and it also settles `REQ-20260920T0745Z-9a41`'s open half - the second
  descriptor pair really is read from `s[20:27]` and `s[28:31]`, one stride along the table.

**What fails there, and how it groups.** Eight, and the pixel each one left behind - the `saw`
row a failing check now prints, in `AARRGGBB`:

| Check | What it left | Shape |
|---|---|---|
| `raster-ops` | `0xff202020` | `glDrawPixels` - CPU writes colour |
| `pixel-transfer` | `0xff0000ff` | `glDrawPixels` |
| `pixel-fragments` | `0xff0000ff` | `glDrawPixels` |
| `index-pixels` | `0xff202020` | `glDrawPixels` |
| `accumulation` | `0xff0000ff` | `glAccum` |
| `array-types` | `0xff202020` | `glBitmap` |
| `blend-constant` | `0x40804000` | `CB_BLEND_RED..ALPHA`, on its own |
| `front-and-back` | `0xff00ffff` | two colour targets, written 2026-09-20 |

**The first six are one shape, not six bugs**: the CPU puts colour into the render target and
the check reads it back. `0xff202020` is the clear colour - nothing landed at all. Their
neighbours that pass say the same thing from the other side: `stencil-pixels` writes *stencil*
from the CPU with the same `glDrawPixels` and passes, and both readback checks pass, because
those reach their surfaces with the CPU at both ends. Colour is the one buffer whose round trip
goes through the GPU - the CP copies the render target into `ctx->readback` after every submit
and every CPU read of colour comes from that copy - and `gl_raster_sync` issues no cache
write-back and no store fence before the submit that reads what the CPU just wrote. That is
`REQ-20260920T2230Z-7c31`, and it is a hypothesis with a shape, not a diagnosis.

**`blend-constant` is one channel.** GL says `0xff80ff00`; the console returned `0x40804000`.
Red `0x80`, blue `0x00` and alpha `0x40` are exactly right - and green is `0x40`, the constant's
*alpha*, where its green of 1.0 belongs. Three channels correct rules out the register offset,
the packet's count and the factor encoding, all of which would spoil every channel together.
`REQ-20260920T2320Z-4b8d` asks which register the green channel reads.

**`front-and-back` is the front half.** Its `saw` is the *back* buffer, and `0xff00ffff` is the
cyan GL calls for, so the back target and the additive blend into it are right; what the check
also wants is yellow in the front.

**And the fault, which is fixed.** The run before this one, `multitexture` took `ILLEGAL_INST`
on two waves at one PC, then `GPU_FAULT_WAVEFRONT_ERROR_ASYNC` and a GPU reset. The PC is the
only datum such a fault leaves, and the log could not say which shader it fell in, because
nothing printed the payload's address. `gl_klog_val("payload-va", ...)` now does, and the next
run resolved it in one subtraction: `0x201390B04 - 0x201390000` is `0xb04`, the second word of
`OOPS_GL_VS_P4_OFFSET`.

**`gl_vs_build_param4` was never called.** It was written, unit-tested against a scratch buffer,
and wired into the offset the draw selects for four parameters - and nothing ever wrote its
seventy words into the payload, so the GPU jumped to `0xb00` and executed the allocation. The
shape is one the collection already guards against from the other side: `app.mk` fails a link
whose *call* has no definition, and this was a *definition* with no call, which links and tests
clean and only hardware can see. One line in `glContextCreate`, beside the three-parameter build
that was there all along, and `multitexture` passes.

`multitexture` still runs last, beside `tex-delete-in-frame`: the ordering is not a workaround
for that bug but a standing rule for a check that has ever taken the GPU down, because a fault
kills the process and costs every row after it.

**What the runs cost to get.** Three of the four produced no complete result, and only the last
of the three found a bug in the GL rather than in the instrument.

1. The first reported nothing at all, because every verdict was written to an array printed only
   once the suite returned, and the suite did not return. The fix is the per-check trace hook in
   `gl1_probe.h`.
2. The second named `raster-ops` and stopped. `px()` calls `glFinish`, and `raster-ops` was the
   first check to scan all 128x96 pixels - **12,288 synchronisations**, each a submission and a
   fence wait, about two hours. It is free on the host, which is why 82/82 always passed there.
3. The third reached the middle of the table and took the GPU down, which cost thirty-nine
   unrun checks and is why a crashing check now runs last.

The whole suite then took **16.6 seconds**. Two of those three were instruments that could not
describe what they found; the lesson they share is that a check suite is a program too, and the
only place its own bugs show is the machine it was built to measure.

## The gap, measured

> **Two more measurements, 2026-09-19 evening - the state and the enums.** Counting entry points
> cannot see a query that answers `GL_INVALID_ENUM`, nor an enum nobody declared. So:
>
> - **Every name GL 1.5's state tables give `glGet*` or `glIsEnabled`** (200, the imaging subset
>   left out) was extracted from the specification's own tables and queried through
>   `glGetIntegerv`, `glGetFloatv`, `glGetBooleanv`, `glGetDoublev` and, for the enables,
>   `glIsEnabled` - GL requires each state be available through every one (6.1.2). Ten names
>   could not even be asked, being neither declared nor answered (`GL_TEXTURE_STACK_DEPTH`,
>   `GL_LIST_INDEX`, `GL_LIST_MODE`, `GL_MAX_LIST_NESTING`, `GL_AUX_BUFFERS`, `GL_DOUBLEBUFFER`,
>   `GL_STEREO`, `GL_SUBPIXEL_BITS`, `GL_CURRENT_RASTER_INDEX`,
>   `GL_MAX_ELEMENTS_VERTICES`/`_INDICES`), and eleven float states answered `glGetFloatv` but
>   refused `glGetIntegerv`. All answer now; the only refusal left is a false match (`GL_ACCUM`,
>   an operation, not state).
> - **Every enum Mesa's headers define in their GL 1.0-1.5 blocks** (879) against `gl.h` here:
>   82 missing, of which the real ones were `glCallLists`' `GL_2_BYTES`..`GL_4_BYTES` (and seven
>   of its ten types refused), the draw and read buffer names (`GL_BACK_LEFT`, the right and
>   auxiliary buffers), GL 1.3's `GL_TRANSPOSE_*_MATRIX` queries, GL 1.5's `GL_SRCn_*` names and
>   GL 1.0's `GL_TEXTURE_COMPONENTS`, and the `_ARB` multitexture enums beside their declared
>   `_ARB` functions. All in now. What remains is extensions (`GL_ARB_imaging`, Mesa's and
>   ATI's), vertex-program and vertex-blend buffer bindings, the imaging subset's colour
>   matrix, and the `GL_VERSION_1_x` header macros - left out while D007 says this is not a GL
>   version, which is now a decision to revisit rather than a description.
> - Found on the way: **`GL_CURRENT_BIT` did not save the raster position**, and `glDrawBuffer`
>   accepted `GL_BACK` alone - `GL_NONE` (drawing off) and `GL_BACK_LEFT` work now, the absent
>   buffers are `GL_INVALID_OPERATION`. The front buffer was the one gap it left, and it
>   closed the same evening - see **Front-buffer rendering** in the gap table.
>
> Rerun them rather than trusting these numbers: the method is the spec's table rows (a name
> glued to its type column and its Get command in the PDF's text) matched against the enum
> names, and a harness querying each through each getter.

Everything below this line used to be a list of what I remembered was missing. On 2026-09-17 it
was replaced with a diff: the entry points Mesa's `include/GL/gl.h` declares (GL 1.0-1.3, 455 of
them) against the ones `include/GL/gl.h` here declares.

| | 2026-09-17, morning | 2026-09-17, evening | 2026-09-19 | 2026-09-19, later | 2026-09-19, evaluators | 2026-09-19, selection | 2026-09-19, pixel maps | 2026-09-19, stipple | 2026-09-19, accumulation | 2026-09-19, 3D textures |
|---|---|---|---|---|---|---|---|---|---|---|
| Present in both | **161** | **350** | **357** | **375** | **398** | **406** | **414** | **417** | **419** | **422** |
| Missing | **294** | **108** | **98** | **80** | **57** | **49** | **41** | **38** | **36** | **33** |

> **Every core GL 1.0-1.3 entry point is declared as of 2026-09-19.** The 33 left are exactly the
> optional ones below - the imaging subset and one vendor extension. What the count cannot see is
> the next section, and it is where the remaining GL 1.x work is.

> **The method changed for the last column, and the change matters.** The first two counted
> *names* - `grep -oE '\bgl[A-Z][A-Za-z0-9_]*'` over both headers - so a function this header
> only *mentions in a comment* counted as declared. `glPolygonMode` was counted present that way
> for its whole life here, and has never been declared. The 2026-09-19 column counts
> **prototypes with the comments stripped**: `GLAPIENTRY gl...(` on Mesa's side, a declaration
> ending `);` on this one. Rerun it rather than trusting any column, including this one.

The 98 split cleanly, and not the way this section used to say. (Since then `glPolygonMode`, the
edge-flag calls, colour-index state, `glSampleCoverage`, the evaluators, selection and
feedback, pixel maps and transfer, stipple, the accumulation buffer and 3D textures have landed -
see **Done** - leaving 33: the same 33, and no core entry point at all.)

- **33 are not part of what GL 1.x requires.** The 32 entry points of the **imaging subset** -
  colour tables, convolution and separable filters, histogram, minmax - are optional in GL 1.2
  and 1.3: an implementation carries them only if it advertises `GL_ARB_imaging`, which
  `glGetString(GL_EXTENSIONS)` does not. The 33rd is `glBlendEquationSeparateATI`, a
  vendor extension. Leaving these out is conformant, not a gap.

  **The extension list is no longer empty** (2026-09-19), because a homebrew port reads it and
  then calls the extension's own entry point names. Those names exist now - `glGenBuffersARB`,
  `glSecondaryColor3fEXT`, `glWindowPos2iARB` and the rest, each the core function under its
  published name - so the promise is kept. The list is the path's: `GL_ARB_multitexture` was on
  it for the software rasteriser and off on the console until a draw there sampled two units,
  which it does since 2026-09-20 (`REQ-20260920T0745Z-9a41`), so it is on both.
  `GL_ARB_texture_cube_map` and `GL_EXT_texture3D` joined both lists on 2026-09-20, when the
  console gained their samples - the latter bringing `glTexImage3DEXT` and `glTexSubImage3DEXT`
  with it, because a list entry whose entry points are missing is the promise this library will
  not make. `GL_ARB_depth_texture` and `GL_ARB_shadow` joined them the same day, when the console
  gained the comparison sample; neither adds an entry point, so what they promise is enums and
  behaviour and both paths have all of it. `GL_ARB_occlusion_query` joined them too, when the GPU
  started counting, and brought its eight entry points with it. **The two lists are identical
  today** - which is the point of having built them separately, not a reason to stop.
- **65 are core GL 1.x, and every one is implementable** - all but three entirely in software,
  with no register or shader involved:

| Family | Entry points | What it takes |
|---|---|---|
| ~~Evaluators~~ | ~~23~~ | **Done** 2026-09-19 - see **Done** |
| ~~Colour-index state~~ | ~~13~~ | **Done** 2026-09-19 |
| ~~Selection and feedback~~ | ~~8~~ | **Done** 2026-09-19 - see **Done** |
| ~~Pixel maps and transfer~~ | ~~8~~ | **Done** 2026-09-19 - see **Done** |
| ~~Stipple~~ | ~~3~~ | **Done** 2026-09-19 - see **Done**. The polygon stipple's hardware half landed 2026-09-20 |
| ~~Edge flags~~ | ~~3~~ | **Done** 2026-09-19, with `glPolygonMode` |
| ~~3D textures~~ | ~~3~~ | **Done** 2026-09-19 in software, 2026-09-20 on the console - see **Done**. Level 0 only there; see **Needs a shader change** |
| ~~Accumulation buffer~~ | ~~2~~ | **Done** 2026-09-19 - see **Done** |
| ~~`glPolygonMode`~~ | ~~1~~ | **Done** 2026-09-19 - see **Done** |
| ~~`glSampleCoverage`~~ | ~~1~~ | **Done** 2026-09-19 |

### What the entry-point count cannot see

The diff above counts functions. **GL 1.x also added features that are only new values for
functions that already exist** - a new target for `glTexImage2D`, a new `type`, a new
`glTexEnv` mode - and those are invisible to it. Checked against the header and the source on
2026-09-19, these are missing, most important first:

| Feature | GL | What is wrong today |
|---|---|---|
| ~~**Two texture units**~~ - found and **done in software** 2026-09-19 | 1.3 | **GL 1.3 requires at least two** - "The number of texture units supported is implementation dependent but must be at least two" (OpenGL 1.3 specification, section 2.6, page 13), and table 6.29 gives `MAX_TEXTURE_UNITS` a minimum of 2. oops-gl had one and reported 1, which this document called conformant from the day the multitexture entry points landed; it never was. **It reports 2 now**, and each unit has its own state (`gl_tex_unit_t`: enables, bindings, environment and combiner, LOD bias, texture matrix stack, generation), current, raster and array texture coordinates, and its coordinate in the vertex; the rasteriser samples every applying unit and runs each environment on what the unit before left, `GL_PREVIOUS` being that and `GL_PRIMARY_COLOR` the fragment's own, GL 1.4's `GL_TEXTUREn` any unit's texel (zero for one applying none, as Mesa reads it). `GL_TEXTURE_BIT`, `GL_ENABLE_BIT`, `GL_CURRENT_BIT` and the client vertex-array bit save every unit and the selectors; lists record `glMultiTexCoord` with its unit; `GL_TEXTURE1`..`GL_TEXTURE31` are declared. Feedback and the evaluators are unit 0's, as Mesa's are. **On the console** two units are applied since 2026-09-20 (`REQ-20260920T0745Z-9a41`): the fourth parameter carries unit 1's coordinate, its sample sits inside whole-quad mode beside unit 0's, and its environment runs on what unit 0's stage left. A unit above the second is still the software rasteriser's alone and earns one log line. gl1-probe's `multitexture` is expected to pass there |
| Mipmap levels on the hardware - **written, not yet seen** | 1.0 | Since 2026-09-19 a complete, power-of-two, mipmap-filtered texture reaches the GPU as a chain laid out by addrlib's own arithmetic for a linear GFX10 surface - smallest level first, base level last, each level's rows at 256 bytes - with LAST_LEVEL, MAX_MIP and MIP_FILTER as radeonsi fills them. The layout is derived, not measured: gl1-probe's `mipmap-levels` draws an 8x8 chain across 4x4 pixels and needs the second level's colour, which is the measurement. A non-power-of-two mipmapped texture keeps sampling its base level (GL rounds levels down, addrlib up). **Found the same evening: the chain could never have been read** - the textured pixel shader sampled with `image_sample_lz`, level zero, so no level of detail was ever computed and `mipmap-levels` would have failed whatever the layout. It samples with `image_sample` in whole-quad mode since (`tools/shader/tex-prolog.s`), which also brings the minification filter and GL 1.4's LOD bias into effect |
| `glClear` through the scissor box and write masks on the hardware - **written, not yet seen** | 1.0 | Found and fixed 2026-09-19: both paths cleared the whole surface whatever the scissor and masks said. A scissored or colour-masked clear of colour or depth is now **drawn** - one quad at the clear values with every other per-fragment effect set aside, as Mesa's `clear_with_quad` does - through the scissor registers and `CB_TARGET_MASK` the draw path already emits; the depth mask drops the depth clear, and the stencil clear keeps to the box and the stencil mask on the CPU. An unscissored, unmasked clear is still the DMA fill, so gl-cube's recorded frame is untouched. The drawn clear is unmeasured on a console: gl1-probe's `scissored-clear` is the measurement |
| ~~**The colour buffer is linear and re-tiled by the CPU**~~ - the scanout path **measured and on, 2026-09-20** | - | The console drew into a linear buffer that the display re-tiled on the CPU at every flip; a title on this console draws straight into the scanout buffers, and so does oops-gl now. Those are `WC_GARLIC` memory (obSCEne `130-layout/memory-type`) in the GPU's 64KB_R_X swizzle. **Both halves of that layout are measured**: inside a block, `-2d7f`'s whole-block dump agrees with the display tiler on all 16,384 pixels where the same bytes read as rows agree on 10,408; across blocks, `-4b19`'s 256 x 256 dump detiles into one run a row with no gaps under the row-major order **and under no other** (`rx-check --shape`, which enumerates them). Verdicts tracked as `tools/rx-check/rx_check_7e21.txt` and `rx_check_4b19.txt`. **The scanout path** (`src/gl/gl_rx.h`, 2026-09-19) does what a title does. The back is the next scanout buffer, with `CB_COLOR0_ATTRIB3` `COLOR_SW_MODE` 27, and a swap flips it as drawn after waiting for it to leave the screen (`oops_display_flip_scanout`, `oops_display_wait_scanout`). The front is the buffer on screen. Every CPU colour access goes through `gl_color_index`, on the display tiler's own vectors (`agc_tile_pixel`), and `glGetFrameReadback` detiles for its callers. `test_pm4_gl_scanout_path_targets` covers it on the host. **On** (`OOPS_GL_RX_MEASURED` 1) since the rows above. It was off while `REQ-20260916T1250Z-6e0f`'s result claimed the measurement and its own rows contradicted it - the target was linear and one pixel was drawn - and while `-7e21`'s first answer dumped 1/64 of each block, all of it fill. `tools/rx-check`'s self-test proves the reading on synthetic rows, one block and four, the four-block round pinning that a transposed block order is caught and named rather than passing. **What the rows do not settle** is in `gl_rx.h`: both tiled arms matched, so `0x08c6c000` and `0x0dc6c000` are indistinguishable here, and the first is taken as the smaller change from the working linear value. **CPU overlays follow it:** gl1-cube's HUD draws with the CPU on `oops_display_get_surface`. Once the scanout path calls `oops_display_use_scanout`, that surface is the next scanout buffer, flagged `OOPS_SURFACE_RX`, and the SDK's CPU drawing calls address that layout (2026-09-19). So the HUD lands in the frame being flipped without a change to the app. It is slower there: its translucent panel reads back write-combined memory |
| ~~Front-buffer rendering~~ | 1.0 | **Done** 2026-09-19, found and closed the same day - see **Done**. `glDrawBuffer(GL_FRONT)` and `glReadBuffer(GL_FRONT)` (and `GL_FRONT_LEFT`, `GL_LEFT`, `GL_FRONT_AND_BACK`) were refused with `GL_INVALID_OPERATION`, because `oops_display` handed this library its back buffer only. **Closed on the console 2026-09-20:** a *draw* under `GL_FRONT_AND_BACK` or `GL_LEFT` reached the back and not the front, with one log line. It now binds `CB_COLOR1_*` to the front (offsets from Mesa `gfx103.json`), carries MRT1 in `CB_TARGET_MASK`, `CB_SHADER_MASK` `0xff` and `SPI_SHADER_COL_FORMAT` `0x99`, and exports twice from both pixel shaders - `exp mrt0 ... vm` then `exp mrt1 ... done vm`, assembled in `tools/shader/mrt1-export.s`, whose third instruction re-assembles the single-target word already in the tree as its cross-check. The registers are obSCEne's `REQ-20260919T2258Z-3f62` (sweep `20260920-082906`, `166-agc/mrt-dual-target`): two targets took 512 pixels each in one draw where the one-target control left the second at its fill. `gl_ps_patch_export` sets the export on **every** draw, so a later `glDrawBuffer(GL_BACK)` stops writing the front, and an unbound second target is zeroed in `CB_COLOR1_INFO` as well as dropped from both masks. `test_pm4_gl_front_buffer_targets` covers both directions; gl1-probe's `front-and-back` is expected to pass on hardware |
| Fog on the hardware - **written, not yet seen** | 1.0 | Since 2026-09-19 fog reaches the console without moving the shader interface: the factor is computed per vertex on the CPU, as the software path computes it (eye distance or GL 1.4's fog coordinate, clamped), and rides in the texture parameter's spare `z`, which the vertex shader already exports; both pixel shaders interpolate it and blend red, green and blue towards the fog colour in a twelve-word slot before the alpha test, the colour as three patched literals. Every word is from `tools/shader/fog.s`. The pinned interface registers are unchanged, and gl-cube, which has no fog, runs the same program but for twelve `s_nop`. gl1-probe's `fog` and `fog-coord` are the measurement |
| ~~`glPixelStorei` keeps three parameters~~ | 1.0 | **Done** 2026-09-19 - every GL 1.2 parameter, both directions; see **Done** |
| ~~Non-byte pixel types, packed types~~ | 1.1, 1.2 | **Done** 2026-09-19 - every plain and packed type, and `GL_RED`/`GL_GREEN`/`GL_BLUE`; see **Done** |
| ~~Sized and other internal formats~~ | 1.1 | **Done** 2026-09-19, with proxy textures - see **Done** |
| ~~Texture environment `GL_BLEND`, RGBA `GL_DECAL` and `GL_COMBINE` on hardware~~ - **written, not yet seen** | 1.0-1.3 | **Done** 2026-09-19. The textured pixel shader moved from payload offset 0x200 to 0x400 (128 words, 119 used) and its combine slot grew from four words to sixty-four. The short forms stay where they suffice, now with `GL_ADD`'s sums clamped; `GL_BLEND`, RGBA `GL_DECAL` and `GL_COMBINE` are a **generated program** - the combiner's arguments gathered, its function per channel, the scale, the clamp - with GL_BLEND and GL_DECAL first restated as combiner settings as Mesa's `calculate_derived_texenv` does. Its words come from an encoder whose every instruction form is checked against `tools/shader/combine.s`, assembled; and `test_pm4_gl_combine_programs_compute_what_software_does` runs each program through a reader for those forms and compares it with the software rasteriser on every mode, base format and a spread of combiner settings. Unmeasured on a console: gl1-probe's `combine` and `tex-env-blend-decal` are the measurement |
| ~~Texture parameter values; `GL_TEXTURE_PRIORITY`, `GL_TEXTURE_RESIDENT`~~ | 1.0, 1.1 | **Done** 2026-09-19 - see **Done** |
| ~~Pixel rectangles skip the fragment operations~~ | 1.0 | **Done** 2026-09-19, found and fixed the same day - see **Done**. On the console their depth and stencil tests were left out until the evening, with one log line. They run there now, against the GPU's tiled surfaces - see the next row |
| ~~Depth textures, shadow comparison~~ | 1.4 | **Done** 2026-09-19 in software, 2026-09-20 on the console: image format `32_FLOAT`, the sampler's own `DEPTH_COMPARE_FUNC`, and a sample slot that asks for one channel and spreads it as `GL_DEPTH_TEXTURE_MODE` says - see **Done**. gl1-probe's `shadow-compare` is expected to pass there |
| ~~Depth and stencil pixel rectangles~~ | 1.0 | **Done** 2026-09-19, found the same day: `glReadPixels`, `glDrawPixels` and `glCopyPixels` refused `GL_DEPTH_COMPONENT`, `GL_STENCIL_INDEX`, `GL_DEPTH` and `GL_STENCIL` - see **Done**. On the hardware path the depth operations were refused at first, with `GL_INVALID_OPERATION` and one log line, because the depth surface is the GPU's, laid out 64KB_Z_X. The stencil ones joined them when the console's stencil test made the stencil buffer a tiled GPU surface too. **Both are addressed from the CPU since the evening of 2026-09-19**, through addrlib's swizzle vectors (`tools/zs-tiling`, `src/gl/gl_zs_tiling.h`). The tool checks every pixel of a three-by-two-block surface against addrlib under the part's `GB_ADDR_CONFIG`. It also runs an eight-pipe control that must disagree. gl1-probe's `depth-readback` and `stencil-readback` are the console measurement, not yet run |
| ~~`GL_COLOR_INDEX` pixel rectangles, the `GL_BITMAP` type~~ | 1.0 | **Done** 2026-09-19 - see **Done** |
| ~~Cube maps~~ | 1.3 | **Done** 2026-09-19 in software, 2026-09-20 on the console: the six faces as one array, `TYPE 0xb`, and the face found from the direction by RDNA2's own `v_cube*` instructions - see **Done**. An incomplete cube map is drawn untextured on both, which is what GL does with one. gl1-probe's `cube-map` is expected to pass there |
| ~~Separate specular colour, `GL_RESCALE_NORMAL`, texture LOD parameters~~ | 1.2 | **Done** 2026-09-19 - see **Done**; the textured half of the separate specular colour on hardware is in **Needs a shader change** |
| `GL_CLAMP_TO_BORDER` and the border colour on hardware - **written, not yet seen** | 1.3 | **Done** in software 2026-09-19, with `GL_MIRRORED_REPEAT` (1.4) and a `GL_CLAMP` that reaches the border - see **Done**. On the hardware a border colour that is none of the sampler's three built-in ones is read from a one-entry table that `TA_BC_BASE_ADDR` points at, as radeonsi does; the register and the table's layout are from Mesa and unmeasured. gl1-probe's `border-and-mirror` is the measurement |
| ~~Two-sided lighting~~ | 1.0 | **Done** 2026-09-19 - see **Done** |
| ~~Smooth points, lines and polygons~~ | 1.0 | **Done** 2026-09-19 in software; **points and lines on the console 2026-09-20**, in the untextured pixel shader's coverage slot (`tools/shader/coverage.s`). This paragraph used to say the coverage needed the fragment's *position*; it does not - the offset from the primitive's centre is linear across the quad the CPU widens it into, so the CPU writes it at the corners and the interpolator carries it in. A **textured** smooth primitive is still aliased, the parameter it would ride in being the texture coordinate, and so is **GL_POLYGON_SMOOTH**, whose coverage is three edge fades rather than one distance; both earn the log line. The console's smooth line does not fade its end caps - one interpolated distance cannot say how far along the segment a pixel is - which GL does not require |

> **This section said on 2026-09-17 that 101 of the gap were families that "mostly should not"
> exist here, and that was wrong.** Only the imaging subset is optional. The rest is core GL 1.x,
> and "unusual" was a judgement about taste rather than about programs: evaluators, selection
> and `glPolygonMode` are all things ordinary GL 1.x code calls. A program that calls one of
> them against this library does not get a degraded picture - it gets the null call described
> next.

## An absent entry point is not absent

**`-Wl,--unresolved-symbols=ignore-all` is in the payload link.** A program calling a GL entry
point this does not declare therefore links cleanly and calls **address zero** at run time: a
`SIGSEGV` at `rip: 0x0000000000000000`, with no build diagnostic and nothing in the log naming the
symbol. That is not a theoretical reading - it is the crash signature that was familiar all day
from a different cause.

So D009's "an absent feature is an absent symbol" does not hold its usual meaning here. Omitting an
entry point does not produce a clean failure; it produces the worst kind. Where a refusal is
*conformant* - an empty compressed-format set - this now declares the entry point and refuses,
which is strictly better than a crash and is what the specification asks for anyway. (One texture
unit was listed here as conformant too, until 2026-09-19. It is not: GL 1.3 requires two - see the
gap table.)

**Whether to extend that to features which are simply unimplemented - none are left among the core entry points, but the rule matters again the day one is added - is an open
decision and is deliberately not taken here.** Declaring them and returning `GL_INVALID_ENUM`
would turn a null jump into a readable error; leaving them out keeps D009 literal. The evidence is
recorded so the choice can be made rather than drifted into. (A post-link check that fails the
build on any undefined symbol outside the `sce*` imports would restore D009's own mechanism
instead, and is the recommended way through; it is not in the build yet.)

## Done

| Feature | Notes |
|---|---|
| Immediate mode, vertex arrays | `glBegin`/`glEnd`, `glDrawArrays`, `glDrawElements` |
| One 2D texture unit | upload, sub-image, parameters, environment |
| Lighting and materials | eight lights, `glColorMaterial` - which, since 2026-09-19, **writes the current colour into the material** it tracks, as Mesa's `_mesa_update_color_material` does (`main/light.c:759`): on each colour change, when the enable goes on and when `glColorMaterial` changes what it tracks while on. So `glGetMaterial` reads the colour back, `glMaterial` leaves a tracked property alone (`vbo/vbo_exec_api.c:590-598`), and the material keeps the last colour when the enable goes off - it was only substituted while lighting before |
| Matrix stacks | model-view, projection, texture |
| Blend, depth, cull, scissor, colour mask | |
| **Display lists** | records calls, not effects - **every call the specification compiles** (2026-09-19; it was 21 operations before, and everything else ran at compile time). Pointer arguments copied when compiled; images unpacked through the compile-time `glPixelStorei` state; the vertex-array draws expanded into the immediate-mode calls they stand for, with the arrays read at compile time; `glCallLists` applying the list base current when it runs; `GL_COMPILE_AND_EXECUTE` recording each call once. Storage grows from the SDK heap instead of a fixed 4096 commands per list. The compressed-texture uploads are the one exception: they execute at compile time, which changes nothing because there are no formats and every one is refused |
| **`glTexSubImage2D`, `glPixelStorei`** | **every GL 1.2 parameter** (2026-09-19; alignment and the unpack row length before): the unpack and pack row lengths, image heights, row, pixel and image skips, `GL_*_SWAP_BYTES` and `GL_*_LSB_FIRST`, each queryable and on the client attribute stack. One module, `gl_pixel.c`, now owns the walk through client memory for every image entry point - `glTexImage*`, `glTexSubImage*`, `glDrawPixels`, `glBitmap`, `glPolygonStipple`, `glReadPixels`, `glGetTexImage` - so they cannot disagree. A display list takes the pixels the store selected when it compiles, packed tight, and replays them under neutral state |
| **Internal formats** (GL 1.1) | **every base and sized format**, the legacy 1 to 4, and GL 1.3's generic compressed formats (stored uncompressed, as the specification allows) - Mesa's `_mesa_base_tex_format` cut to GL 1.x (2026-09-19; the argument was ignored before, and every texture was RGBA). Storage is still RGBA8, holding each texel as its base format expands it - `(0, 0, 0, A)`, `(L, L, L, 1)`, `(I, I, I, I)` - which is what the sampler and a shader's lookup see. `glGetTexImage` reports luminance and intensity in red alone, as Mesa does; `GL_TEXTURE_INTERNAL_FORMAT` answers the format as named and the six size queries eight bits for each component kept. A mip chain of mixed formats is incomplete. An unknown format is `GL_INVALID_VALUE`, and `glCopyTexImage*` refuses 1 to 4 |
| **Texture environment by base format** | GL's table of texture functions as Mesa's `calculate_derived_texenv` states it: an alpha texture never touches the colour, luminance and RGB ones never the alpha, intensity is all four, and `GL_ADD` and `GL_BLEND` of an intensity texture treat alpha like colour. The software rasteriser does all five modes - `GL_BLEND` was drawn as `GL_MODULATE` until 2026-09-19. The hardware's four combine words are chosen per draw from the mode and the bound texture's base format - `s_nop`, `v_mov_b32`, `v_mul_f32`, `v_add_f32`, from `tools/shader/tex-env.s` - which brings `GL_ADD` and RGB `GL_DECAL` to the console; `GL_BLEND` and RGBA `GL_DECAL` followed as generated programs - see the gap row. An unknown mode is refused where it used to be stored and drawn as modulate |
| **Texture parameters** | Values checked as Mesa's `set_tex_parameteri` checks them - a wrap mode or filter GL does not have is `GL_INVALID_ENUM` and changes nothing, where any value used to be stored (2026-09-19). `GL_TEXTURE_PRIORITY` (and `glPrioritizeTextures`, which validated and dropped it) and `GL_TEXTURE_BORDER_COLOR` kept, clamped to [0, 1]; `GL_TEXTURE_RESIDENT` answered. The float forms are their own path: `glTexParameterf` truncated to an integer, and lists compiled it that way. Wrap modes: `GL_CLAMP_TO_BORDER` and `GL_MIRRORED_REPEAT` added, and `GL_CLAMP` is GL's rather than `GL_CLAMP_TO_EDGE` - the coordinate clamped to [0, 1], so a linear filter at the edge takes half the border colour. The hardware sampler's CLAMP_X/Y and border colour type follow radeonsi's `si_tex_wrap` and `si_translate_border_color`. The LOD parameters remain - see the gap table |
| **Two-sided lighting** | `GL_LIGHT_MODEL_TWO_SIDE` (2026-09-19; refused before, and before that a field nothing read). The triangle stage winds a polygon in normalised device coordinates - the sign culling reads - before it lights the vertices, and a polygon facing away is lit with the back material and its normal reversed. Its outline or corners under `glPolygonMode` take its side; `GL_LINES` and `GL_POINTS` are lit from the front. On both paths, since lighting is CPU work on both. Found with it: **`glColorMaterial`'s face was ignored** - `GL_BACK` changed the front material - and neither it nor the property was checked, queryable or on `GL_LIGHTING_BIT` |
| **Vertex array types** (GL 1.1) | Every type GL 1.1 lets each array hold, read as that type - positions and texture coordinates as values, colours and normals normalised as the immediate-mode calls of their type convert - and checked as Mesa's `varray.c` checks them (a type outside the list `GL_INVALID_ENUM`, a size or negative stride `GL_INVALID_VALUE`). **Until 2026-09-19 nothing was checked and every array was read as floats** (a colour array as unsigned bytes too): a `GL_SHORT` position array drew from reinterpreted bits. Found with it: colours were not clamped to [0, 1] before rasterising unless lit, so a negative one wrapped to full intensity |
| **`glWindowPos`** (GL 1.4) | All sixteen: the raster position set in window coordinates, always valid, z through the depth range, colour and texture coordinate the current ones unlit (Mesa's `window_pos3f`, `main/rastpos.c`); compiled into lists |
| **Secondary colour, `GL_COLOR_SUM`** (GL 1.4) | All sixteen `glSecondaryColor3*` spellings, normalised as `glColor`'s are, and `glSecondaryColorPointer` - any of the eight types, 3 or 4 components as Mesa accepts them (`main/varray.c:1536-1553`). Unlit, `GL_COLOR_SUM` adds the vertex's secondary colour, clamped, after texturing and before fog; lit, lighting's own secondary colour takes over and the sum runs whatever the enable says (Mesa's test, `main/ff_fragment_shader.c:69-78`). Flat shading takes the provoking vertex's; lists record it, from arrays too; `GL_CURRENT_BIT`, `GL_FOG_BIT`/`GL_ENABLE_BIT` and the client vertex-array bit save it - the enable restored where Mesa's `attrib.c` drops it. The hardware sums per vertex, exact untextured. Found with it: **the client arrays started with size 0 and type 0**, so `glGetIntegerv(GL_VERTEX_ARRAY_SIZE)` answered 0 until a pointer call; they start at Mesa's four (three, one) `GL_FLOAT`s now |
| **Fog coordinate, `GL_FOG_INDEX`** (GL 1.4, 1.0) | `glFogCoord{f,d}{,v}` and `glFogCoordPointer` (`GL_FLOAT` or `GL_DOUBLE`, Mesa's `main/varray.c:1408`). Under `GL_FOG_COORD_SRC` = `GL_FOG_COORD` fog reads the vertex's coordinate - as given, no absolute value (`main/ffvertex_prog.c:1062-1064`) - interpolated like the eye distance was; the raster distance follows it (`main/rastpos.c:475-479`, `:730-733`). Both GL 1.4 and GL 1.5 enum spellings. Compiled into lists, from arrays too; the coordinate with `GL_CURRENT_BIT`, the source with `GL_FOG_BIT` (Mesa's pop leaves it out), the array with the client vertex-array bit. On the console the coordinate reaches the fog factor the CPU computes per vertex, so it goes wherever fog goes - written there since 2026-09-19, unmeasured. And **`GL_FOG_INDEX` is state now** - kept, queried, saved with the fog group - where it was refused with `GL_INVALID_ENUM` (Mesa keeps it, `main/fog.c:136-142`) |
| **Point parameters, multi-draw** (GL 1.4) | `glPointParameter{f,i}{,v}`: every point's size is clamped to `[GL_POINT_SIZE_MIN, GL_POINT_SIZE_MAX]`, as Mesa clamps it unattenuated too, and `GL_POINT_DISTANCE_ATTENUATION` (a, b, c) divides it first by `sqrt(a + b d + c d^2)` - d the eye distance, where Mesa takes `|z_eye|` - on both paths, since a point becomes its square on the CPU. Checked and defaulted as Mesa's `main/points.c` does; compiled into lists; `GL_POINT_BIT`. `GL_POINT_FADE_THRESHOLD_SIZE` is state: its fade is for multisampled points, and there is no multisample buffer. `glMultiDrawArrays` and `glMultiDrawElements` check every count before drawing any (Mesa `main/draw.c:530-547`, `:303-320`), skip empty ones, and compile as their draws. **Every GL 1.4 entry point is declared** |
| **LOD bias, `GL_GENERATE_MIPMAP`, wrapping stencil operations** (GL 1.4) | `GL_TEXTURE_LOD_BIAS` per texture and per unit (`glTexEnv`'s `GL_TEXTURE_FILTER_CONTROL`, Mesa `main/texenv.c:448-460`), summed and clamped to `GL_MAX_TEXTURE_LOD_BIAS` 14 and added to the level of detail before the LOD clamp; on the hardware the draw puts the sum into `SQ_IMG_SAMP_WORD2`'s `LOD_BIAS` as radeonsi encodes it for GFX10 (`ac_descriptors.c:144-145`) - derived, unmeasured; gl1-probe's `lod-bias` is the measurement - and one that could only fail until the pixel shader stopped sampling at level zero (2026-09-19, see the mipmap row). `GL_GENERATE_MIPMAP` rebuilds the levels above the base whenever the base changes (Mesa's condition, `main/teximage.c:2889-2897`) by a 2x2 (2x2x2) box filter rounded as Mesa's software generation rounds - on the CPU, so the hardware's mip chain carries them. `GL_GENERATE_MIPMAP_HINT` is kept. `GL_INCR_WRAP` and `GL_DECR_WRAP` wrap where `GL_INCR`/`GL_DECR` saturate. All saved with their attribute groups |
| **Depth textures, shadow comparison** (GL 1.4) | `GL_DEPTH_COMPONENT` and its 16/24/32 sizes as internal formats, on 1D and 2D targets (Mesa `main/teximage.c:1744-1790`), uploaded, copied from the depth buffer and read back as `GL_DEPTH_COMPONENT` only - `GL_INVALID_OPERATION` for colour data into one or depth into a colour one (`:1795-1832`). Stored as 32-bit floats (`GL_TEXTURE_DEPTH_SIZE` 32). Sampled with `GL_TEXTURE_COMPARE_MODE` `GL_COMPARE_R_TO_TEXTURE` against the clamped r under any of the eight `GL_TEXTURE_COMPARE_FUNC`s, per texel before filtering; the result read as `GL_DEPTH_TEXTURE_MODE`'s luminance, intensity or alpha by the texture environment; a border's depth is the border colour's red. `GL_GENERATE_MIPMAP` averages depths. **On the console since 2026-09-20**: the image format is `GFX10_FORMAT_32_FLOAT` (22) rather than `8_8_8_8_UNORM` (56), the sampler's `DEPTH_COMPARE_FUNC` carries `GL_TEXTURE_COMPARE_FUNC` (`-6c80` measured a pass and a fail either side of the stored depth), and the sample slot holds `tools/shader/tex-shadow.s` - `dmask:0x1` because one value comes back, spread across `v4..v7` as the depth mode says, and `image_sample_c` with the clamped reference ahead of s and t under the comparison. The border goes into the table as the border colour's red in all four, not expanded by the depth mode, because it is a stored depth the comparison still runs against |
| **Depth and stencil pixel rectangles** (GL 1.0) | `glReadPixels` and `glDrawPixels` of `GL_DEPTH_COMPONENT` and `GL_STENCIL_INDEX`, `glCopyPixels` of `GL_DEPTH` and `GL_STENCIL` - every plain type, no packed one (`GL_INVALID_OPERATION`, as Mesa's `_mesa_error_check_format_and_type`). Depth through `GL_DEPTH_SCALE`/`BIAS` and clamped; stencil through `GL_INDEX_SHIFT`/`OFFSET` and `GL_MAP_STENCIL`'s map, in Mesa's order (`main/pixeltransfer.c`). A drawn depth pixel is a fragment at its own z in the raster colour, so the depth test decides it and a disabled one writes no depth; a stencil index is written straight to the buffer through the scissor and the stencil write mask. All three were refused until 2026-09-19. On the console they have read and written the GPU's tiled surfaces since that evening - see the gap table and the next row |
| **Front-buffer rendering** (GL 1.0) | `glDrawBuffer` and `glReadBuffer` take every name a double-buffered mono visual has. They follow Mesa's `draw_buffer_enum_to_bitmask` and `read_buffer_enum_to_index` (`main/buffers.c:145-170`, `:209-226`): `GL_FRONT` and `GL_FRONT_LEFT` name the front, `GL_BACK` and `GL_BACK_LEFT` the back, and `GL_LEFT` and `GL_FRONT_AND_BACK` both when drawn, the front when read. The front is oops-gl's own surface, the display's size, allocated the first time a program names one (`GL_OUT_OF_MEMORY` without memory) and filled with the picture on screen (`oops_display_read_shown`; on the host, the back). Every write reaches the buffers `glDrawBuffer` names: triangles, clears, pixel rectangles and the accumulation buffer's return, each blended and masked against its own pixel. Every read takes the buffer `glReadBuffer` names: `glReadPixels`, `glCopyPixels`' source, `glCopyTexImage` and the accumulation buffer's load. `glFlush` and `glFinish` put a front that has been drawn into on screen through `oops_display_present`, which leaves the back as it is. `glSwapBuffers` makes the front the picture it presented. On the console a frame begun on one buffer is submitted before draws move to the other, because `CB_COLOR0_BASE` is set when a frame begins. `test_gl_front_buffer`, `test_pm4_gl_front_buffer_targets`; gl1-probe's `front-buffer` |
| **Depth and stencil surfaces from the CPU** (console, 2026-09-19) | The console's depth (`Z_32_FLOAT`) and stencil (`STENCIL_8`) surfaces are 64KB_Z_X: 64 KiB blocks row by row across the padded pitch. Inside a block, a pixel's offset is the XOR of one vector per set bit of its x and y - 128 x 128 pixels a block for depth, 256 x 256 for stencil. `tools/zs-tiling` builds Mesa's addrlib and computes the vectors under the chip identity and the `GB_ADDR_CONFIG` oops-mesa derived from the display tiler (`0x4`, 16 pipes). It computes them for a depth-stencil pair set up as `ac_surface.c` sets one up, then checks every pixel of a three-by-two-block surface against addrlib: 0 of 98304 disagree for depth, 0 of 393216 for stencil. An eight-pipe control disagrees, as it must. `src/gl/gl_zs_tiling.h` carries the vectors, and every CPU access to depth or stencil goes through `gl_zs_depth_ptr`/`gl_zs_stencil_ptr`. That covers the pixel rectangles, the pixel rectangles' own depth and stencil tests, and a depth texture copied from the frame. The access follows the flush every pixel operation makes. No HTILE is bound, and the frame's closing `RELEASE_MEM` writes the DB caches back and invalidates GL2 (`GCR` `0x603`), so each side sees the other's writes. `test_pm4_gl_zs_tiling_is_a_permutation`, and the round trips at the end of `test_pm4_gl_stencil_reaches_its_registers` |
| **Colour-index images, `GL_BITMAP`** (GL 1.0) | `GL_COLOR_INDEX` images in an RGBA context, for `glDrawPixels` and every texture upload: each index through `GL_INDEX_SHIFT`/`OFFSET` and then the `GL_PIXEL_MAP_I_TO_R/G/B/A` tables, masked to each table's size and clamped, with no RGBA scale, bias or colour map after - Mesa's `_mesa_unpack_color_index_to_rgba_float` (`main/pack.c:1500-1548`). `glReadPixels` of them is `GL_INVALID_OPERATION` (an RGBA buffer holds no indices) and `glGetTexImage` `GL_INVALID_ENUM`. And the `GL_BITMAP` type for colour and stencil indices - one bit a pixel, rows as glBitmap's under the unpack and pack state, `GL_*_LSB_FIRST` included - for drawing, uploading, reading stencil back and compiling into lists; an enum error with any other format (Mesa `main/glformats.c:1949-1953`). Both refused until 2026-09-19 |
| **Buffer mapping, all nine usages** (GL 1.5) | `glMapBuffer` (`GL_READ_ONLY`, `GL_WRITE_ONLY`, `GL_READ_WRITE`), `glUnmapBuffer`, `glGetBufferPointerv`, `glGetBufferSubData`, `GL_BUFFER_ACCESS` and `GL_BUFFER_MAPPED`, checked in Mesa's order (`main/bufferobj.c`). The store is process memory, so the mapped pointer is the store itself. While mapped a buffer cannot be mapped again, updated, read back or drawn from - `GL_INVALID_OPERATION` - and a new `glBufferData` or a delete unmaps it. `glBufferData` takes the `_READ` and `_COPY` usages too, which it refused |
| **Occlusion queries** (GL 1.5) | `glGenQueries`, `glDeleteQueries`, `glIsQuery`, `glBeginQuery`, `glEndQuery` (both compiled into lists), `glGetQueryiv`, `glGetQueryObjectiv`, `glGetQueryObjectuiv` for `GL_SAMPLES_PASSED`, with Mesa's rules (`main/queryobj.c`): a name is a query object once begun, a new name at `glBeginQuery` is made one, one active query. Counted exactly by the software rasteriser - every fragment past the alpha, stencil and depth tests, pixel rectangles included - and always available. **The GPU counts them on the console since 2026-09-20**: two `ZPASS_DONE` events bracket the query, each dumping sixteen render backends' counters, and the result is the sum of the differences with `PERFECT_ZPASS_COUNTS` on (`REQ-20260919T2048Z-4d19`). `GL_QUERY_COUNTER_BITS` is 32 on both paths; it was 0 there, GL 1.5's declaration that the count carries no information. A query whose draws never test depth is not counted there and says so - the counters need a bound depth surface, and setting `ZPASS_ENABLE` without one wedged the GPU in `REQ-20260919T1600Z-e3a7`. Found with the software half: **the rasteriser drew pixel centres on a shared edge twice** - see the tie rule under Fixed in the SDK changelog |
| **Separate specular colour, `GL_RESCALE_NORMAL`** (GL 1.2) | `GL_LIGHT_MODEL_COLOR_CONTROL`: under `GL_SEPARATE_SPECULAR_COLOR` lighting keeps the specular term as a secondary colour, which the software rasteriser interpolates and adds after texturing and before fog (GL 1.2, 3.9); flat shading holds it too. `GL_RESCALE_NORMAL` scales the eye-space normal by Mesa's factor, the inverse modelview's third row's length (`main/light.c:1090-1103`). Found on the way, both 2026-09-19: **every normal was normalised** whatever `GL_NORMALIZE` said, so a program's own normal lengths never reached the light - they do now, which is what GL does; and **a shininess of 0 had no specular at all**, where `(n.h)^0` is 1. `GL_LIGHT_MODEL_LOCAL_VIEWER` became queryable and joined `GL_LIGHTING_BIT` with the colour control |
| **Texture LOD parameters** (GL 1.2) | `GL_TEXTURE_BASE_LEVEL`, `GL_TEXTURE_MAX_LEVEL`, `GL_TEXTURE_MIN_LOD`, `GL_TEXTURE_MAX_LOD` (2026-09-19; refused before). Completeness counts from the base level and stops at the maximum one, so a program that uploads one level and sets the maximum level to 0 has a complete texture under a mipmap filter; a base level with no image, or a mipmapped texture whose maximum is below its base, is incomplete (Mesa, `main/texobj.c:729-760`). The software sampler clamps the level of detail and counts levels from the base. **On the hardware**, a base level other than 0 is sampled through a chain built from it - one level if the filter reads no mipmaps - since the image the descriptor otherwise names is level 0's; the maximum level cuts the chain; the clamp is the sampler's MIN_LOD and MAX_LOD in 4.8 fixed point (`ac_descriptors.c:139-140`). All unmeasured on a console: gl1-probe's `lod-params`. `glPushAttrib(GL_TEXTURE_BIT)` now saves the bound textures' parameters as well as their bindings, as Mesa's does |
| **Proxy textures** (GL 1.1, 1.2) | `GL_PROXY_TEXTURE_1D`, `_2D`, `_3D`: `glTexImage` against one allocates nothing, raises no size error, and leaves the level's size and format - or zeros, when it would not fit - for `glGetTexLevelParameter`. Never compiled into a list (Mesa, `main/dlist.c:4163`) |
| **Pixel types** | **every plain type** - `GL_BYTE`, `GL_SHORT`, `GL_INT` and their unsigned forms, `GL_FLOAT` - and **the twelve GL 1.2 packed types**, reading and writing; `GL_RED`, `GL_GREEN`, `GL_BLUE` as formats. A packed type names the format's first component in its most significant bits and `_REV` its least, checked against Mesa's `formats.csv`; which packed type fits which format follows Mesa's `glformats.c`. The signed conversions are the specification's `(2c + 1) / (2^b - 1)` and its inverse, a tie toward zero as Mesa's `FLOAT_TO_SHORT` (`main/macros.h:83`) has it. Storage is still RGBA8, so a float or 16-bit upload keeps eight bits a channel |
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
| **The rest of the type-and-arity grid** | every remaining `glVertex`/`glColor`/`glTexCoord`/`glNormal` spelling. **Integer colours and normals normalise; positions and texture coordinates do not** - conversions from Mesa's `macros.h`, and normals use the same macros as colours rather than a plain cast. `glTexCoord4`'s `q` is a projective divide, which widened the current texture coordinate to four components. The vertex keeps all four undivided and the software rasteriser divides **per fragment**, after interpolation, since 2026-09-19 - before that the vertex divided, which is exact only while q is constant across a primitive. The console's textured pixel shader divides per fragment too since the same evening, q in the texture parameter's `w` - written, not yet seen |
| **Multitexture, two units** (2026-09-19; one unit before) | `glMultiTexCoord*`, `glActiveTexture`, `glClientActiveTexture` and their `*ARB` spellings. `GL_MAX_TEXTURE_UNITS` reports 2 and a unit past it is `GL_INVALID_ENUM`. This row once said one unit was what the specification requires; **the maximum is not allowed to be 1** (GL 1.3, 2.6) - see **Two texture units** in the gap table |
| **Texture coordinate generation** | `glTexGen*`, `glGetTexGen*`, the four enables. Object-linear, eye-linear, sphere map, and GL 1.3's reflection map and normal map for s, t and r (refused until cube maps landed, 2026-09-19). Every mode reads the eye-space normal lighting uses - through the inverse-transpose, unit length only under `GL_NORMALIZE` or `GL_RESCALE_NORMAL`, as Mesa's fixed-function program takes it; sphere mapping used the modelview's own 3x3 and always normalised before. `GL_EYE_PLANE` is stored through the inverse modelview of the moment it was set, which needed a full 4x4 `mat4_invert` |
| **Antialiasing (software)** | `GL_POINT_SMOOTH`, `GL_LINE_SMOOTH`, `GL_POLYGON_SMOOTH` - enables on `GL_ENABLE_BIT` and the point, line and polygon bits. Each fragment's alpha is multiplied, after fog, by the fraction of its pixel the primitive covers: a point is a disc of the unrounded size, a line a rectangle of the unrounded width (its ends too, but for a stipple's dashes), a polygon faded across its own edges and not the diagonals its triangles share, so no seam runs through it. Each coverage is the linear approximation - exact where an edge crosses a pixel straight - and a polygon corner the diagonal leaves comes out half covered where a quarter is right. Smooth sizes step by an eighth, which `GL_SMOOTH_*_GRANULARITY` reports |
| **`GL_COMBINE`** (GL 1.3) | The combiner: `GL_COMBINE_RGB` and `_ALPHA` (`GL_REPLACE`, `GL_MODULATE`, `GL_ADD`, `GL_ADD_SIGNED`, `GL_INTERPOLATE`, `GL_SUBTRACT`, and `GL_DOT3_RGB`/`_RGBA` for the colour), three sources and operands each, and `GL_RGB_SCALE`/`GL_ALPHA_SCALE` - checked as Mesa's `main/texenv.c:107-370` checks them, computed as `main/ff_fragment_shader.c:563-735` does, queryable, on `GL_TEXTURE_BIT` and in lists. `GL_TEXTURE0` is accepted as a source, GL 1.4's crossbar naming the one unit. The scalar `glTexEnvf` stopped truncating through `glTexEnvi`. **On the hardware** it is a program generated into the pixel shader since the same day - written, not yet seen; see the gap table |
| **Cube maps (software)** (GL 1.3) | `GL_TEXTURE_CUBE_MAP`'s binding, enable (outranking 3D, 2D and 1D) and default texture; its six faces through their own targets in `glTexImage2D`, `glTexSubImage2D`, `glCopyTexImage2D`, `glCopyTexSubImage2D`, `glGetTexImage` and `glGetTexLevelParameter`, each face square (Mesa, `main/teximage.c:1079-1100`) and up to `GL_MAX_CUBE_MAP_TEXTURE_SIZE` 1024 - the faces live on the CPU; `GL_PROXY_TEXTURE_CUBE_MAP`. Cube completeness: six faces of one size and format, each mipmap complete under a mipmap filter (`main/texobj.c:800-830`). The lookup by direction is softpipe's `convert_cube` (`sp_tex_sample.c:3220-3291`), GL 1.3's table 3.19, and the level of detail compares neighbouring pixels projected onto the centre's face. **On the hardware a cube-mapped draw is untextured** and says so once in the log; see below |
| **User clip planes** | `glClipPlane`, `glGetClipPlane`, `GL_CLIP_PLANE0..5`. Two registers and no shader change; **three coordinate spaces**, object in, eye stored, clip in the register |
| **Stencil (software)** | `glStencilFunc`, `glStencilOp`, `glStencilMask`, `glClearStencil`, and `GL_STENCIL_BUFFER_BIT` on the attribute stack. The hardware surface is outstanding - see below |
| **Raster position and pixel operations** | all 24 `glRasterPos*` spellings, `glDrawPixels`, `glBitmap`, `glCopyPixels`, `glPixelZoom`. An invalid raster position draws **nothing**; `glBitmap` moves the position even when it draws nothing. **Each pixel is a fragment** (since 2026-09-19; written straight into the colour buffer before): at the raster z, textured by the raster texture coordinate's texel, fogged by the raster distance, then through the triangle rasteriser's own per-fragment tail - scissor, alpha, stencil and depth tests, blending, logic op, masks. `glCopyPixels` reads its whole source before writing (overlapping copies smeared) and zooms; the zoom is GL's centre rule. The colour sum is not applied - GL 1.x has no raster secondary colour |
| **1D textures** | `glTexImage1D`, `glTexSubImage1D`, `glCopyTexImage1D`, `glCopyTexSubImage1D`. **Its own binding point**, its own enable and its own default texture, with 2D winning when both are enabled |
| **Compressed textures** | the seven entry points, with **no formats**: `GL_NUM_COMPRESSED_TEXTURE_FORMATS` is 0 and every upload is refused, which is what the specification says an implementation with an empty format set does |
| **Points and lines** | `GL_POINTS`, `GL_LINES`, `GL_LINE_STRIP`, `GL_LINE_LOOP`, `glPointSize`, `glLineWidth` - **as triangles**: a line is a screen-width quad and a point a square, built back through the inverse model-view-projection. The one-and two-vertex primitives the geometry engine will not take are never submitted |
| **Fog** | `glFog*`, `GL_FOG`, `GL_FOG_BIT`. Eye-space distance, factor per vertex, blend per pixel, alpha untouched. The hardware path is written since 2026-09-19 and not yet seen - see **Fog on the hardware** above |
| **Mipmaps, completeness, the software sampler** | Every level of a texture kept, per-level queries, sub-images and copies; **completeness** (an incomplete texture draws untextured, as on any GL); texture 0 sampled when nothing is bound; and a software sampler that follows the filters - GL-exact nearest, bilinear, the magnification/minification switch, `*_MIPMAP_NEAREST` and `*_MIPMAP_LINEAR` from a per-pixel level of detail - with **perspective-correct** interpolation of every attribute but depth, and the texture coordinate's q divided out per fragment after it. The hardware reads a mip chain since the same day - written, not yet measured (see above) |
| **Colour-index state, multisample state, dithering, hints** | The ten `glIndex*`, `glClearIndex`, `glIndexMask`, `glIndexPointer`, `GL_INDEX_LOGIC_OP`; `glSampleCoverage` and the four multisample enables; `GL_DITHER`; every GL 1.x `glHint` target. **State only, and that is the whole of it**: an RGBA context keeps colour-index state and draws nothing with it, and with no multisample buffer the specification gives the multisample state no effect. The attribute stack gained `GL_POINT_BIT`, `GL_LINE_BIT`, `GL_HINT_BIT` and `GL_MULTISAMPLE_BIT` for state that already existed |
| **`glPolygonMode`, edge flags** | `glPolygonMode`, `glEdgeFlag`, `glEdgeFlagv`, `glEdgeFlagPointer`, `GL_POLYGON_OFFSET_LINE`/`_POINT`. An outline is the primitive's own edges - never a quad's or polygon's triangulation diagonals - drawn through the line expansion; culling is decided for the polygon first. Primitive assembly is one function now, shared by `glEnd` and both array draws |
| **`glLogicOp`, `glBlendColor`** | `CB_COLOR_CONTROL`'s `ROP3` and `CB_BLEND_RED..ALPHA`, both from radeonsi; the constant-colour factors; `glBlendFunc`/`glBlendEquation` now refuse what is not a factor or an equation, and default to GL's `GL_ONE`/`GL_ZERO` |
| **Evaluators** | All 23: `glMap1*`/`glMap2*`, `glGetMap*`, `glEvalCoord*`, `glMapGrid*`, `glEvalPoint*`, `glEvalMesh*`, with the eighteen map enables, `GL_AUTO_NORMAL`, the grid queries and `GL_EVAL_BIT`. **CPU arithmetic ending in ordinary `glVertex` calls**, so an evaluated patch is lit, textured, compiled and drawn like typed-out geometry. Evaluation leaves the current colour, normal and texture coordinate alone; `GL_AUTO_NORMAL` follows the domain's direction, where Mesa's does not; `GL_FILL` meshes are the specification's quad strips. What `glutSolidTeapot` draws through |
| **Selection and feedback** | `glRenderMode`, `glSelectBuffer`, `glFeedbackBuffer`, `glInitNames`, `glLoadName`, `glPushName`, `glPopName`, `glPassThrough`, the queries, and `gluPickMatrix` in the GLU. Primitive assembly reports instead of drawing - each polygon triangle, line and point **before** a line or point is widened - so one path serves every way of drawing. **Clipped geometrically**, against the view volume and the user planes, which the drawing path never needed: a hit's depth range and a feedback polygon are what is left after clipping. Culled polygons are no hit; `glPolygonMode` decides what a polygon is reported as; nothing reaches the framebuffer, `glClear` included. Feedback texture coordinates are all four components as the vertex holds them, undivided (Mesa `main/feedback.c:136-139`) - `(s/q, t/q, 0, 1)` until 2026-09-19 |
| **Pixel maps and transfer** | `glPixelTransfer{f,i}`, `glPixelMap{fv,uiv,usv}`, `glGetPixelMap{fv,uiv,usv}`, their queries and `GL_PIXEL_MODE_BIT`. Scale, bias, colour-map lookup and clamp - Mesa's order - on every colour rectangle: `glDrawPixels`, `glCopyPixels`, the texture uploads, the texture copies (once, not once reading and again uploading) and `glReadPixels`. The depth, stencil and index halves apply to their rectangles since 2026-09-19 - `GL_DEPTH_SCALE`/`BIAS`; `GL_INDEX_SHIFT`/`OFFSET` with `GL_MAP_STENCIL`'s map for stencil; the shift, offset and `GL_PIXEL_MAP_I_TO_R/G/B/A` for colour indices. `glReadPixels` into luminance now sums R, G and B, as GL does |
| **Stipple** | `glLineStipple`, `glPolygonStipple`, `glGetPolygonStipple`, both enables, the pattern and repeat queries and `GL_POLYGON_STIPPLE_BIT`. **A stippled line is cut into its dashes before it is widened** - counted along the major axis, carried across a strip, restarted per separate line and per outlined polygon, attributes perspective-correct along it - so its dashes are ordinary quads and **the hardware draws them too**. The polygon stipple is a 32x32 window-space mask applied to filled polygons, **on both paths since 2026-09-20**: the console draw sets `SPI_PS_INPUT_ENA`/`_ADDR` to `0x302` so the fragment's position arrives in `v2` and `v3` (obSCEne `REQ-20260919T2258Z-c7d4`), and a sixteen-word slot in both pixel shaders loads the mask row and kills the lanes whose bit is clear (`tools/shader/polygon-stipple.s`). The CPU writes the mask rotated for the window height and bit-reversed, so the slot every fragment runs is a load and a shift rather than four operations; `test_pm4_gl_polygon_stipple_discards_in_the_shader` checks the two forms agree for every pixel of a 32x32 window. gl1-probe's `polygon-stipple` is expected to pass on the console |
| **3D textures** | `glTexImage3D`, `glTexSubImage3D`, `glCopyTexSubImage3D`, `GL_TEXTURE_3D`'s binding, enable and default texture (3D beating 2D beating 1D), `GL_TEXTURE_WRAP_R`, `GL_TEXTURE_DEPTH`, `GL_MAX_3D_TEXTURE_SIZE` (256 - the volume is on the CPU), `GL_UNPACK_IMAGE_HEIGHT` and `GL_PACK_IMAGE_HEIGHT`, mip levels that halve depth too, and completeness counting it. The vertex carries r - through generation and the texture matrix - and the software sampler filters in three dimensions, eight texels for GL_LINEAR, with r's rate of change in the level of detail. **On the console since 2026-09-20**: the descriptor's `TYPE` is `0xa` with the last slice in `WORD4`, the vertex carries r in its third parameter, and the pixel shader's sample slot divides it by q and samples with `dim:SQ_RSRC_IMG_3D` (`tools/shader/tex-3d.s`). Level 0 only there - no chain is built for a volume, so a minifying filter reads the base level and the log says so once; see below |
| **Accumulation buffer** | `glAccum` (all five operations), `glClearAccum`, `glClear`'s `GL_ACCUM_BUFFER_BIT`, `GL_ACCUM_CLEAR_VALUE`, the accumulation and colour and depth bit counts, and `GL_ACCUM_BUFFER_BIT` on the attribute stack - which makes every GL 1.x attribute group savable. Signed 16-bit channels on the CPU, allocated on first use; reads through `glReadPixels`' flush-and-readback and the return a CPU write like `glDrawPixels`', so **it works on the hardware path as it stands**. Operations and the clear keep to the scissor box; the return goes through the colour mask; sums clamp where Mesa's 16-bit arithmetic wraps |
| **The texture matrix** | Applied, from 2026-09-19 - to every vertex's texture coordinate after generation, and to the raster position's. It had been kept, loaded and queried and never used. The raster position is now processed as the vertex it is: its colour lit, its coordinate generated and transformed, its w the clip w |
| **The GLU a port calls** | `gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluPickMatrix` and `gluErrorString` from the start; `gluBuild2DMipmaps`, `gluBuild1DMipmaps`, `gluScaleImage`, `gluProject`, `gluUnProject` and `gluGetString` added 2026-09-20 (`src/gl/gl_glu.c`). GLU is not GL, but a port that cannot find it does not build, and mipmap chains were written with `gluBuild2DMipmaps` until `GL_GENERATE_MIPMAP` (1.4) - which is to say in most of the code being ported. **The filter is a box**, as SGI's is: an output pixel is the average of the input pixels its footprint covers, and each mipmap level is built from the level above rather than from the original, so a level is the average of everything above it. The builders scale to powers of two, set the unpack state they need and put the caller's back. Errors are GLU's own codes, returned and not recorded in `glGetError`. The **quadrics** followed the same day: `gluSphere`, `gluCylinder`, `gluDisk`, `gluPartialDisk` and the seven calls that configure them, emitted as ordinary `glVertex` calls so they light, texture, compile into a list and reach the hardware path like typed-out geometry. The poles are triangle fans and the bands quad strips, as GLU does it. Two conventions are pinned by reading the geometry back through `GL_FEEDBACK`, because a port cannot see either go wrong in its own source: `s` at 0 on the +y axis and 0.25 on +x, and GLU_OUTSIDE winding counter-clockwise seen from outside - a sphere wound the other way vanishes under the default cull. **Not here:** the tessellator and NURBS - a program needing them fails to link, which says so where a stub that draws nothing would not |

## Left in 1.x, with what it costs

### The core entry points left

See the table under "The gap, measured". Everything there is software, and 3D textures are on
the console as well since 2026-09-20.

### Cheap and additive - no hardware risk

Nothing in this group touched a register or a shader, so the cost was writing it.

**This group is now empty**: the type-and-arity grid, multitexture on one unit, texture
generation, the raster position family, 1D textures and the compressed-texture refusals all
landed on 2026-09-17 and have moved to **Done** above.

> The paragraph that stood here said the ~100 genuinely distinct features were "waiting to be
> decided on, and most of them should stay absent", and listed `glStencil*`, `glBitmap` and
> `glTexGen*` among them. Four of those were decided the other way and are implemented, and so
> since are evaluators, selection and feedback, and colour-index state; of the rest, only the
> imaging subset is optional, and the accumulation buffer is costed above. The list was a snapshot of
> what had not been costed yet, not a ruling, and reading it as one is how a gap stays a gap.

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


### Needs a shader change - real risk to the oracle frame

> **Fog's hardware half was written 2026-09-19, the first way below** - the factor in the
> texture parameter's `z`, twelve pixel-shader words, no interface change (`gl_ps_patch_fog`,
> `tools/shader/fog.s`). It is unmeasured; gl1-probe's `fog` and `fog-coord` are expected to pass
> on the console now, and are the measurement. What follows is how it was costed.
>
> **Fog is implemented in software as of 2026-09-19** and the paragraphs below are its hardware
> half. They were written before two things were known, and both shrink the job:
>
> - **It need not move the shader interface at all.** The factor can be computed per vertex on
>   the CPU - where the software path computes it, and where texture generation already runs -
>   and ride in the texture-coordinate parameter's unused `z`: the vertex shader exports that
>   parameter as a full `vec4` already. Then only the pixel shaders change, to blend towards the
>   fog colour before export, and their words come out of the assembler like every other shader
>   word here.
> - **The interface can move if it has to.** obSCEne sweep `20260917-200129` ran
>   `166-agc/primitive-draw-param3` on oops-gl's own non-passthrough stage (`0x00c12010`) with a
>   third parameter export (`SPI_VS_OUT_CONFIG` 0x4, `SPI_PS_IN_CONTROL` 0x3,
>   `SPI_PS_INPUT_CNTL_2` 0x2; arm `primary`) and with that plus `POS_Z` (`SPI_PS_INPUT_ENA`
>   0x402; arm `pos-z`), and **both retired and drew** - `fence-hit 1`, 512 modified pixels, the
>   same as the two-export control. The log rows were read, not just the resolution: they are
>   under the arm names `control-2param`, `primary` and `pos-z`, not the `arm1-3param-unpacked`
>   and `arm2-3param-packed` that `REQ-...-7c40`'s resolution names. What the rows establish is
>   that a third export and `POS_Z` do not stop the draw; they do not show the third parameter's
>   *values* arriving intact, because the probe's expected values are not in the log.
>
> gl1-probe's `fog` check was expected to fail on hardware until the pixel shaders changed; they
> have.
>
> **The polygon stipple landed on the console 2026-09-20.** RDNA2 has no stipple hardware;
> radeonsi discards in a pixel-shader prolog that looks the fragment's window position up in the
> 32x32 mask, and oops-gl now does the same. `SPI_PS_INPUT_ENA`/`_ADDR` `0x302` brings `POS_X`
> and `POS_Y` in after the barycentrics - **measured**, not assumed: obSCEne's
> `REQ-20260919T2258Z-c7d4` reported `v2` as 39.5 and `v3` as 31.5 for the pixel it sampled,
> against a control at `0x2` whose `v2` was zero. The mask lives at `OOPS_GL_STIPPLE_OFFSET`,
> rotated and bit-reversed so the slot is a load and a shift. The line stipple needed none of
> this: its dashes are cut on the CPU.
>
> **Antialiasing landed for points and lines on the console 2026-09-20** (software since
> 2026-09-19), and this paragraph had the reason it was waiting wrong. It said the fragment's
> position was the hard part and the primitive's shape had still to reach the shader. Neither is
> true: the *offset* from the primitive's centre is linear across the quad the CPU already widens
> it into, so the CPU writes it at the corners and the interpolator carries it in - the position
> is not needed at all. Fifteen words in the untextured pixel shader
> (`tools/shader/coverage.s`), and gl1-probe's `smooth` is expected to pass.
>
> **A textured smooth primitive and GL_POLYGON_SMOOTH are what is left.** The first has no spare
> interpolant, the offset riding in the parameter the texture coordinate uses. The second needs
> two things this does not have: three edge fades multiplied rather than one distance, and the
> pixels a triangle only *partly* covers - which the rasteriser does not raise, so the outer half
> of every edge would simply not be drawn. Both say so in the log.
>
> **Cube maps landed on the console 2026-09-20** (software since 2026-09-19). All three of their
> needs are met: the six faces upload as one array of slices (`gl_tex_cube_upload`), the
> descriptor carries `TYPE 0xb` with six slices, and the pixel shader's sample slot does the
> lookup by direction - the face and the place on it - with RDNA2's own `V_CUBEID_F32`,
> `V_CUBESC_F32`, `V_CUBETC_F32` and `V_CUBEMA_F32` (`tools/shader/tex-cube.s`) before sampling
> with `dim:SQ_RSRC_IMG_CUBE`. The direction's r rides in the third parameter, which such a draw
> exports whether or not a colour sum wants one. obSCEne's `REQ-20260920T0745Z-6c80` measured the
> receiving side; the slice stride the array relies on is derived rather than measured and is
> asked about in `REQ-20260920T1050Z-5d7c`. `GL_ARB_texture_cube_map` is on both extension lists.
> gl1-probe's `cube-map` is expected to pass on hardware.
>
> **Occlusion queries needed no shader at all, and are done** (the console, 2026-09-20). Two
> `EVENT_WRITE ZPASS_DONE` packets bracket the queried draws - event type 21, `EVENT_INDEX` 1,
> the dword `0x00000115` - each dumping every render backend's counter sixteen bytes apart, and
> the answer is the sum of the differences. `REQ-20260919T2048Z-4d19` measured all of it: sixteen
> backends (`max-render-backends 0x10`, `enabled-rb-mask 0xffff`), bit 63 as the valid marker,
> and a sum of `0x200` against a draw of exactly 512 pixels.
>
> **The counters were already running.** `DB_COUNT_CONTROL`'s `ZPASS_ENABLE` is bits [8,11]
> (`gfx103.json:12552-12565`) and the measured depth-block recipe this library has always emitted
> is `0x11000100` - that field set. What a query adds is `PERFECT_ZPASS_COUNTS` and
> `DISABLE_CONSERVATIVE_ZPASS_COUNTS`, making the `0x11000106` `-4d19` measured exact against,
> and it adds them as a separate register write after the depth surface is bound rather than
> inside the depth block, so a frame with no query emits the stream the gl-cube oracle pins.
>
> **A query whose draws never test depth is the one case left.** `REQ-20260919T1600Z-e3a7` set
> `ZPASS_ENABLE` with no depth target and the depth block stalled before the pixel shader ran -
> the fence never retired. So counting starts at the first draw that binds the depth surface and
> never before one; a query without that keeps the CPU's count and says so in the log.
> `GL_QUERY_COUNTER_BITS` is 32 on both paths now. `GL_ARB_occlusion_query` is on both extension
> lists, its eight entry points having arrived with it.
>
> **Depth textures needed the same three pieces, and have them** (software 2026-09-19, the
> console 2026-09-20). The descriptor's `FORMAT` is `GFX10_FORMAT_32_FLOAT`, 22
> (`gfx10-rsrc.json:27`, beside the 56 this library's descriptors already carried, which is the
> cross-check that the table is the right one). The comparison reference reaches the pixel shader
> in the third parameter, like a volume's r and a cube map's direction. And the sample is
> `image_sample_c` with the sampler's `DEPTH_COMPARE_FUNC` set, which `-6c80` measured working.
>
> **The depth mode is not the destination swizzle**, which is what this paragraph used to say it
> would be: `dmask:0x1` asks for the one value a comparison returns, and three moves spread it
> across `v4..v7` as `GL_DEPTH_TEXTURE_MODE` says. That keeps it out of the descriptor, where it
> would have collided with the border colour - a depth texture's border is a *stored depth* the
> comparison still runs against, so it goes into the table as the border colour's red in all
> four rather than expanded by the mode.
>
> A shadow map is made by copying the depth buffer, which the console has done since 2026-09-19,
> when the depth surface's 64KB_Z_X layout became addressable from the CPU (see **Done**). So the
> whole of `GL_ARB_shadow` runs there, and gl1-probe's `shadow-compare` is expected to pass.
> **One value in it is documentation rather than measurement**: which address register
> `image_sample_c` reads the reference from. `-6c80`'s arms prove the reference is read from
> whichever register they varied but not its index, and `REQ-20260920T1340Z-b4e1` asks for it.
>
> **The textured half of the colour sum had the same wait, and is written** - the colour sum of
> `GL_SEPARATE_SPECULAR_COLOR` and GL 1.4's `GL_COLOR_SUM` (software since 2026-09-19, the
> console that night, not yet run there). **The third parameter is measured:** in
> `REQ-20260919T1745Z-9c3e`'s sweep `20260919-212620`, the unpacked form carried
> `(0.25, 0.5, 0.75, 1.0)` to the pixel shader byte for byte. That form is `SPI_VS_OUT_CONFIG`
> 0x4, `SPI_PS_IN_CONTROL` 0x3, `SPI_PS_INPUT_CNTL_2` 0x2, with a control on two parameters that
> matched. The packed form hung the GPU in the same sweep, and it is not used. So:
> - **The draw:** a textured draw whose secondary colour is not zero runs a three-parameter
>   vertex shader (`tools/shader/vs-param3.s`, at payload 0x700) with 64-byte vertices. The
>   third vec4 is `{secondary r, g, b, unit 0's r}`, so the r that 3D, cube-map and
>   depth-compare sampling will read is already on board.
> - **The shader:** the textured pixel shader adds the secondary colour after the combine and
>   before fog, clamped (`tools/shader/colour-sum.s`, a twelve-word slot at 108). Fog, the alpha
>   test and the export moved up twelve.
> - **The switch:** the vertex stage switches back to two parameters for any other draw, and
>   only on a change, so gl-cube's command stream is untouched.
> - **Untextured,** the per-vertex sum stays, since with no texture between them it is GL's sum
>   except where it saturates between the vertices.
>
> gl1-probe's `separate-specular` should pass on the console now, and it is the measurement.
> `colour-sum`, untextured, is expected to pass as before. **Next on the same interface:** the
> second texture unit needs a fourth parameter, `{s, t, r, q}` of unit 1. The mechanism is the
> measured one, but nothing has run four parameters on a console yet.
>
> **A q that varies across a primitive needed it too, and has it** (software since 2026-09-19,
> the console the same evening): the vertex carries s, t and q undivided, q in the texture
> parameter's `w` beside fog's `z`, and the textured pixel shader divides after interpolating -
> a `v_rcp_f32` and two `v_mul_f32` before the sample, from `tools/shader/tex-prolog.s`. No
> interface register moved. gl1-probe's `projective-texture` is expected to pass on the console
> now, unmeasured.
>
> **A second texture unit, or r, does move the interface.** The texture parameter's four
> components are s, t, fog and q, and the colour parameter's are the colour: unit 1's coordinate,
> and the r that 3D textures, cube maps and a depth texture's comparison read, need a third
> parameter export.
>
> **3D textures needed more than that, and have it** (software 2026-09-19, the console
> 2026-09-20). All three pieces are in: the descriptor's `TYPE` is `0xa` with the last slice in
> `WORD4` (`-6c80` measured both), the volume laid out slice after slice as addrlib lays out a
> linear 3D surface - the same derivation the mip chain's layout was, and the same one the cube
> array uses, asked about in `-5d7c`; **r carried to the pixel shader** in the third parameter's
> `w`, the export the colour sum already added; and a sample slot holding
> `tools/shader/tex-3d.s`, which divides r by the `1 / q` the prolog left in `v12` and samples
> with `dim:SQ_RSRC_IMG_3D`. gl1-probe's `texture-3d` is expected to pass there now.
>
> **What is still level 0 only is the mip chain.** A volume's levels halve depth as well, and
> `gl_tex_chain_layout` lays out a 2D chain, so `LAST_LEVEL` stays 0 and a minifying filter
> reads the base level on the console while the software rasteriser reads the chain. A draw
> whose filter would have used one says so once in the log.

> **Superseded for fog, 2026-09-19.** The analysis below assumed the shader had to derive the
> factor from the fragment's depth. It does not: GL 1.x lets the factor be computed per vertex and
> interpolated - the software path always has - so the CPU computes it and the texture
> parameter's spare `z` carries it, and nothing below had to move. The analysis still holds for
> anything that truly needs the fragment's position: the polygon stipple and antialiasing.

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
against the alpha and depth tests. **The console's half was written 2026-09-19 and is
unmeasured.** obSCEne closed `REQ-20260917T1845Z-3d5b` as not-possible - its fixture cannot run
oops-gl's stage - so it was written from radeonsi and gfx103.json, and gl1-probe's `stencil` is
the measurement:

- a STENCIL_8 surface at the depth surface's 64KB_Z_X swizzle, both axes padded to 256 (8-bit
  64 KiB blocks, addrlib `addrlib2.cpp:1698-1709`), made live by `DB_STENCIL_INFO` `0x20000181` -
  the measured recipe's `0x20000180` with the format set - and its four bases, only in a frame
  that tests stencil, so gl-cube's recorded stream is untouched;
- per stencil-tested draw, `STENCIL_ENABLE` and `STENCILFUNC` in `DB_DEPTH_CONTROL`, and
  `DB_STENCIL_CONTROL`/`DB_STENCILREFMASK`/`_BF` as radeonsi fills them (`si_state.c:1325-1333`,
  `:1353-1378`, `:1418-1428`), one state for both faces;
- a whole clear as a fill of the clear byte (a constant is the same in any layout), a boxed or
  masked one drawn with `REPLACE` through the write mask;
- the stencil pixel operations, refused at first because the surface is tiled, and addressed
  through that tiling since the same evening (**Depth and stencil surfaces from the CPU**, in
  **Done**).

> This paragraph used to say stencil "has the same shape one level down: the depth buffer is
> `Z_32_FLOAT` with no stencil plane, so it needs a different depth format and a reallocated
> buffer, and `DB_Z_INFO` is pinned by that test." **That was wrong on this part.** Stencil is a
> *separate surface* with its own registers - `DB_STENCIL_INFO` at context offset `0x011`,
> `DB_STENCIL_READ_BASE` at `0x013`, `DB_STENCIL_WRITE_BASE` at `0x015`, `DB_STENCILREFMASK` at
> `0x10C` (`mesa/src/amd/registers/gfx103.json`) - so `Z_32_FLOAT` having no stencil plane costs
> nothing and `DB_Z_INFO` does not have to move. The depth block already writes every one of
> those registers, zeroed. What is missing is a surface and the `DB_STENCIL_INFO` value that
> turns it on, which is obSCEne `REQ-20260917T1845Z-3d5b`.

> **The second unit is done in software as of 2026-09-19** (the gap table's first row), and the
> console leaves it out of the draw with one log line; what follows is its hardware half, which
> stands. One correction to the last paragraph: the software side was not small after all - it
> was the largest GL 1.x gap left, because one unit is short of GL 1.3.

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
count alone. (The third export itself is no longer the unknown: sweep `20260917-200129`'s
`primary` arm drew with one - see the fog note above.)

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

### Measured shut - and drawn another way

**One- and two-vertex primitives.** A measured negative. obSCEne submitted both on retail
hardware across five sweeps, including one using **oops-gl's own `VGT_SHADER_STAGES_EN` value**
(`0x00c12010`, sweep `20260917-010918`, `REQ-...5a3e`), and every run recorded `fence-hit 0` and
`pixel-hit 0`: the pipe stops rather than the primitives drawing wrongly. The same path with three
vertices retires and draws, so the geometry engine wants three vertices per primitive.

> This section used to say that made **points and lines** themselves shut, and `glPolygonMode`'s
> wireframe with them. It did not: the measurement closed the *primitive*, not the picture. Points
> and lines are drawn as triangles now (see **Done**), and `glPolygonMode` can use the same
> expansion on a triangle's edges. gl1-probe's `points-and-lines` check is the hardware answer to
> whether the expansion is right, and it has not yet run on a console.

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
| Code generation | **The export-count gate is measured.** `REQ-...-f9d3`'s probe never launched a wavefront; its re-file `REQ-...-7c40` did, on oops-gl's own non-passthrough stage (`0x00c12010`, sweep `20260917-200129`), and a third parameter export retired and drew like the two-export control (arm `primary`). What that does not yet show is the third parameter's values arriving intact - gl2-cube's first hardware run is where that is read |
| Instruction encoding | **started** - `src/gl/glsl_emit.c`. VOP1/VOP2 formats, the constant forms, and `mat4 * vec4`, every field verified against clang's own output |
| Instruction selection from the AST | **started** - `src/gl/glsl_gen.c`. A value is consecutive VGPRs, one per component, `mat4` column-major; a bump allocator with a per-statement mark; `+ - *` component-wise, scalar broadcast either way round, `mat4 * vec4`, unary minus, swizzle reads, `vecN`/`matN` constructors (and `matN(s)` fills the **diagonal**, which is a different rule from `vecN(s)`), assignment, declarations with initialisers, blocks. Everything else - `/`, integer arithmetic, comparisons, calls, swizzle writes, compound assignment - sets `error` and emits nothing, because an instruction whose encoding has not been read out of an assembler is not guessed |
| Anything about the shader *interface* | **partly** - varyings are parameter exports, and the export count is two in every shader oops-gl ships. `REQ-...-3a91` and `REQ-...-f9d3` could not answer whether it can be three; `REQ-...-7c40` did (see Code generation), so three is measured to draw and the values are what is left |

So the back end waits on what fog's hardware half and a second texture unit also wait on - a
pixel-shader and vertex-shader rewrite, now that the export count is measured - and on GL 1.x
being finished first. The front end waits on nothing, and none of it is wasted.

Nothing in 1.x is wasted when it arrives: a GL 2.0 context still has to answer every 1.x call.

## How shader words get written here

Not from memory. `tools/shader/` holds the source, it is assembled for gfx1030, and the words go
into the C with the instruction beside them. A wrong encoding cannot fail loudly - it assembles
into the payload, the hardware does something else, and the frame is wrong rather than the build
stopped. See `tools/shader/README.md`.
