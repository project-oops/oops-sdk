# Porting an OpenGL program to this SDK

This is for someone holding a GL 1.x program - a demo, a small game, a piece of homebrew - who
wants it running on a Prospero-generation console. It says what is here, what is not, and which
of the differences will bite you. It does not say the port will be easy; it says where the work
is.

`oops-apps/src/oops-gl/glut-demo` is a worked example: a GLUT program with no SDK call in it
except its entry point, built for the target. Read it alongside this.

## The shortest possible version

1. Keep your `main`. Add an entry point beside it that calls it (below).
2. Include what you already include: `<GL/gl.h>`, `<GL/glu.h>`, `<GL/glut.h>`, `<math.h>`,
   `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<assert.h>`, `<time.h>`.
3. Copy a Makefile from an app under `oops-apps/src/oops-gl` and put your sources in it.
4. **If you use shaders, call `glContextSetVersion(2, 0)` after making the context.** A context
   has the entry points its version defines and no others, and the default is 1.1 - so without
   it `glCreateShader` returns 0 and records GL_INVALID_OPERATION. A GL 1.x port needs no such
   line.
5. **Check the symbol table**, not just the build - see *The one that will get you* below.

## What is here

**OpenGL 1.0 through 1.5**, every entry point, with the fixed-function pipeline behind it:
matrices, lighting, materials, texturing with mipmaps and the texture environment, fog, blending,
alpha test, depth and stencil, display lists, vertex arrays, buffer objects, evaluators, selection
and feedback, the accumulation buffer, pixel rectangles. `docs/GL_ROADMAP.md` is the detailed
account, including which features the console path draws differently from the software one.

**The version you claim is the version you get.** `glContextSetVersion(1, 1)` gives a GL 1.1
context and `glCreateShader` on it is GL_INVALID_OPERATION; `glContextSetVersion(2, 0)` gives
the programmable pipeline. On a real driver the linker enforces that; here it is a runtime
check, so a port meets the same refusals it will meet on a driver that really is the version it
claims. The default is 1.1, and **2.0 is never a default** - a GL 1.x program should not be able
to reach a pipeline it never asked for.

**The gate is on GL 2.0's entry points and not within GL 1.x**, and that is deliberate. Every
1.2 through 1.5 feature here is also advertised in `glGetString(GL_EXTENSIONS)` -
`GL_ARB_multitexture`, `GL_ARB_vertex_buffer_object`, `GL_EXT_fog_coord` and the rest - and an
extension is available to a context whatever its core version. So a GL 1.1 context genuinely has
buffer objects, and refusing `glBindBuffer` while `glBindBufferARB` beside it worked would be a
rule about spelling rather than about capability. Nothing advertises the programmable pipeline
as an extension, which is why the claim is the only door to that one.

**OpenGL 2.0's programmable pipeline.** `glCreateShader` through `glUseProgram`, uniforms,
generic vertex attributes, separate stencil and blend state, and **GLSL 1.10 and 1.20** behind
them: a shader compiles, a program links, and both stages run.

**It runs on a console too** (measured 2026-09-21). The fragment stage compiles to real gfx1030
instructions, the draw path binds them, and obSCEne's `REQ-20260921T1615Z-4e77` and
`REQ-20260921T1730Z-6c0d` measured a generated shader retiring, its interpolated parameters
arriving bit for bit, and its uniform block loading intact.

**What a compiled fragment shader can do** is arithmetic, swizzle reads and writes,
constructors, the built-in library, file-scope `const`s, uniforms, `texture2D` through up to two
samplers, comparisons, `?:`, `if`/`else` and `discard`. What it cannot: **loops**, because a mask
cannot express a per-lane trip count and a real branch is needed; the projective, cube, volume
and shadow texture lookups; user-defined functions; integer arithmetic; and the inverse
trigonometric functions, where the only lowering is a polynomial of somebody's choosing. A shader
the back end will not take is refused with a sentence naming what is missing, and still runs on
the software path - the draw is what fails, with `GL_INVALID_OPERATION`, rather than quietly
drawing something else. A GL 1.x port is unaffected: it never binds a program.

