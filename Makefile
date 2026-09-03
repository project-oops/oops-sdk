# oops-sdk Makefile
# Builds standalone static libraries for target-side homebrew

CC := clang
AR := ar
TARGET_FLAGS := -target x86_64-unknown-freebsd -ffreestanding -fno-builtin -nostdlib -fPIC -fno-stack-protector
CFLAGS := $(TARGET_FLAGS) -std=c11 -Wall -Wextra -Werror -Iinclude
BUILD := build

OBJS := \
    $(BUILD)/display.o \
    $(BUILD)/agc/agc_display.o \
    $(BUILD)/agc/agc_tiler.o \
    $(BUILD)/gnm/gnm_display.o \
    $(BUILD)/input/input.o \
    $(BUILD)/audio/audio.o \
    $(BUILD)/memory/memory.o \
    $(BUILD)/system/system.o \
    $(BUILD)/time/time.o \
    $(BUILD)/thread/thread.o \
    $(BUILD)/net/net.o

.PHONY: all clean

all: $(BUILD)/liboops.a $(BUILD)/liboops_display.a

$(BUILD)/liboops.a: $(OBJS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $(OBJS)

$(BUILD)/liboops_display.a: $(BUILD)/liboops.a
	@mkdir -p $(@D)
	cp $< $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
