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
console is a smooth primitive using **two** texture units and a volume's mip chain. A smooth
*textured* primitive left that list on 2026-09-21 and passed on hardware the same day, and
`GL_POLYGON_SMOOTH` followed it and passed there too. The volume chain's layout was
written too and the part read the base level anyway, so that one waits on a measurement rather
than on code (`REQ-20260921T1300Z-9b73`). Two units needs a fifth parameter export, which has
never run on this part (`REQ-20260921T1210Z-4f16`).

## What the console actually does - 2026-09-20, the first rows

Everything above this section was written from the host software rasteriser and from obSCEne's
register measurements. On 2026-09-20 gl1-probe ran on a console, and after four runs - the first
three of which each found a fault in the instrument rather than in the GL - the suite ran to the
end and printed its own verdict. The next morning, with the readback fix below, it printed this
one:

```
gl1-probe: 82/85 passed on hardware
```

**Every check in the suite has a verdict from real hardware**, which is the first time any of
these claims has been answerable rather than argued, and it is the number this document should
be read against. It was 74 the evening before; six of the eight failures were one bug. The two
that remain are `blend-constant` and `front-and-back`, and `depth-range-in-frame` moves between
them: it has failed four times and passed four times, so two different totals on one build are
both honest readings - 82 and 81 came back from the same build within minutes. The suite grew to
85 on 2026-09-21, and each new check ran on a console the same day: `smooth-textured` and
`polygon-smooth` both passed the first time they ran, and `volume-mipmap` failed twice with the
same pixel and is the third standing failure.

**The suite cannot measure the descriptor ring, and the run that tried is worth recording.** The
ring exists so a frame that changes texture takes another slot rather than submitting, and the
number that would show it is the suite's total submissions: 193 before it, **194 after**, for
one more check. Per check it is identical - `combine` submits six times either way,
`multitexture` five. The reason is that 83 of the 85 checks read a pixel back, and a readback
submits; the probe's submissions are its own measuring, not its texture changes. The mechanism
is pinned on the host, where `test_pm4_gl_descriptor_ring_wraps_into_a_submission` and two
rewritten tests show a texture change taking a slot and the stream carrying on. What would show
the *benefit* is a program that draws many textures a frame and reads nothing back - which is a
port, not a probe.

**And a program ran.** `oops-apps/src/oops-gl/glut-demo` - a GLUT program with no SDK call in it
but its entry point - drew its lit solids and its `glutBitmapString` frame counter on a console
on 2026-09-21, for 11,769 frames at a steady ten-to-twelve milliseconds a flip, with every
submission returning `0x0` and none of this library's "a feature this path does not apply" lines
in the log. A check suite says the pieces work; that says a port does.

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

**The first six were one bug, and it is fixed** (2026-09-21). The CPU puts colour into the
render target and the check reads it back through `frame()`, which is
`glGetFrameReadbackSampled` - and that handed out `ctx->readback`, **the CP's copy as of the
last submit**, without asking whether it was still current. A CPU pixel rectangle builds no
command stream, so no submit follows it and nothing re-copies the buffer; the check read the
frame as it was before its own pixels, which is why `0xff202020`, the clear colour, came back
from three of them.

**The passing neighbours were the discriminator all along.** `read-pixels` passes because
glReadPixels already asks `gl_color_read_source` which buffer is current, and `stencil-pixels`
and both readback checks pass because those surfaces are the CPU's own and never travel through
`readback`. Read as corroboration for a memory-ordering story they sent the search the wrong
way for a day: a CPU store fence shipped on 2026-09-20 and the console returned all eight
failing pixels byte for byte unchanged, which is what finally ruled it out. The fence stays -
the CP's DMA does need it - but it fixed nothing that was measured, and
`REQ-20260920T2230Z-7c31` is withdrawn.

**`blend-constant` is one channel.** GL says `0xff80ff00`; the console returned `0x40804000`.
Red `0x80`, blue `0x00` and alpha `0x40` are exactly right - and green is `0x40`, the constant's
*alpha*, where its green of 1.0 belongs. Three channels correct rules out the register offset,
the packet's count and the factor encoding, all of which would spoil every channel together.

**And the register is now named, by the probe itself** (2026-09-21, build `10:39`). The check's
own colours could not do it: its constant is `(0.5, 1, 0, 0.25)`, so green and alpha are the only
two channels that differ and there is no third value to tell "green read alpha" from "green read
something that happens to be 0.25". `blend_constant_diagnose` draws four more rectangles first,
every channel a value no other channel has, and prints each pixel as a `saw` row. What came back:

| Row | Constant, factor | Wanted | Measured |
|---|---|---|---|
| `blend-constant/colour` | `(0.25, 0.5, 0.75, 1.0)`, `GL_CONSTANT_COLOR` | `0xff4080bf` | **`0xff40ffbf`** |
| `blend-constant/no-a` | the same, alpha moved to `0.0` | `0x004080bf` | **`0x004000bf`** |
| `blend-constant/c-alpha` | `(0.25, 1.0, 0.75, 0.5)`, `GL_CONSTANT_ALPHA` | `0x80808080` | `0x80808080` |
| `blend-constant/rewrite` | `colour` again, after another constant that frame | `0xff4080bf` | **`0xff40ffbf`** |

Red `0x40` and blue `0xbf` are exact in every arm - 0.75 quantises to `0xbf`, not `0xc0` - and
green is the constant's **alpha** in both `CONSTANT_COLOR` arms. `no-a` is the one that settles
it: moving only the alpha, 1.0 to 0.0, moves green with it, `0xff` to `0x00`, while
`CB_BLEND_GREEN` held 0.5 throughout. Green is not stale and not unwritten; it is reading
`CB_BLEND_ALPHA`'s live content. `c-alpha` is right in every channel, because under
`CONSTANT_ALPHA` every channel is *meant* to read alpha - which is exactly why the original
check could not separate these. And `rewrite` is byte-identical to `colour`, so neither the
packet's position in the frame nor a preceding constant write changes anything.

**So: with `COLOR_SRCBLEND = BLEND_CONSTANT_COLOR` (13) on this part, the green component takes
`CB_BLEND_ALPHA` (`0x108`) where red and blue take their own.** The write is not in question -
the four go out as one `SET_CONTEXT_REG` at `0x105` in R, G, B, A order, pinned by
`test_pm4_gl_logic_op_and_blend_constant_reach_their_registers` - so what remains is whether
some register this library never programs is responsible. This part has RB+ and oops-gl sets
none of `SX_MRT0_BLEND_OPT`, `SX_BLEND_OPT_EPSILON`, `SX_BLEND_OPT_CONTROL` or
`SX_PS_DOWNCONVERT`, which is the first place to look.

`REQ-20260920T2320Z-4b8d` asked obSCEne this and came back without it: all three arms returned
`0xffffffff`, because that fixture's standalone target sets `BLEND_BYPASS` and its blender never
ran. **oops-gl was the better instrument** - it renders into the display's own scanout buffer,
and a blended write is a read-modify-write that a standalone allocation will not take.
`REQ-20260921T1040Z-2e9f` now carries the four rows and asks the narrower question.

**`depth-range-in-frame` is a third, and its command stream is not the fault.** It failed on
2026-09-21 with `saw 0xffff0000` - **red**, the first of its two rectangles. It draws red under
`glDepthRange(0.5, 1.0)` and green under `glDepthRange(0.0, 0.5)` at the same NDC z with
`GL_LESS`, so window z is 0.75 then 0.25 and the green belongs on top; red survives exactly when
both land at the same depth and the second fails `GL_LESS` on equality. That is the pixel a lost
second write produces - but the write is not lost.
`test_pm4_gl_depth_range_changes_within_a_frame` runs that sequence and finds both
`PA_CL_VPORT_ZSCALE` and `ZOFFSET` emitted twice inside one frame, `0x3f400000` then
`0x3e800000`, the second with its own draw. Only the offset moves between those two calls; the
scale is 0.25 both times, so a check that watched the scale would have seen nothing. The
register writes are there, and what the part does with a depth range changed between two draws
of one frame is the open question. It passed once and has failed twice, so it is not reliably
either.

**`front-and-back` is the front half, and it is blending** (found 2026-09-21, fixed in code,
**not yet run on a console**). Its `saw` is the *back* buffer, and `0xff00ffff` is the cyan GL
calls for, so the back target and the additive blend into it are right; what the check also
wants is yellow in the front. The front had every surface register it needed - `CB_COLOR1_BASE`,
`_BASE_EXT`, `_INFO`, `_ATTRIB2`, `_ATTRIB3`, its half of `CB_TARGET_MASK`, its export - and no
blend control, because **blending is per target on this part** and only MRT0's was ever written.
`CB_BLEND1_CONTROL` is `0x028784` (Mesa `src/amd/common/amdgfxregs.h:12802`), context offset
`0x1e1`, and radeonsi writes the whole run as `R_028780_CB_BLEND0_CONTROL + i * 4`
(`si_state.c:420`). So the back added its fragment and the front *replaced* with it: the back
came out cyan and the front came out the fragment's own green instead of yellow. It is now
emitted beside `0x1e0` in one two-dword packet, carrying the same state when a second target is
bound and zero when it is not, pinned by `test_pm4_gl_front_buffer_targets` in both directions.

