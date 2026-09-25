# OOPS_FEATURES: Declarative Subsystem Capabilities

`OOPS_FEATURES` is the declarative capability interface provided by `oops-sdk` for applications built with `oops-apps/common/app.mk`.

An application declares **what capabilities it requires** (intent), rather than hand-listing SDK source files (implementation):

```makefile
# In your application's Makefile:
OOPS_FEATURES += keyboard net http audio
```

---

## 1. Motivation & Architecture

Historically, application Makefiles hand-listed individual SDK source files in `PAYLOAD_SRCS`:
```makefile
# ANTI-PATTERN: Leaking SDK implementation into applications
PAYLOAD_SRCS = main.c \
               $(OOPS_SDK_DIR)/src/input/input.c \
               $(OOPS_SDK_DIR)/src/input/keyboard.c \
               $(OOPS_SDK_DIR)/src/net/net.c \
               $(OOPS_SDK_DIR)/src/net/netctl.c \
               $(OOPS_SDK_DIR)/src/net/dns.c \
               $(OOPS_SDK_DIR)/src/net/http.c \
               $(OOPS_SDK_DIR)/src/system/zip.c
```

This caused multiple points of friction:
1. **Fragility**: When an SDK subsystem was refactored into additional files (such as `oops-gl` adding shader compiler modules), every application's link broke with undefined symbol errors until their Makefiles were individually updated.
2. **Hidden Symbol Dependencies**: Subsystems depend on platform libraries (`libSceKeyboard`, `libSceHttp`, etc.). If an app forgot to ensure `common/symbols.txt` mapped those imports, `obscene-tool mkmodule` could not tag them for the loader, causing silent runtime failures on hardware.
3. **Leaked Implementation**: Title authors had to know the internal file layout and module structure of the SDK.

### The Single Source of Truth
The SDK owns its source files, their dependencies, and their `libSce*` import claims.
Therefore, `oops-sdk/oops-sdk.mk` defines:
* `OOPS_FEATURE_<name>_SRCS`: The source files implementing capability `<name>`.
* `OOPS_FEATURE_<name>_SYMS`: The required `libSce*` library imports.
* `OOPS_FEATURES_AVAILABLE`: The roster of all supported capabilities.

`oops-apps/common/app.mk` is the consumer front-door:
* Validates requested features against `OOPS_FEATURES_AVAILABLE`.
* Appends `$(OOPS_FEATURE_<f>_SRCS)` to `PAYLOAD_SRCS`.
* Passes `$(OOPS_FEATURE_<f>_SYMS)` to `mkmodule` for module tagging.
* Emits a fatal `$(error ...)` if an unknown feature is requested.

---

## 2. Complete Capabilities Reference

| Feature | Header API | Description | SDK Sources (`OOPS_FEATURE_<f>_SRCS`) | Platform Imports (`_SYMS`) |
|---|---|---|---|---|
| `base` | `<oops/system.h>` | Core runtime (*implicit always-on*) | `system.c`, `freestd.c`, `syscall.c`, `offsets.c`, `fs.c`, `sysmodule.c`, `procparam.c`, `memory.c`, `heap.c`, `time.c` | `libkernel`, `libSceLibcInternal`, `libSceSystemService`, `libSceUserService`, `libSceSysUtil` |
| `display` | `<oops/display.h>` | Video scanout & AGC/GNM command buffers | `src/display.c`, `src/agc/*.c` (or `src/gnm/*.c`) | `libSceVideoOut`, `libSceAgc`, `libSceAgcDriver`, `libSceGnmDriver` |
| `draw` | `<oops/draw.h>` | 2D CPU software drawing canvas | `src/draw/draw.c` | *(none)* |
| `png` | `<oops/draw.h>` | PNG image decompressor | `src/draw/png.c` | *(none)* |
| `hud` | `<oops/hud.h>` | On-screen debug HUD overlay | `src/hud/hud.c` | *(none)* |
| `input` | `<oops/input.h>` | DualSense / DualShock gamepad polling | `src/input/input.c` | `libScePad`, `libSceUserService` |
| `keyboard` | `<oops/keyboard.h>` | USB & Bluetooth keyboard events | `src/input/keyboard.c` | `libSceKeyboard` |
| `mouse` | `<oops/mouse.h>` | USB & Bluetooth mouse polling | `src/input/mouse.c` | `libSceMouse` |
| `audio` | `<oops/audio.h>` | Low-latency PCM audio streaming | `src/audio/audio.c` | `libSceAudioOut` |
| `audiodec` | `<oops/audiodec.h>` | Hardware audio decoding (AT9, AAC, MP3) | `src/audio/audiodec.c` | `libSceAudiodec`, `libSceAjm` |
| `videodec` | `<oops/videodec.h>` | Hardware H.264 / AVC video decoding | `src/videodec/videodec.c` | `libSceVideodec2` |
| `net` | `<oops/net.h>`, `<oops/netctl.h>` | BSD sockets, link status & DNS client | `src/net/net.c`, `src/net/netctl.c`, `src/net/dns.c` | `libSceNet`, `libSceNetCtl`, `libkernel` |
| `dns` | `<oops/dns.h>` | Standalone RFC 1035 DNS resolver | `src/net/dns.c` | `libSceNet` |
| `http` | `<oops/http.h>` | HTTP/HTTPS client & streaming download | `src/net/http.c`, `src/system/zip.c` | `libSceHttp`, `libSceSsl`, `libSceNet` |
| `zip` | `<oops/zip.h>` | Freestanding Deflate & ZIP extractor | `src/system/zip.c` | *(none)* |
| `savedata` | `<oops/savedata.h>` | User save data storage & mounts | `src/system/savedata.c` | `libSceSaveData` |
| `dialog` | `<oops/dialog.h>` | System IME (OSK) & message dialogs | `src/system/dialog.c` | `libSceCommonDialog`, `libSceImeDialog`, `libSceMsgDialog` |
| `thread` | `<oops/thread.h>` | POSIX threads, semaphores & mutexes | `src/thread/thread.c` | `libkernel` |
| `jit` | `<oops/jit.h>` | Dynamic JIT shared memory allocation | `src/system/jit.c` | `libkernel` |
| `escalate` | `<oops/escalate.h>` | Privilege escalation & jailbreak | `src/system/escalate.c` | `libkernel` |
| `inject` | `<oops/krw.h>` | Kernel R/W & target process injection | `src/system/krw.c`, `procctl.c`, `loader.c`, `target.c`, `inject.c` | `libkernel` |
| `pkg` | `<oops/pkg.h>` | Background package installer client | `src/system/pkg.c` | `libSceAppInstUtil` |
| `suspend` | `<oops/system.h>` | System suspend/resume event hooks | `src/system/suspend.c` | `libSceSystemService` |
| `glu` | `<GL/glu.h>` | OpenGL GLU utility matrix/projection | `src/gl/gl_glu.c` | *(none)* |
| `glut` | `<GL/glut.h>` | OpenGL GLUT toolkit & bitmap fonts | `src/gl/glut.c`, `src/gl/glut_font.c` | *(none)* |
| `js` | `<oops/js.h>` | Standalone QuickJS JavaScript engine (ES2020) | `src/js/js.c`, `src/js/quickjs/*.c` | *(none)* |
| `html` | `<oops/html.h>` | Standalone litehtml HTML5/CSS layout & renderer | `src/html/oops_html.cpp`, `src/html/html_container.cpp`, `src/html/litehtml/src/*.cpp`, `src/html/litehtml/src/gumbo/*.c` | *(none)* |
| `webview` | `<oops/webview.h>` | Assembled Webview browser (QuickJS + litehtml + DOM + fetch) | `src/webview/*.cpp` | *(none; depends on `js`, `html`, `http`, `input`)* |

