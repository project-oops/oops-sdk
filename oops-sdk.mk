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

OOPS_SDK_C_SRCS := \
    $(OOPS_SDK_DIR)/src/display.c \
    $(OOPS_SDK_GRAPHICS_SRCS) \
    $(OOPS_SDK_DIR)/src/input/input.c \
    $(OOPS_SDK_DIR)/src/input/keyboard.c \
    $(OOPS_SDK_DIR)/src/input/mouse.c \
    $(OOPS_SDK_DIR)/src/audio/audio.c \
    $(OOPS_SDK_DIR)/src/audio/audiodec.c \
    $(OOPS_SDK_DIR)/src/videodec/videodec.c \
    $(OOPS_SDK_DIR)/src/memory/memory.c \
    $(OOPS_SDK_DIR)/src/memory/heap.c \
    $(OOPS_SDK_DIR)/src/system/system.c \
    $(OOPS_SDK_DIR)/src/system/offsets.c \
    $(OOPS_SDK_DIR)/src/system/sysmodule.c \
    $(OOPS_SDK_DIR)/src/system/dialog.c \
    $(OOPS_SDK_DIR)/src/system/savedata.c \
    $(OOPS_SDK_DIR)/src/system/escalate.c \
    $(OOPS_SDK_DIR)/src/system/jit.c \
    $(OOPS_SDK_DIR)/src/system/freestd.c \
    $(OOPS_SDK_DIR)/src/system/syscall.c \
    $(OOPS_SDK_DIR)/src/system/krw.c \
    $(OOPS_SDK_DIR)/src/system/procctl.c \
    $(OOPS_SDK_DIR)/src/system/loader.c \
    $(OOPS_SDK_DIR)/src/system/target.c \
    $(OOPS_SDK_DIR)/src/system/inject.c \
    $(OOPS_SDK_DIR)/src/system/fs.c \
    $(OOPS_SDK_DIR)/src/time/time.c \
    $(OOPS_SDK_DIR)/src/thread/thread.c \
    $(OOPS_SDK_DIR)/src/net/net.c \
    $(OOPS_SDK_DIR)/src/net/netctl.c \
    $(OOPS_SDK_DIR)/src/net/dns.c \
    $(OOPS_SDK_DIR)/src/draw/draw.c \
    $(OOPS_SDK_DIR)/src/math/math.c

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
