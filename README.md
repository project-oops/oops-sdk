# oops-sdk

**Clean-Room Freestanding C Runtime and Hardware Abstraction SDK for Prospero.**

`oops-sdk` is a clean-room, freestanding C runtime (`-ffreestanding -nostdlib`) for developing native homebrew on 8th and 9th generation console hardware (Orbis and Prospero). It exposes direct hardware interfaces for RDNA2 AGC display and tile swizzling, DualSense controller polling, PCM audio streaming, GPU direct memory allocation, and threading—all without relying on proprietary vendor headers or libraries.

| 📖 **[Developer User Guide](docs/USER_GUIDE.md)** | 📚 **[Complete API Reference](docs/API_REFERENCE.md)** | 📐 **[Architecture Decisions](docs/DECISIONS.md)** |
| :--- | :--- | :--- |
| *Step-by-step tutorials from Hello World to 3D graphics, audio, and JIT.* | *Exhaustive technical reference covering 21 subsystems and ~90 functions.* | *Numbered ADRs (D001–D006) capturing provenance, compiler design, and W^X.* |

---

## Role in THE LOOP

Within the [OOPS ecosystem](../docs/THE_LOOP.md), `oops-sdk` is the **Clean-Room Target Runtime**:

```
oops-sdk (Freestanding C Runtime & Hardware Abstraction)
    │
    ├──► oops-apps (Test Titles: gl-cube, wipeout, home, pltauth-patch)
    │        │
    │        ▼
    │    Packaged by SELFish ──► Deployed by Prosperous ──► Tested on PS5
    │                                                        │
    │    ┌───────────────────────────────────────────────────┘
    │    ▼
    └──► obSCEne (Hardware Conformance Probe)
             │
             ▼
         Ground Truth Oracle to Orbistoun
```

1. **Common Target Foundation**: Both consumer applications ([`oops-apps`](../oops-apps/)) and hardware probes ([`obSCEne`](../obscene/)) build on `oops-sdk`.
2. **Zero Proprietary SDK Headers**: Replaces proprietary headers with mathematically verified structures, hardware register layouts, and clean-room freestanding C runtime stubs (`-nostdlib -ffreestanding`).
3. **Fail-Safe Hardware Invariants**: Enforces strict hardware safety invariants (e.g. CPU fallback on fence timeouts, graceful error returns instead of kernel panic) to protect physical console silicon.
4. **First-Class Hardware Features**: Native RDNA2 AGC rasterization and 64 KB micro-tile swizzling, a fixed-function 3D instrument lowering straight to PM4, W^X dual-mapped JIT memory allocation, DualSense polling, and privilege escalation broker.

---

## Developer Quickstart

### 1. Integrate into a Consumer Makefile
`oops-sdk` is consumed via **source inclusion** rather than a prebuilt archive, ensuring all sources compile under the consumer's target flags (`-target x86_64-unknown-freebsd -ffreestanding` or `-target x86_64-scei-ps4`):

```makefile
OOPS_SDK ?= $(abspath ../../oops-sdk)
include $(OOPS_SDK)/oops-sdk.mk

INCLUDE += $(OOPS_SDK_INCLUDE)
MY_SRCS += $(OOPS_SDK_C_SRCS)
```

Alternatively, `make all` builds a static target archive `liboops.a` for standard freestanding linking.

### 2. Run Local Unit Tests
```bash
make test    # compiles and runs headless test stubs on host PC (139 tests)
```

---

## Supported Subsystems

Detailed signatures, parameters, return codes, hardware invariants, and code examples are documented in **[docs/API_REFERENCE.md](docs/API_REFERENCE.md)**. Include individual subsystem headers or the unified header `<oops/oops.h>`:

