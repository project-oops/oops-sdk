# D012 - One renderer API, and the display open moves inside it

**Status:** proposed
**Date:** 2026-09-21

## The choice

A title says which renderer it wants at build time and writes the same source either way:

```make
OOPS_RENDERER = gl1        # or gl2, or mesa
```

```c
oops_gfx_t *gfx = oops_gfx_create(&(oops_gfx_desc_t){ .width = 1920, .height = 1080,
                                                      .depth = true });
/* ... ordinary GL ... */
oops_gfx_present(gfx);
```

`oops_gfx_create` **opens the display itself**. A title no longer calls `oops_display_open` before
creating a context, and `glContextCreate(disp)` - which takes a display the title opened - is
superseded by it.

That inversion is the whole content of this decision. Everything else here is the bookkeeping that
follows from it.

## Why the display open has to move

Not taste. [D011](D011-the-display-names-its-scanout-buffers-once.md) measured that VideoOut
registration is **single-shot and immutable**: a buffer set cannot be extended, replaced or
released, and `sceVideoOutUnregisterBuffer` is not exported at all. Whatever is named at open is
what the display can ever show.

So a renderer that wants its own target scanned out - rather than copied into somebody else's
buffer every frame - has to hand that target over *at open*. Which means it has to already own the
target when the display opens. Which means **the renderer opens the display, or it does not get
direct scanout.**

That is not hypothetical, and the number is large. oops-mesa's frame before and after it took
ownership:

| | copy path | direct scanout |
|---|---|---|
| frame time | 36769 us | **16682 us** |
| frame rate | 27.2 fps | **59.94 fps** |
| CPU pixel work | 24203 us | **0** |

The copy path was not slow because it was badly written. It was slow because the renderer did not
own the display and therefore could not name its own buffers, so every frame had to be read out and
tiled into buffers somebody else had registered. `oops_display_open_adopting` exists because of
this, and a title that calls `oops_display_open` first has already lost the option.

oops-gl is on the losing side of that today, and
`obSCEne REQ-20260919T1927Z-7e21` is the request that wants it moved. This decision is the API
change that makes the move possible rather than a special case.

## The API

```c
typedef struct oops_gfx oops_gfx_t;

typedef struct {
    uint32_t width, height;   /* 0, 0 means the display's own extent */
    bool     depth;           /* a depth buffer is wanted */
    bool     vsync;           /* present paces itself to scanout */
} oops_gfx_desc_t;

oops_gfx_t      *oops_gfx_create(const oops_gfx_desc_t *desc);
bool             oops_gfx_present(oops_gfx_t *gfx);
void             oops_gfx_extent(const oops_gfx_t *gfx, uint32_t *w, uint32_t *h);
oops_display_t  *oops_gfx_display(oops_gfx_t *gfx);
const char      *oops_gfx_backend_name(void);
void             oops_gfx_destroy(oops_gfx_t *gfx);
```

Five functions and one struct, because that is the whole intersection of what the three backends
can honestly do. Each field and each omission is argued below.

### `width`/`height` are the title's, not the display's

Taken from oops-mesa's `oops_gl_create`, which already works this way and says why: a title may
render smaller than the screen and let the compositor scale, which is the cheaper path at 4K. The
display is told about the buffer this creates, not the other way round. `0, 0` asks for the
display's extent, which is what most titles want and what oops-gl does today.

### There is no colour format

`B8G8R8A8_UNORM`, not a parameter. It is what the display controller reports for its own scanout
(`REQ-20260909T1315Z-71dc`, sweep `20260909-144348`: pixel format `0x80000000`, stride exactly
`width * 4`). There is no second format any of this has evidence for, and
[D009](D009-an-absent-feature-is-an-absent-symbol.md) says an absent feature is an absent symbol
rather than a parameter that only takes one value.

### There is no `make_current`

There is exactly one context. It is created with the display and never switched, on all three
backends - oops-gl has one implicit context, and oops-mesa's `oops_gl_create` makes its context
current on the calling thread and keeps it there. A `make_current` that always succeeds would be a
verb with nothing behind it ([D001](D001-a-verb-with-nothing-behind-it-fails.md)), and a caller who
believed a second context existed would draw into the first one and see no error.

