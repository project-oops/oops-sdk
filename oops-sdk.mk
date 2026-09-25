# oops-sdk build integration
# Include this file in consumer Makefiles:
#   OOPS_SDK ?= $(abspath ../oops-sdk)
#   include $(OOPS_SDK)/oops-sdk.mk

OOPS_SDK_DIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# Target mode: orbis | neo | prospero | trinity
# Default: prospero
TARGET ?= prospero

ifeq ($(TARGET),orbis)
    OOPS_TARGET_NUM := 1
    OOPS_TARGET_TAG := ORB
else ifeq ($(TARGET),neo)
    OOPS_TARGET_NUM := 2
    OOPS_TARGET_TAG := NEO
else ifeq ($(TARGET),prospero)
    OOPS_TARGET_NUM := 3
    OOPS_TARGET_TAG := PRO
else ifeq ($(TARGET),trinity)
    OOPS_TARGET_NUM := 4
    OOPS_TARGET_TAG := TRI
else
    $(error Unknown oops-sdk TARGET '$(TARGET)': expected orbis, neo, prospero, or trinity)
endif

OOPS_TARGET_FLAGS := -DOOPS_TARGET=$(OOPS_TARGET_NUM)
OOPS_SDK_INCLUDE := -I$(OOPS_SDK_DIR)/include -I$(OOPS_SDK_DIR) $(OOPS_TARGET_FLAGS)

# Graphics backend source segregation:
# Orbis / Neo targets compile GNM only
# Prospero / Trinity native targets compile AGC only
ifeq ($(filter 1 2,$(OOPS_TARGET_NUM)),)
    # Prospero / Trinity native
    OOPS_SDK_GRAPHICS_SRCS := \
        $(OOPS_SDK_DIR)/src/agc/agc_display.c \
        $(OOPS_SDK_DIR)/src/agc/agc_tiler.c \
        $(OOPS_SDK_DIR)/src/agc/agc_compute.c \
        $(OOPS_SDK_DIR)/src/agc/agc_draw.c
else
    # Orbis / Neo
    OOPS_SDK_GRAPHICS_SRCS := \
        $(OOPS_SDK_DIR)/src/gnm/gnm_display.c
endif

# ==============================================================================
# OOPS_FEATURES — Declarative Subsystem Capabilities
# ==============================================================================
# The SDK is the source of truth for:
#   capability -> { sources, symbol claims, include flags }
# An application sets `OOPS_FEATURES += <name>` in its Makefile, and app.mk
# maps intent to the required implementation sources and libSce* import claims.

# Base runtime (implicit always-on, linked into every payload)
OOPS_FEATURE_base_SRCS := \
    $(OOPS_SDK_DIR)/src/system/system.c \
    $(OOPS_SDK_DIR)/src/system/freestd.c \
    $(OOPS_SDK_DIR)/src/system/syscall.c \
    $(OOPS_SDK_DIR)/src/system/offsets.c \
    $(OOPS_SDK_DIR)/src/system/fs.c \
    $(OOPS_SDK_DIR)/src/system/sysmodule.c \
    $(OOPS_SDK_DIR)/src/system/procparam.c \
    $(OOPS_SDK_DIR)/src/memory/memory.c \
    $(OOPS_SDK_DIR)/src/memory/heap.c \
    $(OOPS_SDK_DIR)/src/time/time.c
OOPS_FEATURE_base_SYMS := libkernel libSceLibcInternal libSceSystemService libSceUserService libSceSysUtil

# Display & Hardware GPU Rasterization (AGC / GNM)
OOPS_FEATURE_display_SRCS := \
    $(OOPS_SDK_DIR)/src/display.c \
    $(OOPS_SDK_GRAPHICS_SRCS)
OOPS_FEATURE_display_SYMS := libSceVideoOut libSceAgc libSceAgcDriver libSceGnmDriver

# 2D Software Drawing Canvas
OOPS_FEATURE_draw_SRCS := \
    $(OOPS_SDK_DIR)/src/draw/draw.c
OOPS_FEATURE_draw_SYMS :=

# PNG Image Decoder
OOPS_FEATURE_png_SRCS := \
    $(OOPS_SDK_DIR)/src/draw/png.c
OOPS_FEATURE_png_SYMS :=

# On-screen Telemetry & Diagnostic HUD
OOPS_FEATURE_hud_SRCS := \
    $(OOPS_SDK_DIR)/src/hud/hud.c
OOPS_FEATURE_hud_SYMS :=

# Controller & Gamepad Input (DualSense / DualShock)
OOPS_FEATURE_input_SRCS := \
    $(OOPS_SDK_DIR)/src/input/input.c
OOPS_FEATURE_input_SYMS := libScePad libSceUserService

# USB & Bluetooth Keyboard
OOPS_FEATURE_keyboard_SRCS := \
    $(OOPS_SDK_DIR)/src/input/keyboard.c