| Subsystem | Header | Key APIs | Capabilities & Hardware Invariants |
|---|---|---|---|
| **[Display & Framebuffer](docs/API_REFERENCE.md#1-display--video-output-oopsdisplayh)** | `<oops/display.h>` | `oops_display_open`, `flip`, `close` | Direct video memory scanning, hardware vsync flip, double buffering, host SDL2/headless fallback |
| **[2D Software Rendering](docs/API_REFERENCE.md#2-2d-software-drawing-canvas-oopsdrawh)** | `<oops/draw.h>` | `oops_draw_clear`, `rect`, `text` | Software rasterizer, 8x8 font rendering, clipping rectangles, RGBA/BGRA blend modes |
| **[Fixed-Function 3D Instrument](docs/API_REFERENCE.md#3-fixed-function-3d-instrument-glglh)** | `<GL/gl.h>`, `<oops/gl.h>` | `glBegin`, `glVertex3f`, `glLoadIdentity` | OpenGL 1.1-class fixed function, lowering directly to AGC PM4 so the stream reads as evidence. **Not** the GL for applications - that is [oops-mesa](../oops-mesa/), which gives OpenGL 3.3 (D007) |
| **[Hardware AGC & Tiler](docs/API_REFERENCE.md#4-hardware-rdna2-agc-graphics-oopsagch-oopsgpuh)** | `<oops/agc.h>`, `<agc/tiler.h>` | `oops_agc_init`, `queue_submit`, `swizzle` | Direct RDNA2 universal queue submit, PM4 packets, fence synchronization, 64 KB micro-tile morton swizzle |
| **[Direct Physical Memory](docs/API_REFERENCE.md#5-memory-management--direct-memory-oopsmemoryh)** | `<oops/memory.h>` | `oops_mem_alloc`, `map_dmem`, `free` | Direct physical memory mapping, Onion (coherent CPU/GPU) and Garlic (high-speed GPU) bus management |
| **[Dynamic Code Gen (JIT)](docs/API_REFERENCE.md#6-jit--dynamic-executable-memory-oopsjith)** | `<oops/jit.h>` | `oops_jit_alloc`, `flush_icache`, `free` | W^X-compliant dual-mapped pages (`rx_addr` execution / `rw_addr` write), auto-fallback to `mprotect_fix` |
| **[Controller & Input](docs/API_REFERENCE.md#7-controller--input-devices-oopsinputh-oopskeyboardh-oopsmouseh)** | `<oops/input.h>` | `oops_input_init`, `poll`, `rumble` | DualSense controller polling (buttons, analog sticks, adaptive triggers, haptics), keyboard & mouse |
| **[PCM Audio Output](docs/API_REFERENCE.md#8-audio-streaming-oopsaudioh)** | `<oops/audio.h>` | `oops_audio_init`, `submit_stereo` | Multi-channel PCM audio streaming (48 kHz 16-bit stereo), hardware port volume control |
| **[Hardware Media Codecs](docs/API_REFERENCE.md#9-hardware-media-codecs-oopsaudiodech-oopsvideodech)** | `<oops/videodec.h>`, `<oops/audiodec.h>` | `oops_videodec_create`, `audiodec_create` | VPU hardware-accelerated H.264/HEVC video decoding, DSP MP3/AAC audio decompression |
| **[Dialogs & IME Keyboard](docs/API_REFERENCE.md#10-system-ui-dialogs--on-screen-ime-oopsdialogh)** | `<oops/dialog.h>` | `oops_dialog_ime_open`, `poll`, `get_text` | System OS virtual keyboard dialog, UTF-8 text entry, asynchronous user confirmation |
| **[Save Data Management](docs/API_REFERENCE.md#11-save-data-management-oopssavedatah)** | `<oops/savedata.h>` | `oops_savedata_mount`, `unmount` | Title save data directory mounting, encrypted partition access, quota management |
| **[Package Management](docs/API_REFERENCE.md#12-package-management-oopspkgh)** | `<oops/pkg.h>` | `oops_pkg_install`, `get_progress` | Background package installer, progress polling, `/data/pkg` installation broker |
| **[Security Escalation](docs/API_REFERENCE.md#13-privilege-escalation--sandbox-escape-oopsescalateh)** | `<oops/escalate.h>` | `oops_escalate_check`, `acquire_root` | Clean-room credential override (`cr_uid 0`), jailbreak escape (`rootvnode`), debug entitlement elevation |
| **[Kernel Read/Write Broker](docs/API_REFERENCE.md#14-kernel-readwrite--syscall-dispatcher-oopskrwh-oopssyscallh)** | `<oops/krw.h>`, `<oops/syscall.h>` | `oops_krw_init`, `kread64`, `kwrite64` | Arbitrary kernel memory primitives, pipe/socket leak or exploit driver broker |
| **[Dynamic Module Loader](docs/API_REFERENCE.md#15-system-modules--dynamic-linking-oopssysmoduleh)** | `<oops/sysmodule.h>` | `oops_sysmodule_load`, `unload` | Dynamic runtime loading of system PRXs (AudioOut, Pad, VideoDec, Ime, NetCtl) |
| **[System Telemetry](docs/API_REFERENCE.md#16-system-information--telemetry-oopssystemh-oopsoffsetsh)** | `<oops/system.h>` | `oops_klog`, `oops_kprintf`, `system_get_info` | Kernel log streaming (`SYS_klog 601`), hardware telemetry, firmware offset tables |
| **[High-Resolution Timing](docs/API_REFERENCE.md#17-high-resolution-timing-oopstimeh)** | `<oops/time.h>` | `oops_time_get_ms`, `sleep_ms` | Hardware TSC counter access, microsecond/millisecond intervals, thread sleep |
| **[Multithreading & Sync](docs/API_REFERENCE.md#18-threading--synchronization-oopsthreadh)** | `<oops/thread.h>` | `oops_thread_create`, `mutex_lock` | Native kernel thread creation, affinity binding, priority control, mutexes, condition variables |
| **[Sockets & DNS](docs/API_REFERENCE.md#19-bsd-sockets--network-telemetry-oopsneth-oopsnetctlh)** | `<oops/net.h>`, `<oops/netctl.h>` | `oops_socket`, `connect`, `net_resolve` | POSIX TCP/UDP sockets, clean-room RFC 1035 UDP DNS resolution, NetCtl telemetry |
| **[Freestanding C Runtime](docs/API_REFERENCE.md#20-freestanding-c-runtime-utilities-oopsfreestdh)** | `<oops/freestd.h>` | `obs_strlen`, `oops_snprintf`, `obs_compute_nid` | Freestanding string manipulation, clean-room printf/snprintf formatting, NID hashing |
| **[Process Injection](docs/API_REFERENCE.md#21-process-control--code-injection-oopsinjecth-oopsprocctlh-oopsprocparamh)** | `<oops/inject.h>` | `oops_inject_elf`, `proc_kill` | Dynamic code injection into running processes via kernel thread hijacking / ptrace |
| **[High-Level Filesystem](docs/API_REFERENCE.md#22-high-level-filesystem-subsystem-oopsfsh)** | `<oops/fs.h>` | `oops_fs_open`, `read_all`, `write_all`, `exists` | Clean-room POSIX-compatible filesystem layer, whole-file slurp/dump, file size query |
| **[Userland Heap Allocator](docs/API_REFERENCE.md#23-freestanding-userland-heap-allocator-oopsheaph)** | `<oops/heap.h>` | `oops_malloc`, `free`, `calloc`, `realloc` | Segregated-fit slab allocator backed by anonymous virtual memory (works in Cat 65536 zero-DMEM) |
| **[Freestanding Math & 3D](docs/API_REFERENCE.md#24-freestanding-math--3d-linear-algebra-oopsmathh)** | `<oops/math.h>` | `oops_sinf`, `vec3_normalize`, `mat4_perspective` | Clean-room math library, polynomial trig, 3D vectors and 4x4 matrices matching RDNA2 layout |

---

## Cross-Project Links

- **[Master OOPS Front Door](../README.md)** — Collection overview and building instructions.
- **[The OOPS Loop](../docs/THE_LOOP.md)** — Master ecosystem loop specification.
- **[oops-apps](../oops-apps/)** — Conforming applications built on `oops-sdk`.
- **[obSCEne](../obscene/)** — Hardware conformance probe built on `oops-sdk`.
- **[SELFish](../selfish/)** — Packages `oops-sdk` binaries into title containers.
- **[Prosperous](../prosperous/)** — Deploys `oops-sdk` payloads to physical hardware.
