# Decisions

Numbered, with reasoning, as they are made. The reasoning is the point - it is what stops a
choice being re-litigated by somebody who only has the choice.

**This log starts late.** The repository existed, built and had a consumer before it had a
decision log, so the choices behind its shape - why a separate repository rather than a
directory inside obSCEne, why the display backends split the way they do - are not recorded
below. They belong to whoever made them, and an entry written by somebody reconstructing the
reasoning afterwards is a guess with a number on it. What is here from D001 onward is
decisions made with the reasoning to hand.

**This table is generated.** Edit an entry under `decisions/`, then run
`tools/split-decisions.sh --index oops-sdk`. A number resolves to exactly one file.

| | # | decision | status | date |
|---|---|---|---|---|
| 🟢 | D001 | [A verb with nothing behind it fails, rather than passing](decisions/D001-a-verb-with-nothing-behind-it-fails.md) | decided | 2026-09-03 |
| 🟢 | D002 | [There is no slot for a vendor archive](decisions/D002-there-is-no-slot-for-a-vendor-archive.md) | decided | 2026-09-03 |
| 🟢 | D003 | [Source inclusion over static archives for freestanding consumers](decisions/D003-source-inclusion-over-static-archives-for-freestanding-consumers.md) | decided | 2026-09-04 |
| 🟢 | D004 | [Privilege escalation, kernel offsets provenance, and explicit opt-in](decisions/D004-privilege-escalation-and-kernel-offsets-provenance.md) | decided | 2026-09-04 |
| 🟢 | D005 | [Three NGG facts first seen in a decrypted module are kept, cited to the open sources](decisions/D005-three-ngg-facts-kept-cited-to-open-sources.md) | decided | 2026-09-14 |
| 🟢 | D006 | [Dynamic code generation and JIT memory interface](decisions/D006-dynamic-code-generation-and-jit-memory-interface.md) | decided | 2026-09-14 |
| 🟢 | D007 | [oops-gl is an instrument scoped to fixed-function, and oops-mesa is the GL anyone runs](decisions/D007-oops-gl-is-an-instrument-scoped-to-fixed.md) | decided | 2026-09-16 |
| 🟢 | D008 | [oops-gl grows to OpenGL 1 and 2, superseding D007's instrument-only scope](decisions/D008-oops-gl-grows-to-opengl-1-and-2-superseding.md) | decided | 2026-09-17 |
| 🟢 | D009 | [An absent feature is an absent symbol, not a function that refuses](decisions/D009-an-absent-feature-is-an-absent-symbol.md) | decided | 2026-09-17 |
| 🟢 | D010 | [oops-sdl is a backend inside upstream SDL, not an implementation of SDL](decisions/D010-oops-sdl-is-a-backend-inside-upstream.md) | decided | 2026-09-21 |
| 🟢 | D011 | [The display names its scanout buffers once, and they must be plain](decisions/D011-the-display-names-its-scanout-buffers-once.md) | decided | 2026-09-21 |
| 🟡 | D012 | [One renderer API, and the display open moves inside it](decisions/D012-one-renderer-api-and-the-display-open-moves-inside-it.md) | proposed | 2026-09-21 |

| | meaning |
|---|---|
| 🟢 | settled, and the reasoning rests on something checkable |
| 🟡 | assumed or proposed - made without input, and in the review queue |
| 🔴 | reversed, superseded or blocked |
| ⚪ | no status recorded |

A date with `~` is **not recorded** - it is worked out from the dated entries either
side, because an entry between two of them was written between their dates. `~` alone
is a day both neighbours agree on; `~a..b` is a span, and no day inside it is claimed;
`~>a` and `~<a` are entries with a dated neighbour on only one side. A bare `-` has no
dated entry either side to reason from.
