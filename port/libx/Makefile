CC ?= gcc
AR ?= ar
RANLIB ?= ranlib

ROOT ?= ../..
BUILD := $(ROOT)/Kernel/build/libx
SYSROOT := $(ROOT)/system
INCLUDE := $(CURDIR)/include
LIBC_INCLUDE := $(ROOT)/port/libc/include

CFLAGS := -std=gnu11 \
	-ffreestanding \
	-fPIC \
	-fno-stack-protector \
	-fno-builtin \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-m64 \
	-mno-red-zone \
	-mgeneral-regs-only \
	-mno-sse \
	-mno-sse2 \
	-mno-mmx \
	-msoft-float \
	-Wall \
	-Wextra \
	-O2 \
	-I$(INCLUDE) \
	-I$(LIBC_INCLUDE)

HEADERS := $(wildcard $(INCLUDE)/X11/*.h)
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(BUILD)/%.o)

.PHONY: all clean

all: $(SYSROOT)/lib/libx.a headers

$(BUILD)/%.o: src/%.c $(HEADERS) $(LIBC_INCLUDE)/os/gui.h
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libx.a: $(OBJECTS)
	mkdir -p $(dir $@)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(SYSROOT)/lib/libx.a: $(BUILD)/libx.a
	mkdir -p $(dir $@)
	cp $< $@

headers: $(HEADERS)
	mkdir -p $(SYSROOT)/include/X11
	cp $(HEADERS) $(SYSROOT)/include/X11/

clean:
	rm -rf $(BUILD)
