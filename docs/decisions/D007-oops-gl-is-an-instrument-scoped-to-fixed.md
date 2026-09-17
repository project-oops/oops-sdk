# D007 - oops-gl is an instrument scoped to fixed-function, and oops-mesa is the GL anyone runs

**decided** · 2026-09-16

### Context

`oops-gl` (`src/gl/`, `include/GL/`) has never had its scope written down. The only statement of
what it is has been a line in `docs/API_REFERENCE.md` calling it *"a clean-room,
hardware-accelerated OpenGL 1.3 / GLES 1.1 fixed-function translation profile"*, repeated as
"OpenGL 1.3" in `README.md` twice and in `docs/README.md` once.

That number was never true and is not the intent. Measured against the tree on 2026-09-16, the
subsystem declares and defines **86 entry points**, and the shape of them is OpenGL 1.1:
immediate mode, vertex arrays, one 2D texture unit, lighting and materials, the matrix stacks,
and blend/depth/cull state. Nothing that distinguishes 1.3 from 1.1 is present - no
`glActiveTexture`, no `glMultiTexCoord*`, no `glClientActiveTexture`, no
`glCompressedTexImage2D`, no `glSampleCoverage`, no transpose-matrix calls. Neither is most of
1.1 outside the cube's path: no stencil, fog, alpha test, display lists, `glReadPixels`,
`glTexSubImage2D`, `glPixelStorei`, `glPolygonOffset`, `glDepthRange`, `glClipPlane` or
`glPolygonMode`. The "GLES 1.1" half is wrong in kind rather than degree: GLES 1.1 has no
`glBegin`, and this is built around it.

Two calls do come from later versions - `glBlendEquation` and `glBlendFuncSeparate` - because the
cube needed them. So the surface is not cleanly any version, which is the reason this entry states
a boundary instead of a number.

### The decision

**oops-gl is a fixed-function measuring instrument, scoped to what the oracle programs need, and
it is not a GL version.** It stops at OpenGL 1.1-class fixed function plus whatever individual
later calls an oracle program demands, each added when a program demands it.

**oops-mesa is the OpenGL anyone actually runs.** It provides OpenGL 3.3 Core and GLSL 3.30
through upstream Mesa and radeonsi, and its own D001 already states the division from its side:
*"This repository supersedes oops-gl for anything a user runs. oops-gl stays in oops-sdk as the
fixed-function measuring instrument it is, and its oracle records become this project's hardware
tests. The two never link into the same title."* This entry is that division recorded from this
side, so a reader of either repository finds it.

### Why the instrument is worth having at all, given Mesa

Because it is the only GL in the collection whose whole command stream is ours and is therefore
**readable as evidence**. `docs/hardware/agc-gl-cube-oracle-fw1240.md` is 1052 dwords of PM4 whose
every register write this repository emitted on purpose, with two pixel hashes stable across 116
and 60 consecutive frames and across separate launches. That record is what orbistoun checks its
RDNA2 translation against, and what oops-mesa measured its radeonsi route against. A stack with
Mesa underneath could not produce it: the point is the absence of anything between the GL call and
the packet.

This also sets what "finished" means. The instrument is finished when the programs that produce
oracle records run and their records are reproducible - not when a conformance suite passes, which
is oops-mesa's unit 8 and not a goal here.

### What this rules out

- **Growing oops-gl toward 1.3, 2.x or GLES.** Anything a real application wants goes to
  oops-mesa. A request to add multitexturing or shaders here is answered with that pointer.
- **Linking both into one title.** Restated from oops-mesa D001 because it is the failure mode
  that would otherwise look reasonable: two GL implementations exporting the same symbols.
- **Treating the entry-point count as coverage.** It is not: 22 of the 86 are exercised by
  neither the oracle program nor the unit tests as of this entry, `glIsHardwareAccelerated` and
  `glDrawElements` among them. Declared surface that nothing runs is the thing D001 of this
  repository already refuses one level down, and closing that gap is ordinary work rather than
  scope growth.

### Consequences

- The four "OpenGL 1.3" claims are corrected in the same change as this entry.
- A new entry point needs a program that wants it. "For completeness" is not a reason here,
  because completeness against a version is explicitly not the goal.
- `glIsHardwareAccelerated()` stays measured rather than declared - it already requires the clear
  test to have passed, the end-of-pipe fence to have returned `0xbeefcafe` and at least one
  confirmed frame. An instrument that reports its own health from the code path it took rather
  than from a measurement is the failure this repository has already had once.