**`#version 120` is a different language from `#version 110`, and the number decides which you
get.** 1.20 converts `int` to `float` implicitly, so `pos * 2` and `clamp(v, 0, 1)` are shaders;
1.10 converts nothing and both are errors. If your shaders were written against 1.20 - which is
most shaders written after about 2006 - say so on the first line, because a 1.10 shader held to
1.10's rules is the wall you will otherwise hit on your first line of arithmetic.

A program with only *one* stage is legal and is the useful halfway house: a vertex shader that
writes `gl_FrontColor` and `gl_TexCoord[]` leaves the fixed-function fragment stage to run, and
a fragment shader with no vertex shader reads what the fixed-function transform produced. That
is how a port replaces one half of its pipeline at a time.

**GLU**: `gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluPickMatrix`, `gluErrorString`,
`gluBuild2DMipmaps`, `gluBuild1DMipmaps`, `gluScaleImage`, `gluProject`, `gluUnProject`,
`gluGetString`, and the quadrics (`gluNewQuadric`, `gluSphere`, `gluCylinder`, `gluDisk`,
`gluPartialDisk` and the calls that configure them).

**GLUT**, the subset programs actually call: one window, the display/reshape/idle/keyboard/
special/mouse/motion/timer callbacks, `glutMainLoop`, `glutPostRedisplay`, `glutSwapBuffers`,
`glutGet`, `glutGetModifiers`, and the solids.

