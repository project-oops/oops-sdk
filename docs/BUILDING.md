# Building oops-sdk

There is one command and it is `bin/oops-sdk`.

```bash
./bin/oops-sdk check
```

## What you need

**`clang`, `ar` and `make`, and nothing else.** oops-sdk is the bottom of the target-side
stack - it depends on no sibling checkout, and a clone of only this repository builds.

**On Windows, build it under WSL.** Not a preference: the toolchain has to cross-compile to a
FreeBSD-derived target, and the Windows shell has no `clang` on `PATH` here. A native
`make` gets as far as the first compile and stops with `CreateProcess ... failed`, which
looks like a broken makefile and is not.

## Why the build is a cross-compile even on Linux

Nothing this repository produces runs on the machine that built it. Every object is compiled
for the target:

| flag | what it is for |
|---|---|
| `-target x86_64-unknown-freebsd` | the platform's ABI. Same architecture as the build host, different operating system - which is exactly the case that gets forgotten, because the code compiles without it and links against the wrong libc |
| `-ffreestanding`, `-fno-builtin` | there is no hosted C environment on the other side. The compiler must not assume a `printf` exists, and must not rewrite a loop into a `memcpy` that nothing provides |
| `-nostdlib` | the payload links against the platform's own libraries, resolved at load time |
| `-fPIC` | the result is mapped wherever the loader puts it |
| `-fno-stack-protector` | the guard symbols come from a runtime that is not linked |
| `-std=c11` | the language, stated rather than inherited from whatever clang defaults to this year |
| `-Wall -Wextra -Werror` | see below |

**`-Werror` is the first gate.** A clean compile of every source, at `-Wall -Wextra -Werror`,
is a real check rather than a formality: the sources build with no warnings today, and a new
one that does not is a failure rather than a line of output somebody scrolls past.

**There is now a second gate: a host test suite.** `make test` builds the subsystems for the
build machine (not the target) and runs unit and integration tests against them - the seam the
`host` display backend exists to make possible. A subsystem that cross-compiles but computes
the wrong answer is caught here rather than on hardware.

## The verbs

| verb | what it does |
|---|---|
| `build` | `make all` - the objects, then the `liboops.a` archive that proves they link |
| `test` | `make test` - the host unit and integration suite |
| `check` | a clean build at `-Werror`; run before anything is called done |
| `clean` | `make clean` |

`lint`, `fmt` and `doc` **fail loudly** rather than exiting 0. Nothing implements them yet, and
a verb that runs nothing and reports success would make `oops all` call this repository green
without having checked it - the failure
[conventions section 3](https://github.com/project-oops/OOPS/blob/main/docs/CONVENTIONS.md#3-honest-failure-over-plausible-output)
exists to prevent. Running one prints what would have to be built first.

## What a consumer has to do: Source Inclusion (`.mk`)

A payload or application includes the makefile helper, which provides `OOPS_SDK_INCLUDE` and `OOPS_SDK_C_SRCS`:

```makefile
OOPS_SDK ?= $(abspath ../oops-sdk)
include $(OOPS_SDK)/oops-sdk.mk

CFLAGS += $(OOPS_SDK_INCLUDE)

mypayload: $(MY_OBJS) $(OOPS_SDK_C_SRCS)
	$(CC) $(CFLAGS) $(LINK_FLAGS) -o $@ $(MY_OBJS) $(OOPS_SDK_C_SRCS)
```

### Why Source Inclusion over Static Archives (`.a`)
1. **Freestanding Flag-Matching**: The consumer's codegen flags (`-target`, `-ffreestanding`, `-fno-builtin`, `-fno-stack-protector`, `-fPIC`, optimization levels) are authoritative across the entire codebase. A `.a` freezes them at SDK build time, risking ABI drift and subtle runtime breakage.
2. **Weak-Symbol Safety**: Every platform entry point in `oops-sdk` is `__attribute__((weak))` and dynamically checked. Compiling sources directly ensures the linker does not drop weak members during archive symbol resolution.
3. **Dead-Code Stripping**: The consumer retains full dead-code stripping via `-ffunction-sections -fdata-sections -Wl,--gc-sections`.

The `Makefile`'s `all` target builds `liboops.a` strictly as a local compile gate (`-Werror`) for `./bin/oops-sdk check`, while consumers link directly via `$(OOPS_SDK_C_SRCS)`.

## From the collection

Once this repository is registered with the collection,
[OOPS](https://github.com/project-oops/OOPS) reaches it the same way it reaches every other:

```bash
./bin/oops check oops-sdk
./bin/oops all                  # the meta gates, then every project's own gate
```

[The collection's BUILDING.md](https://github.com/project-oops/OOPS/blob/main/docs/BUILDING.md)
covers `bootstrap`, `gates`, `all` and the rest.