**That was a real bug and it was not the whole of this one.** With `CB_BLEND1_CONTROL` emitted,
the check still failed on hardware, `saw 0xff00ffff` unchanged. The `saw` row never said
anything about it: it reads the centre of the probe region, which is the **back**, and this
check's back has been the cyan GL asks for since the check was written. Three consecutive runs
reported the half that was already right. The check now prints its own `read_centre` results,
before and after the blended draw and then a second time, and those rows say what is left:

| Row | 10:46 | 11:32 | 11:39 | Wanted |
|---|---|---|---|---|
| `was-f` (front before) | - | `0xffff0000` | `0xffff0000` | red, **exact** |
| `was-b` (back before) | - | `0xff0000ff` | `0xff0000ff` | blue, **exact** |
| `front` | `0xffffff14` | `0xffffff56` | `0xffffff66` | `0xffffff00` |
| `back` | `0xff00ff00` | `0xff00ff46` | `0xff00ff46` | `0xff00ffff` |
| `front2`, `back2` (read again) | - | - | identical to above | - |

**The fix works and a second thing is wrong.** The front is red plus green in every run, which
it had never been: `CB_BLEND1_CONTROL` was the reason it replaced instead of adding. What
remains is **the blue channel alone, on both targets, in opposite directions** - the back loses
the `0xff` it started with, the front gains blue it never had, and the two do not sum to `0xff`.
Red and green are exact on both. The read path is not the suspect: `was-f` and `was-b` are
byte-exact through the same helper moments earlier, and `front-buffer` makes those same two
reads and passes. Nor is it a value still settling, because reading again with nothing drawn
between returns the same bytes - though the same code gave `0x14`, `0x56` and `0x66` for the
front across three runs, so something outside the draw moves it. `REQ-20260921T1150Z-5c07`
carries all of it, and points at the RB+ registers this library never programs, as
`blend-constant` does - two single-channel errors in the colour block with every other channel
exact, which may or may not be one thing.

