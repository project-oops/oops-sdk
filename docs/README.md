# oops-sdk documentation

What target-side payloads in [OOPS](https://github.com/project-oops/OOPS) share: the display,
the controller, sound, memory the GPU can see, the clock, threads and sockets - behind one
set of headers, in freestanding C.

Not one of the four. This is infrastructure underneath them, and a repository rather than a
project. Its Rust counterpart is
[oops-libs](https://github.com/project-oops/oops-libs), which is what the **host-side tools**
share; nothing links both.

New here? The [root README](../README.md) has the subsystems, the two display backends, and
how a consumer wires it into a Makefile.

## Guide

- **[BUILDING.md](BUILDING.md)** - `bin/oops-sdk`, what each verb does, why the build is a
  cross-compile even on Linux, and what a consumer has to do to link the result.

## Project memory

- [DECISIONS.md](DECISIONS.md) - a generated index over `decisions/`, one file per entry.
  Every non-obvious choice, numbered, with the reasoning.

Shared rules - provenance, naming, decision logs, honest failure, gates - are in
[the OOPS conventions](https://github.com/project-oops/OOPS/blob/main/docs/CONVENTIONS.md) and
not restated here.

## The words

Vocabulary is the collection's, not this repository's. Nothing here defines a term of its own:

- [the collection's glossary](https://github.com/project-oops/OOPS/blob/main/docs/GLOSSARY.md) - standard ELF, `DT_`/`PT_`, and the cross-repository word collisions
- [SELFish](https://github.com/project-oops/SELFish/blob/main/docs/GLOSSARY.md) - NID, fSELF, PFS, packages, the generation split
- [obSCEne](https://github.com/project-oops/obSCEne/blob/main/docs/GLOSSARY.md) - checks, the census, `ps4_mode` against native

**payload**, **target** and **host** are defined for all repositories in
[CONVENTIONS.md section 2](https://github.com/project-oops/OOPS/blob/main/docs/CONVENTIONS.md#the-words-for-our-own-layers)
and are not repeated here. They are the three words this repository turns on: it is built for
the **target**, linked into a **payload**, and its `host` display backend is the one that runs
nowhere near either.

## Adding to a log

The long-running documents are **directories with a generated index**. Add a file under
`decisions/`, then regenerate the table:

```bash
tools/split-decisions.sh --index oops-sdk
```

Do not edit the index by hand - it is overwritten. The split exists because two sessions
appending to one file collide, and because a log past half a megabyte stops rendering on
GitHub entirely.