The SDL backend already handles this the honest way - it hands out a sentinel, succeeds on that
one and *fails* on anything else - and that stays where it is. SDL's model has contexts, so SDL's
backend does the adapting. The SDK's API does not pretend to have them.

### The backend is a build switch, never a runtime one

Because it is not free. Mesa is roughly 20 MB of archives and a hosted sysroot; oops-gl is a source
list. A runtime enum would mean both were linked in, which is the wrong bill for a title that wants
one. `oops_gfx_backend_name()` exists so a title can *log* what it got, not choose it.

This also keeps `USE_MESA` honest. It is already a build switch that changes the C library, the
link script and the undefined-symbol check; making the renderer choice a different mechanism from
the thing it already implies would be two switches for one decision.

### `oops_gfx_display()` is an accessor, not a leak

SDL's video backend needs the display for `GL_GetDrawableSize` and for binding the event pump, and
a title wanting `oops_draw_*` on a CPU surface needs it too. Hiding it would mean re-exporting the
display's API through this one, so it is handed out instead.

### `glSwapBuffers()` stays, as a wrapper

It takes no arguments and presents the implicit context, which is what GL source expects and what
every port will call. It becomes a one-line wrapper over `oops_gfx_present` on the current handle.
Keeping it is a compatibility affordance for ported code; new titles use the handle.

## What this does for SDL, which is the second payoff

`SDL_prosperovideo.c` looks renderer-agnostic and is one call away from being it. It includes
`oops/display.h` and never calls a single `gl*` function - the context is a sentinel, and
`GL_SwapWindow` is `oops_display_flip`. Two things bind it to oops-gl:

| | today | after |
|---|---|---|
| `GL_SwapWindow` | `oops_display_flip(disp)` | `oops_gfx_present(gfx)` |
| `GL_GetProcAddress` | `NULL` for everything | the dispatch table, when the backend has one |

The first is a correctness bug the day anyone builds SDL against Mesa: `oops_display_flip` would
flip a buffer Mesa never drew into, because Mesa's buffers are its own and its present path owns
the flip index and the scanout pacing. The second is why `GLEW`, `glad` and most GL 2+ SDL software
fails at init.

`PROSPERO_VideoInit` calls `oops_gfx_create` instead of `oops_display_open`, holds the handle, and
SDL works on every backend without knowing which one it has. That is
[D010](D010-oops-sdl-is-a-backend-inside-upstream.md) working as intended - the adapting happens
inside the backend, and upstream is untouched.

## What is explicitly not decided here

- **Whether `glContextCreate` is deleted or kept as a deprecated wrapper.** That is oops-gl's call,
  and it depends on how many external callers exist. This decision needs it to stop being the *only*
  way in; it does not need it gone.
- **The HUD.** `oops_hud_*` drawn in GL 1.1 is the natural companion to this and is argued
  separately, because it is a library on top of this API rather than part of it.
- **Whether oops-gl actually takes direct scanout.** This makes it possible. Whether the linear
  scratch copy is worth removing is a measurement oops-gl owns, and `-7e21` is where it lives.

## What would move this to decided

The oops-sdk half is filed as `REQ-20260921T1626Z-4c19` on oops-sdk's own inbox. This becomes 🟢
when oops-gl agrees the display open can move - that is the part this repository cannot decide
alone, because it changes `glContextCreate`'s contract and oops-gl owns it.

If it comes back refused, the fallback is worse but workable: keep two APIs and have oops-mesa's
titles use theirs. That is where we are today, so nothing is lost by asking.

## What each side builds

| | |
|---|---|
| **oops-sdk** | `include/oops/gfx.h`; the gl1/gl2 backend over the existing context code, with the display open moved inside it; `glSwapBuffers` rewired as a wrapper; the SDL backend's two functions |
| **oops-mesa** | the same five functions over the DRI loader - `oops_gl_create`/`oops_gl_present` already are them under other names - and providing them when `OOPS_RENDERER = mesa` |
| **oops-apps** | `OOPS_RENDERER` in `common/app.mk`; every `main` switched; the duplicated matrix helpers deleted in favour of `oops/math.h` |
