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
    $(OOPS_SDK_DIR)/src/gnm/gnm_display.c

OOPS_SDK_LIB := $(OOPS_SDK_DIR)/build/liboops.a
ifeq ($(wildcard $(OOPS_SDK_LIB)),)
    OOPS_SDK_LIB := $(OOPS_SDK_DIR)/build/liboops_display.a
endif

OOPS_SDK_LIBS := $(OOPS_SDK_LIB)
