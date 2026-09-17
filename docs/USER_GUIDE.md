# oops-sdk Developer Guide & Tutorial

**Practical Guide to Building Native Homebrew on PS5 with the Clean-Room OOPS SDK.**

Welcome to the **oops-sdk** developer guide. This manual walks you through building native, freestanding C/C++ applications for PlayStation 5 (Prospero) and PlayStation 4 (Orbis) hardware using `oops-sdk`.

For exhaustive function signatures, parameters, and return codes across all 21 subsystems, see the **[Complete API Reference](API_REFERENCE.md)**.

---

## Table of Contents

1. [Freestanding C Architecture](#1-freestanding-c-architecture)
2. [Project Setup & Makefile Integration](#2-project-setup--makefile-integration)
3. [Tutorial 1: Display Output & 2D Software Drawing](#3-tutorial-1-display-output--2d-software-drawing)
4. [Tutorial 2: DualSense Controller Input & Feedback](#4-tutorial-2-dualsense-controller-input--feedback)
5. [Tutorial 3: 3D Hardware Graphics with OpenGL (`oops-gl`)](#5-tutorial-3-3d-hardware-graphics-with-opengl-oops-gl)
6. [Tutorial 4: Stereo PCM Audio Output](#6-tutorial-4-stereo-pcm-audio-output)
7. [Tutorial 5: Dynamic Code Execution & JIT Allocation](#7-tutorial-5-dynamic-code-execution--jit-allocation)
8. [Tutorial 6: System Dialogs, On-Screen Keyboard & Save Data](#8-tutorial-6-system-dialogs-on-screen-keyboard--save-data)
9. [Tutorial 7: Standard Runtime Shimming (Filesystem, Dynamic Heap, Math & Telemetry)](#9-tutorial-7-standard-runtime-shimming-filesystem-dynamic-heap-math--telemetry)
10. [Special & Advanced Subsystems (Escalation & PKGs)](#10-special--advanced-subsystems-escalation--pkgs)
11. [Hardware Safety & Clean-Room Invariants](#11-hardware-safety--clean-room-invariants)

---

## 1. Freestanding C Architecture

`oops-sdk` operates strictly freestanding (`-ffreestanding -nostdlib`):
* **Zero Vendor SDK Files**: Replaces proprietary headers with mathematically verified structures and clean-room libc stubs.
* **No Desktop Glibc**: Primitives like `obs_strlen`, `obs_memcpy`, and `obs_memset` provide memory operations. Debug text is emitted via `obs_klog()` directly into the kernel telemetry ring.
* **Dynamic Linking via NIDs**: The runtime resolves platform libraries (`libkernel`, `libScePad`, `libSceAudioOut`) dynamically via symbol NID hashes.

---

## 2. Project Setup & Makefile Integration

All `oops-sdk` applications integrate through the canonical application Makefile (`oops-apps/common/app.mk`).

Create a project directory with the following structure:
```
my-app/
├── Makefile
└── src/
    └── main.c
```

### `Makefile`
```makefile
APP_NAME    := my-homebrew
TITLE_ID    := MYHB00001
TITLE_NAME  := "My Native Homebrew"

# Sources to compile
SRCS        := src/main.c

# Include canonical OOPS application rules:
include ../common/app.mk
```

### Building Targets:
* `make check`: Run host-side unit tests and memory models.
* `make elf`: Build a plain ELF payload for loading over `pros send` (`port 9021`).
* `make eboot`: Package into a fake-signed `eboot.bin` container via `SELFish`.
* `make title`: Create a complete, conforming native Big App directory layout (`PPSAxxxxx`).

---

## 3. Tutorial 1: Display Output & 2D Software Drawing

Open exclusive HDMI scanout (`OBS_VIDEO_BUS_MAIN`) and render 2D shapes and text.

```c
#include <oops/oops.h>

int main(void) {
    obs_klog("[APP] Starting 2D display demo...\n");

    // 1. Open primary display (1080p, double-buffered)
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 1920, 1080);
    if (!oops_display_is_ready(disp)) {
        obs_klog("[APP] Failed to acquire display!\n");
        return -1;
    }

    int frame = 0;
    while (1) {
        // 2. Wrap back buffer into a 2D drawing surface
        oops_surface_t surf = oops_display_get_surface(disp);

        // 3. Clear background
        oops_draw_clear(&surf, OOPS_COLOR_BLACK);

        // 4. Draw shapes
        oops_draw_fill_rect(&surf, 100, 100, 400, 200, OOPS_COLOR_BLUE);
        oops_draw_rect(&surf, 100, 100, 400, 200, OOPS_COLOR_WHITE);
        oops_draw_fill_circle(&surf, 800, 300, 80, OOPS_COLOR_RED);

        // 5. Render text using built-in bitmap font
        oops_draw_text(&surf, 120, 140, "Hello from oops-sdk!", OOPS_COLOR_WHITE, 2);

        // 6. Flip back buffer to HDMI output (sync with VBLANK)
        oops_display_flip(disp);
        frame++;
    }

    oops_display_close(disp);
    return 0;
}
```

---

## 4. Tutorial 2: DualSense Controller Input & Feedback

Poll button states, analog sticks, and control lightbars and haptic vibration.

```c
#include <oops/oops.h>

void handle_input(void) {
    oops_input_init();

    oops_pad_state_t pad;
    while (1) {
        // Poll gamepad 0
        if (oops_input_poll(0, &pad) == 0) {
            if (pad.buttons & OOPS_BUTTON_CROSS) {
                obs_klog("Cross pressed!\n");
                // Turn lightbar green and pulse strong motor
                oops_input_set_lightbar(0, 0, 255, 0);
                oops_input_set_rumble(0, 0, 128);
            }
            if (pad.buttons & OOPS_BUTTON_CIRCLE) {
                // Turn lightbar red and stop rumble
                oops_input_set_lightbar(0, 255, 0, 0);
                oops_input_set_rumble(0, 0, 0);
            }

            // Analog sticks: -128 to 127
            int lx = pad.left_stick_x;
            int ly = pad.left_stick_y;
        }

        oops_time_sleep_ms(16); // 60 Hz poll rate
    }
}
```

---

## 5. Tutorial 3: 3D Hardware Graphics with the fixed-function instrument (`oops-gl`)

`oops-sdk` bundles a fixed-function 3D engine (`<GL/gl.h>`) running directly on RDNA2 AGC hardware. Its surface is OpenGL 1.1-class - immediate mode, vertex arrays, one 2D texture unit, lighting, the matrix stacks - and it is deliberately not a GL version: it exists so the command stream stays readable as a hardware record (D007).

**Writing an application? Use [oops-mesa](../../oops-mesa/) instead**, which gives OpenGL 3.3 Core and GLSL 3.30. Follow this tutorial when you want to see PM4 come out of a draw call.

```c
#include <oops/oops.h>
#include <GL/gl.h>

void render_cube(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 1920, 1080);
    
    // 1. Create OpenGL context on top of the display
    void *ctx = glContextCreate(disp);
    glContextMakeCurrent(ctx);

    // 2. Configure 3D state
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    // 3. Set projection matrix
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // 60 deg FOV, 16:9 aspect ratio
    glFrustum(-1.777f * 0.1f, 1.777f * 0.1f, -0.1f, 0.1f, 0.1f, 100.0f);

    float angle = 0.0f;
    while (1) {
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -4.0f);
        glRotatef(angle, 1.0f, 1.0f, 0.0f);

        // Render colored triangle
        glBegin(GL_TRIANGLES);
            glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.0f, 0.0f);
            glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(-1.0f, -1.0f, 0.0f);
            glColor3f(0.0f, 0.0f, 1.0f); glVertex3f( 1.0f, -1.0f, 0.0f);
        glEnd();

        // Swap buffers and submit AGC command stream to GPU
        glSwapBuffers();
        angle += 1.0f;
    }

    glContextDestroy(ctx);
    oops_display_close(disp);
}
```

---

## 6. Tutorial 4: Stereo PCM Audio Output

Output uncompressed 16-bit stereo PCM audio (48,000 Hz).

```c
#include <oops/oops.h>

void play_audio(void) {
    // Open 48kHz, 2-channel stereo port
    oops_audio_port_t *audio = oops_audio_open(48000, 2);
    if (!audio) return;

    // Buffer for 1024 frames (2048 int16 samples)
    int16_t samples[2048];
    for (int i = 0; i < 1024; i++) {
        int16_t val = (i % 64 < 32) ? 8000 : -8000; // Square wave
        samples[i * 2 + 0] = val; // Left channel
        samples[i * 2 + 1] = val; // Right channel
    }

    // Stream audio continuously
    for (int loop = 0; loop < 100; loop++) {
        oops_audio_write(audio, samples, 1024);
    }

    oops_audio_close(audio);
}
```

---

## 7. Tutorial 5: Dynamic Code Execution & JIT Allocation

When building recompilers, emulators, or dynamic runtime translation engines, allocate executable memory with W^X safety:

```c
#include <oops/oops.h>

void test_jit(void) {
    if (!oops_jit_is_available()) {
        obs_klog("JIT not available: fallback to CPU interpreter\n");
        return;
    }

    oops_jit_memory_t jit;
    if (oops_jit_alloc(64 * 1024, &jit) != 0) {
        return;
    }

    // Emit machine code into the writable view (rw_addr):
    // x86_64: int add_five(int x) { return x + 5; }
    // 8d 47 05 : lea eax, [rdi + 5]
    // c3       : ret
    uint8_t *code = (uint8_t *)jit.rw_addr;
    code[0] = 0x8d;
    code[1] = 0x47;
    code[2] = 0x05;
    code[3] = 0xc3;

    // Flush instruction cache and serialize memory fences
    oops_jit_flush_icache(jit.rx_addr, 4);

    // Call the generated function via the executable view (rx_addr)
    typedef int (*jit_fn_t)(int);
    jit_fn_t add_five = (jit_fn_t)(uintptr_t)jit.rx_addr;
    int result = add_five(37); // returns 42

    // Release memory
    oops_jit_free(&jit);
}
```

---

## 8. Tutorial 6: System Dialogs, On-Screen Keyboard & Save Data

### A. On-Screen Virtual Keyboard (IME)
```c
oops_ime_param_t param = {
    .title = "Player Name",
    .initial_text = "Player 1",
    .max_text_len = 32
};
oops_dialog_ime_open(&param);

// Wait for user to submit or cancel keyboard
while (oops_dialog_ime_poll() == OOPS_IME_STATUS_RUNNING) {
    oops_display_flip(disp);
}

char name_buf[32];
oops_ime_result_t res;
oops_dialog_ime_get_result(name_buf, sizeof(name_buf), &res);
if (res == OOPS_IME_RESULT_OK) {
    obs_klog(name_buf);
}
oops_dialog_ime_close();
```

### B. Encrypted Save Data
```c
char mount_path[64];
// Mount encrypted save slot "SAVE0000"
if (oops_savedata_mount("SAVE0000", OOPS_SAVEDATA_MODE_CREATE, mount_path, sizeof(mount_path)) == 0) {
    char save_file[128];
    snprintf(save_file, sizeof(save_file), "%s/save.bin", mount_path);

    int fd = open(save_file, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        write(fd, &high_score, sizeof(high_score));
        close(fd);
    }

    // Commit changes to encrypted SSD storage
    oops_savedata_unmount(mount_path, true);
}
```

---

## 9. Tutorial 7: Standard Runtime Shimming (Filesystem, Dynamic Heap, Math & Telemetry)

When porting third-party engines, emulators, or open-source software to PlayStation 5, `oops-sdk` provides clean-room shims that replace standard desktop C runtime facilities without relying on glibc or proprietary Sony SDK headers.

### A. Dynamic Memory Allocation (`<oops/heap.h>`)
Standard console SDKs require reserving fixed Direct Memory (DMEM) pools, which fail outright in Category 65536 System Apps where the OS grants 0 bytes of DMEM. `oops-sdk`'s heap allocator works identically everywhere:

```c
#include <oops/heap.h>

// Allocate 16-byte aligned dynamic memory
char *buffer = (char *)oops_malloc(4096);
if (buffer) {
    obs_strcpy(buffer, "Dynamic allocation successful");
    oops_free(buffer);
}

// Zero-initialized array allocation
int *scores = (int *)oops_calloc(64, sizeof(int));
oops_free(scores);
```

### B. High-Level Filesystem (`<oops/fs.h>`)
Easily read assets, textures, and ROMs directly from the title directory (`/app0`) or user storage:

```c
#include <oops/fs.h>

// Slurp an entire configuration or asset file into memory in one call
void *file_data = NULL;
size_t file_size = 0;
if (oops_fs_read_all("/app0/assets/config.json", &file_data, &file_size) == 0) {
    oops_kprintf("[FS] Loaded config: %u bytes\n", (unsigned int)file_size);

    // Process file_data...

    // Release allocated buffer
    oops_fs_free_data(file_data);
}

// Check file existence and query size without reading
if (oops_fs_exists("/app0/assets/level1.bin")) {
    int64_t sz = oops_fs_file_size("/app0/assets/level1.bin");
    oops_kprintf("[FS] level1.bin exists, size = %lld bytes\n", (long long)sz);
}
```

### C. Freestanding 3D Math & Matrix Transforms (`<oops/math.h>`)
Compute camera transformations, trigonometry, and vector operations directly matching RDNA2 AGC vertex pipeline layout:

```c
#include <oops/math.h>

// 1. Setup projection matrix (60 deg FOV, 16:9 aspect ratio, near 0.1, far 100.0)
oops_mat4_t proj;
oops_mat4_perspective(&proj, 60.0f * OOPS_DEG2RAD, 16.0f / 9.0f, 0.1f, 100.0f);

// 2. Setup camera view looking at origin from (0, 2, 5)
oops_vec3_t eye    = oops_vec3_make(0.0f, 2.0f, 5.0f);
oops_vec3_t center = oops_vec3_make(0.0f, 0.0f, 0.0f);
oops_vec3_t up     = oops_vec3_make(0.0f, 1.0f, 0.0f);
oops_mat4_t view;
oops_mat4_lookat(&view, eye, center, up);

// 3. Model transformation: rotate 45 degrees around Y-axis
oops_mat4_t model;
oops_mat4_identity(&model);
oops_mat4_rotate(&model, 45.0f * OOPS_DEG2RAD, 0.0f, 1.0f, 0.0f);
```

### D. Real-Time Telemetry & Formatted Logging (`<oops/system.h>`)
Output debug telemetry that streams directly over `pros logs` or the serial kernel ring buffer:

```c
#include <oops/system.h>

oops_klog("[INIT] Subsystem online\n");
oops_kprintf("[FRAME] Delta: %u ms | Heap active: %u KB\n",
             (unsigned int)frame_ms, (unsigned int)(stats.allocated_bytes / 1024));
```

---

## 10. Special & Advanced Subsystems (Escalation & PKGs)

### Privilege Escalation (`<oops/escalate.h>`)
Standard native Big Apps run with full GPU and display privileges out of the box. Privilege escalation is **only** needed for system maintenance utilities, FTP daemons, or debugger background services:
```c
// Jailbreak current process: zero UIDs, set SYSTEM_AUTHID, escape sandbox jail
if (oops_jailbreak_process(0) == 0) {
    obs_klog("Now running as root with full filesystem access\n");
}
```

### Background PKG Installation (`<oops/pkg.h>`)
```c
oops_pkg_init();
oops_pkg_install("/data/homebrew/myapp.pkg");

uint32_t pct = 0;
while (pct < 100) {
    oops_pkg_get_progress("UP0000-MYHB00001_00-0000000000000000", &pct);
    oops_time_sleep_ms(500);
}
oops_pkg_term();
```

---

## 11. Hardware Safety & Clean-Room Invariants

1. **Hardware Failsafe Ring Protection**:
   * All RDNA2 AGC submissions must handle fence timeouts safely. Never lock the physical hardware command ring in a busy loop.
2. **Clean-Room Verification**:
   * Never copy or inspect proprietary Sony SDK headers (`kernel.h`, `pad.h`, `agc.h`).
   * All interfaces in `oops-sdk` are derived from mathematically verified reverse-engineering, open-source references, and physical silicon probes ([CONVENTIONS §1](../docs/CONVENTIONS.md)).
