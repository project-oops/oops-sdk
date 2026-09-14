# OOPS SDK Complete API Reference

**Clean-Room Freestanding C Runtime and Hardware Abstraction SDK for Orbis & Prospero.**

This document provides a comprehensive technical API reference for all subsystems, data structures, and functions exposed by `oops-sdk`.

---

## Table of Contents

1. [Display & Video Output (`<oops/display.h>`)](#1-display--video-output-oopsdisplayh)
2. [2D Software Drawing Canvas (`<oops/draw.h>`)](#2-2d-software-drawing-canvas-oopsdrawh)
3. [OpenGL 1.3 3D Graphics Engine (`<GL/gl.h>`)](#3-opengl-13-3d-graphics-engine-glglh)
4. [Hardware RDNA2 AGC Graphics (`<oops/agc.h>`, `<oops/gpu.h>`)](#4-hardware-rdna2-agc-graphics-oopsagch-oopsgpuh)
5. [Memory Management & Direct Memory (`<oops/memory.h>`)](#5-memory-management--direct-memory-oopsmemoryh)
6. [JIT & Dynamic Executable Memory (`<oops/jit.h>`)](#6-jit--dynamic-executable-memory-oopsjith)
7. [Controller & Input Devices (`<oops/input.h>`, `<oops/keyboard.h>`, `<oops/mouse.h>`)](#7-controller--input-devices-oopsinputh-oopskeyboardh-oopsmouseh)
8. [Audio Streaming (`<oops/audio.h>`)](#8-audio-streaming-oopsaudioh)
9. [Hardware Media Codecs (`<oops/audiodec.h>`, `<oops/videodec.h>`)](#9-hardware-media-codecs-oopsaudiodech-oopsvideodech)
10. [System UI: Dialogs & On-Screen IME (`<oops/dialog.h>`)](#10-system-ui-dialogs--on-screen-ime-oopsdialogh)
11. [Save Data Management (`<oops/savedata.h>`)](#11-save-data-management-oopssavedatah)
12. [Package & Application Management (`<oops/pkg.h>`)](#12-package--application-management-oopspkgh)
13. [Privilege Escalation & Sandbox Escape (`<oops/escalate.h>`)](#13-privilege-escalation--sandbox-escape-oopsescalateh)
14. [Kernel Read/Write & Syscall Dispatcher (`<oops/krw.h>`, `<oops/syscall.h>`)](#14-kernel-readwrite--syscall-dispatcher-oopskrwh-oopssyscallh)
15. [System Modules & Dynamic Linking (`<oops/sysmodule.h>`)](#15-system-modules--dynamic-linking-oopssysmoduleh)
16. [System Information & Telemetry (`<oops/system.h>`, `<oops/offsets.h>`)](#16-system-information--telemetry-oopssystemh-oopsoffsetsh)
17. [High-Resolution Timing (`<oops/time.h>`)](#17-high-resolution-timing-oopstimeh)
18. [Threading & Synchronization (`<oops/thread.h>`)](#18-threading--synchronization-oopsthreadh)
19. [BSD Sockets & Network Telemetry (`<oops/net.h>`, `<oops/netctl.h>`)](#19-bsd-sockets--network-telemetry-oopsneth-oopsnetctlh)
20. [Freestanding C Runtime Utilities (`<oops/freestd.h>`)](#20-freestanding-c-runtime-utilities-oopsfreestdh)
21. [Process Control & Code Injection (`<oops/inject.h>`, `<oops/procctl.h>`, `<oops/procparam.h>`)](#21-process-control--code-injection-oopsinjecth-oopsprocctlh-oopsprocparamh)
22. [High-Level Filesystem Subsystem (`<oops/fs.h>`)](#22-high-level-filesystem-subsystem-oopsfsh)
23. [Freestanding Userland Heap Allocator (`<oops/heap.h>`)](#23-freestanding-userland-heap-allocator-oopsheaph)
24. [Freestanding Math & 3D Linear Algebra (`<oops/math.h>`)](#24-freestanding-math--3d-linear-algebra-oopsmathh)

---

## 1. Display & Video Output (`<oops/display.h>`)

The display subsystem opens exclusive HDMI video scanout on Bus 0 (`OBS_VIDEO_BUS_MAIN`), configures double-buffered direct video memory, and presents frames synchronized with hardware VBLANK.

### `oops_display_t *oops_display_open(oops_display_backend_t backend, unsigned int width, unsigned int height)`
* **When to use**: During application startup to acquire the screen.
* **Backend options**:
  * `OOPS_DISPLAY_BACKEND_AUTO` (0): Automatically select the compiled hardware generation (AGC on Prospero, GNM on Orbis).
  * `OOPS_DISPLAY_BACKEND_AGC` (5): Force RDNA2 AGC hardware queue.
  * `OOPS_DISPLAY_BACKEND_GNM` (4): Force GCN GNM driver.
* **Returns**: Non-null display handle. Always check `oops_display_is_ready(disp)` afterwards.
* **Example**:
  ```c
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 1920, 1080);
  if (!oops_display_is_ready(disp)) {
      obs_klog("Display failed to open!\n");
  }
  ```

### `int oops_display_is_ready(const oops_display_t *disp)`
* **Returns**: `1` if the display was opened and video buffers are registered with SceVideoOut; `0` on failure.

### `int oops_display_is_gpu_accelerated(const oops_display_t *disp)`
* **Returns**: `1` if backed by hardware GPU queues (AGC/GNM); `0` if running on host fallback.

### `uint32_t *oops_display_get_framebuffer(oops_display_t *disp)`
* **When to use**: Direct 32bpp ARGB pixel access to the back buffer before flipping.
* **Returns**: Pointer to the active back buffer pixels in write-combined direct memory.

### `int oops_display_flip(oops_display_t *disp)`
* **When to use**: At the end of each frame to swap front and back buffers and wait for scanout.
* **Returns**: `0` on success; negative error code on failure.

### `void oops_display_clear(oops_display_t *disp, uint32_t color)`
* **When to use**: Fast fill of the entire active back buffer with an ARGB color word (e.g. `0xFF000000` for black).

### `void oops_display_close(oops_display_t *disp)`
* **When to use**: Clean shutdown to unregister video buffers and release the HDMI bus.

---

## 2. 2D Software Drawing Canvas (`<oops/draw.h>`)

High-performance 2D rasterizer operating on linear ARGB pixel surfaces. Supports clipping, alpha blending, antialiasing primitives, bitmap fonts, and sprite blitting.

### `oops_surface_t oops_display_get_surface(oops_display_t *disp)`
* **When to use**: Wraps the display's current back buffer in an `oops_surface_t` for 2D draw calls.
* **Returns**: Surface struct containing `pixels`, `width`, `height`, and row `pitch`.

### `void oops_draw_clear(oops_surface_t *surf, oops_color_t color)`
* **When to use**: Clear the surface with a 32-bit ARGB color (`OOPS_RGB(r, g, b)` or `OOPS_RGBA(r, g, b, a)`).

### `void oops_draw_rect(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color)`
* **When to use**: Draw a 1-pixel stroked rectangle outline with clipping bounds.

### `void oops_draw_fill_rect(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color)`
* **When to use**: Fill a solid rectangular area with clipping.

### `void oops_draw_line(oops_surface_t *surf, int x0, int y0, int x1, int y1, oops_color_t color)`
* **When to use**: Bresenham line rasterization between two points.

### `void oops_draw_circle(oops_surface_t *surf, int cx, int cy, int r, oops_color_t color)`
### `void oops_draw_fill_circle(oops_surface_t *surf, int cx, int cy, int r, oops_color_t color)`
* **When to use**: Draw outlined or filled circles using midpoint algorithms.

### `int oops_draw_text(oops_surface_t *surf, int x, int y, const char *text, oops_color_t color, int scale)`
* **When to use**: Render ASCII text using the built-in 8x8 bitmap font.
* **Parameters**: `scale` is an integer multiplier (1 = 8x8, 2 = 16x16, etc.).
* **Returns**: Width in pixels of the rendered text.

### `void oops_draw_blit(oops_surface_t *dst, int dx, int dy, const oops_surface_t *src, int sx, int sy, int sw, int sh)`
### `void oops_draw_blit_blend(oops_surface_t *dst, int dx, int dy, const oops_surface_t *src, int sx, int sy, int sw, int sh)`
* **When to use**: Blit a sub-rectangle from a source surface (or sprite) to the destination. `blit_blend` performs full per-pixel alpha blending.

---

## 3. OpenGL 1.3 3D Graphics Engine (`<GL/gl.h>`)

`oops-gl` is a clean-room, hardware-accelerated OpenGL 1.3 / GLES 1.1 fixed-function translation profile. It maps standard GL calls directly to RDNA2 AGC command buffers (PM4 DCBs) without vendor graphics drivers.

### Context Lifecycle

#### `void *glContextCreate(struct oops_display *disp)`
* **When to use**: Initialize the 3D pipeline on top of an active display. Allocates depth buffer (Z24/S8), projection/modelview matrix stacks, and hardware ring memory.
* **Returns**: Opaque `GLContext*` pointer.

#### `void glContextMakeCurrent(void *ctx)`
* **When to use**: Bind the active OpenGL context to the calling thread.

#### `void glSwapBuffers(void)`
* **When to use**: End of frame. Submits pending AGC command packets, synchronizes end-of-pipe fences, and flips display.

#### `void glContextDestroy(void *ctx)`
* **When to use**: Clean teardown of 3D context, depth buffers, and texture memory.

### Standard Fixed-Function APIs
* **State & Enablers**: `glEnable(cap)`, `glDisable(cap)`, `glIsEnabled(cap)` (`GL_DEPTH_TEST`, `GL_CULL_FACE`, `GL_LIGHTING`, `GL_TEXTURE_2D`, `GL_BLEND`).
* **Matrix Operations**: `glMatrixMode(mode)`, `glLoadIdentity()`, `glPushMatrix()`, `glPopMatrix()`, `glTranslatef(x, y, z)`, `glRotatef(angle, x, y, z)`, `glScalef(x, y, z)`, `glMultMatrixf(m)`, `glFrustum(...)`, `glOrtho(...)`.
* **Immediate Mode**: `glBegin(mode)` (`GL_TRIANGLES`, `GL_QUADS`, `GL_LINES`, etc.), `glEnd()`, `glVertex3f(x, y, z)`, `glColor4f(r, g, b, a)`, `glNormal3f(x, y, z)`, `glTexCoord2f(u, v)`.
* **Vertex Arrays**: `glVertexPointer(...)`, `glColorPointer(...)`, `glNormalPointer(...)`, `glTexCoordPointer(...)`, `glDrawArrays(mode, first, count)`.
* **Textures**: `glGenTextures(n, ids)`, `glBindTexture(target, id)`, `glTexImage2D(...)`, `glTexParameteri(...)`, `glDeleteTextures(n, ids)`.
* **Lighting**: `glLightfv(light, pname, params)`, `glMaterialfv(face, pname, params)`.

### Hardware Telemetry Extensions
* `GLboolean glIsHardwareAccelerated(void)`: Returns `GL_TRUE` only when RDNA2 AGC hardware submission, end-of-pipe fence (`0xbeefcafe`), and GPU clock counters are verified.
* `void glGetHardwareStatus(gl_hw_status_t *out)`: Retrieves detailed GPU clock cycles, clear color test matches, and fence confirmation stats.
* `void glRequestHardwareDump(void)`: Instructs the pipeline to log the next frame's raw PM4 packets, shader words, and descriptors to `klog` for analysis.

---

## 4. Hardware RDNA2 AGC Graphics (`<oops/agc.h>`, `<oops/gpu.h>`)

Low-level bare-metal access to PS5 RDNA2 GFX10.3 hardware command queues, compute dispatchers, and tile micro-swizzling.

### `int oops_gpu_available(void)`
* **Returns**: `1` if native AGC graphics hardware is present; `0` if running on host/emulator.

### `int oops_gpu_queue_init(oops_gpu_queue_t *queue, oops_gpu_queue_type_t type)`
* **When to use**: Initialize a universal graphics (`OOPS_GPU_QUEUE_GRAPHICS`) or compute (`OOPS_GPU_QUEUE_COMPUTE`) ring.

### `int oops_agc_dispatch_compute(oops_gpu_queue_t *queue, const oops_agc_compute_desc_t *desc)`
* **When to use**: Dispatch RDNA2 Wave32 compute shaders directly to asynchronous compute pipes.

### Micro-Tile Swizzling (`<oops/agc_tiler.h>`)
* `void oops_agc_tile_surface_span(...)`: Converts linear 32bpp ARGB raster buffers into RDNA2 64 KB micro-tiled physical layouts (`ADDR_SURF_MICRO_TILING`) required for hardware display scanout.

---

## 5. Memory Management & Direct Memory (`<oops/memory.h>`)

Prospero divides memory between CPU-cached coherent system RAM and GPU write-combined video memory. This subsystem manages direct physical allocations without glibc.

### Memory Types (`oops_mem_type_t`)
* `OOPS_MEM_WB_ONION` (0): Write-back CPU cached, GPU coherent system RAM.
* `OOPS_MEM_WC_GARLIC` (3): Write-combined GPU memory. Ideal for display framebuffers, dynamic vertex streams, and textures.
* `OOPS_MEM_WB_GARLIC` (10): GPU write-back cache. Ideal for compute scratchpads and render targets.

### `void *oops_mem_alloc(size_t size, size_t alignment, oops_mem_type_t type)`
* **When to use**: High-level managed allocation of direct memory. Automatically rounded to 64 KB boundaries.
* **Returns**: Virtual address pointer, or `NULL` on failure.

### `void oops_mem_free(void *ptr)`
* **When to use**: Release a managed direct memory block.

### `int64_t oops_mem_get_phys(const void *ptr)`
* **When to use**: Look up the hardware physical bus offset of a managed allocation (needed when passing buffers to GPU descriptors or video registers). Returns `-1` if pointer is not managed.

### Low-Level Primitives
* `int oops_mem_alloc_direct(size_t size, size_t alignment, oops_mem_type_t type, int64_t *out_phys)`
* `int oops_mem_map_direct(void **out_vaddr, size_t size, int prot, int flags, int64_t phys, size_t alignment)`
* `int oops_mem_unmap(void *vaddr, size_t size)`
* `int oops_mem_free_direct(int64_t phys, size_t size)`

---

## 6. JIT & Dynamic Executable Memory (`<oops/jit.h>`)

Enables runtime code generation (compilers, emulators, recompilers) with W^X enforcement and progressive fallback across firmware environments.

### `int oops_jit_is_available(void)`
* **Returns**: `1` if dynamic code execution can be performed in the current process; `0` if restricted.

### `int oops_jit_get_method(void)`
* **Returns**: Active strategy (`OOPS_JIT_METHOD_SHARED_MEM`, `OOPS_JIT_METHOD_MPROTECT`, or `OOPS_JIT_METHOD_HOST`).

### `int oops_jit_alloc(size_t size, oops_jit_memory_t *out_mem)`
* **When to use**: Allocate a code cache buffer.
* **Strategy**: Attempts Sony shared memory dual-mapping (`rx_addr` and `rw_addr` pointing to the same physical memory). If unprivileged (`EPERM`), falls back to `mmap` + `mprotect` (relaxed W^X under `kstuff-lite`).
* **Example**:
  ```c
  oops_jit_memory_t jit;
  if (oops_jit_alloc(64 * 1024, &jit) == 0) {
      // Emit x86_64 machine code into writable view:
      uint8_t *code = (uint8_t *)jit.rw_addr;
      code[0] = 0xb8; *(uint32_t*)(code + 1) = 42; code[5] = 0xc3; // mov eax, 42; ret
      
      // Flush instruction cache before jumping:
      oops_jit_flush_icache(jit.rx_addr, 6);
      
      // Execute from rx_addr:
      int (*fn)(void) = (int (*)(void))(uintptr_t)jit.rx_addr;
      int val = fn(); // returns 42
      
      oops_jit_free(&jit);
  }
  ```

### `int oops_jit_flush_icache(const void *addr, size_t size)`
* **When to use**: Mandatory after modifying `rw_addr` and before jumping to `rx_addr`. Clears CPU pipeline caches and executes a memory fence (`mfence`).

### `int oops_jit_free(oops_jit_memory_t *mem)`
* **When to use**: Unmaps all views, closes shared memory descriptors, and zeroes the memory struct.

---

## 7. Controller & Input Devices (`<oops/input.h>`, `<oops/keyboard.h>`, `<oops/mouse.h>`)

Provides low-latency polling for DualSense gamepads, motion sensors, adaptive triggers, and USB/Bluetooth keyboards and mice.

### DualSense Gamepads (`<oops/input.h>`)

#### `int oops_input_init(void)`
* **When to use**: Initialize `libScePad` and open controller ports.

#### `int oops_input_poll(int pad_index, oops_pad_state_t *out_state)`
* **When to use**: Read the latest instantaneous gamepad state once per frame.
* **State fields**:
  * `buttons`: Bitmask (`OOPS_BUTTON_CROSS`, `OOPS_BUTTON_CIRCLE`, `OOPS_BUTTON_TRIANGLE`, `OOPS_BUTTON_SQUARE`, `OOPS_BUTTON_L1`, `OOPS_BUTTON_R1`, `OOPS_BUTTON_L2`, `OOPS_BUTTON_R2`, `OOPS_BUTTON_UP`, `OOPS_BUTTON_DOWN`, `OOPS_BUTTON_LEFT`, `OOPS_BUTTON_RIGHT`, `OOPS_BUTTON_OPTIONS`, `OOPS_BUTTON_TOUCHPAD`).
  * `left_stick_x`, `left_stick_y`: -128 to 127.
  * `right_stick_x`, `right_stick_y`: -128 to 127.
  * `l2_analog`, `r2_analog`: 0 to 255.
  * `touch[2]`: Capacitive touch coordinates (`x`: 0–1919, `y`: 0–941, `active`).
  * `motion`: 3-axis accelerometer and gyroscope vectors.

#### `int oops_input_read_batch(int pad_index, oops_pad_state_t *out_samples, int max_samples)`
* **When to use**: Read all hardware samples buffered since the last call (up to 64 samples) to prevent missing rapid button presses.

#### `int oops_input_set_rumble(int pad_index, uint8_t weak, uint8_t strong)`
#### `int oops_input_set_lightbar(int pad_index, uint8_t r, uint8_t g, uint8_t b)`
* **When to use**: Control vibration motors and the RGB LED lightbar.

### Keyboard & Mouse (`<oops/keyboard.h>`, `<oops/mouse.h>`)
* `int oops_keyboard_read(oops_key_event_t *events, unsigned int max)`: Read raw USB keycode presses and releases.
* `int oops_mouse_read(oops_mouse_state_t *state)`: Read relative mouse delta coordinates (`dx`, `dy`, `wheel`) and button states.

---

## 8. Audio Streaming (`<oops/audio.h>`)

Direct stereo 16-bit PCM streaming via `libSceAudioOut`.

### `oops_audio_port_t *oops_audio_open(uint32_t sample_rate, uint32_t channels)`
* **When to use**: Open an audio output port (typically 48000 Hz, 2 channels).
* **Returns**: Non-null port handle, or `NULL` on failure.

### `int oops_audio_write(oops_audio_port_t *port, const int16_t *pcm_samples, uint32_t frame_count)`
* **When to use**: Submit interleaved stereo PCM samples (`L, R, L, R...`). Automatically manages hardware chunk boundaries and buffers partial tails.

### `int oops_audio_set_volume(oops_audio_port_t *port, float left, float right)`
* **When to use**: Set volume scalar (`0.0f` to `1.0f`).

### `void oops_audio_close(oops_audio_port_t *port)`
* **When to use**: Flushes remaining samples and closes the port.

---

## 9. Hardware Media Codecs (`<oops/audiodec.h>`, `<oops/videodec.h>`)

Hardware acceleration for video decompression (H.264 / HEVC) and audio decode (AJM coprocessor).

### Video Decode (`<oops/videodec.h>`)
* `oops_videodec_t *oops_videodec_create(const oops_videodec_config_t *config)`: Instantiate hardware H.264 / H.265 video decoder.
* `int oops_videodec_decode(oops_videodec_t *dec, const void *nal_unit, size_t size, oops_videodec_frame_t *out_frame)`: Decompress an access unit to GPU direct memory surface.
* `void oops_videodec_destroy(oops_videodec_t *dec)`: Release codec engine.

### Audio Decode (`<oops/audiodec.h>`)
* `oops_audiodec_t *oops_audiodec_create(const oops_audiodec_config_t *config)`: Initialize MP3 / AAC / ATRAC9 decode channel.
* `int oops_audiodec_decode(...)`: Hardware AJM audio packet decompression.

---

## 10. System UI: Dialogs & On-Screen IME (`<oops/dialog.h>`)

Native system overlays that render directly over the application using the OS compositor.

### On-Screen Virtual Keyboard (IME)
```c
oops_ime_param_t param = {
    .user_id = -1,
    .type = OOPS_IME_TYPE_DEFAULT,
    .title = "Enter Server Address",
    .max_text_len = 64
};
oops_dialog_ime_open(&param);

while (oops_dialog_ime_poll() == OOPS_IME_STATUS_RUNNING) {
    // Continue running frame loop / flip
}

char result_buf[64];
oops_ime_result_t result;
oops_dialog_ime_get_result(result_buf, sizeof(result_buf), &result);
if (result == OOPS_IME_RESULT_OK) {
    obs_klog(result_buf);
}
oops_dialog_ime_close();
```

### System Message Dialogs
* `int oops_dialog_message_show(const char *msg, oops_msg_dialog_type_t type, oops_msg_dialog_button_t buttons)`: Open modal system error/alert box.
* `int oops_dialog_message_poll(oops_msg_dialog_result_t *out_result)`: Non-blocking check for user button selection (`OK`, `YES`, `NO`, `CANCEL`).

---

## 11. Save Data Management (`<oops/savedata.h>`)

Safe, encrypted title save data mounting via `libSceSaveData`.

### `int oops_savedata_init(void)`
* Automatically loads the save data system module.

### `int oops_savedata_mount(const char *dir_name, oops_savedata_mode_t mode, char *out_mount_path, size_t max_path_len)`
* **When to use**: Mount an encrypted save directory to standard POSIX file path (e.g. `/savedata0`).
* **Modes**: `OOPS_SAVEDATA_MODE_READ_ONLY`, `OOPS_SAVEDATA_MODE_READ_WRITE`, `OOPS_SAVEDATA_MODE_CREATE`.
* **Example**:
  ```c
  char mount[64];
  if (oops_savedata_mount("SAVE0000", OOPS_SAVEDATA_MODE_CREATE, mount, sizeof(mount)) == 0) {
      char filepath[128];
      snprintf(filepath, sizeof(filepath), "%s/game.dat", mount);
      int fd = open(filepath, O_WRONLY | O_CREAT, 0644);
      write(fd, my_save_bytes, size);
      close(fd);
      oops_savedata_unmount(mount, true); // commit changes
  }
  ```

### `int oops_savedata_unmount(const char *mount_path, bool commit)`
* **When to use**: Unmounts the save slot. If `commit == true`, encrypts and flushes changes to internal SSD.

---

## 12. Package & Application Management (`<oops/pkg.h>`)

Asynchronous package installation and title management via `libSceAppInstUtil`.

* `int oops_pkg_init(void)`: Initialize installer service.
* `int oops_pkg_install(const char *pkg_path)`: Initiate background installation of a local `.pkg` file.
* `int oops_pkg_get_progress(const char *content_id, uint32_t *out_pct)`: Query percentage (0–100).
* `int oops_app_exists(const char *title_id, bool *out_exists)`: Verify if a title is registered.
* `int oops_app_uninstall(const char *title_id)`: Delete an installed title.

---

## 13. Privilege Escalation & Sandbox Escape (`<oops/escalate.h>`)

Low-level kernel credential escalation and filesystem jailbreak.

> [!NOTE]
> Standard Big App homebrew (games, emulators) run with full GPU and display privileges out of the box and **do not** require privilege escalation. These functions are intended for background maintenance utilities, FTP daemons, and system tools.

### `int oops_jailbreak_process(int pid)`
* **When to use**: Grants full root (`0`) credentials and escapes filesystem sandbox jails:
  * Zeroes `cr_uid`, `cr_ruid`, `cr_svuid`, `cr_rgid`.
  * Sets `cr_sceAuthID = 0x3000000000000001` (`OOPS_SYSTEM_AUTHID`).
  * Sets `cr_sceCaps` to all-ones (`0xFFFFFFFFFFFFFFFF`).
  * Points `fd_rdir` and `fd_jdir` to kernel `rootvnode`.
* **Parameters**: Pass `0` or `-1` to target the calling process.
* **Returns**: `0` on success; `-1` if unsupported or without kernel primitives.

### `int oops_escape_jail(void)`
* **When to use**: Escapes the `/app0` sandbox without modifying Sony credentials, pointing directory descriptors to `/`.

---

## 14. Kernel Read/Write & Syscall Dispatcher (`<oops/krw.h>`, `<oops/syscall.h>`)

Primitives for interacting directly with kernel memory and dispatching system calls through verified libkernel trampolines.

### Syscall Trampoline (`<oops/syscall.h>`)
* `long sys_call(long num, long a1, long a2, long a3, long a4, long a5, long a6)`: Dispatches arbitrary FreeBSD/Prospero syscalls through libkernel's registered trampoline (bypassing PPRBUG-22859 mitigation).

### Kernel Memory Operations (`<oops/escalate.h>`)
* `int oops_kernel_copyout(uint64_t kaddr, void *buf, size_t size)`: Read from arbitrary kernel virtual address.
* `int oops_kernel_copyin(const void *buf, uint64_t kaddr, size_t size)`: Write to arbitrary kernel virtual address.
* `uint64_t oops_kernel_find_proc_by_pid(int pid)`: Walk kernel `allproc` chain to locate a target process struct.

---

## 15. System Modules & Dynamic Linking (`<oops/sysmodule.h>`)

Dynamically load internal PRX modules at runtime using `libSceSysmodule`.

### Module Identifiers (`oops_sysmodule_id_t`)
* `OOPS_SYSMODULE_PAD` (`0x0027`)
* `OOPS_SYSMODULE_AUDIO_OUT` (`0x00A0`)
* `OOPS_SYSMODULE_SAVE_DATA` (`0x0090`)
* `OOPS_SYSMODULE_IME_DIALOG` (`0x0093`)
* `OOPS_SYSMODULE_MSG_DIALOG` (`0x0094`)
* `OOPS_SYSMODULE_NET_CTL` (`0x0014`)

### `int oops_sysmodule_load(oops_sysmodule_id_t id)`
* Loads the requested system module into the process address space.

### `int oops_sysmodule_unload(oops_sysmodule_id_t id)`
* Unloads a previously loaded module.

---

## 16. System Information & Telemetry (`<oops/system.h>`, `<oops/offsets.h>`)

Query hardware metrics, console firmware versions, and kernel offset tables.

### `int oops_system_get_info(oops_system_info_t *info)`
* Retrieves:
  * `firmware_raw`: Hex version (e.g. `0x12400000` for FW 12.40).
  * `firmware_str`: Human-readable version string (`"12.40"`).
  * `model`: Hardware generation (`OOPS_HW_PROSPERO` or `OOPS_HW_ORBIS`).
  * `soc_revision`: Processor silicon stepping.

### `int oops_system_get_telemetry(oops_hw_telemetry_t *telemetry)`
* Reads live SoC thermal sensors, fan speed (RPM), memory clocks, and power state.

---

## 17. High-Resolution Timing (`<oops/time.h>`)

Monotonic timestamps and microsecond sleeps based on x86 invariant TSC frequency.

* `uint64_t oops_time_monotonic_raw(void)`: Unadjusted TSC counter (`rdtsc`).
* `uint64_t oops_time_monotonic_us(void)`: Microseconds elapsed since boot.
* `uint64_t oops_time_monotonic_ms(void)`: Milliseconds elapsed since boot.
* `uint64_t oops_time_get_frequency(void)`: Invariant TSC tick frequency in Hz.
* `void oops_time_sleep_us(uint64_t us)`: Microsecond sleep delay.
* `void oops_time_sleep_ms(uint32_t ms)`: Millisecond sleep delay.

---

## 18. Threading & Synchronization (`<oops/thread.h>`)

Clean-room POSIX-compatible threading primitives backed by kernel `sys_thr_*` syscalls.

* `int oops_thread_create(oops_thread_t *thread, const char *name, void *(*func)(void *), void *arg, int priority, size_t stack_size)`
* `int oops_thread_join(oops_thread_t thread, void **out_val)`
* `int oops_mutex_init(oops_mutex_t *mtx)`
* `int oops_mutex_lock(oops_mutex_t *mtx)`
* `int oops_mutex_unlock(oops_mutex_t *mtx)`
* `int oops_cond_init(oops_cond_t *cond)`
* `int oops_cond_wait(oops_cond_t *cond, oops_mutex_t *mtx)`
* `int oops_cond_signal(oops_cond_t *cond)`

---

## 19. BSD Sockets & Network Telemetry (`<oops/net.h>`, `<oops/netctl.h>`)

POSIX socket wrappers for networking, paired with `libSceNetCtl` interface telemetry and freestanding RFC 1035 DNS resolution.

### Socket Interface (`<oops/net.h>`)
* `int oops_socket(int domain, int type, int protocol)`: Open a BSD socket endpoint (`OOPS_AF_INET`, `OOPS_SOCK_STREAM` or `OOPS_SOCK_DGRAM`).
* `int oops_connect(int sock, const char *server_ip, uint16_t port)`: Connect to remote host. Transparently resolves domain hostnames (e.g. `"api.example.com"`) to IPv4 addresses via `oops_net_resolve`.
* `int oops_bind(int sock, const char *ip, uint16_t port)`: Bind socket to local address and port.
* `int oops_listen(int sock, int backlog)`: Listen for incoming connections.
* `int oops_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port)`: Accept incoming client connection.
* `long oops_send(int sock, const void *buf, size_t len, int flags)`: Send data over stream socket.
* `long oops_recv(int sock, void *buf, size_t len, int flags)`: Receive data from stream socket.
* `long oops_sendto(int sock, const void *buf, size_t len, int flags, const char *to_ip, uint16_t to_port)`: Send UDP datagram.
* `long oops_recvfrom(int sock, void *buf, size_t len, int flags, char *from_ip, size_t ip_len, uint16_t *from_port)`: Receive UDP datagram.
* `int oops_set_nonblocking(int sock, int nonblocking)`: Toggle non-blocking I/O mode.
* `int oops_net_would_block(long rc)`: Check if an operation would block (`EAGAIN` / `EWOULDBLOCK`).
* `void oops_close(int sock)`: Close socket descriptor.

### DNS Hostname Resolution (`<oops/net.h>`)
* `int oops_net_resolve(const char *hostname, char *out_ip, size_t out_len)`:
  * **When to use**: To resolve a domain name (e.g. `"archive.org"`) into a dotted-quad IPv4 string (`"207.241.224.2"`).
  * **Target behavior**: Queries `netctl` DNS servers first, with automatic fallback to public resolvers (`1.1.1.1`, `8.8.8.8`) via clean-room UDP RFC 1035 query engine.
* `int oops_dns_build_query(const char *hostname, uint16_t tx_id, uint8_t *out_buf, size_t max_len)`: Encodes an RFC 1035 A-record DNS query packet.
* `int oops_dns_parse_response(const uint8_t *resp, size_t resp_len, uint16_t expected_tx_id, char *out_ip, size_t out_len)`: Parses an RFC 1035 DNS response, handling label compression pointers and CNAME chains.

### Network Control (`<oops/netctl.h>`)
* `int oops_net_ctl_init(void)`: Initialize network control subsystem and load `libSceNetCtl`.
* `int oops_net_ctl_get_info(oops_net_info_t *out_info)`: Query active IP, netmask, default gateway, primary/secondary DNS, MAC address, link state, and Wi-Fi RSSI percentage.

---

## 20. Freestanding C Runtime Utilities (`<oops/freestd.h>`)

Zero-libc implementations of memory, string, formatting, and hashing primitives.

* **Memory**: `void *obs_memset(void *s, int c, size_t n)`, `void *obs_memcpy(void *d, const void *s, size_t n)`, `int obs_memcmp(...)`
* **String**: `size_t obs_strlen(const char *s)`, `int obs_strcmp(...)`, `int obs_strncmp(...)`, `char *obs_strcpy(...)`, `char *obs_strncpy(...)`
* **Numeric Formatting**: `size_t obs_format_hex(char *buf, uint64_t val)`, `size_t obs_format_i64(char *buf, int64_t val)`, `size_t obs_format_u64(...)`
* **String Formatting**:
  * `int oops_snprintf(char *buf, size_t size, const char *fmt, ...)`: Standard clean-room `snprintf` supporting `%s`, `%c`, `%d`, `%i`, `%u`, `%x`, `%X`, `%p`, `%%`, padding width, zero-fill, and length modifiers (`l`, `ll`, `z`).
  * `int oops_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)`: Variadic list string formatting.
* **NID Hashing**: `void obs_compute_nid(const char *name, char out_nid[12])`: Generates canonical Sony NID hashes verified against SELFish format authority.

---

## 21. Process Control & Code Injection (`<oops/inject.h>`, `<oops/procctl.h>`, `<oops/procparam.h>`)

Cross-process debugging, process hooking, and remote dynamic ELF loading.

* `int oops_inject_elf(pid_t target_pid, const uint8_t *elf_data, size_t elf_size, const char *entry_symbol)`: Maps and executes an ELF payload inside another running process.
* `int oops_proc_kill(pid_t pid, int sig)`: Terminate process.
* `int oops_proc_get_name(pid_t pid, char *out_name, size_t max_len)`: Retrieve executable name.

---

## 22. High-Level Filesystem Subsystem (`<oops/fs.h>`)

Freestanding POSIX-conforming filesystem layer wrapping kernel syscalls (`SYS_open`, `SYS_close`, `SYS_read`, `SYS_write`, `SYS_lseek`, `SYS_mkdir`, `SYS_unlink`, `SYS_stat`).

### Core Functions
* `int oops_fs_open(const char *path, int flags, int mode)`: Open file descriptor. Supports `OOPS_O_RDONLY`, `OOPS_O_WRONLY`, `OOPS_O_RDWR`, `OOPS_O_CREAT`, `OOPS_O_TRUNC`, `OOPS_O_APPEND`.
* `int oops_fs_close(int fd)`: Close file descriptor.
* `long oops_fs_read(int fd, void *buf, size_t count)`: Read data from file.
* `long oops_fs_write(int fd, const void *buf, size_t count)`: Write data to file.
* `int64_t oops_fs_seek(int fd, int64_t offset, int whence)`: Seek within file (`OOPS_SEEK_SET`, `OOPS_SEEK_CUR`, `OOPS_SEEK_END`).
* `int64_t oops_fs_tell(int fd)`: Return current byte offset within file.
* `bool oops_fs_exists(const char *path)`: Check if a path exists on disk.
* `int64_t oops_fs_file_size(const char *path)`: Determine file size in bytes without opening for read.
* `int oops_fs_read_all(const char *path, void **out_data, size_t *out_size)`: Slurp entire file into a buffer allocated from `oops_malloc`. Call `oops_fs_free_data` when done.
* `void oops_fs_free_data(void *data)`: Free buffer allocated by `oops_fs_read_all`.
* `int oops_fs_write_all(const char *path, const void *data, size_t size)`: Write an entire memory buffer to disk, replacing existing contents.
* `int oops_fs_mkdir(const char *path, int mode)`: Create a directory.
* `int oops_fs_unlink(const char *path)`: Remove a file from disk.

---

## 23. Freestanding Userland Heap Allocator (`<oops/heap.h>`)

Clean-room segregated-fit slab and anonymous virtual memory allocator.

### Category 65536 & Direct Memory Independence
On the PS5, System Applications (Category `65536` titles, e.g. UI overlays, background daemons, and system launch payloads) are allocated **0 bytes of Direct Memory (DMEM)** by the OS. Standard PlayStation SDK memory calls fail instantly in this environment.
`oops-sdk`'s heap allocator solves this by backing allocations with anonymous virtual memory (`SYS_mmap 477` with `MAP_PRIVATE | MAP_ANON 0x1002`). It operates identically in game titles (`gd`), system titles, and elfldr payloads.

### Allocator Functions
* `void *oops_malloc(size_t size)`: Allocate memory with 16-byte alignment. Allocations <= 2048 bytes use cached slabs (32, 64, 128, 256, 512, 1024, 2048 bytes); larger allocations are mapped directly with virtual memory pages.
* `void oops_free(void *ptr)`: Free previously allocated memory block. Safe to pass `NULL`.
* `void *oops_calloc(size_t num, size_t size)`: Allocate zeroed memory buffer with integer overflow protection.
* `void *oops_realloc(void *ptr, size_t new_size)`: Resize existing allocation, preserving contents.
* `void oops_heap_get_stats(oops_heap_stats_t *stats)`: Query live heap telemetry (`allocated_bytes`, `reserved_bytes`, `active_allocations`, `mmap_chunks`).

---

## 24. Freestanding Math & 3D Linear Algebra (`<oops/math.h>`)

Clean-room, zero-libc scalar mathematics, 3D vector operations, and 4x4 transform matrices matching RDNA2 PM4 uniform layout.

### Scalar Operations
* `float oops_fabsf(float x)`: Absolute value.
* `float oops_sqrtf(float x)`: Square root.
* `float oops_clampf(float x, float min, float max)`: Clamp value between bounds.
* `float oops_lerpf(float a, float b, float t)`: Linear interpolation.
* `float oops_floorf(float x)`, `float oops_ceilf(float x)`: Truncation and ceiling.
* `float oops_fmodf(float x, float y)`: Floating-point remainder.
* `float oops_sinf(float x)`, `float oops_cosf(float x)`: Quadrant-reduced polynomial trigonometric evaluations.
* `float oops_tanf(float x)`, `float oops_atan2f(float y, float x)`: Tangent and two-argument arctangent.
* `float oops_expf(float x)`, `float oops_logf(float x)`, `float oops_powf(float base, float exp)`: Exponential, natural logarithm, and power.

### 3D Vector Operations (`oops_vec3_t`)
* `oops_vec3_make(float x, float y, float z)`: Construct 3D vector.
* `float oops_vec3_dot(oops_vec3_t a, oops_vec3_t b)`: Vector dot product.
* `oops_vec3_t oops_vec3_cross(oops_vec3_t a, oops_vec3_t b)`: Vector cross product.
* `float oops_vec3_length(oops_vec3_t v)`: Vector magnitude.
* `oops_vec3_t oops_vec3_normalize(oops_vec3_t v)`: Unit normal vector.

### 4x4 Matrix Transforms (`oops_mat4_t`)
* Column-major 16-element float matrix layout directly compatible with OpenGL and RDNA2 AGC vertex constant buffers.
* `void oops_mat4_identity(oops_mat4_t *out)`: Load identity matrix.
* `void oops_mat4_mul(oops_mat4_t *out, const oops_mat4_t *a, const oops_mat4_t *b)`: Matrix multiply ($out = a \times b$).
* `void oops_mat4_perspective(oops_mat4_t *out, float fovy_rad, float aspect, float z_near, float z_far)`: Symmetrical perspective projection matrix.
* `void oops_mat4_ortho(oops_mat4_t *out, float l, float r, float b, float t, float n, float f)`: Orthographic projection matrix.
* `void oops_mat4_lookat(oops_mat4_t *out, oops_vec3_t eye, oops_vec3_t center, oops_vec3_t up)`: Camera view transform matrix.
* `void oops_mat4_translate(oops_mat4_t *out, float tx, float ty, float tz)`: Translation transform.
* `void oops_mat4_rotate(oops_mat4_t *out, float angle_rad, float rx, float ry, float rz)`: Arbitrary axis rotation transform.
* `void oops_mat4_scale(oops_mat4_t *out, float sx, float sy, float sz)`: Scale transform.

