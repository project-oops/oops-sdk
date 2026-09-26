# D005 - Three NGG facts first seen in a decrypted module are kept, cited to the open sources


**Status:** decided
**Date:** 2026-09-14

## What happened

On 2026-09-14, while the GL cube's vertex shader still launched no pixel waves,
`/system/sys/AgcCompositor.elf` was pulled from the console and its NGG vertex shader was run
through `llvm-mc --disassemble`. A module read from a jailbroken console's filesystem is decrypted
material, and disassembling it is what the OOPS conventions (section 1) forbid. Three facts in
`src/gl/gl_context.c`'s vertex shader trace to that reading:

1. an `s_waitcnt expcnt(0)` between the primitive export and the change of the exec mask, without
   which no pixel wave ever launched;
2. the primitive export word `0x20280600`: vertex indices at bits 0, 10 and 20, edge flags set;
3. `GS_ALLOC_REQ` with m0 holding the vertex count in the low bits and the primitive count
   shifted by twelve.

The pulled file was deleted from the session's scratch directory the same day; it was never in a
repository. Worklog 539 in orbistoun records the incident in full.

## The choice

The three facts stay in the code, and their citation is the open-source AMD graphics stack and
the public ISA reference, not the module: Mesa's NGG lowering (`ac_nir_lower_ngg`) emits exactly
this sequence for every RDNA primitive shader, and the RDNA ISA reference states the ordering
rules for `s_sendmsg` and the export counters. The shader comment and `ACKNOWLEDGEMENTS.md` say
so. Decided by the owner, with the alternative (strip the three facts and re-derive them from the
open sources first) on the table.

## Why

- **The facts are not the module's.** Each is a property of the hardware's primitive-shader
  protocol that the open-source driver documents for the same generation, and each was then
  confirmed on the console by the cube rendering. Nothing Sony-specific was learned; what the
  reading supplied was the *order in which to look*, and that order is already public.
- **Re-deriving would produce the same three lines** with a citation to the same open sources,
  at the cost of a day on the console. The distributable artefact would be identical.
- **The record is what matters.** The conventions' boundary exists so that a reader can tell
  where every fact came from. This entry, the worklog and the acknowledgement give that reader
  the whole chain: what was read, what came of it, and what it is now cited to.

## What this does not settle

Older comments in `src/agc/agc_draw.c` (the depth block "aligned with AgcCompositor.elf") and
`src/gl/gl_draw.c` name the same module for a register list that predates this record. Their
origin was not established today; whether that list came from a runtime measurement (obSCEne's
`166-agc/primitive-draw` reads register state from a running console, which is `runtime`
evidence) or from reading the file is worth the same check before the next release.
