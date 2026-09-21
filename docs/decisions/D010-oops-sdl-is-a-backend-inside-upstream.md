# D010 - oops-sdl is a backend inside upstream SDL, not an implementation of SDL

**Status:** decided
**Date:** 2026-09-21

## The choice

**We do not write an SDL.** We pin upstream SDL at a commit, write a platform backend for it,
and register that backend with the smallest patch that will do the job. The same answer covers
every platform facade a ported title arrives wearing.

The backend is ours and is committed. Upstream's tree is fetched, never committed, exactly as
`oops-apps/src/oops-titles/README.md` already specifies for a title's own source.

**This repository is not where it lives.** D002 says there is no slot for a vendor archive here
and means it, so the dependency sits in `oops-apps` beside the titles that consume it, using the
`upstream.lock` + `patches/` + `common/upstream-fetch.sh` machinery that already exists.

## The rule this follows

**We are a bridge. Upstream is the thing, we are the connection to it.** Custom implementations
of things upstream already maintains go stale, and staleness is the cost that never appears in
the estimate. A pinned upstream plus a bump is work we can choose to do; a reimplementation is
work that arrives whether we choose it or not.

The first draft of this entry decided the opposite, on the strength of a count: 60 SDL functions
in Neverball's shipping code, nearly all of them one-line forwards onto headers this SDK already
exposes. That count is correct and it is the wrong question, for two reasons that only appear
when you look past the first title:

- **It is per title, and it grows.** 60 is what *Neverball* calls. Armagetron touches 231
  distinct `SDL_` names. Every title after the first adds to a surface we would own, and the
  second one already costs more than the backend does.
- **It counted the easy half.** Forwarding `SDL_CreateThread` is trivial. `SDL_RWops`,
  pixel-format conversion, the event queue, the audio callback thread and the joystick mapping
  database are not, and upstream has all of them written, tested and maintained.

## What upstream already did for us, measured

Read from `release-2.30.9` (`c98c4fbf`). Every one of these is a reason the port is smaller than
it sounds, and none of them was obvious before looking:

**SDL ships its own libc.** `src/stdlib/` carries `SDL_string.c`, `SDL_stdlib.c`, `SDL_malloc.c`,
`SDL_qsort.c` and `SDL_iconv.c`, and `HAVE_LIBC` turns the host's off. This was the whole
objection to the upstream route on a `-ffreestanding -nostdlib` target, and upstream answers it
itself.

**`include/SDL_config_minimal.h` needs three headers**: `stdarg.h`, `stddef.h` and `stdint.h`.
A freestanding compiler is required to provide all three. So SDL at minimal config needs
**nothing from this SDK's libc at all** - the dependency runs the other way, through the backend
we write.

**The backend is a documented shape, not a fork.** `src/video/dummy/` is upstream's own template
for a new platform and is **397 lines including headers and licence blocks**. `VideoBootStrap` is
a four-field struct, so registering a driver is one entry in one table - which is the entire
patch, and the kind that rebases cleanly for years.

**Small and odd platforms are a supported case.** Upstream ships configs for ngage, os2 and xbox.
This is not a platform shape upstream will be surprised by.

**And it links.** Seven subsystems later - video and the GL context, events, the pad, threads,
audio, timers and the base path - the whole of SDL2 plus our seven backend files **links against
this SDK with zero undefined symbols**: 130 SDL objects and 39 SDK objects into one image, for
`x86_64-unknown-freebsd -ffreestanding`, with no host libc, no pthread and no POSIX beyond what
this SDK declares. Before linking it wanted 62 symbols from outside itself, 44 of them `oops_*`
and 18 from `include/libc`; every one resolves.

The whole of our side is **seven files and a 181-line patch**, and almost all of that patch is
one `#elif` arm per registration table.

