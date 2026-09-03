# D002 - There is no slot for a vendor archive


**decided** · 2026-09-03

The first version of this repository documented a `lib/` directory holding a vendor tiler
archive, and carried a `!lib/*.a` negation in `.gitignore` whose only possible effect was to
let such a file be committed.

Neither the directory nor the archive ever existed, so nothing leaked. Both are gone anyway,
and `.gitignore` now carries a comment saying why there is no negation - because the
combination is how a vendor binary arrives: a documented place to put one, and a rule that
un-ignores exactly the file that would fill it, in a repository whose gitignore otherwise
excludes every `.a` it builds.

This repository reaches the platform the way the rest of the collection does: **weak
declarations against symbols the platform already exports**, checked before they are called.
It carries no vendor code, and after this there is nowhere for any to land.

Conventions section 1. The rule was already written down; what was missing was that nothing
enforced it here, which is the same shape as the 350 MB of vendor packages that sat
staged-ready in obSCEne because the ban was documented and unguarded.
