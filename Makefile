# oops-sdk Makefile
# Builds standalone static libraries for target-side homebrew

CC := clang
AR := ar

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
TARGET_FLAGS := -target x86_64-unknown-freebsd -ffreestanding -fno-builtin -nostdlib -fPIC -fno-stack-protector
CFLAGS := $(TARGET_FLAGS) $(OOPS_TARGET_FLAGS) -std=c11 -Wall -Wextra -Werror -Iinclude
BUILD := build/$(TARGET)

# Graphics backend source segregation:
# Orbis / Neo targets compile GNM only
# Prospero / Trinity native targets compile AGC only
ifeq ($(filter 1 2,$(OOPS_TARGET_NUM)),)
    # Prospero / Trinity native
    GRAPHICS_OBJS := \
        $(BUILD)/agc/agc_display.o \
        $(BUILD)/agc/agc_tiler.o \
        $(BUILD)/agc/agc_compute.o \
        $(BUILD)/agc/agc_draw.o
else
    # Orbis / Neo
    GRAPHICS_OBJS := \
        $(BUILD)/gnm/gnm_display.o
endif

OBJS := \
    $(BUILD)/display.o \
    $(GRAPHICS_OBJS) \
    $(BUILD)/gl/gl_context.o \
    $(BUILD)/gl/gl_state.o \
    $(BUILD)/gl/gl_matrix.o \
    $(BUILD)/gl/gl_draw.o \
    $(BUILD)/input/input.o \
    $(BUILD)/input/keyboard.o \
    $(BUILD)/input/mouse.o \
    $(BUILD)/audio/audio.o \
    $(BUILD)/audio/audiodec.o \
    $(BUILD)/videodec/videodec.o \
    $(BUILD)/memory/memory.o \
    $(BUILD)/system/system.o \
    $(BUILD)/system/offsets.o \
    $(BUILD)/system/sysmodule.o \
    $(BUILD)/system/dialog.o \
    $(BUILD)/system/savedata.o \
    $(BUILD)/system/escalate.o \
    $(BUILD)/time/time.o \
    $(BUILD)/thread/thread.o \
    $(BUILD)/net/net.o \
    $(BUILD)/net/netctl.o \
    $(BUILD)/draw/draw.o \
    $(BUILD)/system/pkg.o \
    $(BUILD)/system/freestd.o \
    $(BUILD)/system/syscall.o \
    $(BUILD)/system/krw.o \
    $(BUILD)/system/procctl.o \
    $(BUILD)/system/loader.o \
    $(BUILD)/system/target.o \
    $(BUILD)/system/inject.o \
    $(BUILD)/system/procparam.o

TEST_SRCS := \
    tests/test_runner.c \
    tests/unit/test_agc_tiler.c \
    tests/unit/test_draw.c \
    tests/unit/test_input.c \
    tests/unit/test_audio.c \
    tests/unit/test_videodec.c \
    tests/unit/test_audiodec.c \
    tests/unit/test_memory.c \
    tests/unit/test_system.c \
    tests/unit/test_offsets.c \
    tests/unit/test_sysmodule.c \
    tests/unit/test_dialog.c \
    tests/unit/test_netctl.c \
    tests/unit/test_savedata.c \
    tests/unit/test_escalate.c \
    tests/unit/test_pkg.c \
    tests/unit/test_time.c \
    tests/unit/test_thread.c \
    tests/unit/test_net.c \
    tests/unit/test_freestd.c \
    tests/unit/test_krw.c \
    tests/unit/test_inject.c \
    tests/unit/test_gpu.c \
    tests/unit/test_pm4.c \
    tests/unit/test_gl.c \
    tests/integration/test_pipeline_draw_tile.c \
    tests/integration/test_memory_surface.c \
    tests/integration/test_thread_worker_pool.c \
    tests/integration/test_net_loopback.c \
    src/agc/agc_tiler.c \
    src/agc/agc_compute.c \
    src/agc/agc_draw.c \
    src/draw/draw.c \
    src/input/input.c \
    src/input/keyboard.c \
    src/input/mouse.c \
    src/audio/audio.c \
    src/audio/audiodec.c \
    src/videodec/videodec.c \
    src/memory/memory.c \
    src/system/system.c \
    src/system/offsets.c \
    src/system/sysmodule.c \
    src/system/dialog.c \
    src/system/savedata.c \
    src/system/escalate.c \
    src/system/pkg.c \
    src/system/freestd.c \
    src/system/syscall.c \
    src/system/krw.c \
    src/system/procctl.c \
    src/system/loader.c \
    src/system/target.c \
    src/system/inject.c \
    src/time/time.c \
    src/thread/thread.c \
    src/net/net.c \
    src/net/netctl.c \
    src/gl/gl_context.c \
    src/gl/gl_state.c \
    src/gl/gl_matrix.c \
    src/gl/gl_draw.c

HOST_CFLAGS := -std=c11 -Wall -Wextra -Iinclude -I. -pthread -lm -DOOPS_HOST_BUILD $(OOPS_TARGET_FLAGS)

.PHONY: all clean test test-unit test-int

all: $(BUILD)/liboops.a

$(BUILD)/liboops.a: $(OBJS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/tests/test_runner: all $(TEST_SRCS)
	@mkdir -p $(BUILD)/tests
	$(CC) $(HOST_CFLAGS) -o $@ $(TEST_SRCS)

test: $(BUILD)/tests/test_runner
	$(BUILD)/tests/test_runner

test-unit: $(BUILD)/tests/test_runner
	$(BUILD)/tests/test_runner unit

test-int: $(BUILD)/tests/test_runner
	$(BUILD)/tests/test_runner int

clean:
	rm -rf build