OOPS_FEATURE_keyboard_SYMS := libSceKeyboard

# USB & Bluetooth Mouse
OOPS_FEATURE_mouse_SRCS := \
    $(OOPS_SDK_DIR)/src/input/mouse.c
OOPS_FEATURE_mouse_SYMS := libSceMouse

# PCM Audio Output
OOPS_FEATURE_audio_SRCS := \
    $(OOPS_SDK_DIR)/src/audio/audio.c
OOPS_FEATURE_audio_SYMS := libSceAudioOut

# Hardware Audio Decoder (AT9, AAC, MP3)
OOPS_FEATURE_audiodec_SRCS := \
    $(OOPS_SDK_DIR)/src/audio/audiodec.c
OOPS_FEATURE_audiodec_SYMS := libSceAudiodec libSceAjm

# Hardware Video Decoder (H.264 / AVC)
OOPS_FEATURE_videodec_SRCS := \
    $(OOPS_SDK_DIR)/src/videodec/videodec.c
OOPS_FEATURE_videodec_SYMS := libSceVideodec2

# BSD Sockets & Network Interface Control
OOPS_FEATURE_net_SRCS := \
    $(OOPS_SDK_DIR)/src/net/net.c \
    $(OOPS_SDK_DIR)/src/net/netctl.c \
    $(OOPS_SDK_DIR)/src/net/dns.c
OOPS_FEATURE_net_SYMS := libSceNet libSceNetCtl libkernel

# Standalone DNS Client (RFC 1035)
OOPS_FEATURE_dns_SRCS := \
    $(OOPS_SDK_DIR)/src/net/dns.c
OOPS_FEATURE_dns_SYMS := libSceNet

# HTTP / HTTPS Client & Streaming Engine
OOPS_FEATURE_http_SRCS := \
    $(OOPS_SDK_DIR)/src/net/http.c \
    $(OOPS_SDK_DIR)/src/system/zip.c
OOPS_FEATURE_http_SYMS := libSceHttp libSceSsl libSceNet

# Freestanding ZIP Archive Extractor & Deflate
OOPS_FEATURE_zip_SRCS := \
    $(OOPS_SDK_DIR)/src/system/zip.c
OOPS_FEATURE_zip_SYMS :=

# Persistent User Save Data
OOPS_FEATURE_savedata_SRCS := \
    $(OOPS_SDK_DIR)/src/system/savedata.c
OOPS_FEATURE_savedata_SYMS := libSceSaveData

# System Message & IME OSK Dialogs
OOPS_FEATURE_dialog_SRCS := \
    $(OOPS_SDK_DIR)/src/system/dialog.c
OOPS_FEATURE_dialog_SYMS := libSceCommonDialog libSceImeDialog libSceMsgDialog

# POSIX Threads, Semaphores & Synchronization
OOPS_FEATURE_thread_SRCS := \
    $(OOPS_SDK_DIR)/src/thread/thread.c
OOPS_FEATURE_thread_SYMS := libkernel

# Dynamic JIT Memory Allocation
OOPS_FEATURE_jit_SRCS := \
    $(OOPS_SDK_DIR)/src/system/jit.c
OOPS_FEATURE_jit_SYMS := libkernel

# Process Privilege Escalation & Jailbreak
OOPS_FEATURE_escalate_SRCS := \
    $(OOPS_SDK_DIR)/src/system/escalate.c
OOPS_FEATURE_escalate_SYMS := libkernel

# Process Injection & Kernel R/W Primitive
OOPS_FEATURE_inject_SRCS := \
    $(OOPS_SDK_DIR)/src/system/krw.c \
    $(OOPS_SDK_DIR)/src/system/procctl.c \
    $(OOPS_SDK_DIR)/src/system/loader.c \
    $(OOPS_SDK_DIR)/src/system/target.c \
    $(OOPS_SDK_DIR)/src/system/inject.c
OOPS_FEATURE_inject_SYMS := libkernel

# Background Package Installer Client
OOPS_FEATURE_pkg_SRCS := \
    $(OOPS_SDK_DIR)/src/system/pkg.c
OOPS_FEATURE_pkg_SYMS := libSceAppInstUtil

# Suspend / Resume Lifecycle Hooks
OOPS_FEATURE_suspend_SRCS := \
    $(OOPS_SDK_DIR)/src/system/suspend.c
OOPS_FEATURE_suspend_SYMS := libSceSystemService

# OpenGL GLU Utility Library
OOPS_FEATURE_glu_SRCS := \
    $(OOPS_SDK_DIR)/src/gl/gl_glu.c
OOPS_FEATURE_glu_SYMS :=

# OpenGL GLUT Toolkit & Bitmap Fonts
OOPS_FEATURE_glut_SRCS := \
    $(OOPS_SDK_DIR)/src/gl/glut.c \
    $(OOPS_SDK_DIR)/src/gl/glut_font.c