It also **builds into a payload**, and **that payload runs on a console.**
`oops-apps/src/oops-gl/sdl-probe` goes through `common/app.mk` like any other app, produces an
ELF whose only undefined symbols are the `sce*` platform imports resolved at load, and on
2026-09-21 drew **600 frames through SDL2 on retail hardware** and parked cleanly on its first
attempt - `SDL_Init`, a window, a GL context, the event pump, 600 `SDL_GL_SwapWindow` calls and
the teardown. The decision above is no longer only an argument about where code should live.

**A flip through SDL costs 9 to 12 milliseconds, and between 10 and 63 microseconds of that is
the submit.** The rest is the CPU walking a linear framebuffer into the GPU's tiled layout, which
belongs to `oops_display_try_gpu_tiler` rather than to SDL. Recorded here because it is the first
figure this repository has for what a frame through SDL costs, and because the obvious reading of
it ("SDL is slow") is the wrong one.

It is stated as a range because a single number was wrong: the first hardware run averaged about
12.3 ms a flip and a later one about 9.7 ms, on the same build. The tiling varies; the submit
never approaches it.

**The link is the measurement, and the compile is not.** One file was dropped from the source
list because it wanted `<sys/stat.h>`; 128 files then compiled clean while `SDL_joystick.c`
called four of its functions unconditionally. A compile sweep cannot see that, which is what
`oops-apps/common/app.mk`'s undefined-symbol check exists for and why this paragraph quotes a
link rather than a build.

## What this makes true for the titles

**SDL 1.2 stops being a second project.** Extreme Tux Racer wants SDL 1.2, and the first draft
made that a second hand-written facade. Upstream maintains `libsdl-org/sdl12-compat`, which is
SDL 1.2's API over SDL2. It becomes another pinned lock file, and our backend count stays at one.

**GLFW gets the same treatment when Craft's turn comes.** Craft needs GLFW, 24 entry points, and
the answer is now settled before the question is asked: pin it, write its platform layer, patch
its table.

**`src/gl/glut.c` stands, and is the last of its kind.** It is written, it is 792 lines, it
works, and rewriting working code to satisfy a rule made afterwards is churn rather than
progress. It is not the template for anything that follows.

## What this costs, stated now rather than discovered later

**Upstream's file list is a maintenance surface.** SDL builds with CMake; `oops-apps` builds with
make, so a bump can move files without the build noticing until the link fails. The undefined
symbol check that `common/app.mk` already applies is what catches it, which is the reason that
check exists.

**Patches rebase.** The mitigation is the rule the titles README already states: if it can live
outside the patch, it must. Our backend is our files in `backend/`; the patch is the bootstrap
table entry and nothing else. A patch set that grows is a signal, not a routine cost.

**We inherit upstream's licence**, which for SDL is zlib and permissive. The tree is fetched and
never committed, so the identity scan's surface stays our backend, our patches and our metadata.

**The libraries beside SDL are the same decision, applied again.** `libpng`, `libjpeg`,
`vorbisfile` and `freetype` are pinned upstreams too, not things we write. Two are narrower than
they look: `oops_png_decode` already exists in `src/draw/png.c` with its own RFC 1951 inflate, and
`audiodec` covers hardware AAC and MP3 - so **Vorbis is a real gap** and, `glut_font.c` being an
8x8 bitmap font, **TrueType is a real gap**. Neither is ours to write either.

## What this rules out

**Hand-written facades as a category.** A third-party API that upstream maintains is pinned and
patched. Writing our own is now the exceptional answer and needs a reason in this log.

**Stub bodies, still.** D009 applies unchanged and applies hardest here, because a backend is
full of calls a console has no answer for. `SetWindowTitle` has nothing to set and **legitimately
does nothing** - that is the honest implementation of a request satisfied by ignoring it.
`GL_CreateContext` returning a plausible handle for a context that does not exist is not. The
test is D009's: would a caller that believes the call worked draw the wrong picture, or no
different a picture.

**Reimplementation-by-drift.** The backend implements upstream's interface. It does not quietly
grow a second copy of something `src/` already has, and when it wants one it says so here.
