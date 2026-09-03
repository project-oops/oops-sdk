# oops-sdk

**OOPS SDK** is a lightweight, freestanding, vendor-term-agnostic C/C++ SDK for PlayStation homebrew development across generations.

It abstracts platform hardware drivers into clean, reusable subsystems without coupling homebrew applications (like [obSCEne](https://github.com/project-oops/obSCEne)) to platform-specific SDK boilerplate or proprietary terms.

---

## Subsystems

### 1. Display Subsystem (`<oops/display.h>`)
Unified framebuffer and display presentation engine with backend dispatching:
* **AGC Backend (Prospero / PS5)** (`<agc/display.h>`, `<agc/tiler.h>`):
  * VideoOut display handle management on bus 0.
  * Direct WC Garlic memory allocation (32 MB aligned to 2MB).
  * 16-page `sceKernelBatchMap` into GPU virtual address space (`0x4000000000ULL`).
  * 64KB RDNA2 hardware tile swizzling via native `AgcGpuAddress`.
  * Flip submission and double buffering.
* **GNM Backend (Orbis / PS4)** (`<gnm/display.h>`):
  * VideoOut display handle management on bus 0.
  * Direct WC Garlic allocation with 64KB page alignment.
  * Native linear scanout presentation and double buffering.
* **Host / Mock Backend**:
  * In-memory linear pixel buffer for headless testing, CI, and off-device verification.

---

## Directory Structure

```
oops-sdk/
├── include/
│   ├── oops/
│   │   └── display.h       # High-level display API
│   ├── agc/
│   │   ├── display.h       # Low-level AGC display driver
│   │   └── tiler.h         # AGC surface tiler
│   └── gnm/
│       └── display.h       # Low-level GNM display driver
├── src/
│   ├── display.c           # Unified dispatcher
│   ├── agc/
│   │   ├── agc_display.c   # AGC display implementation
│   │   └── agc_tiler.cpp   # AGC RDNA2 tile wrapper
│   └── gnm/
│       └── gnm_display.c   # GNM display implementation
├── lib/
│   └── libSceAgcGpuAddress.a # Native RDNA2 tiler archive
├── Makefile                # Builds liboops_display.a
├── oops-sdk.mk             # Consumer makefile helper
└── README.md
```

---

## Consumer Integration

Include `oops-sdk.mk` in your application Makefile:

```makefile
OOPS_SDK_DIR ?= ../oops-sdk
include $(OOPS_SDK_DIR)/oops-sdk.mk

# Append include path
CFLAGS += $(OOPS_SDK_INC)
CXXFLAGS += $(OOPS_SDK_INC)

# Append library flags to your link command
TARGET_LDFLAGS += $(OOPS_SDK_LDFLAGS)
```

### Example Usage (`<oops/display.h>`)

```c
#include <oops/display.h>

// Open display with AGC on PS5 or GNM on PS4
oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 1920, 1080);
if (oops_display_is_ready(disp)) {
    uint32_t *fb = oops_display_get_framebuffer(disp);
    oops_display_clear(disp, 0xFF0D1116u); // Dark background
    
    // Draw your UI/game...
    
    oops_display_flip(disp);
}
```
