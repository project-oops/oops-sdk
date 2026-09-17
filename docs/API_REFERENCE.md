# OOPS SDK Complete API Reference

**Clean-Room Freestanding C Runtime and Hardware Abstraction SDK for Orbis & Prospero.**

This document provides a comprehensive technical API reference for all subsystems, data structures, and functions exposed by `oops-sdk`.

---

## Table of Contents

1. [Display & Video Output (`<oops/display.h>`)](#1-display--video-output-oopsdisplayh)
2. [2D Software Drawing Canvas (`<oops/draw.h>`)](#2-2d-software-drawing-canvas-oopsdrawh)
3. [Fixed-Function 3D Instrument (`<GL/gl.h>`)](#3-fixed-function-3d-instrument-glglh)
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
21. [Process Control & Code Injection (`<oops/inject.h>`)](#21-process-control--code-injection-oopsinjecth)
22. [High-Level Filesystem Subsystem (`<oops/fs.h>`)](#22-high-level-filesystem-subsystem-oopsfsh)
23. [Freestanding Userland Heap Allocator (`<oops/heap.h>`)](#23-freestanding-userland-heap-allocator-oopsheaph)
24. [Freestanding Math & 3D Linear Algebra (`<oops/math.h>`)](#24-freestanding-math--3d-linear-algebra-oopsmathh)
25. [Target Platform Identification (`<oops/target.h>`)](#25-target-platform-identification-oopstargeth)

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
      oops_klog("APP", "Display failed to open!\n");
  }
  ```

### `int oops_display_is_ready(const oops_display_t *disp)`
* **Returns**: `1` if the display was opened and video buffers are registered with SceVideoOut; `0` on failure.

### `int oops_display_is_gpu_accelerated(const oops_display_t *disp)`
* **Returns**: `1` if backed by hardware GPU queues (AGC/GNM); `0` if running on host fallback.

### `uint32_t *oops_display_get_framebuffer(oops_display_t *disp)`
* **When to use**: Direct 32bpp ARGB pixel access to the back buffer before flipping.
* **Returns**: Pointer to the active back buffer pixels in write-combined direct memory.

### `unsigned int oops_display_get_width(const oops_display_t *disp)`
### `unsigned int oops_display_get_height(const oops_display_t *disp)`
* **Returns**: The opened display's dimensions.

### `uint64_t oops_display_get_flip_count(const oops_display_t *disp)`
* **Returns**: Completed flips as the hardware counts them, or submitted flips on a backend where the status query is unavailable.

### `int oops_display_get_last_error(const oops_display_t *disp)`
* **Returns**: The last backend error code recorded for this display.

### `const char *oops_display_get_backend_name(const oops_display_t *disp)`
* **Returns**: A human-readable name for the backend actually in use (`"agc"`, `"gnm"`, or the host fallback's name).

### `int oops_display_get_video_handle(const oops_display_t *disp)`
* **Returns**: The underlying platform video-out handle, for callers that need to pass it to another system library.

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

### `oops_surface_t oops_surface_from_sprite(const oops_sprite_t *sprite)`
* **When to use**: Wrap a build-time sprite (32-bit pixels compiled into `.rodata`) as a read-only source surface, without copying. Use the result only as a blit source. Returns an empty surface (`NULL` pixels) for a `NULL` sprite.

### `void oops_draw_clear(oops_surface_t *surf, oops_color_t color)`
* **When to use**: Clear the surface with a 32-bit ARGB color (`OOPS_RGB(r, g, b)` or `OOPS_RGBA(r, g, b, a)`).

### `void oops_draw_pixel(oops_surface_t *surf, int x, int y, oops_color_t color)`
* **When to use**: Set a single clipped pixel.

### `void oops_draw_rect(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color)`
* **When to use**: Fill a solid rectangular area with clipping. This is the only rectangle fill call — there is no separate stroked-outline function; draw a smaller inset rect on top for a border effect.

### `void oops_draw_rect_blend(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color)`
* **When to use**: Fill a rectangle compositing `color` over what is already there (straight-alpha source-over). Alpha 0 draws nothing, 255 is a solid fill.

### `void oops_draw_rect_gradient(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color_a, oops_color_t color_b, int vertical)`
* **When to use**: Fill a rectangle with a linear gradient from `color_a` to `color_b`, top-to-bottom when `vertical` is non-zero and left-to-right otherwise.

### `void oops_draw_line(oops_surface_t *surf, int x0, int y0, int x1, int y1, oops_color_t color)`
* **When to use**: Bresenham line rasterization between two points.

### `void oops_draw_circle(oops_surface_t *surf, int cx, int cy, int radius, oops_color_t color, int filled)`
* **When to use**: Draw a circle using a midpoint algorithm. `filled` selects a solid disc (non-zero) or a one-pixel outline (zero) — there is no separate `oops_draw_fill_circle` call.

### Blended variants
* `void oops_draw_pixel_blend(oops_surface_t *surf, int x, int y, oops_color_t color)`
* `void oops_draw_line_blend(oops_surface_t *surf, int x0, int y0, int x1, int y1, oops_color_t color)`
* `void oops_draw_circle_blend(oops_surface_t *surf, int cx, int cy, int radius, oops_color_t color, int filled)`
* **When to use**: Composite `color` over the destination with its own alpha (source-over), for translucent overlays.

### `int oops_draw_text(oops_surface_t *surf, int x, int y, const char *text, oops_color_t color, int scale)`
* **When to use**: Render text using the embedded 8x8 console font (ASCII `0x20`-`0x7E`, upper and lower case).
* **Parameters**: `scale` is an integer multiplier (1 = 8x8, 2 = 16x16, etc.).
* **Returns**: The x position after the last glyph.

### `int oops_draw_text_width(const char *text, int scale)`
* **When to use**: Measure the pixel width `oops_draw_text` would occupy (the longest line, if multi-line) without drawing, so a caller can centre or right-align text.

### `void oops_draw_blit(oops_surface_t *dst, int dx, int dy, const oops_surface_t *src, int sx, int sy, int sw, int sh)`
### `void oops_draw_blit_blend(oops_surface_t *dst, int dx, int dy, const oops_surface_t *src, int sx, int sy, int sw, int sh)`
* **When to use**: Blit a sub-rectangle from a source surface (or sprite, via `oops_surface_from_sprite`) to the destination. `oops_draw_blit` ignores source alpha; `oops_draw_blit_blend` performs full per-pixel alpha blending.

### `int oops_png_decode(const void *png_data, size_t png_size, uint32_t *out_pixels, uint32_t target_w, uint32_t target_h, uint32_t *out_orig_w, uint32_t *out_orig_h)`
* **When to use**: Decode a PNG image from memory into 32bpp ARGB pixels, optionally resampling to `target_w`x`target_h`. Pass `0`/`0` to decode at original dimensions. Returns `0` on success, a negative error code on failure.

---

## 3. Fixed-Function 3D Instrument (`<GL/gl.h>`)

`oops-gl` is a clean-room, hardware-accelerated fixed-function 3D pipeline that maps GL-shaped calls directly to RDNA2 AGC command buffers (PM4 DCBs) with no vendor graphics driver underneath.

**It is not an OpenGL version, and it is not the GL to write an application against.** D007 scoped it to OpenGL 1.1-class fixed function; **D008 (2026-09-17) superseded that instrument-only scope** and grew it toward 1.x and 2.x. As of this writing its surface includes immediate mode, vertex arrays, buffer objects (GL 1.5), one 2D texture unit, lighting and materials, the matrix stacks, blend/depth/cull, alpha test, and display lists. **Multitexturing, GLSL shaders (GL 2.0), stencil, and fog are still absent** - each needs a shader-interface change that risks the pinned gl-cube oracle frame and has to be re-recorded on hardware (D009). Points, lines, and `glPolygonMode` wireframe are measured shut: the hardware's primitive assembler stalls rather than draws them. See [`docs/GL_ROADMAP.md`](GL_ROADMAP.md) for the measured, not remembered, gap against Mesa's declared entry points.

What it is *for* is evidence: because nothing sits between the call and the packet, its command stream is readable as a hardware record. `docs/hardware/agc-gl-cube-oracle-fw1240.md` is one such record, and orbistoun checks its own RDNA2 translation against it.

**For applications, use [oops-mesa](../../oops-mesa/), which provides OpenGL 3.3 Core and GLSL 3.30** through upstream Mesa and radeonsi. The two never link into the same title.

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
* `void glSetHardwarePrelude(const GLuint *words, GLuint count)`: Words every later frame's command stream opens with, ahead of oops-gl's own state. An unvalidated experiment hook so another driver's preamble can be put in front of this one and measured.

---

## 4. Hardware RDNA2 AGC Graphics (`<oops/agc.h>`, `<oops/gpu.h>`)

Low-level bare-metal access to PS5 RDNA2 GFX10.3 hardware command queues, compute dispatchers, and tile micro-swizzling.

### Queues and shaders (`<oops/gpu.h>`)

### `int oops_gpu_available(void)`
* **Returns**: `1` if native AGC graphics hardware is present; `0` if running on host/emulator.

### `oops_gpu_queue_t *oops_gpu_create_compute_queue(void)`
### `oops_gpu_queue_t *oops_gpu_create_graphics_queue(void)`
* **When to use**: Create an AGC hardware compute (Type 3) or universal graphics (Type 0) queue. Allocates direct coherent Onion WB memory for command stream buffers and synchronization fences. There is no separate "init a caller-owned queue struct" call — the queue object itself is allocated and returned.
* **Returns**: A queue handle, or `NULL` on host or if creation fails.

### `void oops_gpu_destroy_queue(oops_gpu_queue_t *queue)`
* **When to use**: Flush, wait on pending fences, and destroy a GPU queue.

### `oops_gpu_shader_t *oops_gpu_create_shader(const void *container_hdr, size_t hdr_size, const void *payload, size_t payload_size)`
* **When to use**: Instantiate an RDNA2 GFX10.3 compute shader from its container header and bytecode payload via `sceAgcCreateShader`.

### `void oops_gpu_destroy_shader(oops_gpu_shader_t *shader)`

### `int oops_gpu_dispatch(oops_gpu_queue_t *queue, const oops_gpu_dispatch_t *dispatch)`
* **When to use**: Execute a compute dispatch: emits the shader-program and user-data `SET_SH_REG` packets, `DISPATCH_DIRECT`, and a `RELEASE_MEM` end-of-pipe fence, then submits and waits on retirement.

### `uint32_t oops_gpu_get_last_fence(const oops_gpu_queue_t *queue)`
* **Returns**: The latest retired fence value from the queue.

### `static inline int oops_agc_dispatch_compute(oops_gpu_queue_t *queue, const oops_gpu_shader_t *shader, uint32_t grid_x, uint32_t grid_y, uint32_t grid_z, const uint32_t *user_data, uint32_t user_data_count)` (`<oops/agc.h>`)
* **When to use**: Convenience wrapper around `oops_gpu_dispatch()` that builds the `oops_gpu_dispatch_t` from explicit grid dimensions and user-data registers.

### `int oops_agc_draw_primitive(oops_gpu_queue_t *queue, const oops_agc_draw_desc_t *desc)` (`<oops/agc.h>`)
* **When to use**: Emit hardware 3D primitive draw packets to a Type 0 universal graphics queue: fixed-function context registers, UCONFIG parameter cache, shader bindings, `DRAW_INDEX_AUTO`, a `RELEASE_MEM` end-of-pipe flush, and a wait for fence retirement. Returns `0` on success, negative on error.

### Micro-Tile Swizzling (`<agc/tiler.h>`, not `<oops/agc_tiler.h>` — the tiler lives under `agc/`, and its functions carry no `oops_` prefix)
* `size_t agc_tile_surface_bytes(uint32_t width, uint32_t height)`: Bytes a tiled destination must hold for a `width`x`height` surface.
* `void agc_tile_init(void)`: Builds the swizzle lookup table. Idempotent; `agc_tile_surface()` calls it on first use.
* `void agc_tile_surface(void *dest, const void *src, uint32_t width, uint32_t height)`: Converts a linear 32bpp RGBX raster buffer into the RDNA2 64 KB micro-tiled display scanout layout (`kRenderTarget = 27`, `64KB_R_X`).
* `static inline void agc_detile_pixel(uint32_t offset_dwords, uint32_t *out_x, uint32_t *out_y)`: Closed-form inverse — recovers the (x, y) pixel a dword offset within a tile came from.

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
* `int oops_mem_batch_map(void *vaddr_base, int64_t phys_base, size_t total_size, size_t page_size, uint8_t prot)`: Map a run of same-sized pages in one call, for a caller that already knows its page size instead of mapping one region at a time.
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
* **When to use**: Open pad 0 for the initial user. A failure is remembered and reported again by later calls until `oops_input_close()`.

#### `int oops_input_poll(unsigned int port, oops_pad_state_t *out_state)`
* **When to use**: Read the latest instantaneous gamepad state once per frame.
* **State fields** (`oops_pad_state_t`):
  * `buttons`: Bitmask (`OOPS_BUTTON_CROSS`, `OOPS_BUTTON_CIRCLE`, `OOPS_BUTTON_TRIANGLE`, `OOPS_BUTTON_SQUARE`, `OOPS_BUTTON_L1`, `OOPS_BUTTON_R1`, `OOPS_BUTTON_L2`, `OOPS_BUTTON_R2`, `OOPS_BUTTON_L3`, `OOPS_BUTTON_R3`, `OOPS_BUTTON_UP`, `OOPS_BUTTON_DOWN`, `OOPS_BUTTON_LEFT`, `OOPS_BUTTON_RIGHT`, `OOPS_BUTTON_OPTIONS`, `OOPS_BUTTON_TOUCHPAD`). `OOPS_BUTTON_CREATE` (bit 16) exists but is contested — this SDK holds it unconfirmed pending an obSCEne button-bits sweep with a controller attached; do not bind it as a shell/system button yet.
  * `left_stick_x`, `left_stick_y`, `right_stick_x`, `right_stick_y`: `int8_t`, -128 to 127.
  * `l2_trigger`, `r2_trigger`: `uint8_t`, 0 to 255.
  * `connected`: non-zero if the pad answered.
  * `touch[2]`: Capacitive touch points (`x`: 0-1919, `y`: 0-941, `id`, `active`).
  * `orientation[4]`: Quaternion `[x, y, z, w]`.
  * `acceleration[3]`: Accelerometer, in G's.
  * `angular_velocity[3]`: Gyroscope, in rad/s.

#### `int oops_input_poll_batch(unsigned int port, oops_pad_state_t *out_states, unsigned int max_samples)`
* **When to use**: Batched low-latency read — fills up to `max_samples` (cap `OOPS_MAX_PAD_SAMPLES`, 64) states from the driver in one request, oldest first, to preserve a press-and-release that falls between two per-frame polls. Returns the driver's count, or `-1` when the port isn't open.

#### `int oops_input_set_rumble(unsigned int port, uint8_t small_motor, uint8_t large_motor)`
#### `int oops_input_set_lightbar(unsigned int port, uint8_t r, uint8_t g, uint8_t b)`
* **When to use**: Control vibration motors and the RGB LED lightbar.

#### `int oops_input_reset_orientation(unsigned int port)`
* **When to use**: Re-zero the orientation quaternion to the pad's current pose.

#### `int oops_input_adaptive_triggers_available(unsigned int port)`
* **Returns**: `1`/`0` — whether the adaptive-trigger entry point resolves for this open port. Does not confirm the pad is a DualSense.

#### `int oops_input_set_trigger_effect(unsigned int port, unsigned int triggers, int mode, uint8_t position, uint8_t position_end, uint8_t strength, uint8_t frequency)`
* **When to use**: Apply an adaptive-trigger effect to L2/R2 (`triggers` is a mask of `OOPS_TRIGGER_L2`/`OOPS_TRIGGER_R2`). `mode` is one of `OOPS_TRIGGER_OFF`/`FEEDBACK`/`WEAPON`/`VIBRATION`. **Capture-gated**: the entry point is confirmed, its parameter layout is not, so this currently returns a negative code rather than pass a guessed struct.

#### `void oops_input_close(void)`

### Keyboard (`<oops/keyboard.h>`)
* `int oops_keyboard_init(void)`, `int oops_keyboard_available(void)`, `void oops_keyboard_close(void)`: Lifecycle.
* `int oops_keyboard_read(oops_key_event_t *out_events, unsigned int max_events)`: Read raw USB keycode presses and releases.
* `uint32_t oops_keyboard_poll_buttons(void)`: Poll the current bitmask of held keys.

### Mouse (`<oops/mouse.h>`)
* `int oops_mouse_init(void)`, `int oops_mouse_available(void)`, `void oops_mouse_close(void)`: Lifecycle.
* `int oops_mouse_read(oops_mouse_state_t *out_samples, unsigned int max_samples)`: Batched read of buffered relative-motion/button samples, like `oops_input_poll_batch` — not a single-state read.

---

## 8. Audio Streaming (`<oops/audio.h>`)

Direct stereo 16-bit PCM streaming. One port, 16-bit signed interleaved stereo only — a channel count the write path can't honour is refused rather than stored.

### `oops_audio_port_t *oops_audio_open(int sample_rate, int channels, int buffer_frames)`
* **When to use**: Open the output port. `buffer_frames` is rounded to a multiple of 256 within 256..2048; every size in that range is accepted at 48000 Hz on FW 12.40, and 44100 Hz is refused.
* **Returns**: Non-null port handle, or `NULL` on failure — check `oops_audio_get_last_error()` for the reason.

### `int oops_audio_get_last_error(void)`
* **Returns**: One of `OOPS_AUDIO_OK`, `OOPS_AUDIO_EUNAVAIL` (platform audio output didn't resolve), `OOPS_AUDIO_EPARAM` (a caller argument was rejected), `OOPS_AUDIO_EBUSY` (the port is already open).

### `int oops_audio_get_chunk_frames(const oops_audio_port_t *port)`
* **Returns**: Frames per hardware chunk after rounding; writing multiples of it never leaves a partial tail.

### `int oops_audio_write(oops_audio_port_t *port, const int16_t *pcm_samples, size_t frame_count)`
* **When to use**: Queue interleaved stereo PCM samples (`L, R, L, R...`). A write hands over whole chunks and holds a partial tail until the next write completes it or `oops_audio_flush()` pads it with silence. Writes block once the hardware queue is full.

### `int oops_audio_flush(oops_audio_port_t *port)`
* **When to use**: Emit a held partial chunk, padded with silence — needed to hear the end of a one-shot sound written in an odd-sized chunk.

### `int oops_audio_set_volume(oops_audio_port_t *port, float left, float right)`
* **When to use**: Set volume scalar (`0.0f` to `1.0f`).

### `void oops_audio_close(oops_audio_port_t *port)`
* **When to use**: Flushes the held tail, drains the queued chunks, and releases the handle. Safe on `NULL`.

---

## 9. Hardware Media Codecs (`<oops/audiodec.h>`, `<oops/videodec.h>`)

Hardware acceleration for video decompression (H.264 / HEVC) and audio decode (AJM coprocessor).

### Video Decode (`<oops/videodec.h>`)
* `int oops_videodec_available(void)`: Whether the hardware decode path resolves here.
* `oops_videodec_t *oops_videodec_open(int codec, uint32_t width, uint32_t height)`: Instantiate a hardware H.264 / HEVC video decoder.
* `int oops_videodec_decode(oops_videodec_t *dec, const void *au, size_t au_size, oops_videodec_frame_t *out_frame)`: Decompress an access unit to a GPU direct-memory surface.
* `void oops_videodec_close(oops_videodec_t *dec)`: Release a decoder opened with `oops_videodec_open`. Safe on `NULL`.
* `int oops_videodec_last_error(void)`: Reason for the last failure.
* **Capture-gated**: `oops_videodec_open`/`oops_videodec_decode` are the settled interface but return `OOPS_VIDEODEC_ELAYOUT` until an obSCEne struct-layout probe confirms the config/input/output layouts.

### Audio Decode (`<oops/audiodec.h>`)
* `int oops_audiodec_available(void)`, `int oops_audiodec_offload_available(void)`: Whether hardware decode, and the `libSceAjm` offload path specifically, resolve here.
* `oops_audiodec_t *oops_audiodec_open(int codec)`: Initialize an MP3 / AAC decode channel.
* `int oops_audiodec_decode(oops_audiodec_t *dec, const void *au, size_t au_size, int16_t *pcm_out, size_t pcm_capacity)`: Hardware AJM audio packet decompression into caller-owned PCM.
* `void oops_audiodec_close(oops_audiodec_t *dec)`: Release a decoder opened with `oops_audiodec_open`. Safe on `NULL`.
* `int oops_audiodec_last_error(void)`: Reason for the last failure.
* **Capture-gated**, same as video decode: returns `OOPS_AUDIODEC_ELAYOUT` until a struct-layout probe confirms the shapes; the interface itself is settled.

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
    oops_klog("IME", result_buf);
}
oops_dialog_ime_close();
```
`oops_ime_param_t` also has `placeholder` (UTF-8 placeholder text, or `NULL`) and `pos_x`/`pos_y` fields not shown above. `void oops_dialog_ime_abort(void)` cancels a running IME session without waiting for the user.

### System Message Dialogs
* `int oops_dialog_message_show(const char *msg, oops_msg_dialog_type_t type, oops_msg_dialog_button_t buttons)`: Open modal system error/alert box.
* `int oops_dialog_message_poll(oops_msg_dialog_result_t *out_result)`: Non-blocking check for user button selection (`OK`, `YES`, `NO`, `CANCEL`).
* `void oops_dialog_message_close(void)`: Close a message dialog once its result has been read.

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

### `void oops_savedata_term(void)`
* **When to use**: Unload the save data system module at shutdown.

---

## 12. Package & Application Management (`<oops/pkg.h>`)

Asynchronous package installation and title management via `libSceAppInstUtil`.

* `int oops_pkg_init(void)`: Initialize installer service.
* `void oops_pkg_term(void)`: Shut down the installer service.
* `int oops_pkg_install(const char *pkg_path)`: Initiate background installation of a local `.pkg` file.
* `int oops_pkg_get_progress(const char *content_id, uint32_t *out_pct)`: Query percentage (0–100).
* `int oops_pkg_get_progress_info(const char *content_id, oops_pkg_progress_info_t *out_info)`: Query the fuller progress record behind the percentage.
* `int oops_app_exists(const char *title_id, bool *out_exists)`: Verify if a title is registered.
* `int oops_app_uninstall(const char *title_id)`: Delete an installed title.

---

## 13. Privilege Escalation & Sandbox Escape (`<oops/escalate.h>`)

Low-level kernel credential escalation and filesystem jailbreak.

> [!NOTE]
> Standard Big App homebrew (games, emulators) run with full GPU and display privileges out of the box and **do not** require privilege escalation. These functions are intended for background maintenance utilities, FTP daemons, and system tools.

### `int oops_kernel_rw_init(void)`
* **When to use**: Establish this header's own kernel read/write prerequisites. Call before `oops_jailbreak_process` or the copyin/copyout primitives below.

### `int oops_kernel_pipe_init(int rwpipe[2], int rwpair[2], uint64_t kpipe_addr, ...)`
* **When to use**: Set up the pipe/socket kernel read-write leak primitive `oops_kernel_copyin`/`oops_kernel_copyout` run on top of.

### `uint64_t oops_kernel_get_current_proc(void)`
* **Returns**: The calling process's own kernel `proc` struct address.

### `int oops_jailbreak_process(int pid)`
* **When to use**: Grants full root (`0`) credentials and escapes filesystem sandbox jails:
  * Zeroes `cr_uid`, `cr_ruid`, `cr_svuid`, `cr_ngroups`, `cr_rgid`.
  * Sets `cr_sceAuthID = 0x3000000000000001` (`OOPS_SYSTEM_AUTHID`).
  * Sets `cr_sceCaps` to all-ones (`0xFFFFFFFFFFFFFFFF`).
  * Points `fd_rdir` and `fd_jdir` to kernel `rootvnode`.
* **Parameters**: Pass `0` or `-1` to target the calling process.
* **Returns**: `0` on success; `-1` if unsupported or without kernel primitives.

### `int oops_escalate_to_system_authid(void)`
* **When to use**: The narrower escalation of just `cr_sceAuthID`, without the rest of `oops_jailbreak_process`'s credential and directory changes.

### `int oops_has_system_authid(void)`
* **Returns**: `1`/`0` — whether the calling process already carries `OOPS_SYSTEM_AUTHID`.

### `int oops_system_escape_sandbox(void)` (`<oops/system.h>`)
* **When to use**: Explicit opt-in sandbox elevation for system launchers and file managers needing global filesystem access (`/user/app`, `/data/homebrew`, `/user/appmeta`).
* **Mechanism**: Connects via loopback TCP (`127.0.0.1:9069`) to resident `sandbox-daemon`, passing the calling process PID and receiving an authorization ACK. Event-driven with zero polling.
* **Returns**: `0` on success; `-1` on error or if `sandbox-daemon` is unreachable.

### `int oops_escape_jail(void)`
* **When to use**: Escapes the `/app0` sandbox without modifying Sony credentials, pointing directory descriptors to `/`.

---

## 14. Kernel Read/Write & Syscall Dispatcher (`<oops/krw.h>`, `<oops/syscall.h>`)

Primitives for interacting directly with kernel memory and dispatching system calls through verified libkernel trampolines.

### Syscall Trampoline (`<oops/syscall.h>`)
* `long sys_call(long num, long a1, long a2, long a3, long a4, long a5, long a6)`: Dispatches arbitrary FreeBSD/Prospero syscalls through libkernel's registered trampoline (bypassing PPRBUG-22859 mitigation).

### Kernel Memory & Process Directory Operations (`<oops/krw.h>`)

`krw.h` is the largest header in the SDK and most of it was previously undocumented. Its
functions carry a `krw_`/`klog_` prefix, not `oops_krw_`.

**Setup and base addresses**
* `int krw_init(const payload_args_t *args)`: Establish the kernel read/write path this whole header runs on.
* `int krw_is_ready(void)`: Whether `krw_init` succeeded.
* `uintptr_t krw_kdata_base(void)`, `uintptr_t krw_ktext_base(void)`: Resolved kernel data/text segment bases.
* `uintptr_t krw_allproc_addr(void)`, `void krw_set_allproc_addr(uintptr_t addr)`: Get/override the resolved `allproc` chain head.
* `uint32_t krw_fw_version(void)`: The running firmware's version word.

**Raw kernel memory access**
* `int krw_copyin(const void *uaddr, uintptr_t kaddr, size_t len)`, `int krw_copyout(uintptr_t kaddr, void *uaddr, size_t len)`: Arbitrary-width copy between userland and kernel memory.
* `uint64_t krw_read64(uintptr_t kaddr)`, `uint32_t krw_read32(uintptr_t kaddr)`, `uint16_t krw_read16(uintptr_t kaddr)`, `uint8_t krw_read8(uintptr_t kaddr)`: Fixed-width kernel reads.
* `int krw_write64(uintptr_t kaddr, uint64_t val)`, `int krw_write32(uintptr_t kaddr, uint32_t val)`, `int krw_write16(uintptr_t kaddr, uint16_t val)`, `int krw_write8(uintptr_t kaddr, uint8_t val)`: Fixed-width kernel writes.
* `int krw_mprotect(pid_t pid, uintptr_t addr, size_t len, int prot)`: Change protection on a target process's mapping via the kernel path rather than its own `mprotect`.

**Process and credential lookup**
* `uintptr_t krw_get_proc(pid_t pid)`, `uintptr_t krw_find_proc_by_name(const char *name)`: Resolve a kernel `proc` struct by PID or executable name.
* `uintptr_t krw_get_ucred(pid_t pid)`: Resolve a process's `ucred` struct.
* `uint64_t krw_get_ucred_authid(pid_t pid)`, `int krw_set_ucred_authid(pid_t pid, uint64_t authid)`: Read/write `cr_sceAuthID`.
* `int krw_get_ucred_caps(pid_t pid, uint8_t caps[16])`, `int krw_set_ucred_caps(pid_t pid, const uint8_t caps[16])`: Read/write `cr_sceCaps`.
* `int krw_get_ucred_attrs(pid_t pid, uint8_t attrs[32])`, `int krw_set_ucred_attrs(pid_t pid, const uint8_t attrs[32])`: Read/write the SCE credential attribute block.
* `uintptr_t krw_get_root_vnode(void)`: Dynamically resolves the true kernel root directory vnode from PID 1 or self.
* `uintptr_t krw_get_proc_cdir(pid_t pid)` / `int krw_set_proc_cdir(pid_t pid, uintptr_t vnode)`: Read/write `fd_cdir` in the process `filedesc`.
* `uintptr_t krw_get_proc_rootdir(pid_t pid)` / `int krw_set_proc_rootdir(pid_t pid, uintptr_t vnode)`: Read/write `fd_rdir`.
* `uintptr_t krw_get_proc_jaildir(pid_t pid)` / `int krw_set_proc_jaildir(pid_t pid, uintptr_t vnode)`: Read/write `fd_jdir`.

**Elevation helpers**
* `int krw_elevate_current_process(void)`, `int krw_elevate_process(pid_t pid)`: Apply this header's own credential elevation (distinct from `escalate.h`'s `oops_jailbreak_process`).
* `int krw_restore_current_process(void)`: Undo an elevation applied to the caller.
* `int krw_swap_ucred(pid_t target_pid)`, `int krw_restore_ucred(void)`: Temporarily borrow another process's credentials and restore afterward.
* `int krw_apply_ptrace_kernel_patch(void)`: Apply the kernel patch this SDK's `procctl_*` ptrace-style primitives (`<oops/inject.h>`) depend on.

**Dynamic linking resolution**
* `uintptr_t krw_find_target_libkernel_base(uintptr_t target_kproc, ...)`: Locate `libkernel`'s base inside a target process.
* `uintptr_t krw_dynlib_resolve(pid_t pid, int sprx_handle, const char *nid)`: Resolve a symbol by NID inside an already-loaded module.
* `uintptr_t krw_dynlib_resolve_any(pid_t pid, const char *sname)`: Resolve a symbol by name across a target's loaded modules.
* `int krw_dump_all_exports(pid_t pid, obs_kexport_table_t *table)`, `const void *obs_kexport_lookup(const obs_kexport_table_t *table, ...)`: Dump and query a target's exported symbol table.

**Kernel-side logging**
* `void klog_write(const char *msg)`, `void klog_write_hex(const char *prefix, uint64_t val)`, `void klog_write_num(const char *prefix, int64_t num)`.

### Legacy Escalation Primitives (`<oops/escalate.h>`)
* `int oops_kernel_copyout(uint64_t kaddr, void *buf, size_t size)`: Read from arbitrary kernel virtual address.
* `int oops_kernel_copyin(const void *buf, uint64_t kaddr, size_t size)`: Write to arbitrary kernel virtual address.
* `uint64_t oops_kernel_find_proc_by_pid(int pid)`: Walk kernel `allproc` chain to locate a target process struct.

---

## 15. System Modules & Dynamic Linking (`<oops/sysmodule.h>`)

Dynamically load internal PRX modules at runtime using `libSceSysmodule`.

### Module Identifiers
`oops_sysmodule_load`/`unload`/`is_loaded` take a plain `uint16_t` — there is no
`oops_sysmodule_id_t` enum type. `include/oops/sysmodule.h` `#define`s the full set (`OOPS_SYSMODULE_PERF`
through `OOPS_SYSMODULE_SHARE`); the ones this SDK's own subsystems load are:
* `OOPS_SYSMODULE_NET_CTL` (`0x0011`)
* `OOPS_SYSMODULE_AUDIO_DEC` (`0x0088`)
* `OOPS_SYSMODULE_IME_DIALOG` (`0x0096`) — loaded by `<oops/dialog.h>`'s IME open.
* `OOPS_SYSMODULE_SAVE_DATA_DIALOG` (`0x00A0`)
* `OOPS_SYSMODULE_MESSAGE_DIALOG` (`0x00A4`)
* `OOPS_SYSMODULE_SAVE_DATA` (`0x00B6`) — loaded by `<oops/savedata.h>`'s mount.

### `int oops_sysmodule_load(uint16_t id)`
* Loads the requested system module into the process address space.

### `int oops_sysmodule_unload(uint16_t id)`
* Unloads a previously loaded module.

### `int oops_sysmodule_is_loaded(uint16_t id)`
* **Returns**: `1` if loaded, `0` if not, `-1` if unsupported.

---

## 16. System Information & Telemetry (`<oops/system.h>`, `<oops/offsets.h>`)

Query hardware metrics, console firmware versions, user identity, and kernel offset tables.
There is no `oops_system_get_telemetry`/`oops_hw_telemetry_t` — telemetry is split across
`oops_system_get_info` (firmware/console identity) and `oops_system_get_hw_info` (live sensors)
below.

### `int oops_system_get_info(oops_system_info_t *out_info)`
* Retrieves (`oops_system_info_t`):
  * `generation`: `4` for Orbis-generation, `5` for Prospero-generation — not a `model` enum.
  * `firmware_raw`: Hex version (e.g. `0x12400009` for FW 12.40).
  * `firmware_str[16]`: Human-readable version string (`"12.40"`).
  * `model_str[32]`: Console model string (e.g. `"CFI-1116A"`).
  * `total_ram_mb`, `direct_mem_mb`: System and Direct Memory budgets.
  * `initial_user_id`, `user_name[32]`: The initial logged-in user.

### `int oops_user_get_initial_user_id(void)`
### `int oops_user_get_name(int32_t user_id, char *out_name, size_t max_len)`
### `int oops_user_get_logged_in_users(int32_t *out_user_ids, size_t max_users, size_t *out_count)`
* **When to use**: Resolve the initial user, a user's display name, or the full logged-in-user list — the pieces `oops_system_get_info`'s `initial_user_id`/`user_name` are built from.

### `int oops_system_notify(const char *text)`
* **When to use**: Post an OS toast-style notification.

### Kernel Log & Telemetry Output (`/dev/klog` on target, `stderr` on host)
* `void oops_log_init(const char *app_id)`, `const char *oops_log_get_app_id(void)`: Set/read the tag prefix later log calls use.
* `void oops_log(const char *fmt, ...)`: `printf`-style logging under that app-id tag.
* `void oops_klog(const char *tag, const char *msg)`: Write a tagged message directly — this, not `obs_klog`, is the SDK's log primitive.
* `void oops_kprintf(const char *tag, const char *fmt, ...)`: `printf`-style variant of `oops_klog`.
* `const char *oops_test_get_last_klog(void)`: Read back the last message logged, for host-side test assertions.

### System Service Controls (`libSceSystemService`)
* `int oops_system_hide_splash(void)`, `int oops_system_power_tick(void)`, `int oops_system_navigate_home(void)`: Splash dismissal, power-save tick, and return-to-home.
* `int oops_system_get_enter_button(int *out_button)`: `0` = Circle, `1` = Cross, per the console's region setting.
* `int oops_system_launch_app(const char *title_id)`: Launch another installed title.

### Hardware Telemetry & Diagnostics
### `int oops_system_get_hw_info(oops_hw_info_t *out_hw)`
* Retrieves (`oops_hw_info_t`): `cpu_temp_c`, `soc_temp_c`, `fan_duty_pct` (0-100), `cpu_freq_hz`, `serial_number[64]`, `model_name[64]` — each numeric field is `-1`/`0` when unavailable rather than absent.
* Individual readings are also available one at a time: `int oops_system_get_cpu_temp(int *out_temp_celsius)`, `int oops_system_get_soc_temp(int sensor_idx, int *out_temp_celsius)`, `int oops_system_get_fan_duty(int *out_duty_pct)`, `int oops_system_get_cpu_freq(uint64_t *out_freq_hz)`, `int oops_system_get_hw_serial(char *out_serial, size_t max_len)`, `int oops_system_get_hw_model(char *out_model, size_t max_len)`.

### Sandbox and namespace
* `int oops_system_check_pltauth(void)`: Whether `/dev/pltauth` is patched for native Prospero category-0 execution (`1` on Orbis/host too).
* `int oops_system_init_namespace(const struct payload_args *args)`: Bind the system namespace and root filesystem, resolving the kernel root vnode.
* `int oops_system_escape_sandbox(void)`: Documented in [§13](#13-privilege-escalation--sandbox-escape-oopsescalateh).

### Firmware Offset Tables (`<oops/offsets.h>`)
* `int oops_offsets_load_default(void)`: Load the offset table for the running firmware from this SDK's built-in defaults.
* `int oops_offsets_load_string(const char *toml_str)`, `int oops_offsets_load_file(const char *path)`: Load an offset table from TOML, in memory or from a path, for a firmware this SDK doesn't ship defaults for yet.
* `const oops_common_offsets_t *oops_offsets_get_common(void)`: Offsets that don't vary by firmware.
* `const oops_fw_offsets_t *oops_offsets_get_fw(uint32_t fw_raw)`, `const oops_fw_offsets_t *oops_offsets_get_fw_str(const char *fw_str)`: Per-firmware offsets, looked up by the raw version word or its string form.
* `void oops_offsets_reset(void)`: Discard a loaded table and fall back to defaults.

---

## 17. High-Resolution Timing (`<oops/time.h>`)

Monotonic timestamps and microsecond sleeps based on x86 invariant TSC frequency.

* `void oops_time_init(void)`: One-time setup; call before the getters below.
* `uint64_t oops_time_get_ticks(void)`: Raw hardware tick counter.
* `uint64_t oops_time_get_counter(void)`: Unadjusted TSC counter (`rdtsc`).
* `uint64_t oops_time_get_frequency(void)`, `uint64_t oops_time_get_counter_frequency(void)`: Invariant TSC tick frequency in Hz.
* `uint64_t oops_time_get_ns(void)`, `uint64_t oops_time_get_us(void)`, `uint64_t oops_time_get_ms(void)`: Nanoseconds/microseconds/milliseconds elapsed since boot.
* `double oops_time_get_seconds(void)`: Seconds elapsed since boot, as a double.
* `void oops_time_sleep_us(uint32_t microseconds)`: Microsecond sleep delay.
* `void oops_time_sleep_ms(uint32_t milliseconds)`: Millisecond sleep delay.

---

## 18. Threading & Synchronization (`<oops/thread.h>`)

Clean-room POSIX-compatible threading primitives backed by kernel `sys_thr_*` syscalls.

### Threads
* `oops_thread_t oops_thread_create(const char *name, void *(*entry)(void *), void *arg, size_t stack_size, int priority)`: Returns the new thread handle directly — there is no separate caller-supplied out-pointer form.
* `int oops_thread_join(oops_thread_t thread, void **out_retval)`
* `int oops_thread_detach(oops_thread_t thread)`
* `void oops_thread_yield(void)`
* `oops_thread_t oops_thread_self(void)`
* `int oops_thread_equal(oops_thread_t t1, oops_thread_t t2)`

### Mutexes and condition variables
* `int oops_mutex_init(oops_mutex_t *mutex, const char *name)`: `name` is required, not optional.
* `int oops_mutex_lock(oops_mutex_t *mutex)`
* `int oops_mutex_trylock(oops_mutex_t *mutex)`
* `int oops_mutex_unlock(oops_mutex_t *mutex)`
* `int oops_mutex_destroy(oops_mutex_t *mutex)`
* `int oops_cond_init(oops_cond_t *cond, const char *name)`: `name` is required, not optional.
* `int oops_cond_wait(oops_cond_t *cond, oops_mutex_t *mutex)`
* `int oops_cond_timedwait(oops_cond_t *cond, oops_mutex_t *mutex, ...)`
* `int oops_cond_signal(oops_cond_t *cond)`
* `int oops_cond_broadcast(oops_cond_t *cond)`
* `int oops_cond_destroy(oops_cond_t *cond)`

### Semaphores
* `int oops_sem_init(oops_sem_t *sem, const char *name, int initial_count, ...)`
* `int oops_sem_wait(oops_sem_t *sem, int count)`
* `int oops_sem_poll(oops_sem_t *sem, int count)`: Non-blocking `oops_sem_wait`.
* `int oops_sem_signal(oops_sem_t *sem, int count)`
* `int oops_sem_destroy(oops_sem_t *sem)`

### Exception handling
* `typedef void (*oops_exception_handler_t)(int signum, void *arg1, void *arg2);`
* `int oops_thread_install_exception_handler(int signum, oops_exception_handler_t handler)`
* `int oops_thread_remove_exception_handler(int signum)`
* `int oops_thread_raise_exception(oops_thread_t thread, int signum)`

---

## 19. BSD Sockets & Network Telemetry (`<oops/net.h>`, `<oops/netctl.h>`)

POSIX socket wrappers for networking, paired with `libSceNetCtl` interface telemetry and freestanding RFC 1035 DNS resolution.

### Socket Interface (`<oops/net.h>`)
* `int oops_net_init(void)`, `void oops_net_term(void)`: Lifecycle for the socket layer itself — call before opening any socket.
* `int oops_net_inet_pton(const char *src, uint32_t *dst)`, `int oops_net_inet_ntop(uint32_t src, char *dst, size_t dst_len)`: Dotted-quad string <-> 32-bit IPv4 address conversion.
* `int oops_socket(int domain, int type, int protocol)`: Open a BSD socket endpoint (`OOPS_AF_INET`, `OOPS_SOCK_STREAM` or `OOPS_SOCK_DGRAM`).
* `int oops_connect(int sock, const char *server_ip, uint16_t port)`: Connect to remote host. Transparently resolves domain hostnames (e.g. `"api.example.com"`) to IPv4 addresses via `oops_net_resolve`.
* `int oops_bind(int sock, const char *ip, uint16_t port)`: Bind socket to local address and port.
* `int oops_listen(int sock, int backlog)`: Listen for incoming connections.
* `int oops_accept(int sock, char *client_ip, size_t ip_len, uint16_t *client_port)`: Accept incoming client connection.
* `long oops_send(int sock, const void *buf, size_t len, int flags)`: Send data over stream socket.
* `long oops_recv(int sock, void *buf, size_t len, int flags)`: Receive data from stream socket.
* `long oops_sendto(int sock, const void *buf, size_t len, int flags, const char *to_ip, uint16_t to_port)`: Send UDP datagram.
* `long oops_recvfrom(int sock, void *buf, size_t len, int flags, char *from_ip, size_t ip_len, uint16_t *from_port)`: Receive UDP datagram.
* `int oops_setsockopt(int sock, int level, int optname, const void *optval, ...)`: Set a socket option.
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
* `void oops_net_ctl_term(void)`: Unload the network control subsystem.

---

## 20. Freestanding C Runtime Utilities (`<oops/freestd.h>`)

Zero-libc implementations of memory, string, formatting, and hashing primitives.

* **Memory**: `void *memset(void *dest, int value, size_t len)`, `void *memcpy(void *dest, const void *src, size_t len)`, `int memcmp(const void *s1, const void *s2, size_t len)` — declared unprefixed (no `obs_`) as the freestanding stand-ins for the hosted libc calls of the same name.
* **String**: `size_t obs_strlen(const char *s)`, `int obs_strcmp(...)`, `int obs_strncmp(...)`, `char *obs_strncpy(char *dest, const char *src, size_t n)` — there is no `obs_strcpy`; use `obs_strncpy` with an explicit length.
* **Numeric Formatting**: `size_t obs_format_hex(char *buf, uint64_t val)`, `size_t obs_format_i64(char *buf, int64_t val)`, `size_t obs_format_u64(...)`
* **String Formatting**:
  * `int oops_snprintf(char *buf, size_t size, const char *fmt, ...)`: Standard clean-room `snprintf` supporting `%s`, `%c`, `%d`, `%i`, `%u`, `%x`, `%X`, `%p`, `%%`, padding width, zero-fill, and length modifiers (`l`, `ll`, `z`).
  * `int oops_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)`: Variadic list string formatting.
* **NID Hashing**: `void obs_compute_nid(const char *name, char out_nid[12])`: Generates canonical Sony NID hashes verified against SELFish format authority.

---

## 21. Process Control & Code Injection (`<oops/inject.h>`)

Cross-process debugging, process hooking, and remote dynamic ELF loading. Everything in this
section is declared in `<oops/inject.h>` itself — there are no separate `procctl.h`/`procparam.h`
headers; the `procctl_*` functions below are just this header's own ptrace-style layer,
unprefixed with `oops_`.

### Target resolution
* `pid_t target_find_by_name(const char *name)`: Resolve a PID by process/executable name.
* `pid_t target_find_foreground_app(void)`: Resolve the current foreground Big App's PID.
* `pid_t target_resolve(const char *target_spec)`: Resolve either form from one call.

### Ptrace-style remote control (`procctl_*`)
* `int procctl_attach(pid_t pid)`, `int procctl_detach(pid_t pid, int sig)`: Attach/detach.
* `int procctl_step(pid_t pid)`, `int procctl_continue(pid_t pid, int sig)`: Single-step / resume.
* `int procctl_getregs(pid_t pid, struct reg *r)`, `int procctl_setregs(pid_t pid, const struct reg *r)`: Register access.
* `int procctl_copyin(pid_t pid, const void *src, uintptr_t dst_addr, size_t len)`, `int procctl_copyout(pid_t pid, uintptr_t src_addr, void *dst, size_t len)`: Memory copy into/out of the target.
* `int procctl_setlong(pid_t pid, uintptr_t addr, uint64_t val)`, `uint64_t procctl_getlong(pid_t pid, uintptr_t addr)`, `int procctl_setint(pid_t pid, uintptr_t addr, uint32_t val)`, `uint32_t procctl_getint(pid_t pid, uintptr_t addr)`: Fixed-width read/write helpers.
* `uintptr_t procctl_remote_mmap(pid_t pid, uintptr_t addr, size_t len, int prot, ...)`, `int procctl_remote_munmap(pid_t pid, uintptr_t addr, size_t len)`, `int procctl_remote_mprotect(pid_t pid, uintptr_t addr, size_t len, int prot)`: Remote memory mapping.
* `long procctl_remote_syscall(pid_t pid, int sysno, uint64_t a1, uint64_t a2, ...)`: Make the target issue a syscall on the caller's behalf.
* `uintptr_t procctl_find_syscall_gadget(pid_t pid, uintptr_t libkernel_base)`, `void procctl_set_syscall_gadget(uintptr_t gadget)`: Locate/override the syscall gadget `procctl_remote_syscall` uses.

### ELF loading
* `int loader_validate_elf(const uint8_t *elf_data, size_t elf_size)`: Sanity-check a payload before mapping it.
* `uintptr_t loader_load_into_proc(pid_t pid, const uint8_t *elf_data, ...)`: Map an ELF into a target process, lower-level than `oops_inject_elf`.
* `int oops_inject_elf(pid_t target_pid, const uint8_t *elf_data, size_t elf_size, const char *entry_symbol)`: Maps and executes an ELF payload inside another running process — the one entry point in this header that carries the `oops_` prefix.

There is no `oops_proc_kill` or `oops_proc_get_name` in this SDK.

---

## 22. High-Level Filesystem Subsystem (`<oops/fs.h>`)

Freestanding POSIX-conforming filesystem layer wrapping kernel syscalls (`SYS_open`, `SYS_close`, `SYS_read`, `SYS_write`, `SYS_lseek`, `SYS_mkdir`, `SYS_unlink`, `SYS_stat`).

### Core Functions
* `int oops_fs_open(const char *path, int flags, int mode)`: Open file descriptor. Supports `OOPS_O_RDONLY`, `OOPS_O_WRONLY`, `OOPS_O_RDWR`, `OOPS_O_CREAT`, `OOPS_O_TRUNC`, `OOPS_O_APPEND`.
* `int oops_fs_close(int fd)`: Close file descriptor.
* `int64_t oops_fs_read(int fd, void *buf, size_t count)`: Read data from file.
* `int64_t oops_fs_write(int fd, const void *buf, size_t count)`: Write data to file.
* `int64_t oops_fs_seek(int fd, int64_t offset, int whence)`: Seek within file (`OOPS_SEEK_SET`, `OOPS_SEEK_CUR`, `OOPS_SEEK_END`).
* `int64_t oops_fs_tell(int fd)`: Return current byte offset within file.
* `int oops_fs_exists(const char *path)`: Check if a path exists on disk (`1`/`0`, not `bool`).
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
* `int oops_heap_get_stats(oops_heap_stats_t *out_stats)`: Query live heap telemetry (`allocated_bytes`, `reserved_bytes`, `active_allocations`, `mmap_chunks`). Returns `0` on success, not `void`.

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

---

## 25. Target Platform Identification (`<oops/target.h>`)

Compile-time platform selection. Pulled into every other header through the unified
`<oops/oops.h>`, but previously had no section of its own here.

* `OOPS_TARGET_ORBIS` (1), `OOPS_TARGET_NEO` (2), `OOPS_TARGET_PROSPERO` (3), `OOPS_TARGET_TRINITY` (4): The four build targets from the domain's format/target axis. `neo` and `trinity` are the mid-generation Pro refreshes of `orbis` and `prospero`, not synonyms for them.
* `OOPS_TARGET`: Which of the above a translation unit is compiled for; defaults to `OOPS_TARGET_PROSPERO` if not set by the consumer's build. Fixed at compile time — it is not the environment the binary actually runs in, which obSCEne measures separately as its `OBS|context`.
* `OOPS_TARGET_IS_ORBIS`, `OOPS_TARGET_IS_PROSPERO`: Generation-classification macros (`orbis`/`neo` vs. `prospero`/`trinity`).
* `static inline oops_target_t oops_get_target(void)`: Returns the target the current binary was compiled for.
* `static inline const char *oops_target_name(oops_target_t target)`: Human-readable target name (`"orbis"`, `"neo"`, `"prospero"`, `"trinity"`).

