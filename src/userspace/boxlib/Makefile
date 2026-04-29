# ============================================================================
# BoxLib Makefile - Cross-platform userspace C library
# ============================================================================
# Uses x86_64-elf-gcc cross-compiler on ALL platforms for consistency.
# Native system gcc is NEVER used.
# ============================================================================

# Cross-compiler toolchain - SAME on all platforms
CC  = x86_64-elf-gcc
AR  = x86_64-elf-ar

# Compiler flags
# -fno-stack-protector: No libc in userspace, canary support unavailable
# -mno-red-zone: Required for kernel/interrupt context compatibility
CFLAGS = -nostdlib -fno-builtin -ffreestanding -mno-red-zone \
         -fno-stack-protector \
         -Iinclude -I../../include -Wall -Wextra -O2 -Wno-unused-parameter

SRCS = src/core/notify.c \
       src/core/result.c \
       src/core/manifest.c \
       src/deck/storage.c \
       src/deck/system.c \
       src/deck/ipc.c \
       src/deck/hardware/vga.c \
       src/deck/hardware/keyboard.c \
       src/deck/hardware/time.c \
       src/io/print.c \
       src/util/string.c \
       src/util/convert.c \
       src/util/heap.c \
       src/arch/x86_64/stack_protector.c
OBJS = $(SRCS:.c=.o)

all: libbox.a ../build/boxlib_start.o

libbox.a: $(OBJS)
	$(AR) rcs $@ $^

# Build entry point
../build/boxlib_start.o: boxlib_start.asm
	@echo "Building entry point..."
	@mkdir -p ../build
	nasm -f elf64 -I include/ $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) libbox.a

.PHONY: all clean
