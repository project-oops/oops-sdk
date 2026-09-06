# oops-sdk build integration
# Include this file in consumer Makefiles:
#   OOPS_SDK ?= $(abspath ../oops-sdk)
#   include $(OOPS_SDK)/oops-sdk.mk

OOPS_SDK_DIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OOPS_SDK_INCLUDE := -I$(OOPS_SDK_DIR)/include -I$(OOPS_SDK_DIR)

OOPS_SDK_C_SRCS := \
    $(OOPS_SDK_DIR)/src/display.c \
    $(OOPS_SDK_DIR)/src/agc/agc_display.c \
    $(OOPS_SDK_DIR)/src/agc/agc_tiler.c \
    $(OOPS_SDK_DIR)/src/gnm/gnm_display.c \
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