OOPS_FEATURE_glut_SYMS :=

# Standalone JavaScript Engine (QuickJS ES2020)
OOPS_FEATURE_js_SRCS := \
    $(OOPS_SDK_DIR)/src/js/js.c \
    $(OOPS_SDK_DIR)/src/js/quickjs/quickjs.c \
    $(OOPS_SDK_DIR)/src/js/quickjs/cutils.c \
    $(OOPS_SDK_DIR)/src/js/quickjs/libbf.c \
    $(OOPS_SDK_DIR)/src/js/quickjs/libregexp.c \
    $(OOPS_SDK_DIR)/src/js/quickjs/libunicode.c
OOPS_FEATURE_js_SYMS :=

# Standalone HTML5/CSS Layout & Renderer (litehtml)
OOPS_FEATURE_html_SRCS := \
    $(OOPS_SDK_DIR)/src/html/oops_html.cpp \
    $(OOPS_SDK_DIR)/src/html/html_container.cpp \
    $(wildcard $(OOPS_SDK_DIR)/src/html/litehtml/src/*.cpp) \
    $(wildcard $(OOPS_SDK_DIR)/src/html/litehtml/src/gumbo/*.c)
OOPS_FEATURE_html_SYMS :=

# Assembled Webview Browser (QuickJS + litehtml + DOM Bridge + Host Env)
OOPS_FEATURE_webview_SRCS := \
    $(OOPS_SDK_DIR)/src/webview/webview.cpp \
    $(OOPS_SDK_DIR)/src/webview/dom_bridge.cpp \
    $(OOPS_SDK_DIR)/src/webview/host_env.cpp
OOPS_FEATURE_webview_SYMS :=
OOPS_FEATURE_webview_DEPS := js html http input

# Complete list of available features exposed by oops-sdk
OOPS_FEATURES_AVAILABLE := \
    base \
    display \
    draw \
    png \
    hud \
    input \
    keyboard \
    mouse \
    audio \
    audiodec \
    videodec \
    net \
    dns \
    http \
    zip \
    savedata \
    dialog \
    thread \
    jit \
    escalate \
    inject \
    pkg \
    suspend \
    glu \
    glut \
    js \
    html \
    webview

# Backwards-compatible aggregate of SDK C sources:
OOPS_SDK_C_SRCS := $(sort \
    $(OOPS_FEATURE_base_SRCS) \
    $(OOPS_FEATURE_display_SRCS) \
    $(OOPS_FEATURE_input_SRCS) \
    $(OOPS_FEATURE_keyboard_SRCS) \
    $(OOPS_FEATURE_mouse_SRCS) \
    $(OOPS_FEATURE_audio_SRCS) \
    $(OOPS_FEATURE_audiodec_SRCS) \
    $(OOPS_FEATURE_videodec_SRCS) \
    $(OOPS_FEATURE_net_SRCS) \
    $(OOPS_FEATURE_http_SRCS) \
    $(OOPS_FEATURE_savedata_SRCS) \
    $(OOPS_FEATURE_dialog_SRCS) \
    $(OOPS_FEATURE_thread_SRCS) \
    $(OOPS_FEATURE_jit_SRCS) \
    $(OOPS_FEATURE_escalate_SRCS) \
    $(OOPS_FEATURE_inject_SRCS) \
    $(OOPS_FEATURE_pkg_SRCS) \
    $(OOPS_FEATURE_suspend_SRCS) \
    $(OOPS_FEATURE_draw_SRCS) \
    $(OOPS_FEATURE_png_SRCS) \
    $(OOPS_SDK_DIR)/src/math/math.c)

# Four-axis release artifact enforcement (OOPS/docs/CONVENTIONS.md section 2).
# Asserts that:
#   1. Every artifact in $(DIST) includes the target generation (-$(TARGET)).
#   2. Native title directories are packaged as archives (.zip or .pkg), never unzipped folders.
#   3. No bare uninformative filenames (e.g. eboot.bin, myapp.elf) are shipped.
define oops_verify_dist
	@if [ -d "$(1)" ]; then \
	    for f in "$(1)"/*; do \
	        [ -e "$$f" ] || continue; \
	        if [ -d "$$f" ]; then \
	            echo "error: $$f in $(1) is a directory; title directories must be packaged as .zip per OOPS CONVENTIONS.md section 2" >&2; \
	            exit 1; \
	        fi; \
	        fname=$$(basename "$$f"); \
	        case "$$fname" in \
	            *-$(TARGET).*|*-$(TARGET)) ;; \
	            *) \
	                echo "error: artifact '$$fname' in $(1) violates OOPS CONVENTIONS.md section 2 (must encode target generation -$(TARGET))" >&2; \
	                exit 1; \
	                ;; \
	        esac; \
	    done; \
	fi
endef
