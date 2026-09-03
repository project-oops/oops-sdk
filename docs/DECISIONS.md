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
