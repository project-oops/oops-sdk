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
else ifeq ($(TARGET),neo)
    OOPS_TARGET_NUM := 2
else ifeq ($(TARGET),prospero)
    OOPS_TARGET_NUM := 3
else ifeq ($(TARGET),trinity)
    OOPS_TARGET_NUM := 4
else
    $(error Unknown oops-sdk TARGET '$(TARGET)': expected orbis, neo, prospero, or trinity)
endif

OOPS_TARGET_FLAGS := -DOOPS_TARGET=$(OOPS_TARGET_NUM)
OOPS_SDK_INCLUDE := -I$(OOPS_SDK_DIR)/include -I$(OOPS_SDK_DIR) $(OOPS_TARGET_FLAGS)

# Graphics backend source segregation:
# Orbis / Neo (PS4) targets compile GNM only
# Prospero / Trinity (PS5 native) targets compile AGC only
ifeq ($(filter 1 2,$(OOPS_TARGET_NUM)),)
    # PS5 native (Prospero / Trinity)
    OOPS_SDK_GRAPHICS_SRCS := \
        $(OOPS_SDK_DIR)/src/agc/agc_display.c \
        $(OOPS_SDK_DIR)/src/agc/agc_tiler.c
else
    # PS4 (Orbis / Neo)
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
    $(OOPS_SDK_DIR)/src/system/system.c \
    $(OOPS_SDK_DIR)/src/system/offsets.c \
    $(OOPS_SDK_DIR)/src/system/sysmodule.c \
    $(OOPS_SDK_DIR)/src/system/dialog.c \
    $(OOPS_SDK_DIR)/src/system/savedata.c \
    $(OOPS_SDK_DIR)/src/system/escalate.c \
    $(OOPS_SDK_DIR)/src/system/freestd.c \
    $(OOPS_SDK_DIR)/src/system/syscall.c \
    $(OOPS_SDK_DIR)/src/system/krw.c \
    $(OOPS_SDK_DIR)/src/system/procctl.c \
    $(OOPS_SDK_DIR)/src/system/loader.c \
    $(OOPS_SDK_DIR)/src/system/target.c \
    $(OOPS_SDK_DIR)/src/system/inject.c \
    $(OOPS_SDK_DIR)/src/time/time.c \
    $(OOPS_SDK_DIR)/src/thread/thread.c \
    $(OOPS_SDK_DIR)/src/net/net.c \
    $(OOPS_SDK_DIR)/src/net/netctl.c \
    $(OOPS_SDK_DIR)/src/draw/draw.c
