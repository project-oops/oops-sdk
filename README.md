# oops-sdk

The hardware, behind an interface, in freestanding C.

A homebrew payload on this platform talks to the same handful of subsystems every time -
put a picture on the screen, read a controller, allocate memory the GPU can see, tell the
time. Every project that has done it has written that layer again. This is that layer,
written once.

> **Status: in progress, and honest about which parts.** The display path is developed and
> exercised on hardware. The other seven subsystems are thin wrappers over the platform
> entry points, written to the shape of the interface rather than to a measured behaviour.
> Treat anything outside display as a declaration of intent that compiles.

## The subsystems

One header each, all of them behind `<oops/oops.h>` if you want the lot.

| header | what it covers |
|---|---|
| **`<oops/display.h>`** | Framebuffer and presentation, with a backend chosen at open time |
| **`<oops/input.h>`** | Controller state, rumble, lightbar |
| **`<oops/audio.h>`** | Opening an output port and writing frames to it |
| **`<oops/memory.h>`** | Direct memory the GPU can address, and mapping it |
| **`<oops/system.h>`** | The initial user, system notifications, what generation this is |
| **`<oops/time.h>`** | Tick counter, frequency, sleeping |
| **`<oops/thread.h>`** | Threads and mutexes over the platform's pthread entry points |
| **`<oops/net.h>`** | Sockets, enough for a payload to answer on a port |

### Display, and the two backends

Display is the one subsystem with real depth, because it is the one that had to work first.
`oops_display_open()` takes a backend or `OOPS_DISPLAY_BACKEND_AUTO`:

| backend | header | what it does |
|---|---|---|
| **agc** | `<agc/display.h>`, `<agc/tiler.h>` | Prospero-generation: display handle on bus 0, write-combined direct memory, a batch map into the GPU's address space, and 64 KB hardware tile swizzling |
| **gnm** | `<gnm/display.h>` | Orbis-generation: display handle on bus 0, direct memory at 64 KB page alignment, linear scanout |
| **host** | - | An in-memory linear buffer, so the interface can be exercised off hardware |

`agc` and `gnm` are the platform's own driver families and are ABI facts rather than brand
names - the same reason obSCEne reports which of them resolves rather than guessing a
generation from it. See
[the OOPS conventions, section 2](https://github.com/project-oops/OOPS/blob/main/docs/CONVENTIONS.md#2-naming-no-vendor-brands-in-prose-or-in-our-own-api).

## Using it

Include the helper from a consumer Makefile. It works out its own location, so the only
thing you have to say is where it is:

```makefile
OOPS_SDK ?= $(abspath ../oops-sdk)
include $(OOPS_SDK)/oops-sdk.mk

INCLUDE  += $(OOPS_SDK_INCLUDE)
LDOBJS   += $(OOPS_SDK_LIBS)
```

The helper defines these and nothing else:

| variable | what it is |
|---|---|
| `OOPS_SDK_INCLUDE` | the `-I` flags for `include/` and the repository root |
| `OOPS_SDK_LIB` | the archive the link needs, as a path - useful as a make prerequisite |
| `OOPS_SDK_LIBS` | what to put on the link line |
| `OOPS_SDK_C_SRCS` | the sources, for a consumer that compiles them rather than linking the archive |

Then a target that builds the archive on demand:

```makefile
$(OOPS_SDK_LIB):
	@$(MAKE) -C $(OOPS_SDK)
```

obSCEne is the first consumer and does exactly this; its `Makefile` is the worked example.

```c
#include <oops/display.h>

oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 1920, 1080);
if (oops_display_is_ready(disp)) {
    oops_display_clear(disp, 0xFF0D1116u);
    oops_display_flip(disp);
}
```

`oops_display_is_ready()` is not decoration. A backend that could not open reports it here
rather than handing back a buffer that goes nowhere, and `oops_display_get_last_error()`
says which step failed.

## Building

The same entry point every OOPS repository carries, so `oops build oops-sdk` and
`./bin/oops-sdk build` are one command reached two ways:

```bash
./bin/oops-sdk build
```

**It builds for the target, not for the machine you are on.** `clang` cross-compiles to
`x86_64-unknown-freebsd`, freestanding, with no standard library - so a Windows checkout
builds it under WSL and not natively. [docs/BUILDING.md](docs/BUILDING.md) has the flags and
what each one is for.

## Licence

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE), at your option.
Consumers link this into what they ship, so it carries the same terms as everything else in
the collection.

## Where it sits

Not one of the four. **OOPS** is Orbistoun, obSCEne, Prosperous and SELFish - four projects
aimed at one console's operating system. This sits underneath them the way
[oops-libs](https://github.com/project-oops/oops-libs) does, and for the same reason: it is a
repository rather than a project.

The split between the two is what each is made of. oops-libs is Rust, and it is what the
**host-side tools** share - build stamps, logging, where a tool writes. oops-sdk is
freestanding C, and it is what **target-side payloads** share. Nothing links both.

Shared rules - provenance, naming, decision logs, honest failure, gates - live in
[the OOPS conventions](https://github.com/project-oops/OOPS/blob/main/docs/CONVENTIONS.md)
and are not restated here.
