# Porting & Building Homebrew with oops-sdk

A comprehensive, SDK-style guide for software architects, systems engineers, and game developers building native homebrew or porting existing C/C++ codebases to Prospero (9th-generation) and Orbis (8th-generation) console hardware using `oops-sdk`.

---

## Table of Contents

1. [Architecture & Philosophy](#1-architecture--philosophy)
2. [Master Graphics & Framework Decision Matrix](#2-master-graphics--framework-decision-matrix)
3. [Frameworks & Ported Libraries (`oops-deps`)](#3-frameworks--ported-libraries-oops-deps)
4. [POSIX Compatibility Layer (`common/posix/`)](#4-posix-compatibility-layer-commonposix)
5. [Console Filesystem & Persistent Storage Model](#5-console-filesystem--persistent-storage-model)
6. [Core SDK Helpers & Toolbelt](#6-core-sdk-helpers--toolbelt)
7. [Step-by-Step Porting Recipes](#7-step-by-step-porting-recipes)
8. [Deep Dive: The `oops-gl` Hardware Instrument](#8-deep-dive-the-oops-gl-hardware-instrument)
9. [Build System, Packaging & Deployment](#9-build-system-packaging--deployment)
10. [Common Pitfalls & Gotchas](#10-common-pitfalls--gotchas)

---

## 1. Architecture & Philosophy

`oops-sdk` provides a clean-room, freestanding C runtime (`-ffreestanding -nostdlib`) and hardware abstraction layer:

- **Zero Vendor SDK Files**: Replaces proprietary SDK headers and libraries with mathematically verified structures, direct syscall dispatchers, and clean-room libc stubs.
- **Fail-Safe Hardware Invariants**: Protects physical console silicon from fatal GPU rings or kernel panics. All submit queues handle fence timeouts gracefully by failing safe to CPU rasterization, logging telemetry to `klog`, and never locking the command processor.
- **Dual-Target and Host Portability**: Targets Prospero and Orbis natively, while maintaining host test stubs (`make test`, `make check`) that run headless on Linux/Windows for automated validation.
- **First-Class OOPS Toolchain Integration**: Seamlessly integrates with the OOPS collection:
  - **`app.mk`**: Canonical application build gate.
  - **`SELFish`**: Cryptographic container packaging (`eboot.bin`, `param.json`, title layouts).
  - **`Prosperous` (`pros`)**: Deployment, execution, and real-time `klog` telemetry capture over the network.
  - **`obSCEne`**: Dynamic symbol census and conformance verification.

---

## 2. Master Graphics & Framework Decision Matrix

When building or porting an application, choosing the correct graphics stack and runtime framework is the most critical architectural decision. `oops-apps` unifies this choice through a single Makefile flag: `OOPS_RENDERER = gl1 | gl2 | mesa`.

| Technology | API & Standard | Environment | Memory Footprint | Primary Use Cases |
| :--- | :--- | :--- | :--- | :--- |
| **`oops-gl` (`gl1`)** | OpenGL 1.1–1.5 Fixed Function | Pure Freestanding (`-ffreestanding`) | ~100–300 KB | Retro 3D games (Quake, DOOM, Neverball), GLUT demos, lightweight tools, pure freestanding binaries. |
| **`oops-gl` (`gl2`)** | OpenGL 2.0, GLSL 1.10 / 1.20 / ES 1.00 | Pure Freestanding (`-ffreestanding`) | ~300–600 KB | Custom vertex/fragment shaders, render-to-texture, procedural materials, programmable lighting, retro-indie games. |
| **`oops-mesa` (`mesa`)** | OpenGL 3.3 Core, GLES 2/3, EGL | Hosted (`USE_MESA = 1`, FreeBSD libc sysroot) | ~15–30 MB | Modern desktop games, complex game engines, GLSL 330+, geometry/compute shaders, multisampled or depth-attached FBOs. |
| **`oops-draw` (2D Canvas)** | 2D CPU Rasterizer (`<oops/draw.h>`) | Pure Freestanding | Zero extra | Console shells (SeaShell), HUDs, text overlays, simple 2D menus, diagnostics. Zero GPU overhead. |
| **`oops-sdl` (SDL2)** | SDL 2.0.22 Windowing, Events, Audio | Either (Freestanding or Hosted) | ~500 KB | Cross-platform games, emulators, and engines already written against SDL2. Works with `oops-gl` and `oops-mesa`. |

### Choosing Your Stack: A Decision Tree

```
Are you porting an existing codebase?
  │
  ├─► Is it already using SDL2?
  │     └─► USE: oops-sdl (oops-deps/sdl2) + oops-gl or oops-mesa
  │
  ├─► Is it a 2D utility, launcher, or diagnostic menu?
  │     └─► USE: oops-draw (<oops/draw.h>) + oops/display.h (e.g. SeaShell)
  │
  ├─► Is it an OpenGL 1.x or 2.0 game (or GLUT)?
  │     └─► USE: oops-gl (OOPS_RENDERER = gl1 or gl2)
  │           (Freestanding, instant startup, compiles fragment shaders directly to gfx1030)
  │
  └─► Does it require modern OpenGL 3.x/4.x, GLSL 330+, or EGL?
        └─► USE: oops-mesa (OOPS_RENDERER = mesa)
              (Hosted Mesa Gallium/Radv stack with FreeBSD C library sysroot)
```

### Render-to-texture under `gl2`

`oops-gl` draws into a framebuffer object on the console: `glFramebufferRenderbuffer` with a
colour renderbuffer, or `glFramebufferTexture2D` with a texture's base level. Both are measured -
`gl2-probe`'s `fbo/renderbuffer` and `fbo/texture`.

**Ask `glCheckFramebufferStatus` and act on the answer**, which is what a port should do anyway.
Three combinations answer `GL_FRAMEBUFFER_UNSUPPORTED` here rather than drawing something wrong:

- a **depth or stencil attachment**, because the console's depth surface is tiled where an
  attachment is linear. The whole framebuffer is refused rather than the depth attachment being
  quietly ignored, so a port that needs a depth buffer in its off-screen pass wants `oops-mesa`.
- a **texture level above 0**. Only the base level is GPU memory; the mip levels above it are
  built on the heap.
- **multisampling**, which `gl2` does not have.

A pass that renders colour into a texture and samples it afterwards - the common shape for
post-processing and for a mirror or a minimap - needs none of those.

---

## 3. Frameworks & Ported Libraries (`oops-deps`)

To eliminate the friction of porting third-party open-source games and engines, OOPS maintains clean-room port shims and build rules for standard open-source dependencies under `oops-apps/src/oops-deps/`.

### 1. `oops-sdl` (SDL 2.0.22)
Located in `oops-apps/src/oops-deps/sdl2/`.
- **Display Backend (`SDL_prosperovideo.c`)**: Direct scanout framebuffer allocation and vsync presentation.
- **Event & Input Backend (`SDL_prosperoevents.c`)**: Maps DualSense controllers, touchpads, keyboard, and mouse directly into `SDL_Event` queues.
- **Audio Backend (`SDL_prosperoaudio.c`)**: Translates `SDL_OpenAudioDevice` and callbacks into native `<oops/audio.h>` 48 kHz PCM stereo streams.
- **GL Context (`SDL_GL_CreateContext`)**: Bridges SDL2 context creation to `oops-gl` (`glContextCreate`) or `oops-mesa` EGL.

### 2. Available Dependency Libraries
Include their corresponding makefile fragments in your application Makefile:

| Library | Makefile Include | Capabilities | Typical Use |
| :--- | :--- | :--- | :--- |
| **`zlib`** | `src/oops-deps/zlib/oops-zlib.mk` | DEFLATE compression & decompression | Game asset archives, PNG decompression |
| **`libpng`** | `src/oops-deps/libpng/oops-libpng.mk` | PNG image decoding and encoding | Textures, icons, UI sprites |
| **`libjpeg-turbo`**| `src/oops-deps/libjpeg-turbo/oops-libjpeg.mk` | SIMD-accelerated JPEG decompression | Background art, photos, title splash screens |
| **`freetype`** | `src/oops-deps/freetype/oops-freetype.mk` | TrueType and OpenType font rasterizer | Vector typography, multilingual font rendering |
| **`sdl2-ttf`** | `src/oops-deps/sdl2-ttf/oops-sdl2-ttf.mk` | SDL2 TrueType font surface rendering | Text drawing in SDL2 applications |
| **`libogg`** | `src/oops-deps/libogg/oops-libogg.mk` | Ogg container bitstream manipulation | Audio streaming container |
| **`libvorbis`** | `src/oops-deps/libvorbis/oops-libvorbis.mk` | Ogg Vorbis audio decoding | Compressed background music and voice lines |

---

## 4. POSIX Compatibility Layer (`common/posix/`)

Most existing Unix/Linux games make standard POSIX filesystem calls (`stat`, `opendir`, `mkdir`, `chdir`). `oops-sdk` is freestanding C and intentionally does not include POSIX operating system calls in its core libc.

Instead, `oops-apps` provides a shared POSIX compatibility layer in `oops-apps/common/posix/posix.c`:

- **Headers Provided**: `<dirent.h>`, `<sys/stat.h>`, `<sys/time.h>`, `<unistd.h>`, `<pwd.h>`, `<locale.h>`, `<errno.h>`.
- **Functions Supported**:
  - `int stat(const char *path, struct stat *out)`
  - `int mkdir(const char *path, mode_t mode)`
  - `int access(const char *path, int mode)`
  - `char *getcwd(char *buf, size_t size)`
  - `int chdir(const char *path)`
  - `DIR *opendir(const char *name)`
  - `struct dirent *readdir(DIR *dirp)`
  - `int closedir(DIR *dirp)`
  - `struct passwd *getpwuid(uid_t uid)` (maps home directory to `/data`)
  - `int gettimeofday(struct timeval *tv, struct timezone *tz)`
- **How to Use**:
  Add `$(OOPS_APPS_ROOT)/common/posix/posix.c` to your `PAYLOAD_SRCS`, and add `-I$(OOPS_APPS_ROOT)/common/posix/include` to your target compiler flags.

---

## 5. Console Filesystem & Persistent Storage Model

Understanding the console's partition layout is essential to prevent silent write failures:

```
/ (Console Root)
├── app0/              [READ-ONLY]  Title package content (eboot.bin, assets, locked shaders)
├── data/              [READ-WRITE] Persistent internal SSD storage partition (survives reboots)
│   ├── homebrew/      Installed homebrew titles (/data/homebrew/<TITLE_ID>/)
│   └── savedata/      Persistent title save slots (/data/savedata/<TITLE_ID>/<SLOT>/)
├── mnt/
│   ├── usb0/          [READ-WRITE] Primary external USB storage (FAT32/exFAT)
│   └── usb1/          [READ-WRITE] Secondary external USB storage
```

### Save Data & Preferences (`<oops/savedata.h>`)
Never hardcode raw filesystem paths for user saves. `oops-sdk` provides a unified save data abstraction that handles retail encrypted containers when running with vendor entitlements, and **automatically falls back to persistent internal SSD storage** (`/data/savedata/<APP_ID>/<DIR_NAME>`) for homebrew:

```c
#include <oops/savedata.h>

// Option A: 1-Call High-Level Whole-File Persistence
void save_game(void) {
    player_state_t state = { .level = 5, .score = 12500 };
    // Automatically creates slot directory, writes file, and commits
    oops_savedata_save_file("SLOT0", "player.bin", &state, sizeof(state));
}

void load_game(void) {
    void *data = NULL;
    size_t size = 0;
    if (oops_savedata_load_file("SLOT0", "player.bin", &data, &size) == 0) {
        player_state_t *state = (player_state_t *)data;
        // ... restore game state ...
        oops_fs_free_data(data);
    }
}

// Option B: Directory Mounting for Multi-File Saves
void multi_file_save(void) {
    char mount_path[128];
    if (oops_savedata_mount("PROFILES", OOPS_SAVEDATA_MODE_CREATE | OOPS_SAVEDATA_MODE_READ_WRITE,
                            mount_path, sizeof(mount_path)) == 0) {
        // mount_path points to /data/savedata/MYAPP0001/PROFILES
        char file_path[256];
        oops_snprintf(file_path, sizeof(file_path), "%s/profile1.dat", mount_path);
        oops_fs_write_all(file_path, &data, sizeof(data));
        
        // Commit changes to disk
        oops_savedata_unmount(mount_path, true);
    }
}
```

---

## 6. Core SDK Helpers & Toolbelt

### 1. Dynamic Memory Allocation (`<oops/heap.h>` & `<oops/memory.h>`)
- **Virtual Memory Heap (`<oops/heap.h>`)**:
  - `oops_malloc(size)`, `oops_calloc(count, size)`, `oops_realloc(ptr, size)`, `oops_free(ptr)`.
  - Backed by anonymous virtual memory pages (`mmap`). Works in **Category 65536** (System/Background Apps) where the OS grants 0 bytes of Direct Memory (DMEM).
- **Direct Physical Memory (`<oops/memory.h>`)**:
  - `oops_mem_alloc()`, `oops_mem_map_direct()`.
  - Allocates contiguous physical memory for GPU hardware queues and display buffers:
    - **Onion**: Coherent CPU/GPU bus memory.
    - **Garlic**: High-bandwidth GPU dedicated memory.

### 2. Whole-File I/O & Directory Traversal (`<oops/fs.h>`)
- Slurp whole files into memory with one call:
  ```c
  void *data = NULL;
  size_t size = 0;
  if (oops_fs_read_all("/app0/assets/map.bin", &data, &size) == 0) {
      // process data...
      oops_fs_free_data(data);
  }
  ```
- Enumerate directory contents cleanly:
  ```c
  oops_dir_t *d = oops_fs_opendir("/app0/levels");
  if (d) {
      oops_dirent_t entry;
      while (oops_fs_readdir(d, &entry) == 1) {
          oops_kprintf("DIR", "Found entry: %s (dir=%d)\n", entry.name, entry.is_directory);
      }
      oops_fs_closedir(d);
  }
  ```

### 3. DualSense Controller Input (`<oops/input.h>`)
```c
oops_input_init();
oops_pad_state_t pad;
if (oops_input_poll(0, &pad) == 0) {
    if (pad.buttons & OOPS_BUTTON_CROSS)  { /* Jump */ }
    if (pad.buttons & OOPS_BUTTON_CIRCLE) { /* Cancel */ }
    float stick_x = (float)pad.left_stick_x / 128.0f; // -1.0 to +1.0
    float stick_y = (float)pad.left_stick_y / 128.0f;
    
    // Set RGB LED bar and haptic rumble
    oops_input_set_lightbar(0, 0, 0, 255); // Blue
    oops_input_set_rumble(0, 0, 100);
}
```

### 4. Stereo PCM Audio Output (`<oops/audio.h>`)
Hardware audio output is fixed at **48,000 Hz, 16-bit signed stereo**:
```c
oops_audio_port_t *audio = oops_audio_open(48000, 2, 1024);
int16_t samples[2048]; // 1024 left, 1024 right
// fill samples...
oops_audio_write(audio, samples, 1024);
oops_audio_close(audio);
```

### 5. Kernel Logging & Live Telemetry (`<oops/system.h>`)
Output formatted telemetry directly to the console's kernel log (`SYS_klog`):
```c
oops_klog("MYAPP", "Application initialized\n");
oops_kprintf("FPS", "Render time: %u ms | Entities: %d\n", frame_time, count);
```
View this stream live on your PC by running:
```powershell
pros.exe logs
```

---

## 7. Step-by-Step Porting Recipes

### Recipe 1: Porting an SDL2 Title
1. Create your application directory under `oops-apps/src/your-app`.
2. Add your source files and an entry point shim (`entry.c`):
   ```c
   #ifndef OOPS_HOST_BUILD
   #include "oops/syscall.h"
   extern int main(int argc, char **argv);
   __attribute__((visibility("default"))) int myapp_start(const payload_args_t *args) {
       if (args) sys_call_init(args);
       return main(0, (char **)0);
   }
   #endif
   ```
3. Create your `Makefile`:
   ```makefile
   APP_NAME   := myapp
   TITLE_ID   := MYAP00001
   TITLE_NAME := "My SDL2 Port"

   OOPS_RENDERER := gl1  # or gl2, or mesa
   OOPS_FEATURES := audio input keyboard
   ENTRY_POINT   := myapp_start

   # Application Sources (your code only - do not hand-list SDK sources)
   PAYLOAD_SRCS  := src/entry.c src/game.c src/render.c

   # Include SDL2 backend and POSIX shims
   include $(OOPS_APPS_ROOT)/src/oops-deps/sdl2/oops-sdl2.mk
   include $(OOPS_APPS_ROOT)/common/posix/posix.mk

   # Include canonical application rules
   include $(OOPS_APPS_ROOT)/common/app.mk
   ```
4. Build and test:
   ```bash
   make check   # runs host model
   make title   # builds complete Prospero title package
   ```

### Recipe 2: Porting an OpenGL 1.x / 2.0 / GLUT Title
1. Set `OOPS_RENDERER := gl1` (for fixed-function) or `gl2` (for programmable GLSL shaders) in your `Makefile`.
2. Include standard headers: `<GL/gl.h>`, `<GL/glu.h>`, `<GL/glut.h>`.
3. If using shaders, ensure your context version is explicitly enabled:
   ```c
   glContextSetVersion(2, 0); // Unlocks GLSL programmable pipeline
   ```
4. Handle the entry point and GLUT loop return:
   - `glutMainLoop()` will return when `glutLeaveMainLoop()` is invoked.
   - DualSense controller input is automatically delivered to GLUT keyboard callbacks:
     - D-Pad: `GLUT_KEY_LEFT`, `GLUT_KEY_RIGHT`, `GLUT_KEY_UP`, `GLUT_KEY_DOWN`
     - Cross: `\r` (Enter)
     - Circle: `\e` (Escape)
     - Options Button: Triggers `glutLeaveMainLoop()` to exit cleanly.

### Recipe 3: Porting a Modern OpenGL 3.3+ Game (`oops-mesa`)
1. In your `Makefile`, declare:
   ```makefile
   OOPS_RENDERER := mesa
   ```
2. Your application compiles as a hosted title against Mesa's Gallium/Radv driver and FreeBSD C library sysroot.
3. Access standard OpenGL 3.3 Core Profile and GLSL 3.30 shaders.

---

## 8. Deep Dive: The `oops-gl` Hardware Instrument

`oops-gl` is a custom clean-room OpenGL implementation whose command pipeline lowers directly into RDNA2 AGC PM4 command buffers.

### 1. OpenGL 1.0–1.5 Fixed Function
- Matrix stacks (`GL_MODELVIEW`, `GL_PROJECTION`, `GL_TEXTURE`).
- Immediate mode (`glBegin` / `glEnd`) and Vertex Arrays / Buffer Objects (VBOs).
- Hardware lighting (up to 8 light sources, materials, ambient/diffuse/specular).
- Texturing: 2 texture units on hardware, mipmapping, texture environments (`GL_MODULATE`, `GL_DECAL`, `GL_BLEND`, `GL_ADD`, `GL_COMBINE`).
- Alpha test, Depth test, Stencil buffers, Blending equations.

### 2. OpenGL 2.0 & GLSL 1.10 / 1.20 Shaders
On Prospero console hardware, fragment shaders are **compiled at runtime into real AMD gfx1030 machine instructions**.

#### What is Generated:
- Float, vector, and matrix arithmetic (`mat2`, `mat3`, `mat4`).
- Integer and boolean types (`int`, `bool`, `ivec`, `bvec`).
- Swizzles on both sides of assignments (`v.zyx = u.xxy;`).
- Conditional branching (`if`/`else`, `?:`, `discard`).
- `texture2D`, `textureCube`, `texture3D`, and `shadow2D` lookups.
- Fragment built-ins: `gl_FragCoord`, `gl_FrontFacing`, `gl_FragDepth`, `gl_Color`, `gl_FragColor`,
  `gl_PointCoord`.
- Derivatives: `dFdx`, `dFdy`, `fwidth`.
- Early `return` inside user functions.

#### `gl_PointCoord`: a fragment shader on its own
`gl_PointCoord` is texture coordinate 0's interpolant, which is how the hardware itself delivers
it — `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX` substitutes the sprite coordinate for a chosen
interpolant, and the point expansion writes it into the same slot that `GL_COORD_REPLACE` fills.
Two consequences, both reported as link errors with a sentence rather than a wrong picture:

- **A fragment shader reading `gl_PointCoord` may not also read `gl_TexCoord`.** They are one
  slot.
- **The program must have no vertex shader.** A point is expanded into its square before the
  vertex stage, sized in object space through the inverse model-view-projection; all four corners
  carry the same attributes, so a vertex shader recomputes them onto one another and the sprite
  is never drawn.

`glEnable(GL_POINT_SPRITE)` is not required. That switch belongs to `ARB_point_sprite` and the
fixed-function path; `gl_PointCoord` is defined for any point, and a program that reads it makes
the coordinate on its own. `GL_POINT_SPRITE_COORD_ORIGIN` still applies, so `t` runs downward
unless the origin is set to `GL_LOWER_LEFT`.

#### Hardware Safety: Trip Guards on Loops
A loop whose condition never terminates does not just draw a wrong frame—it hangs the GPU command processor, requiring a physical power cycle. To guarantee hardware safety:
- **`while` and `do { } while` are not generated for the console at all.** The trip guard takes
  its bound from a `for`'s initialiser, bound and step, and a `while` offers none. Write
  `for (int i = 0; i < <a bound>; i++)` with a `break` for the real condition — both are
  generated, and that rewrite is what the compiler's own refusal message tells you to use. The
  software rasteriser runs `while` and `do` correctly, so a shader using them links, passes a
  host run, and is refused at the draw on hardware with `GL_INVALID_OPERATION`.
- **Every branched loop is bounded by a compile-time trip guard.**
- Loops with constant counts (`for (int i = 0; i < 64; i++)`) compile and run with guaranteed termination.
- Loops with non-constant bounds dependent on uniforms (`for (int i = 0; i < someUniform; i++)`) are **refused by the compiler** with an explanatory log message.
- To port a loop with variable bounds, use a fixed ceiling with a runtime `break`:
  ```glsl
  for (int i = 0; i < 128; i++) {
      if (float(i) >= myUniformBound) break;
      // loop body...
  }
  ```

---

## 9. Build System, Packaging & Deployment

### 1. Declarative Subsystem Capabilities (`OOPS_FEATURES`)
Applications declare **capabilities**, not SDK implementation sources. In your Makefile:
```makefile
OOPS_FEATURES += keyboard net http audio savedata
```
The SDK (`oops-sdk.mk`) maps capabilities to the required implementation sources and `libSce*` import claims, while `app.mk` injects them automatically. The base runtime (`system.c`, `freestd.c`, `heap.c`, `fs.c`, `libc.c`, `math.c`, etc.) is always linked implicitly.

See the complete capability matrix and reference in [**`docs/FEATURES.md`**](FEATURES.md).

### 2. Standard Targets
Every application Makefile using `common/app.mk` provides four primary targets:
- `make check`: Compiles host-side models and runs unit tests.
- `make elf`: Compiles the freestanding target ELF payload.
- `make eboot`: Wraps the ELF into a fake-signed `eboot.bin` container via `SELFish`.
- `make title`: Synthesizes a complete, conforming Big App title directory under `build/title/<TITLE_ID>/` with `param.json`, fake-signed `keystone`, `nptitle.dat`, and standard icons.

### 2. Deploying with Prosperous (`pros`)
Deploy and run on hardware via the CLI:
```powershell
# Restore/upload title package to console SSD
pros.exe restore build/title/MYAP0001 /data/homebrew/MYAP0001

# Launch the title
pros.exe launch MYAP0001

# Monitor live kernel telemetry
pros.exe logs
```

---

## 10. Common Pitfalls & Gotchas

1. **The Undefined Symbols Check (`app.mk`)**:
   `app.mk` validates every symbol in your target ELF. A payload link ignores unresolved symbols by default, which would ordinarily cause a fatal crash on the console at runtime. If a function is called that has no definition, `app.mk` fails the build immediately and names the missing symbol.
2. **Never Statically Link Weak Imports**:
   Do not introduce unresolved static weak symbols into an ELF intended for packaging. The `mkmodule` tool enforces that all dynamic imports resolve to declared PRX libraries in `symbols.txt`. For optional platform functions, resolve them dynamically via `sceKernelDlsym`.
3. **Never Write to `/app0`**:
   The `/app0` mount point is strictly **read-only**. Any attempt to create or write files to `/app0` will fail with permission errors (`EPERM` / `EROFS`). All persistent saves and configuration files must be directed to `<oops/savedata.h>` or `/data`.
4. **Memory Alignment**:
   Direct Memory buffers submitted to AGC or audio hardware must meet hardware alignment requirements:
   - Framebuffers & textures: 256-byte or 64 KB tile alignment.
   - Audio ring buffers: 64-byte alignment.
   - Heap allocations via `oops_malloc`: Guaranteed 16-byte aligned.
