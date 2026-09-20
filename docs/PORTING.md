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
4. **Check the symbol table**, not just the build - see *The one that will get you* below.

## What is here

**OpenGL 1.0 through 1.5**, every entry point, with the fixed-function pipeline behind it:
matrices, lighting, materials, texturing with mipmaps and the texture environment, fog, blending,
alpha test, depth and stencil, display lists, vertex arrays, buffer objects, evaluators, selection
and feedback, the accumulation buffer, pixel rectangles. `docs/GL_ROADMAP.md` is the detailed
account, including which features the console path draws differently from the software one.

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
| C++ runtime, exceptions, the standard library | This is a C SDK. A C++ port is a much larger job |
| Colour-index visuals | `glutGet(GLUT_DISPLAY_MODE_POSSIBLE)` answers 0 for them |

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
console path is hardware, and a few features are still drawn differently or not at all -
a smooth *textured* point or line and `GL_POLYGON_SMOOTH` (plain smooth points and lines are
drawn there), texture units above the second, a volume's mip chain, and an
occlusion query whose draws never test depth. Each one logs a line saying so the first time it
matters, and `docs/GL_ROADMAP.md` lists them with what each is waiting on. A port that looks
right on the host and wrong on the console should read the log first; it will usually name the
feature.
