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

**`-Werror` is the check.** There is no test suite, so a clean compile of every source is the
whole of what `check` verifies. That is a real gate rather than a formality - the eleven
sources build with no warnings today, and a new one that does not is a failure rather than a
line of output somebody scrolls past.

## The verbs

| verb | what it does |
|---|---|
| `build` | `make all` - the objects, then `liboops.a` |
| `check` | `make clean` then `make all`, so the result is a full compile rather than whatever was already built |
| `clean` | `make clean` |

`test`, `lint`, `fmt` and `doc` **fail loudly** rather than exiting 0. Nothing implements
them, and a verb that runs nothing and reports success would make `oops all` call this
repository green without having checked it - the failure
[conventions section 3](https://github.com/project-oops/OOPS/blob/main/docs/CONVENTIONS.md#3-honest-failure-over-plausible-output)
exists to prevent. Running one prints what would have to be built first.

## What a consumer has to do

A payload does not check this repository out and copy files. It includes the makefile helper,
which works out its own location:

```makefile
OOPS_SDK ?= $(abspath ../oops-sdk)
include $(OOPS_SDK)/oops-sdk.mk

INCLUDE += $(OOPS_SDK_INCLUDE)

# Build the archive on demand rather than requiring a separate step.
$(OOPS_SDK_LIB):
	@$(MAKE) -C $(OOPS_SDK)

mypayload: $(MY_OBJS) $(OOPS_SDK_LIB)
	$(CC) $(LINK_FLAGS) -o $@ $(MY_OBJS) $(OOPS_SDK_LIBS)
```

Four variables, and the helper defines no others: `OOPS_SDK_INCLUDE`, `OOPS_SDK_LIB`,
`OOPS_SDK_LIBS`, `OOPS_SDK_C_SRCS`. **Make does not warn about an undefined variable** - it
expands to nothing and the compile carries on without the include path - so a typo here fails
as a missing header several steps later rather than at the line that caused it.

obSCEne's `Makefile` is the worked example, and links the archive into both the module and the
eboot.

## From the collection

Once this repository is registered with the collection,
[OOPS](https://github.com/project-oops/OOPS) reaches it the same way it reaches every other:

```bash
./bin/oops check oops-sdk
./bin/oops all                  # the meta gates, then every project's own gate
```

[The collection's BUILDING.md](https://github.com/project-oops/OOPS/blob/main/docs/BUILDING.md)
covers `bootstrap`, `gates`, `all` and the rest.
