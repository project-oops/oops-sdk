# D001 - A verb with nothing behind it fails, rather than passing


**decided** · 2026-09-03

Every OOPS repository is reached through `bin/<project>`, and the collection sweeps them with
`oops all`. This repository has a build and nothing else: no test suite, no linter, no
formatter. The obvious way to satisfy the interface is a `test` verb that runs nothing and
exits 0.

It does not. `test`, `lint`, `fmt` and `doc` print what would have to be built first and exit
1.

The reasoning is conventions section 3 applied to the one place it is easiest to break.
`oops all` prints a green line per project, and a verb that succeeds without checking anything
produces exactly that line - so the collection would report this repository as checked, and
the report would be wrong in the direction nobody investigates. A red line saying "there is no
test harness here yet" is worth more than a green one that means nothing, and it stays
uncomfortable until somebody builds the harness, which is the point.

`check` is a clean rebuild of every source at `-Wall -Wextra -Werror`. That is a real gate and
the whole of what is verified today, and `docs/BUILDING.md` says so in those words rather than
implying a suite that does not exist.
