# oops-sdk

**Freestanding C runtime and hardware SDK for building native homebrew.**

`oops-sdk` is a clean-room, freestanding C runtime (`-ffreestanding -nostdlib`) for developing
native homebrew on the Orbis and Prospero consoles. It gives applications direct access to the
platform's display, input, audio, memory, timing and more — without any proprietary vendor
headers or libraries.

| 🚢 **[Porting Guide](docs/PORTING.md)** | 📖 **[User Guide](docs/USER_GUIDE.md)** | 📚 **[API Reference](docs/API_REFERENCE.md)** | 📐 **[Decisions](docs/DECISIONS.md)** |
| :--- | :--- | :--- | :--- |
| *Porting existing games and engines: SDL2, OpenGL, libc stubs, save data.* | *Step-by-step tutorials, from Hello World to 3D graphics and audio.* | *Every subsystem header, with signatures, return codes and examples.* | *Numbered design decisions and their reasoning.* |

---

## Role in THE LOOP

Within the [OOPS ecosystem](../docs/THE_LOOP.md), `oops-sdk` is the **clean-room target
runtime**: the common foundation that both the test applications and the hardware probe are
built on.

```
oops-sdk ──► oops-apps ─┐
        └──► obSCEne  ──┴─► packaged by SELFish ──► run on hardware / in Orbistoun
```

- **One target foundation.** Both consumer applications ([oops-apps](../oops-apps/)) and the hardware probe ([obSCEne](../obscene/)) build on `oops-sdk`.
- **No proprietary headers.** Platform structures and register layouts are reconstructed from public documentation and hardware measurement, never a vendor SDK.
- **Hardware-safe by default.** The runtime favours graceful error returns over anything that could put physical console silicon at risk.

---

## Developer Quickstart

### Integrate into a Makefile
`oops-sdk` is consumed by source inclusion, so its sources compile under the consumer's own
target flags:

```makefile
OOPS_SDK ?= $(abspath ../../oops-sdk)
include $(OOPS_SDK)/oops-sdk.mk

INCLUDE += $(OOPS_SDK_INCLUDE)
MY_SRCS += $(OOPS_SDK_C_SRCS)
```

### Run the unit tests
```bash
make test    # builds and runs the host-side test suite
```

---

## Choosing a graphics stack

Picking a rendering path — fixed-function GL, programmable GL, desktop-class OpenGL over Mesa,
2D software drawing, or SDL2 — is the main architectural choice when porting an application. The
trade-offs, with recipes and the ported dependency libraries (`zlib`, `libpng`, `freetype` and
the rest), are laid out in the **[Porting Guide](docs/PORTING.md)**.

## The subsystems

`oops-sdk` covers display, 2D drawing, fixed-function 3D, audio, input, memory, timing,
threading, filesystem, networking and save data, among others. Include the unified header
`<oops/oops.h>`, or an individual subsystem header. Every subsystem — its APIs, return codes and
hardware notes — is documented in the **[API Reference](docs/API_REFERENCE.md)** and indexed in
**[API_INDEX.md](docs/API_INDEX.md)**.

---

## Cross-Project Links

- **[Master OOPS Front Door](../README.md)** — Collection overview and building instructions.
- **[The OOPS Loop](../docs/THE_LOOP.md)** — Master ecosystem loop specification.
- **[oops-apps](../oops-apps/)** — Conforming applications built on `oops-sdk`.
- **[obSCEne](../obscene/)** — Hardware conformance probe built on `oops-sdk`.
- **[SELFish](../selfish/)** — Packages `oops-sdk` binaries into title containers.
- **[Prosperous](../prosperous/)** — Deploys `oops-sdk` payloads to physical hardware.
