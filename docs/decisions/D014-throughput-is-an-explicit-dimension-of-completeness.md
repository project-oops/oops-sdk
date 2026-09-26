# D014 - Throughput is an explicit dimension of completeness for oops-gl

**Status:** decided
**Date:** 2026-09-26

Completeness for oops-gl includes throughput alongside API conformance. A pipeline
that renders a correct frame but runs at unplayable frame rates on real geometry is
incomplete.

**Why:** Under conformance-only gates (D008), GL 1.4/1.5 and GL 2.0 probe suites passed,
yet Craft ran at 0.05 fps (150 µs per triangle) because GL 2.0 vertex shaders were
interpreted on the CPU in `glsl_exec.c` and batched geometry was broken into individual
3-vertex draw packets. Compiling vertex shaders to native gfx1030 machine code
(`glsl_vs.c`) and adding a resident vertex buffer draw path in `gl_draw.c` brings triangle
submission to the GPU while retaining `glsl_exec.c` as the reference interpreter.

**Rejected:**
- *CPU-only vertex pipeline optimization:* Cannot bridge a 200x gap when 89% of the frame budget is CPU vertex transform.
- *Full GPU shader compilation without resident buffers:* Interpretive or per-triangle packet emission still bottlenecks on command processor and bus throughput.
- *Replacing glsl_exec.c:* The CPU interpreter is the golden reference for conformance and debug oracle validation.