---

## 3. Base Runtime (Implicit Always-On)

Every application links the `base` feature automatically, even if `OOPS_FEATURES` is unset:
* `system.c`: Process lifecycle, firmware detection, klog logging, `/app0/oops-log` channels.
* `freestd.c`: Clean-room libc primitives (`obs_strlen`, `obs_memcpy`, `obs_format_hex`, etc.).
* `syscall.c` & `offsets.c`: Direct system call dispatch and kernel data structures.
* `fs.c`: Sandboxed and persistent filesystem traversal and POSIX file IO.
* `sysmodule.c`: Dynamic library loading (`sceSysmoduleLoadModule`).
* `procparam.c`: Title execution environment and process parameters.
* `memory.c` & `heap.c`: Direct memory (`sceKernelAllocateDirectMemory`) and userland allocator.
* `time.c`: Monotonic high-resolution clock and TSC frequency.
* `libc.c`, `scanf.c`, `math.c`: Freestanding C runtime stubs (for non-Mesa titles).

---

## 4. How to Use in an Application

In `Makefile`:
```makefile
APP_NAME    := my-homebrew
ENTRY_POINT := my_app_start

# Declare capabilities:
OOPS_FEATURES := display draw input keyboard audio

# If using OpenGL, declare renderer:
# OOPS_RENDERER = gl1 | gl2 | mesa

# Only list your own application sources:
PAYLOAD_SRCS := main.c app_logic.c

include $(OOPS_APPS_ROOT)/common/app.mk
```

### Relationship with `OOPS_RENDERER`
* `OOPS_RENDERER = gl1 | gl2` automatically pulls `oops-gl` sources (`OOPS_GL_SRCS`).
* `OOPS_RENDERER = mesa` automatically pulls `oops-mesa` hosted sysroot and libraries.
* Subsystems used alongside graphics (e.g. `keyboard`, `audio`, `savedata`) are declared via `OOPS_FEATURES`.

---

## 5. Adding a New Feature to the SDK

To add a new capability `<feat>`:
1. **Define Sources**: In `oops-sdk/oops-sdk.mk`, define:
   ```makefile
   OOPS_FEATURE_<feat>_SRCS := $(OOPS_SDK_DIR)/src/path/to/source.c
   ```
2. **Define Symbol Claims**: Specify the required platform library imports:
   ```makefile
   OOPS_FEATURE_<feat>_SYMS := libSce<LibraryName>
   ```
3. **Add Symbol Manifest**: Create `oops-sdk/symbols/<feat>.syms` containing the `<library> <symbol>` lines.
4. **Register in Roster**: Append `<feat>` to `OOPS_FEATURES_AVAILABLE` in `oops-sdk.mk`.
5. **Document**: Add the entry to the table in `docs/FEATURES.md` and regenerate the API index (`bash tools/api-index.sh`).