What sent that search to the wrong place for an hour was a stale comment: `gl_draw_targets` in
`gl_context.c` still said the hardware had one colour target and dropped the second buffer with
a log line, which had not been true since the MRT1 work landed the day before. The tiling of a
target that does not exist is a good thing to spend an hour on and a bad thing to be told.

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
| ~~3D textures~~ | ~~3~~ | **Done** 2026-09-19 in software, 2026-09-20 on the console - see **Done**. Level 0 only there; the mip chain's layout was written 2026-09-21 and the part read the base level anyway (`REQ-20260921T1300Z-9b73`) |
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
| ~~**Two texture units**~~ - found and **done in software** 2026-09-19 | 1.3 | **GL 1.3 requires at least two** - "The number of texture units supported is implementation dependent but must be at least two" (OpenGL 1.3 specification, section 2.6, page 13), and table 6.29 gives `MAX_TEXTURE_UNITS` a minimum of 2. oops-gl had one and reported 1, which this document called conformant from the day the multitexture entry points landed; it never was. **It reports 2 now**, and each unit has its own state (`gl_tex_unit_t`: enables, bindings, environment and combiner, LOD bias, texture matrix stack, generation), current, raster and array texture coordinates, and its coordinate in the vertex; the rasteriser samples every applying unit and runs each environment on what the unit before left, `GL_PREVIOUS` being that and `GL_PRIMARY_COLOR` the fragment's own, GL 1.4's `GL_TEXTUREn` any unit's texel (zero for one applying none, as Mesa reads it). `GL_TEXTURE_BIT`, `GL_ENABLE_BIT`, `GL_CURRENT_BIT` and the client vertex-array bit save every unit and the selectors; lists record `glMultiTexCoord` with its unit; `GL_TEXTURE1`..`GL_TEXTURE31` are declared. Feedback and the evaluators are unit 0's, as Mesa's are. **On the console** two units are applied since 2026-09-20 (`REQ-20260920T0745Z-9a41`): the fourth parameter carries unit 1's coordinate, its sample sits inside whole-quad mode beside unit 0's, and its environment runs on what unit 0's stage left. gl1-probe's `multitexture` passes there, 2026-09-21. **There is no unit above the second on either path**, and this entry said such a unit was "the software rasteriser's alone" until 2026-09-21: `OOPS_GL_MAX_TEXTURE_UNITS` is 2, every per-unit array is that size, and `glActiveTexture(GL_TEXTURE2)` is refused with `GL_INVALID_ENUM` in software exactly as on the console. GL 1.3's minimum is two, so this is conformant - but a third unit is a whole-library feature, and its console half is gated on a fifth parameter export that has never been measured on this part (`REQ-20260921T1210Z-4f16`) |
| ~~Mipmap levels on the hardware~~ - **seen 2026-09-21**, `mipmap-levels` passes | 1.0 | Since 2026-09-19 a complete, power-of-two, mipmap-filtered texture reaches the GPU as a chain laid out by addrlib's own arithmetic for a linear GFX10 surface - smallest level first, base level last, each level's rows at 256 bytes - with LAST_LEVEL, MAX_MIP and MIP_FILTER as radeonsi fills them. The layout is derived, not measured: gl1-probe's `mipmap-levels` draws an 8x8 chain across 4x4 pixels and needs the second level's colour, which is the measurement. A non-power-of-two mipmapped texture keeps sampling its base level (GL rounds levels down, addrlib up). **Found the same evening: the chain could never have been read** - the textured pixel shader sampled with `image_sample_lz`, level zero, so no level of detail was ever computed and `mipmap-levels` would have failed whatever the layout. It samples with `image_sample` in whole-quad mode since (`tools/shader/tex-prolog.s`), which also brings the minification filter and GL 1.4's LOD bias into effect |
| ~~`glClear` through the scissor box and write masks on the hardware~~ - **seen 2026-09-21**, `scissored-clear` passes | 1.0 | Found and fixed 2026-09-19: both paths cleared the whole surface whatever the scissor and masks said. A scissored or colour-masked clear of colour or depth is now **drawn** - one quad at the clear values with every other per-fragment effect set aside, as Mesa's `clear_with_quad` does - through the scissor registers and `CB_TARGET_MASK` the draw path already emits; the depth mask drops the depth clear, and the stencil clear keeps to the box and the stencil mask on the CPU. An unscissored, unmasked clear is still the DMA fill, so gl-cube's recorded frame is untouched. The drawn clear is unmeasured on a console: gl1-probe's `scissored-clear` is the measurement |
| ~~**The colour buffer is linear and re-tiled by the CPU**~~ - the scanout path **measured and on, 2026-09-20** | - | The console drew into a linear buffer that the display re-tiled on the CPU at every flip; a title on this console draws straight into the scanout buffers, and so does oops-gl now. Those are `WC_GARLIC` memory (obSCEne `130-layout/memory-type`) in the GPU's 64KB_R_X swizzle. **Both halves of that layout are measured**: inside a block, `-2d7f`'s whole-block dump agrees with the display tiler on all 16,384 pixels where the same bytes read as rows agree on 10,408; across blocks, `-4b19`'s 256 x 256 dump detiles into one run a row with no gaps under the row-major order **and under no other** (`rx-check --shape`, which enumerates them). Verdicts tracked as `tools/rx-check/rx_check_7e21.txt` and `rx_check_4b19.txt`. **The scanout path** (`src/gl/gl_rx.h`, 2026-09-19) does what a title does. The back is the next scanout buffer, with `CB_COLOR0_ATTRIB3` `COLOR_SW_MODE` 27, and a swap flips it as drawn after waiting for it to leave the screen (`oops_display_flip_scanout`, `oops_display_wait_scanout`). The front is the buffer on screen. Every CPU colour access goes through `gl_color_index`, on the display tiler's own vectors (`agc_tile_pixel`), and `glGetFrameReadback` detiles for its callers. `test_pm4_gl_scanout_path_targets` covers it on the host. **On** (`OOPS_GL_RX_MEASURED` 1) since the rows above. It was off while `REQ-20260916T1250Z-6e0f`'s result claimed the measurement and its own rows contradicted it - the target was linear and one pixel was drawn - and while `-7e21`'s first answer dumped 1/64 of each block, all of it fill. `tools/rx-check`'s self-test proves the reading on synthetic rows, one block and four, the four-block round pinning that a transposed block order is caught and named rather than passing. **What the rows do not settle** is in `gl_rx.h`: both tiled arms matched, so `0x08c6c000` and `0x0dc6c000` are indistinguishable here, and the first is taken as the smaller change from the working linear value. **CPU overlays follow it:** gl1-cube's HUD draws with the CPU on `oops_display_get_surface`. Once the scanout path calls `oops_display_use_scanout`, that surface is the next scanout buffer, flagged `OOPS_SURFACE_RX`, and the SDK's CPU drawing calls address that layout (2026-09-19). So the HUD lands in the frame being flipped without a change to the app. It is slower there: its translucent panel reads back write-combined memory |
| ~~Front-buffer rendering~~ | 1.0 | **Done** 2026-09-19, found and closed the same day - see **Done**. `glDrawBuffer(GL_FRONT)` and `glReadBuffer(GL_FRONT)` (and `GL_FRONT_LEFT`, `GL_LEFT`, `GL_FRONT_AND_BACK`) were refused with `GL_INVALID_OPERATION`, because `oops_display` handed this library its back buffer only. **Closed on the console 2026-09-20:** a *draw* under `GL_FRONT_AND_BACK` or `GL_LEFT` reached the back and not the front, with one log line. It now binds `CB_COLOR1_*` to the front (offsets from Mesa `gfx103.json`), carries MRT1 in `CB_TARGET_MASK`, `CB_SHADER_MASK` `0xff` and `SPI_SHADER_COL_FORMAT` `0x99`, and exports twice from both pixel shaders - `exp mrt0 ... vm` then `exp mrt1 ... done vm`, assembled in `tools/shader/mrt1-export.s`, whose third instruction re-assembles the single-target word already in the tree as its cross-check. The registers are obSCEne's `REQ-20260919T2258Z-3f62` (sweep `20260920-082906`, `166-agc/mrt-dual-target`): two targets took 512 pixels each in one draw where the one-target control left the second at its fill. `gl_ps_patch_export` sets the export on **every** draw, so a later `glDrawBuffer(GL_BACK)` stops writing the front, and an unbound second target is zeroed in `CB_COLOR1_INFO` as well as dropped from both masks. `test_pm4_gl_front_buffer_targets` covers both directions. **That expectation was wrong on the part**: gl1-probe's `front-and-back` failed its front half on 2026-09-20 and again on 2026-09-21, because the second target had every surface register and no `CB_BLEND1_CONTROL` - blending is per MRT here, so the front replaced where the back added. Fixed the same day and awaiting its console run; see **What fails there** above |
| ~~Fog on the hardware~~ - **seen 2026-09-21**, `fog` and `fog-coord` pass | 1.0 | Since 2026-09-19 fog reaches the console without moving the shader interface: the factor is computed per vertex on the CPU, as the software path computes it (eye distance or GL 1.4's fog coordinate, clamped), and rides in the texture parameter's spare `z`, which the vertex shader already exports; both pixel shaders interpolate it and blend red, green and blue towards the fog colour in a twelve-word slot before the alpha test, the colour as three patched literals. Every word is from `tools/shader/fog.s`. The pinned interface registers are unchanged, and gl-cube, which has no fog, runs the same program but for twelve `s_nop`. gl1-probe's `fog` and `fog-coord` are the measurement |
| ~~`glPixelStorei` keeps three parameters~~ | 1.0 | **Done** 2026-09-19 - every GL 1.2 parameter, both directions; see **Done** |
| ~~Non-byte pixel types, packed types~~ | 1.1, 1.2 | **Done** 2026-09-19 - every plain and packed type, and `GL_RED`/`GL_GREEN`/`GL_BLUE`; see **Done** |
| ~~Sized and other internal formats~~ | 1.1 | **Done** 2026-09-19, with proxy textures - see **Done** |
| ~~Texture environment `GL_BLEND`, RGBA `GL_DECAL` and `GL_COMBINE` on hardware~~ - **seen 2026-09-21**, `combine` and `tex-env-blend-decal` pass | 1.0-1.3 | **Done** 2026-09-19. The textured pixel shader moved from payload offset 0x200 to 0x400 (128 words, 119 used) and its combine slot grew from four words to sixty-four. The short forms stay where they suffice, now with `GL_ADD`'s sums clamped; `GL_BLEND`, RGBA `GL_DECAL` and `GL_COMBINE` are a **generated program** - the combiner's arguments gathered, its function per channel, the scale, the clamp - with GL_BLEND and GL_DECAL first restated as combiner settings as Mesa's `calculate_derived_texenv` does. Its words come from an encoder whose every instruction form is checked against `tools/shader/combine.s`, assembled; and `test_pm4_gl_combine_programs_compute_what_software_does` runs each program through a reader for those forms and compares it with the software rasteriser on every mode, base format and a spread of combiner settings. Unmeasured on a console: gl1-probe's `combine` and `tex-env-blend-decal` are the measurement |
| ~~Texture parameter values; `GL_TEXTURE_PRIORITY`, `GL_TEXTURE_RESIDENT`~~ | 1.0, 1.1 | **Done** 2026-09-19 - see **Done** |
| ~~Pixel rectangles skip the fragment operations~~ | 1.0 | **Done** 2026-09-19, found and fixed the same day - see **Done**. On the console their depth and stencil tests were left out until the evening, with one log line. They run there now, against the GPU's tiled surfaces - see the next row |
| ~~Depth textures, shadow comparison~~ | 1.4 | **Done** 2026-09-19 in software, 2026-09-20 on the console: image format `32_FLOAT`, the sampler's own `DEPTH_COMPARE_FUNC`, and a sample slot that asks for one channel and spreads it as `GL_DEPTH_TEXTURE_MODE` says - see **Done**. gl1-probe's `shadow-compare` passes there, 2026-09-21 |
| ~~Depth and stencil pixel rectangles~~ | 1.0 | **Done** 2026-09-19, found the same day: `glReadPixels`, `glDrawPixels` and `glCopyPixels` refused `GL_DEPTH_COMPONENT`, `GL_STENCIL_INDEX`, `GL_DEPTH` and `GL_STENCIL` - see **Done**. On the hardware path the depth operations were refused at first, with `GL_INVALID_OPERATION` and one log line, because the depth surface is the GPU's, laid out 64KB_Z_X. The stencil ones joined them when the console's stencil test made the stencil buffer a tiled GPU surface too. **Both are addressed from the CPU since the evening of 2026-09-19**, through addrlib's swizzle vectors (`tools/zs-tiling`, `src/gl/gl_zs_tiling.h`). The tool checks every pixel of a three-by-two-block surface against addrlib under the part's `GB_ADDR_CONFIG`. It also runs an eight-pipe control that must disagree. gl1-probe's `depth-readback` and `stencil-readback` are the console measurement, and both pass there, 2026-09-21 |
| ~~`GL_COLOR_INDEX` pixel rectangles, the `GL_BITMAP` type~~ | 1.0 | **Done** 2026-09-19 - see **Done** |
| ~~Cube maps~~ | 1.3 | **Done** 2026-09-19 in software, 2026-09-20 on the console: the six faces as one array, `TYPE 0xb`, and the face found from the direction by RDNA2's own `v_cube*` instructions - see **Done**. An incomplete cube map is drawn untextured on both, which is what GL does with one. gl1-probe's `cube-map` passes there, 2026-09-21 |
| ~~Separate specular colour, `GL_RESCALE_NORMAL`, texture LOD parameters~~ | 1.2 | **Done** 2026-09-19 - see **Done**; the textured half of the separate specular colour on hardware is in **Needs a shader change** |
| ~~`GL_CLAMP_TO_BORDER` and the border colour on hardware~~ - **seen 2026-09-21**, `border-and-mirror` passes | 1.3 | **Done** in software 2026-09-19, with `GL_MIRRORED_REPEAT` (1.4) and a `GL_CLAMP` that reaches the border - see **Done**. On the hardware a border colour that is none of the sampler's three built-in ones is read from a one-entry table that `TA_BC_BASE_ADDR` points at, as radeonsi does; the register and the table's layout are from Mesa and unmeasured. gl1-probe's `border-and-mirror` is the measurement |
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
| **The rest of the type-and-arity grid** | every remaining `glVertex`/`glColor`/`glTexCoord`/`glNormal` spelling. **Integer colours and normals normalise; positions and texture coordinates do not** - conversions from Mesa's `macros.h`, and normals use the same macros as colours rather than a plain cast. `glTexCoord4`'s `q` is a projective divide, which widened the current texture coordinate to four components. The vertex keeps all four undivided and the software rasteriser divides **per fragment**, after interpolation, since 2026-09-19 - before that the vertex divided, which is exact only while q is constant across a primitive. The console's textured pixel shader divides per fragment too since the same evening, q in the texture parameter's `w` - **seen 2026-09-21**, `projective-texture` passes |
| **Multitexture, two units** (2026-09-19; one unit before) | `glMultiTexCoord*`, `glActiveTexture`, `glClientActiveTexture` and their `*ARB` spellings. `GL_MAX_TEXTURE_UNITS` reports 2 and a unit past it is `GL_INVALID_ENUM`. This row once said one unit was what the specification requires; **the maximum is not allowed to be 1** (GL 1.3, 2.6) - see **Two texture units** in the gap table |
| **Texture coordinate generation** | `glTexGen*`, `glGetTexGen*`, the four enables. Object-linear, eye-linear, sphere map, and GL 1.3's reflection map and normal map for s, t and r (refused until cube maps landed, 2026-09-19). Every mode reads the eye-space normal lighting uses - through the inverse-transpose, unit length only under `GL_NORMALIZE` or `GL_RESCALE_NORMAL`, as Mesa's fixed-function program takes it; sphere mapping used the modelview's own 3x3 and always normalised before. `GL_EYE_PLANE` is stored through the inverse modelview of the moment it was set, which needed a full 4x4 `mat4_invert` |
| **Antialiasing (software)** | `GL_POINT_SMOOTH`, `GL_LINE_SMOOTH`, `GL_POLYGON_SMOOTH` - enables on `GL_ENABLE_BIT` and the point, line and polygon bits. Each fragment's alpha is multiplied, after fog, by the fraction of its pixel the primitive covers: a point is a disc of the unrounded size, a line a rectangle of the unrounded width (its ends too, but for a stipple's dashes), a polygon faded across its own edges and not the diagonals its triangles share, so no seam runs through it. Each coverage is the linear approximation - exact where an edge crosses a pixel straight - and a polygon corner the diagonal leaves comes out half covered where a quarter is right. Smooth sizes step by an eighth, which `GL_SMOOTH_*_GRANULARITY` reports |
| **`GL_COMBINE`** (GL 1.3) | The combiner: `GL_COMBINE_RGB` and `_ALPHA` (`GL_REPLACE`, `GL_MODULATE`, `GL_ADD`, `GL_ADD_SIGNED`, `GL_INTERPOLATE`, `GL_SUBTRACT`, and `GL_DOT3_RGB`/`_RGBA` for the colour), three sources and operands each, and `GL_RGB_SCALE`/`GL_ALPHA_SCALE` - checked as Mesa's `main/texenv.c:107-370` checks them, computed as `main/ff_fragment_shader.c:563-735` does, queryable, on `GL_TEXTURE_BIT` and in lists. `GL_TEXTURE0` is accepted as a source, GL 1.4's crossbar naming the one unit. The scalar `glTexEnvf` stopped truncating through `glTexEnvi`. **On the hardware** it is a program generated into the pixel shader since the same day - **seen 2026-09-21**, `combine` passes; see the gap table |
| **Cube maps (software)** (GL 1.3) | `GL_TEXTURE_CUBE_MAP`'s binding, enable (outranking 3D, 2D and 1D) and default texture; its six faces through their own targets in `glTexImage2D`, `glTexSubImage2D`, `glCopyTexImage2D`, `glCopyTexSubImage2D`, `glGetTexImage` and `glGetTexLevelParameter`, each face square (Mesa, `main/teximage.c:1079-1100`) and up to `GL_MAX_CUBE_MAP_TEXTURE_SIZE` 1024 - the faces live on the CPU; `GL_PROXY_TEXTURE_CUBE_MAP`. Cube completeness: six faces of one size and format, each mipmap complete under a mipmap filter (`main/texobj.c:800-830`). The lookup by direction is softpipe's `convert_cube` (`sp_tex_sample.c:3220-3291`), GL 1.3's table 3.19, and the level of detail compares neighbouring pixels projected onto the centre's face. **On the hardware a cube-mapped draw is untextured** and says so once in the log; see below |
| **User clip planes** | `glClipPlane`, `glGetClipPlane`, `GL_CLIP_PLANE0..5`. Two registers and no shader change; **three coordinate spaces**, object in, eye stored, clip in the register |
| **Stencil (software)** | `glStencilFunc`, `glStencilOp`, `glStencilMask`, `glClearStencil`, and `GL_STENCIL_BUFFER_BIT` on the attribute stack. The hardware surface is outstanding - see below |
| **Raster position and pixel operations** | all 24 `glRasterPos*` spellings, `glDrawPixels`, `glBitmap`, `glCopyPixels`, `glPixelZoom`. An invalid raster position draws **nothing**; `glBitmap` moves the position even when it draws nothing. **Each pixel is a fragment** (since 2026-09-19; written straight into the colour buffer before): at the raster z, textured by the raster texture coordinate's texel, fogged by the raster distance, then through the triangle rasteriser's own per-fragment tail - scissor, alpha, stencil and depth tests, blending, logic op, masks. `glCopyPixels` reads its whole source before writing (overlapping copies smeared) and zooms; the zoom is GL's centre rule. The colour sum is not applied - GL 1.x has no raster secondary colour |
| **1D textures** | `glTexImage1D`, `glTexSubImage1D`, `glCopyTexImage1D`, `glCopyTexSubImage1D`. **Its own binding point**, its own enable and its own default texture, with 2D winning when both are enabled |
| **Compressed textures** | the seven entry points, with **no formats**: `GL_NUM_COMPRESSED_TEXTURE_FORMATS` is 0 and every upload is refused, which is what the specification says an implementation with an empty format set does |
| **Points and lines** | `GL_POINTS`, `GL_LINES`, `GL_LINE_STRIP`, `GL_LINE_LOOP`, `glPointSize`, `glLineWidth` - **as triangles**: a line is a screen-width quad and a point a square, built back through the inverse model-view-projection. The one-and two-vertex primitives the geometry engine will not take are never submitted |
| **Fog** | `glFog*`, `GL_FOG`, `GL_FOG_BIT`. Eye-space distance, factor per vertex, blend per pixel, alpha untouched. The hardware path is written since 2026-09-19 and **seen 2026-09-21** - see **Fog on the hardware** above |
| **Mipmaps, completeness, the software sampler** | Every level of a texture kept, per-level queries, sub-images and copies; **completeness** (an incomplete texture draws untextured, as on any GL); texture 0 sampled when nothing is bound; and a software sampler that follows the filters - GL-exact nearest, bilinear, the magnification/minification switch, `*_MIPMAP_NEAREST` and `*_MIPMAP_LINEAR` from a per-pixel level of detail - with **perspective-correct** interpolation of every attribute but depth, and the texture coordinate's q divided out per fragment after it. The hardware reads a mip chain since the same day - written, not yet measured (see above) |
| **Colour-index state, multisample state, dithering, hints** | The ten `glIndex*`, `glClearIndex`, `glIndexMask`, `glIndexPointer`, `GL_INDEX_LOGIC_OP`; `glSampleCoverage` and the four multisample enables; `GL_DITHER`; every GL 1.x `glHint` target. **State only, and that is the whole of it**: an RGBA context keeps colour-index state and draws nothing with it, and with no multisample buffer the specification gives the multisample state no effect. The attribute stack gained `GL_POINT_BIT`, `GL_LINE_BIT`, `GL_HINT_BIT` and `GL_MULTISAMPLE_BIT` for state that already existed |
| **`glPolygonMode`, edge flags** | `glPolygonMode`, `glEdgeFlag`, `glEdgeFlagv`, `glEdgeFlagPointer`, `GL_POLYGON_OFFSET_LINE`/`_POINT`. An outline is the primitive's own edges - never a quad's or polygon's triangulation diagonals - drawn through the line expansion; culling is decided for the polygon first. Primitive assembly is one function now, shared by `glEnd` and both array draws |
| **`glLogicOp`, `glBlendColor`** | `CB_COLOR_CONTROL`'s `ROP3` and `CB_BLEND_RED..ALPHA`, both from radeonsi; the constant-colour factors; `glBlendFunc`/`glBlendEquation` now refuse what is not a factor or an equation, and default to GL's `GL_ONE`/`GL_ZERO` |
| **Evaluators** | All 23: `glMap1*`/`glMap2*`, `glGetMap*`, `glEvalCoord*`, `glMapGrid*`, `glEvalPoint*`, `glEvalMesh*`, with the eighteen map enables, `GL_AUTO_NORMAL`, the grid queries and `GL_EVAL_BIT`. **CPU arithmetic ending in ordinary `glVertex` calls**, so an evaluated patch is lit, textured, compiled and drawn like typed-out geometry. Evaluation leaves the current colour, normal and texture coordinate alone; `GL_AUTO_NORMAL` follows the domain's direction, where Mesa's does not; `GL_FILL` meshes are the specification's quad strips. What `glutSolidTeapot` draws through |
| **Selection and feedback** | `glRenderMode`, `glSelectBuffer`, `glFeedbackBuffer`, `glInitNames`, `glLoadName`, `glPushName`, `glPopName`, `glPassThrough`, the queries, and `gluPickMatrix` in the GLU. Primitive assembly reports instead of drawing - each polygon triangle, line and point **before** a line or point is widened - so one path serves every way of drawing. **Clipped geometrically**, against the view volume and the user planes, which the drawing path never needed: a hit's depth range and a feedback polygon are what is left after clipping. Culled polygons are no hit; `glPolygonMode` decides what a polygon is reported as; nothing reaches the framebuffer, `glClear` included. Feedback texture coordinates are all four components as the vertex holds them, undivided (Mesa `main/feedback.c:136-139`) - `(s/q, t/q, 0, 1)` until 2026-09-19 |
| **Pixel maps and transfer** | `glPixelTransfer{f,i}`, `glPixelMap{fv,uiv,usv}`, `glGetPixelMap{fv,uiv,usv}`, their queries and `GL_PIXEL_MODE_BIT`. Scale, bias, colour-map lookup and clamp - Mesa's order - on every colour rectangle: `glDrawPixels`, `glCopyPixels`, the texture uploads, the texture copies (once, not once reading and again uploading) and `glReadPixels`. The depth, stencil and index halves apply to their rectangles since 2026-09-19 - `GL_DEPTH_SCALE`/`BIAS`; `GL_INDEX_SHIFT`/`OFFSET` with `GL_MAP_STENCIL`'s map for stencil; the shift, offset and `GL_PIXEL_MAP_I_TO_R/G/B/A` for colour indices. `glReadPixels` into luminance now sums R, G and B, as GL does |
| **Stipple** | `glLineStipple`, `glPolygonStipple`, `glGetPolygonStipple`, both enables, the pattern and repeat queries and `GL_POLYGON_STIPPLE_BIT`. **A stippled line is cut into its dashes before it is widened** - counted along the major axis, carried across a strip, restarted per separate line and per outlined polygon, attributes perspective-correct along it - so its dashes are ordinary quads and **the hardware draws them too**. The polygon stipple is a 32x32 window-space mask applied to filled polygons, **on both paths since 2026-09-20**: the console draw sets `SPI_PS_INPUT_ENA`/`_ADDR` to `0x302` so the fragment's position arrives in `v2` and `v3` (obSCEne `REQ-20260919T2258Z-c7d4`), and a sixteen-word slot in both pixel shaders loads the mask row and kills the lanes whose bit is clear (`tools/shader/polygon-stipple.s`). The CPU writes the mask rotated for the window height and bit-reversed, so the slot every fragment runs is a load and a shift rather than four operations; `test_pm4_gl_polygon_stipple_discards_in_the_shader` checks the two forms agree for every pixel of a 32x32 window. gl1-probe's `polygon-stipple` passes on the console, 2026-09-21 |
| **3D textures** | `glTexImage3D`, `glTexSubImage3D`, `glCopyTexSubImage3D`, `GL_TEXTURE_3D`'s binding, enable and default texture (3D beating 2D beating 1D), `GL_TEXTURE_WRAP_R`, `GL_TEXTURE_DEPTH`, `GL_MAX_3D_TEXTURE_SIZE` (256 - the volume is on the CPU), `GL_UNPACK_IMAGE_HEIGHT` and `GL_PACK_IMAGE_HEIGHT`, mip levels that halve depth too, and completeness counting it. The vertex carries r - through generation and the texture matrix - and the software sampler filters in three dimensions, eight texels for GL_LINEAR, with r's rate of change in the level of detail. **On the console since 2026-09-20**: the descriptor's `TYPE` is `0xa` with the last slice in `WORD4`, the vertex carries r in its third parameter, and the pixel shader's sample slot divides it by q and samples with `dim:SQ_RSRC_IMG_3D` (`tools/shader/tex-3d.s`). **Level 0 only there, and since 2026-09-21 that is a measured refusal rather than a structural one.** The chain layout was two-dimensional; `gl_tex_chain_layout_3d` halves all three axes and the copy walks slices, and the console read the base level anyway - `volume-mipmap` answered `saw 0xffff0000`, red, with `LAST_LEVEL` 1 and `MAX_MIP` 1 in the descriptor. So **where a level sits inside a 3D image is not the 2D rule with a depth term**, and `REQ-20260921T1300Z-9b73` asks what it is. `texture-3d` measures the slice stride *within* level 0 and `mipmap-levels` measures a *2D* chain's level placement; neither covers this, and this row claimed they did before the check had run |
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
> `tools/shader/fog.s`). **`fog` and `fog-coord` both pass on hardware** (2026-09-20 onward);
> this said they were "expected to pass" and unmeasured until the audit of 2026-09-21. What
> follows is how it was costed.
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
> (`tools/shader/coverage.s`), and gl1-probe's `smooth` **passes on hardware**.
>
> **A textured smooth primitive landed on the console 2026-09-21, and GL_POLYGON_SMOOTH is what
> is left.** This paragraph said the first had no spare interpolant, the offset riding in the
> parameter the texture coordinate uses, and that was true when it was written. What changed is
> the fourth parameter: it carries the second texture unit's coordinate, a draw with **one** unit
> does not read it, and such a draw can escalate to four parameters to give the offset a home.
> The cost is eighty bytes a vertex and the four-parameter vertex shader - both of which a
> two-unit draw already uses, and both of which ran on a console for the first time earlier the
> same day, when `gl_vs_build_param4` turned out never to have been called. So this was unblocked
> by a bug fix rather than by a design.
>
> The shader is `tools/shader/coverage-tex.s`: `coverage.s`'s fifteen words with the six
> interpolations' ATTR field moved from 1 to 3, which is `+0x800` each, and nothing else touched.
> That file assembles both forms and its seven cross-check lines came back as words already in
> the tree, so "only the ATTR field moved" is checked rather than asserted. The textured pixel
> shader gained a coverage slot at 258, between fog and the alpha test, where the untextured one
> keeps its own; the alpha test and the export moved up sixteen, as they did for fog. A branch
> over the slot is no smoothing, so a frame that never smooths runs the program it always ran.
> gl1-probe's **`smooth-textured` passed on hardware the first time it ran.**
>
> **GL_POLYGON_SMOOTH followed the same day and passed on hardware the first time it ran**, and
> this paragraph had given two reasons it could
> not: three edge fades multiplied rather than one distance, and the pixels a triangle only
> *partly* covers, which the rasteriser does not raise, so the outer half of every edge would
> not be drawn. Both were true and neither was structural.
>
> The coverage slot holds either form now - twenty-eight words rather than sixteen, and the
> alpha test and export moved up in both shaders to make room. And the CPU **widens the
> triangle** by a pixel about its incenter, which moves every edge outward by the same distance;
> that is the trick that already turns a point and a line into a quad, so the fragments exist and
> the shader's kill removes whatever the widening added beyond the fade. Each vertex keeps its
> own attributes, as a widened line's corners do.
>
> The three distances need no per-vertex geometry: the signed distance to edge *i* is `λ_i·h_i`,
> so vertex *j* carries its own height in component *j* and zero in the others and the
> interpolator does the rest. An edge that is **not** antialiased - the diagonal a polygon was
> triangulated along - carries 1.0 at all three vertices, which interpolates to 1.0 and fades
> nothing. Each rides as `d*w` with `w` in the fourth slot, because `v_interp` is
> perspective-correct and a distance to a line is not; dividing one by the other in the shader
> is the identity the textured prolog already uses for `q`. `tools/shader/coverage-poly.s`.
>
> **Two texture units is now the only case still aliased**, for points, lines and polygons alike:
> the fourth parameter is the second texture's coordinate then, and only its `z` is spare - one
> float where three are needed. It says so in the log.
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
> gl1-probe's `cube-map` **passes on hardware**.
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
> whole of `GL_ARB_shadow` runs there, and gl1-probe's `shadow-compare` **passes on hardware** -
> which also settles the open half of `-b4e1` below: the chosen register order is the one that
> works.
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
> `colour-sum`, untextured, **passes on hardware**. **Next on the same interface:** the
> second texture unit needs a fourth parameter, `{s, t, r, q}` of unit 1. The mechanism is the
> measured one, but nothing has run four parameters on a console yet.
>
> **A q that varies across a primitive needed it too, and has it** (software since 2026-09-19,
> the console the same evening): the vertex carries s, t and q undivided, q in the texture
> parameter's `w` beside fog's `z`, and the textured pixel shader divides after interpolating -
> a `v_rcp_f32` and two `v_mul_f32` before the sample, from `tools/shader/tex-prolog.s`. No
> interface register moved. gl1-probe's `projective-texture` **passes on hardware**; this said
> "expected to pass, unmeasured" until the audit of 2026-09-21.
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
> with `dim:SQ_RSRC_IMG_3D`. gl1-probe's `texture-3d` **passes on hardware**, and its passing is
> what measures the slice stride a volume's mip chain relies on - it reads slice 1 of a 2x2x2
> volume at `pitch * height` from slice 0.
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