**The C library a port uses**: `<math.h>`, `<string.h>`, `<stdlib.h>`, `<stdio.h>` (including
`fopen`/`fread`/`fgets` over the console's filesystem), `<ctype.h>`, `<assert.h>`, `<time.h>`.
The compiler provides `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`, `<limits.h>`,
`<float.h>`.

Two parts of it are there because of what a GL 1.x port does rather than because a C library
has them. **`qsort`** - a fixed-function pipeline blends in the order the triangles arrive, so
anything see-through is sorted back to front by the program, every frame. And **`sscanf`,
`strtok`, `strdup`, `strspn`, `strcspn`, `strpbrk`** - an OBJ, MTL or level loader is a `fgets`
and an `sscanf("%f %f %f")`, and without those the loader is the part of the port that has to be
rewritten. `isnan` and its kin are macros over the compiler's builtins, so `if (isnan(x))`
compiles.

`sscanf` is checked against the host's own library rather than against a list of expectations:
`test_freestd_sscanf_agrees_with_the_host` runs the same inputs through both and compares. That
is where its three differences from C were found, all of them in what *fails* rather than in
what converts.

## What is not here, and what happens if you call it

Everything in this section **fails to link** rather than doing something plausible. That is
deliberate: a stub that draws nothing or returns 1970 is a bug you find on a console, and a link
error is one you find on your desk.

| Missing | Notes |
|---|---|
| GLUT subwindows, menus, overlays, game mode | One window, always the display's size |
| `glutReshapeWindow`, `glutPositionWindow`, `glutWarpPointer` | The window *is* the display, at its origin, and there is no pointer. `glutFullScreen`, `glutSetWindow` and the titles **are** here: each of those gets exactly what it asked for |
| `GLUT_BITMAP_HELVETICA_*`, `GLUT_BITMAP_TIMES_ROMAN_*`, `glutStrokeCharacter` | Those are proportional fonts and a stroke font. `glutBitmapCharacter` **is** here for `GLUT_BITMAP_8_BY_13` and `GLUT_BITMAP_9_BY_15`, with glyphs drawn in this SDK rather than taken from X11 - a fixed-width font offered under a proportional name would return the wrong `glutBitmapWidth` and break the layout of anything that measures before it draws |
| `glutSolidTeapot` | 306 control points this does not carry. The five Platonic solids **are** here, derived from their definitions rather than from anyone's table |
| GLU tessellator, NURBS | |
| `localtime`, `gmtime`, `mktime`, `strftime` | There is no calendar - see `<libc/time.h>`. Whether one can be read at all is asked on the obSCEne bus (`REQ-20260920T1415Z-3f8a`); the arithmetic is the easy part |
| `fscanf`, `scanf` | Both need to put a character back when a conversion reads one too many, and this SDK's file handles have no pushback. **`sscanf` is here**: read the line with `fgets` and scan the line, which is what the code being ported does anyway |
| GLSL `struct`, and the built-in uniform structures | `gl_LightSource[]`, `gl_Fog`, `gl_FrontMaterial` and `gl_DepthRange` are **refused by name** at compile, so you are told what is missing rather than doubting your spelling. The matrices, `gl_Vertex`, `gl_Color`, `gl_MultiTexCoord*`, `gl_TexCoord[]` and the rest of the non-struct built-ins **are** here |
| GLSL 1.30 and later | `#version 130` is refused **by number** rather than compiled as 1.20. A 1.30 shader takes `in`/`out` in place of `attribute`/`varying` and means its integer arithmetic; compiling it as an earlier dialect is a wrong picture with no diagnostic attached to it. **1.10 and 1.20 are both here** |
| GLSL non-square matrices | `mat2x3` and its relatives are 1.20's and are not implemented. `mat2`, `mat3` and `mat4` are, and so are 1.20's `transpose` and `outerProduct` over them |
| `gl_PointCoord`, point sprites | GL 2.0's point sprite is not drawn, so `gl_PointCoord` is refused by name rather than being a `vec2` that always reads (0, 0) |
| `noise1` through `noise4` | Named in the refusal, for the same reason. The specification permits them to return zero, and nothing should rely on that |
| Framebuffer objects, `glDrawBuffers` to colour attachments | GL 3.0. `glDrawBuffers` on the window-system framebuffer is here and means what the specification says it means there: the same fragment colour to several buffers |
| C++ runtime, exceptions, the standard library | This is a C SDK. A C++ port is a much larger job |
| Colour-index visuals | `glutGet(GLUT_DISPLAY_MODE_POSSIBLE)` answers 0 for them |

## Porting a shader: what the console's back end generates

A GL 2.0 program has two implementations here and they are not the same code. The software
rasteriser runs your GLSL on the CPU and implements all of it. The console compiles your
**fragment** shader to gfx1030 at `glLinkProgram`; the vertex shader runs on the CPU on both
paths, so nothing in this section is about it.

**A shader the back end will not generate still links.** `glLinkProgram` succeeds,
`GL_LINK_STATUS` is true, and the program draws correctly on the software path. What it does on
a console is nothing: the draw records `GL_INVALID_OPERATION` and logs one line naming what
could not be generated, once per program. `glGetProgramHardwareLog` returns that same line, which is the check to make at startup if you
want to know before you draw.

`GL_PROGRAM_HW_PS_WORDS` is the instruction count and is zero for **two** different programs: one
that was refused, and one with no fragment shader at all - the second is not a failure, since the
fixed-function pixel shader runs for it. The log tells them apart: it names a reason for the
first and is empty for the second.

That arrangement is deliberate. A back end that quietly substituted something plausible would
put a picture on screen that your shader did not ask for, and you would be debugging the
picture instead of reading the reason.

### What is generated

Float, vector and matrix arithmetic; `int` and `bool`, including `ivec` and `bvec`; swizzles on
both sides of an assignment; the comparisons, the logical operators and `?:`; `if`/`else`;
`discard`; `texture2D` through two samplers; uniforms of every scalar and vector type;
user-defined functions, including `out` and `inout` parameters; the vector relational family
(`lessThan` and its relatives, `any`, `all`, `not`); the derivatives `dFdx`, `dFdy` and
`fwidth`; and the fragment built-ins `gl_FragCoord`, `gl_FrontFacing`, `gl_FragDepth`,
`gl_Color` and `gl_FragColor`.

Integer division truncates toward zero and is exact - there is no divide instruction on this
part, so the quotient is computed from a reciprocal and then corrected, which is why `7 / 7` is
1 and not 0.

### What is refused, and what it says

| Refused | What you get, and what to write instead |
|---|---|
| A loop whose trip count is not known at compile time | Loops are **unrolled**, not branched: a loop whose condition never goes false hangs the part rather than drawing the wrong colour, and that is not a failure this SDK will risk on your behalf. `for (int i = 0; i < 8; i++)` unrolls; a bound that is a uniform does not, and says so. The limit is 64 trips, and the message carries that number |
| `break` and `continue` | Each needs the exec mask carried through the rest of the loop. Write the loop without them - a `if (cond) { ... }` around the rest of the body is the usual shape and unrolls fine |
| An early `return` from a function | Same reason. A function whose body ends in its `return` is generated; one that returns from inside an `if` is not |
| A `void` function used for its side effects on globals | A `void` function **is** generated - `out` and `inout` parameters carry results back. What is not is one whose effect is to assign to a global |
| `asin`, `acos`, `atan`, `refract` | No instruction on this part, and a polynomial of unmeasured accuracy is not written in their place. Each is refused **by name**, so you are told which one |
| `textureCube`, `texture3D`, the `Proj` and shadow forms | Only `texture2D` is generated. The others are each a different lookup rather than the same one with a flag |
| Matrix by matrix, matrix by scalar | `mat * vec` and `vec * mat` are generated at every square size, and they are different products - the second is the transpose's |
| More than 16 floats of varyings, more than 32 floats of uniforms, more than two samplers | Each is refused with its own number in the message, so you know what to cut to |

Everything above is refused **by name with a line and column**, not as a general failure. If a
shader will not compile for the console, the log tells you which construct and where.

### If you are reading this against an older build

The whole of the above arrived between 2026-09-21 and 2026-09-22. A build from before that
compiles a much smaller subset and refuses the rest the same way, so the diagnosis method is
unchanged: read the line the draw logged.

## Porting to the Mesa renderer, which is a different job

Everything above describes oops-gl, this SDK's own OpenGL. A title can instead set
`OOPS_RENDERER = mesa` in its Makefile and get **upstream Mesa with radeonsi and ACO**
(`oops-mesa`). The scope there is OpenGL 3.3 and the driver reports 4.6 - see `oops-mesa#D014`
for which of those is a promise and `oops-mesa/docs/GL_SURFACE.md` for what is reachable.

Most of this document still applies, because most of it is about the C surface. What changes:

**GLUT works on both, and that is not an accident.** `src/gl/glut.c` names no OpenGL
implementation - it goes through `oops/gfx.h` and otherwise calls plain GL - so the same GLUT
drives oops-gl or Mesa depending on one line in the Makefile. A GLUT port does not have to choose
a renderer up front. This is the single biggest reason a GL program ports here at all, and it is
worth not breaking.

**GLU does not.** `src/gl/gl_glu.c` reaches into oops-gl for `gl_sin`, `gl_sqrt`,
`gl_pixel_transfer_rgbaf` and more, and those live in files that between them define forty `gl*`
entry points - linking them beside Mesa would put a second `glMatrixMode` in the binary. So a
Mesa-linked title currently **cannot use this SDK's GLU**, including `gluNewQuadric` and the
`glutSolid*` shapes built on it. Filed as `REQ-20260922T0940Z-5c17`; until it lands, a title that
needs them carries its own, and `oops-apps/src/oops-titles/mesa-demos/shim/` shows what that
costs.

**A requested window size is a hint.** `glutInitWindowSize` is a request to a window manager and
there is not one. The Mesa backend opens the display at its own extent when the requested size
cannot be scanned out, and `glutGet(GLUT_WINDOW_WIDTH)` and the reshape callback then report what
was actually opened - exactly as on a desktop whose window manager gave you something else. Read
them; do not assume you got what you asked for.

### If the program loads GL through glad, GLEW or epoxy

Modern GL programs resolve entry points at run time through a loader, because on a desktop libGL
exports only GL 1.1 and everything newer arrives as a function pointer. **That problem does not
exist here.** Mesa is linked statically and every `gl*` name is bound at link time, so the loading
half of any of those libraries is a no-op and does not need porting.

**The reporting half does, and it is the part that will catch you.** Loaders also expose booleans
- glad's `GLAD_GL_EXT_fog_coord`, GLEW's `GLEW_EXT_fog_coord` - that programs read to decide
whether to take an extension path at all. Defining them all to 1 is one line and is wrong in a way
that produces no diagnostic: the program takes a path for an extension that is not there and draws
nothing. Answer them from `glutExtensionSupported` (below) or from `glGetString` directly, so a
flag is false when the driver says so and the program takes its own "not supported" path.

`oops-apps/src/oops-titles/mesa-demos/shim/include/glad/glad.h` is a worked example: eleven names,
all queried, none asserted.

### `glutExtensionSupported` is here, and you should use it

`int glutExtensionSupported(const char *name)` matches **whole words** against the driver's
extension list, and asks both the flat `glGetString(GL_EXTENSIONS)` and the indexed
`glGetStringi` form so it keeps answering on a core profile.

Whole-word matching is the point. A plain `strstr` finds `GL_EXT_texture` inside
`GL_EXT_texture3D`, so a driver offering only the second reports both - and the bug surfaces as a
blank screen much later. If you write your own, match boundaries.

## The one that will get you

**A payload link ignores unresolved symbols.** It has to: the console's own modules resolve
`sce*` imports when the payload loads. The consequence is that a call to a function nobody
defines **links cleanly** and faults when it runs.

So a clean build is not evidence. This is:

```sh
nm -u build/your-app.elf | sed 's/^[[:space:]]*//;s/^w //;s/^U //' \
  | grep -vE '^sce[A-Z]|^sysctlbyname$'
```

Anything that prints is a function your program calls and nothing provides. The expected output
is nothing at all.

**A build that uses `oops-apps/common/app.mk` runs this for you** since 2026-09-20: every
payload link is followed by the same check, a symbol nothing defines fails the build, and the
ELF is deleted so a payload that would fault on the console is not left looking finished. Run
the command above by hand for a port with a build of its own.

That makefile also makes itself a prerequisite of the ELF, which it was not before. A payload's
ELF depends on its sources, not on the file that lists them, so adding a source did not relink
what was already built - and the check then answered about the *previous* build, which is how a
fix that had worked looked like it had not.

This is not hypothetical, and not once. `glut-demo` was built with `-Werror` against these
headers before `libc.c` was in any source list: it linked without a single diagnostic and left
eleven standard C functions undefined. On 2026-09-20 it called `glutBitmapString` before
`glut_font.c` was in its list, and then named `obs_vsscanf` before `scanf.c` was in
`CORE_SDK_SRCS`. Three times in one day is what moved the check out of this guide and into the
build. `oops-sdk/tools/libc-check` does the same job on the SDK's own side, and
`make checks` in `oops-sdk` runs it.

**If you build against the Mesa sysroot too**, `make checks` also runs `tools/header-check`,
which compiles this SDK's `<GL/gl.h>` beside a real `GL/glext.h`. That is the only place the two
sets of GL headers meet, and it is where a macro redefined with a different token sequence, or a
declaration whose types drifted, shows up - neither of which a test including only our own header
can see. It found both on 2026-09-20 only because an app failed to build; now it looks.

## The entry point

A payload is entered **by name**, not by the C runtime, so there is no `main` for the loader to
find. Keep yours and add this beside it:

```c
#ifndef OOPS_HOST_BUILD
#include "oops/syscall.h"

int your_app_start(const payload_args_t *args);

__attribute__((visibility("default"))) int your_app_start(const payload_args_t *args) {
    if (args) sys_call_init(args);
    return main(0, (char **)0);
}
#endif
```

and name it in your Makefile: `ENTRY_POINT := your_app_start`. That is the whole of the
platform-specific work in a well-behaved port.

## Differences you have to know about

**`glutMainLoop` returns.** GLUT promises it never does. A console program whose only exit is a
window close has no way to stop, so `glutLeaveMainLoop()` - freeglut's spelling - ends it and
`glutMainLoop` returns to your `main`.

**The pad arrives as keys.** Most consoles have no keyboard attached, so without this many ports
run and cannot be controlled: the d-pad is delivered as `GLUT_KEY_LEFT`/`UP`/`RIGHT`/`DOWN`
through your special callback, cross and circle as `\r` and escape through your keyboard
callback, and **the option button leaves the main loop**. `glutOopsPadKeys(0)` turns it off if
you read the pad yourself.

**`printf` goes to the kernel log**, one line at a time, tagged `stdout` or `stderr`. There is no
terminal. If your program's output is data rather than diagnostics, write a file.

**`time()` is not a wall clock.** It counts from the payload's start. `srand(time(NULL))` and
elapsed measurements are fine; printing a date is not, which is why the calendar functions are
absent rather than approximate.

**`glGetString(GL_VERSION)` says 1.1 by default**, because that is the honest class of what is
implemented everywhere. If your program gates a feature on the badge and refuses to run, call
`glContextSetVersion(1, 4)` - it changes what is reported and nothing else.

**The extension string is path-dependent and short.** An extension is listed only when its own
entry points exist *and* the current path keeps the promise. The two lists are identical today -
`GL_ARB_multitexture`, `GL_ARB_texture_cube_map`, `GL_EXT_texture3D`, `GL_ARB_depth_texture`,
`GL_ARB_shadow` and `GL_ARB_occlusion_query` were the last to differ and joined the console's on
2026-09-20 - but they are built separately for a reason, and a feature whose hardware half is
unfinished drops off the console's one again. Gate on the extension string rather than the version where you can: it is
the one that tells the truth about the path you are actually running on.

**A zero-sized texture image releases the level**, as the specification says it does: a width,
height or depth of 0 frees that level's storage and leaves it absent, which makes the texture
incomplete if it was needed - so a draw with it is untextured rather than one reading storage
nothing allocated. A negative size is still `GL_INVALID_VALUE`. This is the shape a loader that
nulls levels out uses, and the library refused it until 2026-09-20.

## Where the console path differs from the software one

The software rasteriser is the reference: it implements the whole of what is listed above. The
console path is hardware, and this is no longer a list of suspicions - as of 2026-09-20
gl1-probe runs its whole suite on a console and reports **74 of 82 checks passing there**. What
follows is what the other eight are.

Six of those eight were one bug and it is fixed: `glGetFrameReadbackSampled` handed back the
frame as of the last GPU submit, so a pixel rectangle the CPU had written since - `glDrawPixels`,
`glBitmap`, `glCopyPixels`, `glAccum` - was missing from what a program read back. **If you are
reading this against a build from before 2026-09-21, a HUD or a `glutBitmapCharacter` font will
seem to vanish; it is the readback, not the draw.**

The two that remain are narrower, and one of them has a fix waiting for its console run.
`glBlendColor`'s green channel comes back as the constant's alpha on the console
(`REQ-20260920T2320Z-4b8d`), so `GL_CONSTANT_COLOR` blending is off in one channel. And
`glDrawBuffer(GL_FRONT_AND_BACK)` reached the back buffer correctly and the front one
incorrectly: blending is per colour target on this part, and only the first target's control
register was written, so the front *replaced* where the back blended. **If you draw into both
buffers with blending on, against a build from before 2026-09-21, the front is wrong and the
back is right.**

Then the features that are drawn differently rather than wrongly: a smooth point or line using
**two** texture units, a volume's mip chain, and an occlusion query whose
draws never test depth. Each of those logs a line saying so the first time it matters. A
minifying filter on a 3D texture reads the base level there while the software rasteriser reads
the chain: the layout for a three-dimensional chain was written on 2026-09-21 and the hardware
read the base level regardless, so where a level sits inside a 3D image is an open measurement
(`REQ-20260921T1300Z-9b73`) rather than missing code. Smooth
points and lines are drawn there, textured or not, and both pass on hardware - the textured one
since 2026-09-21, when its coverage moved into the second texture unit's interpolant.

This list said "texture units above the second" until 2026-09-21, which was wrong and would have
sent a port looking for a console bug that is not there. **There are two texture units and no
more**, in the software rasteriser as much as on hardware: `GL_MAX_TEXTURE_UNITS` reports 2 and
`glActiveTexture(GL_TEXTURE2)` is refused with `GL_INVALID_ENUM`. GL 1.3 requires at least two,
so that is conformant; a program wanting more will find out at the `glActiveTexture` rather than
in a wrong pixel.

`docs/GL_ROADMAP.md` lists every one with the pixel it produced and what it is waiting on. A
port that looks right on the host and wrong on the console should read the log first; it will
usually name the feature.
