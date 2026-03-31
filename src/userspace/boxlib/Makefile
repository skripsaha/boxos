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

SRCS = src/core/notify.c src/core/result.c src/core/string.c \
       src/core/io.c src/core/file.c src/core/system.c \
       src/core/yield.c src/core/result_wait.c \
       src/core/ipc.c src/core/format.c src/core/convert.c \
       src/core/io_mode.c src/core/heap.c src/core/stack_protector.c \
       src/time.c \
       src/io/keyboard.c src/io/vga.c
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