## 2.0

OpenGL 2.0 is GLSL: `glCreateShader`, `glShaderSource`, `glCompileShader`, `glLinkProgram`,
`glUseProgram`, uniforms and vertex attributes - and behind them a compiler. That is the largest
single piece of work in this repository and the reason D007 originally pointed at Mesa.

### The software reference runs shaders, as of 2026-09-21

**`glUseProgram` draws.** The entry points exist, the linker builds a program's interface, and
the software path runs the vertex shader per vertex and the fragment shader per fragment -
against which `gl2-probe` measures 66 checks and `gl2-cube` draws a cube.

**All 67 pass on the console**, measured 2026-09-25, which is the property the suite exists for -
a check that passes on the reference and fails on the hardware is a hardware-path defect, and
that run found two.

One was a stale check: `do-while` asserted a console refusal that stopped being true when the
generator learned to compile it. The other was a defect no host suite could have found -
framebuffer objects were refused on the console because the predicate asked whether the
*display's* buffers were tiled, and on a console they always are. The software rasteriser never
sets that flag, so every arm passed on the reference while the hardware answered
GL_FRAMEBUFFER_UNSUPPORTED to all three.

This is the same arrangement every other stage here has: a software implementation that defines
what the answer is, and a console path that has to agree with it.

**The console half runs** (measured 2026-09-21, run 23). `glsl_ps.c` compiles a fragment shader
to gfx1030 and the draw path copies it into the payload and points the pixel stage at it; a
program the back end will not generate for is still refused and logged, rather than drawn with
the fixed-function instruments. obSCEne's `REQ-20260921T1615Z-4e77` was the gate and it answered:
`166-agc/compiled-ps` bound shaders generated by this compiler, with no hand-written preamble and
no `m0` setup, and all three arms retired - a constant export gave the colour asked for, and an
interpolated parameter came back `0x40 0x80 0xbf`, which is (0.25, 0.5, 0.75) bit for bit.

