# oops-sdk

The hardware, behind an interface, in freestanding C.

A homebrew payload on this platform talks to the same handful of subsystems every time -
put a picture on the screen, read a controller, allocate memory the GPU can see, tell the
time. Every project that has done it has written that layer again. This is that layer,
written once.

> **Status: in progress, and honest about which parts.** The display path is developed and
> exercised on hardware, and a few more paths have now run there too - the table below says
> which, and in which context. Everything not in it is a thin wrapper over the platform's
> entry points, written to the shape of the interface rather than to a measured behaviour:
> a declaration of intent that compiles.

### What has run on hardware

Measured by obSCEne's hardware sweeps on firmware 12.40, on prospero (the current-generation PS5 base), 2026-09-08. A
sweep runs three legs: a title launched as an **eboot**, a bare **payload**, and an installed
**pkg** (the app context). Which libraries resolve differs by leg, and that split is a finding
in itself, so each row says where a thing worked. The probe ids are obSCEne's, for the reader
who wants the bytes.

| subsystem | what ran | where | probe |
|---|---|---|---|
| display (agc) | opened, registered two tiled buffers, presented frames | eboot, pkg | obSCEne's own display path |
| input, pad | `oops_input_init` and `oops_input_poll` returned a record cleanly; the driver record is 120 bytes, so the batched read strides on a measured size | eboot, pkg | `100-input/oops-sdk-poll`, `read-extent`, `batched-read` |
| input, adaptive triggers | the effect entry point resolves; its parameter is unconfirmed, so the call still refuses | pkg only | `100-input/dualsense-symbols` |
| audio | `oops_audio_open`, `oops_audio_set_volume`, `oops_audio_write` of 512 frames all returned 0; the stereo format selector, the accepted chunk sizes and rate, and the blocking depth are measured | eboot, pkg | `090-audio/oops-sdk-pcm`, `090-audio/format-selector`, `open-shapes`, `blocking` |
| keyboard, mouse | both libraries and all four entry points resolve; the keyboard record is 96 bytes, fields unconfirmed, so both reads still refuse | pkg only | `101-input-ext` |
| video out | the flip-status record is 64 bytes in the shape the gnm path declares; the submit call queues without blocking, five deep after a burst and drained within 200 ms, so the agc path now drains the queue before reusing a buffer | pkg, eboot | `080-video/flip-status`, `visual-flip` |
| net (payload) | an unsigned payload's weak references to the POSIX socket exports bind at load, and a payload opened, bound, listened, accepted, received and echoed on a socket end to end | payload | `102-net`, `080-video/visual-flip` sweep 20260909-144348 |

Where a row says a read "still refuses", the header for that subsystem says what capture
would open it and the code returns a distinct negative code rather than a plausible zero.

## The subsystems

One header each, all of them behind `<oops/oops.h>` if you want the lot. **Display is the one
with real depth** (below); the rest are thin wrappers over the platform's own entry points -
the controller, an audio output port, direct memory the GPU can address, the clock, threads
and mutexes, and sockets enough for a payload to answer on a port - with more added as
payloads need them.

**`include/` is the authority for the current set**, not this paragraph: the list grows, and a
table here would lag it. Point a newcomer at the headers rather than at prose that goes stale.

### Display, and the two backends

Display is the one subsystem with real depth, because it is the one that had to work first.
`oops_display_open()` takes a backend or `OOPS_DISPLAY_BACKEND_AUTO`:

| backend | header | what it does |
|---|---|---|
| **agc** | `<agc/display.h>`, `<agc/tiler.h>` | Prospero-generation: display handle on bus 0, write-combined direct memory, a batch map into the GPU's address space, and 64 KB hardware tile swizzling |
| **gnm** | `<gnm/display.h>` | Orbis-generation: display handle on bus 0, direct memory at 64 KB page alignment, linear scanout |

Off hardware there is no third backend. `make test` stubs the display getters and drives the
draw canvas and the tiler on plain memory, which is how the interface is exercised without one.

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

INCLUDE += $(OOPS_SDK_INCLUDE)
# Compile the SDK's sources alongside your own - see below for why source, not an archive.
MY_SRCS += $(OOPS_SDK_C_SRCS)
```

The helper defines just these:

| variable | what it is |
|---|---|
| `OOPS_SDK_INCLUDE` | the `-I` flags for `include/` and the repository root |
| `OOPS_SDK_C_SRCS` | the SDK's source files, to compile with your own |

**The consumer compiles the sources; there is no archive.** A prebuilt `.a` would freeze the
SDK's compile flags, and on a freestanding target the SDK and the consumer must agree on the
target triple, `-ffreestanding`, stack-protector and the rest - so the consumer's flags are
made authoritative by compiling the sources under them. It also means a subsystem the consumer
never calls is simply never compiled in. obSCEne is the first consumer and does exactly this -
it compiles `$(OOPS_SDK_C_SRCS)` into its module and eboot objects; its `Makefile` is the
worked example.

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
