# D009 - An absent feature is an absent symbol, not a function that refuses

**Status:** decided
**Date:** 2026-09-17

## The choice

oops-gl refuses arguments it cannot honour. It does **not** declare entry points that exist only
to refuse.

Concretely: `glEnable(GL_FOG)` raises `GL_INVALID_ENUM`, and `glFogf` is not declared at all.

This has been the practice since D007 without being written down, which is how it came to be
re-litigated. It is written down now because the *reason* is not obvious and the wrong choice
looks kinder.

## Why absence is the louder signal

A shim that declares `glPolygonStipple` and refuses it produces a program that **links, runs, and
draws the wrong picture**. Almost nothing checks `glGetError` on a state-setting call, so the
refusal is a message nobody reads. The porter discovers the gap as a rendering bug, some distance
from the call that caused it.

A shim that omits `glPolygonStipple` produces a **link error, at build time, naming the symbol**.
The porter learns the same fact before running anything, in the one form that cannot be ignored.

The kinder-looking option is the one that wastes an afternoon.

## The line, and which side things fall on

The distinction is not "is the feature supported" but **"does this entry point exist for any
other reason"**:

| | |
|---|---|
| **Exists anyway, so it refuses the parts it cannot do** | `glEnable`, `glDisable`, `glIsEnabled`, `glGetIntegerv`, `glHint`, `glTexEnvi`, `glDrawBuffer`, `glPushAttrib`, `glBegin` |
| **Exists only for the absent feature, so it is not declared** | `glFog*`, `glStencil*`, `glAccum*`, `glMap*`/`glEvalCoord*`, `glSelectBuffer`/`glRenderMode`, `glPolygonStipple`, `glLineStipple`, `glBitmap`, `glDrawPixels`, `glTexGen*`, `glIndex*` |

`glEnable` has to exist to enable depth testing, so it is the right place to say that fog does
not exist here. `glFogf` has no other job.

**A feature that is partly present still refuses rather than half-works.** `glBegin(GL_POINTS)`
is refused because points cannot be drawn (measured, `REQ-...-9b71`), and `glBegin` stays.

## What this rules out

**Stub bodies.** An entry point that is declared, does nothing, and returns success is the worst
of the three options and is not used anywhere in oops-gl. If a call cannot do its job it either
raises an error or does not exist.

**Advertising an extension whose entry points are absent.** The same rule one level up: an
extension string is a promise about that extension's own names. `GL_EXTENSIONS` returned
`GL_EXT_vertex_array` while `glVertexPointerEXT` and `glDrawArraysEXT` did not exist; it now
returns an empty list, which is the true statement that no extension's entry points are
provided. Core `glGenBuffers` without `glGenBuffersARB` is not `GL_ARB_vertex_buffer_object`.

## What this costs

A framework that calls an absent entry point unconditionally at start-up will not link, even
where it would have tolerated the feature being missing. That is a real cost and it is accepted:
the alternative is that every such framework gets a silently wrong picture instead, and the
failure moves from the build to the screen.

If a specific entry point turns out to be called unconditionally by something worth porting, the
answer is to **implement it**, not to stub it.