**`REQ-20260921T1730Z-6c0d` then answered for uniforms**, and its second arm is the more useful
half: the block arrives intact with `s_waitcnt lgkmcnt(0)`, and reads **all zeros** without it.
A scalar load is asynchronous, so the wait is not a formality - and a compiler that omitted it
would produce shaders that compute with whatever those registers held, silently, on hardware
only. `glsl_emit_s_waitcnt_lgkm` is emitted once after the loads and before the first read.

**One arm is not closed.** `-4e77`'s third read parameter 3 and returned a pixel the resolution
does not mention, and which a hand-written shader reading the same parameter in the same run does
not agree with - `REQ-20260921T1810Z-3d92` asks for the two rows that would settle it. It matters
because a varying's float position becomes `parameter = n / 4`, so a program with 13 to 16
varying floats is the case those two arms differ over.

**Only the fragment stage is compiled.** The hardware vertex shader is a passthrough - the CPU
builds each vertex already in clip space and the shader loads and exports it - so a GL 2.0 vertex
shader runs on the CPU in `glsl_exec.c`, writing the same vertex the fixed-function path writes.
There is nothing for a vertex-shader compiler to do that the interpreter is not already doing.

What landed, in one day:

| Piece | Where |
|---|---|
| The GL 2.0 API - 90-odd entry points, shader and program objects in one name space, deferred deletion, uniforms, generic vertex attributes, separate stencil and blend enumerants | `include/GL/gl.h`, `src/gl/gl_shader.c` |
| **Version gating** - a context has the entry points its version defines and no others, so a GL 1.x program cannot reach the programmable pipeline by accident. On GL 2.0 completely; not within 1.x, because every 1.2-1.5 feature here is also an advertised ARB or EXT extension and an extension is available whatever the core version | `gl_require_version` in `src/gl/gl_internal.h` |
| The built-in library - GLSL 1.10's functions resolved by rule over genType, and the `gl_` variables and fixed-function uniforms | `src/gl/glsl_builtin.c` |
| Compile and link - the preprocessor slotted in front of the parser, reference-counted compiled units, the uniform/attribute/varying tables and their locations | `src/gl/glsl_link.c` |
| The interpreter - the reference execution of a shader, with derivatives taken by re-evaluating against the neighbouring pixel's interpolants | `src/gl/glsl_exec.c` |
| The draw path - a program replaces the transform, the lighting and the texture combiner; everything after the screen vertices is common to both pipelines | `src/gl/gl_draw.c` |

