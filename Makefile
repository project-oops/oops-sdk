# oops-sdk Makefile
# Builds standalone static libraries for target-side homebrew

CC := clang
AR := ar

include $(dir $(lastword $(MAKEFILE_LIST)))toolchain.mk

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
# `include/libc` is on the **target** path only: <math.h>, <string.h> and <stdlib.h> under the
# names a port calls. HOST_CFLAGS below must not have it, or a host test including <string.h>
# gets the freestanding one instead of the real library.
CFLAGS := $(TARGET_FLAGS) $(OOPS_TARGET_FLAGS) -std=c11 -Wall -Wextra -Werror -Iinclude -Iinclude/libc
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
    $(BUILD)/gl/gfx.o \
    $(BUILD)/gl/gl_state.o \
    $(BUILD)/gl/gl_matrix.o \
    $(BUILD)/gl/gl_glu.o \
    $(BUILD)/gl/glut.o \
    $(BUILD)/gl/gl_draw.o \
    $(BUILD)/gl/gl_list.o \
    $(BUILD)/gl/gl_attrib.o \
    $(BUILD)/gl/gl_raster.o \
    $(BUILD)/gl/gl_eval.o \
    $(BUILD)/gl/gl_select.o \
    $(BUILD)/gl/gl_pixel.o \
    $(BUILD)/gl/glsl_lex.o \
    $(BUILD)/gl/glsl_parse.o \
    $(BUILD)/gl/glsl_pp.o \
    $(BUILD)/gl/glsl_sema.o \
    $(BUILD)/gl/glsl_emit.o \
    $(BUILD)/gl/glsl_gen.o \
    $(BUILD)/gl/glsl_builtin.o \
    $(BUILD)/gl/glsl_link.o \
    $(BUILD)/gl/glsl_exec.o \
    $(BUILD)/gl/glsl_ps.o \
    $(BUILD)/gl/glsl_vs.o \
    $(BUILD)/gl/gl_shader.o \
    $(BUILD)/input/input.o \
    $(BUILD)/input/keyboard.o \
    $(BUILD)/input/mouse.o \
    $(BUILD)/audio/audio.o \
    $(BUILD)/audio/audiodec.o \
    $(BUILD)/videodec/videodec.o \
    $(BUILD)/memory/memory.o \
    $(BUILD)/memory/heap.o \
    $(BUILD)/math/math.o \
    $(BUILD)/system/system.o \
    $(BUILD)/system/offsets.o \
    $(BUILD)/system/sysmodule.o \
    $(BUILD)/system/dialog.o \
    $(BUILD)/system/savedata.o \
    $(BUILD)/system/escalate.o \
    $(BUILD)/system/jit.o \
    $(BUILD)/system/fs.o \
    $(BUILD)/time/time.o \
    $(BUILD)/thread/thread.o \
    $(BUILD)/net/net.o \
    $(BUILD)/net/netctl.o \
    $(BUILD)/net/dns.o \
    $(BUILD)/net/http.o \
    $(BUILD)/draw/draw.o \
    $(BUILD)/draw/png.o \
    $(BUILD)/hud/hud.o \
    $(BUILD)/system/pkg.o \
    $(BUILD)/system/zip.o \
    $(BUILD)/system/freestd.o \
    $(BUILD)/system/scanf.o \
    $(BUILD)/system/libc.o \
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
    tests/unit/test_gl2.c \
    tests/unit/test_jit.c \
    tests/unit/test_fs.c \
    tests/unit/test_heap.c \
    tests/unit/test_math.c \
    tests/unit/test_dns.c \
    tests/unit/test_zip.c \
    tests/unit/test_http.c \
    tests/integration/test_pipeline_draw_tile.c \
    tests/integration/test_memory_surface.c \
    tests/integration/test_thread_worker_pool.c \
    tests/integration/test_net_loopback.c \
    src/agc/agc_tiler.c \
    src/agc/agc_compute.c \
    src/agc/agc_draw.c \
    src/draw/draw.c \
    src/draw/png.c \
    src/hud/hud.c \
    src/input/input.c \
    src/input/keyboard.c \
    src/input/mouse.c \
    src/audio/audio.c \
    src/audio/audiodec.c \
    src/videodec/videodec.c \
    src/memory/memory.c \
    src/memory/heap.c \
    src/math/math.c \
    src/system/system.c \
    src/system/offsets.c \
    src/system/sysmodule.c \
    src/system/dialog.c \
    src/system/savedata.c \
    src/system/escalate.c \
    src/system/jit.c \
    src/system/fs.c \
    src/system/pkg.c \
    src/system/zip.c \
    src/system/freestd.c \
    src/system/scanf.c \
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
    src/net/dns.c \
    src/net/http.c \
    src/gl/gl_context.c \
    src/gl/gfx.c \
    src/gl/gl_state.c \
    src/gl/gl_matrix.c \
    src/gl/gl_glu.c \
    src/gl/glut.c \
    src/gl/glut_font.c \
    src/gl/gl_draw.c \
    src/gl/gl_list.c \
    src/gl/gl_attrib.c \
    src/gl/gl_raster.c \
    src/gl/gl_eval.c \
    src/gl/gl_select.c \
    src/gl/gl_pixel.c \
    src/gl/glsl_lex.c \
    src/gl/glsl_parse.c \
    src/gl/glsl_pp.c \
    src/gl/glsl_sema.c \
    src/gl/glsl_emit.c \
    src/gl/glsl_gen.c \
    src/gl/glsl_builtin.c \
    src/gl/glsl_link.c \
    src/gl/glsl_exec.c \
    src/gl/glsl_ps.c \
    src/gl/glsl_vs.c \
    src/gl/gl_shader.c

HOST_CFLAGS := -std=c11 -Wall -Wextra -Iinclude -I. -pthread -lm -D_GNU_SOURCE -DOOPS_HOST_BUILD $(OOPS_TARGET_FLAGS)

.PHONY: all clean test test-unit test-int checks libc-check header-check

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

# The two checks that a test cannot make, because what they look at is the link and the
# preprocessor rather than a value a function returns. `libc-check` fails when a name the
# `<libc/*.h>` headers declare is left undefined by a payload link - which links cleanly and
# faults on the console. `header-check` fails when `include/GL/gl.h` stops compiling beside a
# real `GL/glext.h`, which is the only place a hosted title's two sets of GL headers meet.
#
# Separate from `test` because both need the target compiler rather than the host one, and a
# `make test` on a machine without it should still run the tests.
libc-check:
	@bash tools/libc-check/build.sh

header-check:
	@bash tools/header-check/build.sh

checks: libc-check header-check

test-unit: $(BUILD)/tests/test_runner
	$(BUILD)/tests/test_runner unit

test-int: $(BUILD)/tests/test_runner
	$(BUILD)/tests/test_runner int

clean:
	rm -rf build
