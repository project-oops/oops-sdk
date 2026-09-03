# Changelog

oops-sdk publishes **no artifact**. It is consumed as a sibling checkout by the payloads in
the collection, which link the archive they built from it - so there is no version and no
release: the commit a consumer was built against is the only version that means anything.

Entries are grouped **Added / Changed / Fixed**, newest first.

Nothing has shipped yet - this is the initial commit.

## [unreleased] - as of 2026-09-03

### Added

- **A repository for what target-side payloads share.** Display, input, audio, direct
  memory, system, time, threads and sockets behind one set of headers, built as a static
  archive a payload links. obSCEne is the first consumer.
- **Two display backends and a host one.** `agc` for Prospero-generation hardware, `gnm` for
  Orbis-generation, and an in-memory buffer so the interface can be exercised with no
  hardware in the room.
- **`oops-sdk.mk`**, so a consumer says where this repository is and gets the include flags,
  the archive path and the sources from one include.

### Fixed

- **The README described a repository that did not exist.** It documented a `lib/` directory
  holding a vendor tiler archive, a `.cpp` source that is a `.c`, one subsystem of eight, and
  two integration variables - `OOPS_SDK_INC` and `OOPS_SDK_LDFLAGS` - that the makefile
  helper does not define. Make does not warn on an undefined variable, so following the
  README produced empty include and link flags **silently**.
- **`.gitignore` carried a `!lib/*.a` negation** whose only effect would have been to let a
  vendor archive be committed. Removed, along with the documented slot for one.