Two things the probe found on its first run, both real: `gl_FrontFacing` was taking its value
from two-sided lighting's flag, which is only set while GL_LIGHTING is on - so every GL 2.0
fragment was told it faced forward - and a sampler was handed the front end's own type enumerant
where the GL one was expected, so every `texture2D` returned opaque black.

### What is left

| Stage | State |
|---|---|
| Lexer, parser, preprocessor, semantic stage | **done** for GLSL 1.10 **and 1.20**, including arrays and the built-in library. 1.20 is implicit int-to-float conversion, `invariant` and `centroid`, `transpose`/`outerProduct`, and the non-square matrices; 1.30 and later are refused by number. The preprocessor is complete for the dialect: `#if`/`#elif` with the full constant-expression grammar, function-like macros, `#extension`, `#pragma` and `#line` |
| `struct`, and the type-name ambiguity | **done**, in all four stages - parser, semantic pass, interpreter and generator - and measured on the console (`gl2-probe`'s `structs`, seven arms). The type-name ambiguity is resolved the way C resolves it: the parser keeps the struct names it has seen and `starts_declaration` asks. The layout is decided once in the semantic pass and every later stage reads it, rather than each deriving its own |
| GLSL 1.20's array constructors | **done** since 2026-09-25. `float[2](a, b)`, in both back ends. The parser already produced the right shape without anyone noticing - `float` is a type name in primary position, `[2]` the postfix index, `(...)` the postfix call, so it arrives as `CALL(INDEX(IDENTIFIER, 2), args)` and nothing was added to the grammar. **Legal in exactly one place, and not by convenience**: an array constructor's value is an array, and here an expression carries a type while only a symbol carries a length, so a declaration's initialiser is the one context that supplies one. 1.20 also allows one as an argument and a return value; those would need an array type in the type system and are refused by name rather than typed as their element and left to mismatch downstream. The length folds into the tree the way a declarator's does, so both back ends read a number |
| GLSL 1.20's whole-array assignment and comparison | **not implemented, and named as such.** `v = w` and `v == w` on arrays are 1.20's, and the refusal says "1.20's and not implemented" for a 1.20 shader while keeping 1.10's own rule - that a whole array is not an l-value (5.8) - for a 1.10 one. Claiming the language forbids it would send a 1.20 shader's author to a specification that agrees with them. `tools/shader-conformance/refused-compile-whole-array-assign-120.frag` holds the line, so the day it goes in the gate says so |
| Non-square matrices (`mat2x3` and the rest) | **done** since 2026-09-25, in all four stages and in the GL API beside them. The type name is 1.20's and a 1.10 shader is told so, by name, from both the declaration and the constructor. `matNxN` is a spelling of `matN` rather than a tenth type |
| Point sprites, and `gl_PointCoord` | **done** since 2026-09-23. `gl_PointCoord` is texture coordinate generation over the sprite, which is what `GL_COORD_REPLACE` does for the fixed-function path - a program reading it asks for the same thing without an enum to ask with. `gl2-probe`'s `point-coord` measures it |
| The object model, uniforms, generic attributes | **done** - `gl2-probe`'s first ten checks |
| Reference execution | **done** - the interpreter. Every check in `gl2-probe` that ends in a pixel measures it |
| Instruction encoding | **done for what the fragment stage needs** - `src/gl/glsl_emit.c`. VOP1/VOP2/VOPC, the interpolators, the export, the lane kill, and the five scalar-load widths with their wait; every field verified against clang, table in `tools/shader/gl2-fragment.s` |
| Instruction selection from the AST | **started** - `src/gl/glsl_gen.c`. Arithmetic including `/`, swizzle reads and writes, plain and compound assignment, `++` and `--` both ways round, constructors, declarations, blocks, `if`, every loop shape, user-defined functions, `struct` in every position a value takes, `gl_TexCoord[]`, **uniform arrays** and the built-in library - the common, exponential and geometric functions of GLSL section 8. A counted `for` unrolls whether or not it declares its own counter, and a `const int` reads as the constant it is, so `const int N = 9; int i; for (i = 0; i < N; ++i) a[i]` folds to nine indices - the three together are what `convolution.frag` needed. A counter declared outside the loop is given its final value afterwards, because unlike the loop's own it is still readable there. Everything else sets `error` and emits nothing, because an instruction whose encoding has not been read out of an assembler is not guessed |
| A whole compiled pixel shader | **done** - `src/gl/glsl_ps.c`: the uniform block loaded into the scalar file, the varyings interpolated into registers of their own, the body, the colour moved into v4..v7 and exported. Compiled at link, copied into the payload at `OOPS_GL_PS_GL2_OFFSET` when a draw needs it, and refused if it would need more than the 136 VGPRs the stage table reserves |
| **Does a generated pixel shader run?** | **yes, measured** - `REQ-20260921T1615Z-4e77`, run 23. `166-agc/compiled-ps` bound shaders this compiler generated, with no hand-written preamble and no `m0` setup; all three arms retired, the constant one exported the colour asked for and the interpolated one returned `0x40 0x80 0xbf`. Arm 3, which read parameter 3, returned a pixel the resolution does not account for - `REQ-20260921T1810Z-3d92` |
| The parameter interface | a varying is a parameter export; `REQ-...-7c40` measured a third retiring and drawing, `REQ-...-9c3e` its values arriving byte for byte, and four are in daily use. A program whose varyings need more than sixteen floats is refused at compile with that number, rather than reading a parameter the vertex stage never exported (`REQ-20260921T1210Z-4f16` is the fifth) |
| Uniforms in a compiled shader | **done, and measured** - the program's value pool goes into a ring of blocks in the payload, its address into the pixel shader's first user SGPR pair, and the shader loads it with `s_load_dwordx16` into s16 and moves what it names into registers. A changed uniform changes the picture with no recompile, which is the point. `REQ-20260921T1730Z-6c0d` confirmed the block arrives bit for bit - **and that without `s_waitcnt lgkmcnt(0)` it reads all zeros**, so the wait is load-bearing rather than a formality |
| Control flow in a compiled shader | **done on the reference** - and there is no branch in it. A fragment shader's `if` is the exec mask: `s_and_saveexec_b32` narrows to the lanes the condition holds for, `s_andn2_b32` flips to the rest for the `else`, `s_mov_b32 exec_lo` puts it back. A body no lane is running still executes and writes nothing, so `s_cbranch_execz` is a saving rather than a requirement - which is what lets the generator emit straight through with no labels and no offsets to backpatch. `discard` takes the lane out of **every enclosing saved mask** before clearing exec, or the innermost restore hands it straight back |
| Texture sampling in a compiled shader | **`texture2D` is generated** - the shader loads its own image and sampler descriptors out of the same block the uniforms come from, samples with `image_sample` (not `_lz`, so the mip chain and the minification filter are used), and waits on `vmcnt` before reading the result. Four descriptor sets a draw, assigned to samplers at link time so the compiler and the draw path cannot disagree about which is which. `REQ-20260921T1830Z-2a45` asks hardware whether the texel comes back. **The projective, cube, volume and shadow lookups are generated too** - `texture2DProj`, `textureCube`, `texture3D`, `texture3DProj`, `shadow2D` and `shadow2DProj` - **and so is the level-of-detail bias**, GLSL 1.10's third argument, through `image_sample_b`. Its opcode was read out of clang rather than guessed: 37 against the plain sample's 32, and the cube form takes one address register more than a cube's three coordinates, which is how the bias is known to go first in the run. Not offered for the shadow forms, whose biased spelling is a third opcode nobody has assembled |
| Whole-quad mode | a sample's level of detail comes from the coordinate's derivative across the 2x2 quad, so a shader that samples runs with `s_wqm_b32` widening `exec` from the prologue, and the live mask restored before the export. **`discard` has to survive that round trip**: the lane comes out of the saved live mask as well as out of `exec`, or the restore at the end of the shader hands it back - the same bug as the `if` case, one level further out |
| A sample *after* a discard | **the known limit.** A discarded lane is off from that point, so it stops contributing to a neighbour's derivative and a later `texture2D` picks its mip level from a smaller quad. GLSL leaves derivatives undefined in non-uniform control flow so this is within the specification, but ACO keeps a second mask to hold the quad whole and a desktop driver will therefore differ. Sampling and *then* discarding - craft's shape, and the common one - is unaffected |
| Loops in a compiled shader | **done** - every shape GLSL can write. A counted `for` is unrolled where it fits, which keeps the counter a compile-time constant so indexing and arithmetic on it fold away; anything else takes the branched path, three scalar masks and a trip guard. `while`, `do`-`while`, a `for` over a counter declared outside, one whose bound is a uniform, one whose body moves its own counter, and `for (;;)` with a `break` are all that second case. **One shape stays refused**, and the distinction is the point: a loop whose trip count is *known* and larger than `GLSL_GEN_MAX_TRIPS` would run the ceiling's worth of trips and draw a wrong colour with no error. That is a truncation rather than a bound. Every other shape falls through because its count is unknown, where the guard is the best answer available and a shader that does what it says never reaches it |
| Non-square matrices (`mat2x3` and the rest) | **done** - the last part of GLSL 1.20, closed 2026-09-25. No shader in any port corpus uses one, so this was completeness rather than a blocked port, and the survey's numbers over craft, SuperTux and mesa-demos are unchanged by it. **A matrix has two sizes and one number was being asked for**: every site read the matrix's dimension and used it as both the stride between columns and the number of columns, which is right for a square matrix and for nothing else. Thirteen of those, across the semantic stage, the interpreter, the generator, the emitter and the linker; `glsl_type_matrix_dim` is gone rather than deprecated, so the compiler names anything still reaching for it. The GL side is GL 2.1's: six `GL_FLOAT_MAT*x*` enums and the six `glUniformMatrix{2x3,3x2,2x4,4x2,3x4,4x3}fv` commands, **checked against the declared type rather than the float count** - `mat2x3` and `mat3x2` are both six floats, so a count check would let either command set either uniform and silently transpose one of them |
| Framebuffer objects | **done, and measured on the console** - `gl2-probe`'s `fbo/renderbuffer` and `fbo/texture`, 2026-09-25: the command processor draws into a renderbuffer in Garlic and into a texture's base level, and the display region is untouched by either. Names, storage, attachments, the completeness rules and `glGenerateMipmap` are in, and a draw into a bound framebuffer object lands in its attachment - the colour target, the depth buffer and the addressing width all follow it, which is what lets an attachment be a different size from the display. The console takes a renderbuffer or a texture's base level because `gl_hw_begin_frame` already reads `CB_COLOR0_BASE` from `ctx->framebuffer` and `CB_COLOR0_INFO` already describes a LINEAR_GENERAL surface; what had to change was allocating a renderbuffer from the GPU allocator rather than the heap. Still refused: a texture level above 0, which is heap memory; any depth attachment on the console, because the depth surface is 64KB_Z_X tiled and the whole framebuffer is refused rather than the depth attachment quietly ignored; and the scanout path. `gl2-probe`'s `framebuffer-objects` has a renderbuffer arm and a texture arm kept separate so one console run says which the command processor accepts |
| Sampler descriptor sets | **four**, which is what the scalar file holds: sets at s4..s51, masks to s64, draw constants s68..s71 and uniforms s72..s103 against a ceiling of s105 (Mesa `ac_gpu_info.c:260`). Two was the limit until 2026-09-25 and it is what stopped SuperTux generating - found by `tools/shader-survey.sh`, since the front end compiled that shader perfectly. `GL_MAX_TEXTURE_UNITS` and `GL_MAX_TEXTURE_IMAGE_UNITS` are separate numbers now, because the first is bounded by the fixed-function descriptor ring and the second by this |
| Uniforms a compiled shader can reach | **the whole 64-float block, since 2026-09-25 - the scalar file no longer bounds it.** A uniform is moved into a VGPR at the top of the shader and read from there afterwards, so the 32 scalar registers that carried it are free the moment the move retires. The prologue therefore *slides* that window along the block, one pass of 32 floats at a time, and what limits a shader is VGPRs - one per uniform float against the 136 the stage table reserves - which the allocator already reports. It is 32 and not 16 so no uniform can straddle a pass: a pass starts at a uniform's offset rounded down to sixteen, nothing is wider than a `mat4`, and `15 + 16 < 32`. Before that the window was loaded once and held, so a shader was capped at 32 floats of *span* and mesa-demos' `CH11-toyball.frag` was refused for wanting 48 of a block that had the room. The two steps before *that* were worth having anyway: the pool is both stages' uniforms together, so a vertex shader's `mat4` used to cost the fragment shader sixteen registers it could not read, and a pool past 32 floats was refused however few the fragment stage named. A matrix uniform is carried since 2026-09-25 as well: the generator already stored a matrix column-major and multiplied one by a vector at any shape, so the gap was cases in `type_from_gl` - three for the square types, six more for the non-square ones, and six for the integer and boolean *vectors*, which are a `vecN` in the pool and a `vecN` in the registers and were left out when their scalars went in |

### What the ports' own shaders ask for

The table above says what is implemented. This says what is *wanted*, which is a different
question and the one worth deciding from - `tools/shader-survey.sh` compiles a directory of real
shaders with this front end and then puts each fragment shader through the code generator.

**The two columns are different gates and only the second is about hardware.** A shader can
compile and still not become gfx1030 instructions, and for these ports that was the usual case:

| | compile | generate |
|---|---|---|
| craft | 16/16 | 8/8 |
| SuperTux | 2/4 | 1/1 |
| SuperTuxKart | 10/100 | 4/4 |
| mesa-demos | 48/51 | 22/23 |
| armagetron-advanced, extreme-tux-racer, neverball, neverputt, sm64, spaghetti-kart | - | - |

**Six of the eleven ports ship no GLSL at all**, which is the first thing the wider survey says:
they are fixed-function titles, and the GL 2.0 shader path is not what any of them is waiting
for. Their gates are GL 1.x, and the table above this one is where their answers live.

SuperTux's two refused are `#version 330`, which it ships beside its ES pair and which is refused
by number; SuperTuxKart is a GL 3.3 engine and ninety of its hundred are 3.30 or later, for the
same reason. **The one mesa-demos shader that does not generate is `infinite-loop.glsl`**, whose
known trip count would be truncated rather than bounded - which is a deliberate refusal and the
file is named for it. It was three that morning: `CH11-toyball.frag` wanted 48 floats of uniforms
against a 32-float scalar window, and `convolution.frag` wanted a uniform array indexed by a
counter declared above its loop, bounded by a `const int`.

**So the only shader in any port corpus that this front end compiles and the generator will not
turn into gfx1030 instructions is one that asks for an infinite loop.**

Everything in this section's table that says "done" was put there because a shader asked for it:
`#version 100` and the precision qualifiers because SuperTux's shaders are ES, constant-expression
array lengths and function overloading because mesa-demos' are, four sampler sets and matrix
uniforms because SuperTux's fragment shader wanted three textures and a `mat3`, the integer and
boolean *vector* uniforms because SuperTuxKart's `coloredquad.frag` is one `uniform ivec4` and a
divide, and the sliding scalar window, uniform arrays, `for (i = 0; ...)` over a counter declared
above the loop, and a `const int` the generator reads as a constant because mesa-demos'
`convolution.frag` is all four at once.

**Each of those refusals named the next one.** `convolution.frag` was refused for a uniform it
could not carry; once it could, for a loop it could not unroll; once it could, for a bound it
could not read. That is what a survey is for, and it is a better order to work in than a list
written from the specification - every step of it was the thing actually standing in the way.

### The other corpus: the specification

A corpus of shipped shaders can only find what someone shipped, and once every one of them
generates it has nothing left to say. `tools/shader-conformance/` is the other half: small
shaders written against the GLSL 1.10/1.20 specification rather than taken from a port, each
named for the outcome it expects - `refused-compile-*` must be refused by the front end,
`refused-gen-*` must compile and be refused by the generator, a `.vert` must compile (the vertex
stage runs on the CPU, so there is no second gate), and everything else must do both.
`tools/shader-conformance/check.sh` asserts that and exits non-zero, so it is a gate rather than
a histogram to read.

**The `refused-` files are why it can fail.** A suite of shaders that all pass says nothing about
whether the harness ran; `refused-compile-switch.frag` uses a word GLSL 1.10 reserves, and if it
ever starts compiling then either the dialect changed or the script stopped looking.

**The vertex stage needs a second instrument, because a corpus can only compile one.** A fragment
shader's answer is a pixel; a vertex shader's is a *position*, and every drawing test here covers
the middle of the framebuffer with a quad and reads the pixel there - so a vertex shader that
computes the wrong thing still draws. `test_gl2_vertex_stage_computes` carries the shader's
working into a varying and reads it back as colour, which turns a disagreement into a channel.
Two of its arms are things that can be wrong while everything around them is right:
`ftransform()` must equal `gl_ModelViewProjectionMatrix * gl_Vertex`, which is the whole reason
the function exists, and `gl_NormalMatrix` is the modelview's upper 3x3 **inverse transposed** -
the one built-in matrix that is derived rather than copied. Under the test's (2, 1, 1) scale the
two readings differ by a factor of four.

It found nine real gaps in its first four runs, none reachable from any port's shaders, and a
tenth batch found none at all - which is what saturation looks like and is why the fifth tick went
on measuring the vertex stage instead of widening the corpus further:

- **an array as a struct member** (`struct S { float w[3]; }`), which GLSL 1.10 4.1.9 allows and
  the semantic stage refused for indexing something that is neither a vector nor a matrix. The
  member table had carried the length all along and both back ends already lay a struct out as
  its members end to end - what was missing was the rule. A member whose element is a *vector*
  needed one thing more: the stride is the element's width, and a member's type is its element
  type, so `vec2 p[3]` looked from the type alone like a six-component value.
- **`p == q` on two structs**, which 5.9 gives to every type but an array. The generator refused
  it although a struct is the same run of registers a matrix is, and the interpreter answered it
  by comparing the first component and stopping - so two structs differing in any later member
  were equal. The generator and the interpreter were wrong in different ways, which is what two
  harnesses are for.
- **a macro inside a function-like macro's body.** Rescanning worked; *splicing* did not. A
  macro's body was queued and re-read so that any macro in it would expand in turn, but the
  expansion was **appended** to that queue rather than inserted where the reading had got to -
  so `#define SCALE(x) ((x) * HALF)` queued `( ( 1.0 ) * HALF )`, read as far as `HALF`, and put
  `0.5` after the closing paren. The parser saw `((1.0) * ) 0.5`. It only shows when the inner
  macro is not the *last* token of the outer body, which is why every macro in every port corpus
  expanded correctly: `#define F(x) G(x)` ends on the nested call, where appending and splicing
  are the same thing.
- **whole-array assignment refused by the wrong stage.** `v = w` was caught by the code
  generator, on the true but irrelevant grounds that a run of registers cannot be copied. GLSL
  1.10 5.8 lists what an l-value is and an array is not among them, so it is ill-formed before
  any back end has an opinion - and the interpreter, which has a float array and would happily
  have copied something, was never asked. The rule has to sit at the assignment rather than in
  `glsl_is_lvalue`, because that recurses *through* the array's name on the way to `v[0]`.
- **a macro used twice with a macro for its argument.** The guard that stops a macro expanding
  inside its own expansion was a flag cleared only when the reader went back to the lexer, which
  made it "expands at most once before the next real token" rather than "not within itself". So
  `#define DUP(a) ((a) + (a))` on `DUP(HALF)` expanded the first `HALF` and left the second as a
  bare identifier, because nothing between them came from the lexer. One argument used once hides
  it, which is every macro in every port corpus. Fixed with an end marker queued behind each
  expansion; `#define A A` still terminates, because the `A` is read while the flag is set.
- **a splice in the wrong direction.** The queue's tail moves over itself when a body is spliced
  in, and which of source and destination is larger depends on how much had been read and how
  long the body is. A loop in one fixed direction is right half the time; `DUP(HALF)` is three
  tokens deep when a one-token body arrives, so the destination is *below* the source and only a
  forward copy survives. This one was introduced by the fix above it and caught by the same
  corpus one batch later, which is the argument for a gate over a report.
- **`gl_FragData[0]`**, which is GLSL 1.10 7.2's other name for the colour a fragment shader
  writes. The built-in table has carried it as a one-element array from the start and the
  interpreter exports from it; only the compiled path had no registers for it, so a shader
  spelling its output that way ran on the software reference and was refused for the console.
- **an array as a function parameter** (6.1). The semantic pass declared the parameter without
  its length, so `w[0]` in the body asked the index rule to index a `float`; the generator would
  not hand a whole array to a call; and the interpreter could not have carried one through an
  `exec_val_t`, which holds one value's worth of floats. It is still a *copy*, as every GLSL
  parameter is - so a body writing `w[0]` leaves the caller's array alone. `out` and `inout`
  arrays stay refused, and for the reason above: copying one back needs the argument to be a
  place, and 5.8 does not make a whole array one.
- **the built-in constants** (7.4) - `gl_MaxTextureUnits` and the rest - which were not declared
  at all, so a shader reading one was told "use of an undeclared name". **Every value is the
  constant the matching `glGetIntegerv` answers with**, because that is the point of them: a
  program sizes an array from the API and a shader sizes a loop from the constant, and nothing
  but a shared definition makes the two agree. They are `const int`, so both back ends fold a use
  into a literal and one may be an array's length.

  `gl_MaxDrawBuffers` is the exception and is refused with a sentence rather than given a number.
  GLSL declares `gl_FragData[gl_MaxDrawBuffers]`, so the constant and that array's length are
  meant to be one fact, and here they are not: `GL_MAX_DRAW_BUFFERS` answers 2 for the front and
  back surfaces, which both receive the same fragment colour, while the fragment stage exports one
  target and `gl_FragData` has one element. Either could be made to agree with the other, and
  which is a design decision about what a draw buffer means for a double-buffered window - so a
  shader asking is told that instead of being handed a number that disagrees with something.
- **`gl_` is reserved** (3.7), and a shader could declare a name beginning with it. The generator
  tripped over the collision for a name it happened to have a built-in for; anything else went
  through. Now the semantic stage refuses the declaration, and the parameter list too.

The survey has been wrong three times, each time about the corpus rather than the compiler -
reading a shader's stage off its file extension, pairing every fragment shader with one fixed
vertex shader, and counting a Prism template or a library with no `main` as a refusal. Each is
written into the script, because an instrument that agrees with you is the one that does not get
checked.

The staging that got here is worth keeping in view, because it is the one that makes each step
measurable:

| Stage | Can be finished now? |
|---|---|
| Lexer | **done** - text in, tokens out, nothing hardware-shaped about it |
| Expression parser and AST | **done** - the full 1.10 precedence ladder, arena-allocated, indices not pointers |
| Declarations and statements | **done** - blocks, selection, iteration, jumps, declarators, functions, translation unit |
| Preprocessor | **done** - `#version`, object-like *and* function-like macros, `#ifdef`/`#ifndef` nesting, `#if`/`#elif` over the full constant-expression grammar, `#extension`, `#pragma` and `#line` |
| `struct`, and the type-name ambiguity | **done** - the parser keeps the struct names it has seen and `starts_declaration` asks, which is how C resolves the same ambiguity |
| Types, scopes, expression checking | **done** - operator rules, swizzles, constructors, shadowing |
| Statement checking, function signatures, l-values | **done** - the front end is complete for what 1.10 shaders use |
| `struct`, arrays beyond a size, built-in functions | **done** - and the struct layout is decided once, in the semantic pass, so the interpreter and the generator read one table rather than each deriving its own. A struct is a run of registers on the console and a run of floats on the reference, which is the only place the two differ |
| Code generation | **The export-count gate is measured.** `REQ-...-f9d3`'s probe never launched a wavefront; its re-file `REQ-...-7c40` did, on oops-gl's own non-passthrough stage (`0x00c12010`, sweep `20260917-200129`), and a third parameter export retired and drew like the two-export control (arm `primary`). What that does not yet show is the third parameter's values arriving intact - gl2-cube's first hardware run is where that is read |
| Instruction encoding | **started** - `src/gl/glsl_emit.c`. VOP1/VOP2 formats, the constant forms, and `mat4 * vec4`, every field verified against clang's own output |
| Instruction selection from the AST | **started** - `src/gl/glsl_gen.c`. A value is consecutive VGPRs, one per component, `mat4` column-major; a bump allocator with a per-statement mark; `+ - * /` component-wise, scalar broadcast either way round, `mat4 * vec4`, unary minus, swizzle reads and writes (a write is a move into each register the swizzle names, not a masked move - which is only true because nothing is packed), `vecN`/`matN` constructors (and `matN(s)` fills the **diagonal**, which is a different rule from `vecN(s)`), plain and compound assignment, `++`/`--`, declarations with initialisers, blocks, every loop shape, user-defined functions, `struct`, and the built-in library. A struct needed no new storage shape - its members are consecutive registers, so reading one is naming a slice rather than moving anything. What is left sets `error` and emits nothing, because an instruction whose encoding has not been read out of an assembler is not guessed, and a polynomial of unmeasured accuracy is not one either |
| The built-in library, generated | GLSL section 8 onto the pinned opcodes: one instruction a component for `sqrt`, `inversesqrt`, `floor`, `ceil`, `fract`, `exp2`, `log2`, `min`, `max`; a short sequence for `abs`, `sign`, `clamp`, `mix`, `step`, `smoothstep`, `mod`, `pow`, `exp`, `log`, `radians`, `degrees`, `sin`, `cos`, `tan`, `dot`, `length`, `distance`, `normalize`, `cross`, `reflect`, `faceforward`. **Two hardware facts sit inside those**: `v_sin_f32` takes revolutions and not radians, and `v_exp_f32`/`v_log_f32` are base two. Each fails by a clean factor if its constant is dropped, which is why `test_gl2_compiled_arithmetic_matches_the_language` runs the emitted words rather than asserting them |
| Anything about the shader *interface* | **partly** - varyings are parameter exports, and the export count is two in every shader oops-gl ships. `REQ-...-3a91` and `REQ-...-f9d3` could not answer whether it can be three; `REQ-...-7c40` did (see Code generation), so three is measured to draw and the values are what is left |

Nothing in 1.x is wasted when 2.0 arrives: a GL 2.0 context still has to answer every 1.x call,
and the reference above shows it - the per-fragment operations, the depth test, blending,
culling and the scissor are the same code for both pipelines, and `gl2-probe` checks that they
still apply around a program rather than being bypassed by one.

## How shader words get written here

Not from memory. `tools/shader/` holds the source, it is assembled for gfx1030, and the words go
into the C with the instruction beside them. A wrong encoding cannot fail loudly - it assembles
into the payload, the hardware does something else, and the frame is wrong rather than the build
stopped. See `tools/shader/README.md`.
