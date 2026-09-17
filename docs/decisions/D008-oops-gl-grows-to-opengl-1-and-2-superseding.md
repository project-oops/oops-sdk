# D008 - oops-gl grows to OpenGL 1 and 2, superseding D007's instrument-only scope

**Status:** decided
**Date:** 2026-09-17

## The choice

**oops-gl is to support OpenGL 1.x and 2.x.** The operator set that goal on 2026-09-17.

This supersedes D007, written the previous evening, which scoped oops-gl to "OpenGL 1.1-class
fixed function plus whatever individual later calls an oracle program demands" and listed
*"growing oops-gl toward 1.3, 2.x or GLES"* under **what this rules out**. That entry is
superseded rather than deleted: its reasoning is still the reason the *instrument* role exists,
and the oracle records it describes remain this repository's hardware tests.

## What changes, and what does not

**Changes.** A missing entry point is now a gap to fill rather than a boundary to point at. D007
said a new entry point needs an oracle program that wants it and that "for completeness" was not
a reason; under this decision completeness against GL 1.x and then 2.x *is* the reason.

**Does not change.** Three things from D007 survive intact, because the operator's goal does not
touch them:

- **The refusal discipline.** A call whose behaviour is not known is still refused with an error
  rather than approximated. The whole of 2026-09-16's work was converting silent wrong answers
  into loud refusals, and a growth target is not a licence to guess - a wrong conversion still
  renders a plausible frame and fails nowhere.
- **`glIsHardwareAccelerated` stays measured**, not declared.
- **oops-mesa still exists and still targets 3.3.** Two implementations at different levels is
  not a contradiction: Mesa is upstream's GL for applications that want the modern core profile,
  and oops-gl is ours, small, and readable as a command stream. They still never link into one
  title.

## What this makes hard, stated now rather than discovered later

**OpenGL 2.0 means GLSL**, and GLSL means a compiler: `glCreateShader`, `glShaderSource`,
`glCompileShader`, `glLinkProgram`, `glUseProgram`, uniforms and vertex attributes, with a
front-end that turns GLSL source into the RDNA2 bytecode this already emits by hand. That is
the largest single piece of work in this repository by a wide margin, and it is the reason D007
pointed at Mesa in the first place.

The order that follows from it: **finish 1.x first**, where the pipeline is fixed-function and
the work is ordinary, and reach 2.0's programmable pipeline last. Nothing about 1.x is wasted
when GLSL arrives - a GL 2.0 context still has to answer every 1.x call.

## Where 1.x stands as this is written

Of the 87 declared entry points, the surface is immediate mode, vertex arrays, one 2D texture
unit, lighting, matrix stacks and blend/depth/cull. Absent, all of them ordinary GL 1.1: display
lists, alpha test, fog, stencil, `glReadPixels`, `glTexSubImage2D`, `glPixelStorei`,
`glPolygonOffset`, `glDepthRange`, `glPolygonMode`, and points and lines.

Points and lines are the one group with a hardware block in front of them: they are measured not
to draw through the NGG passthrough path this uses, across four sweeps, and need a
non-passthrough stage (obSCEne `REQ-20260916T2223Z-b6d4`). Everything else on that list is
software and reachable now.
