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
# -mno-red-zone:        Required for kernel/interrupt context compatibility
# -fno-ipa-ra:          Force-save caller-saved regs around every call.
#                       Without this, GCC's interprocedural register
#                       allocator notices that some leaf functions don't
#                       touch r10/r11 and parks `out` arguments in r10
#                       across calls — but if anything (kernel save/restore
#                       bug, IRQ stack-trash, scheduler context switch
#                       hiccup) corrupts the supposedly-preserved register,
#                       we silently dereference a wild pointer. Saving on
#                       the stack across every call costs almost nothing
#                       and removes a whole class of subtle UB. Hunted
#                       on 2026-04-29 (random page-fault to .text).
CFLAGS = -nostdlib -fno-builtin -ffreestanding -mno-red-zone \
         -fno-stack-protector -fno-ipa-ra \
         -Iinclude -I../../include -Wall -Wextra -O2 -Wno-unused-parameter

SRCS = src/core/notify.c \
       src/core/result.c \
       src/core/manifest.c \
       src/print.c \
       src/file.c \
       src/system.c \
       src/ipc.c \
       src/time.c \
       src/memory.c \
       src/vga.c \
       src/keyboard.c \
       src/string.c \
       src/convert.c \
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
