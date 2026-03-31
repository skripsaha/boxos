# ============================================================================
# BoxOS Display Server Makefile
# ============================================================================
# Uses x86_64-elf-gcc cross-compiler on ALL platforms for consistency.
# ============================================================================

# Cross-compiler toolchain - SAME on all platforms
CC      = x86_64-elf-gcc

# Compiler flags
# -fno-stack-protector: No libc in userspace, canary support unavailable
# -fno-pic: Position-independent code not needed for static binary
# -mno-red-zone: Required for interrupt context compatibility
CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static \
         -Wall -Wextra -g \
         -I../boxlib/include \
         -I../../include

BOXLIB_DIR   = ../boxlib
LDFLAGS      = -nostdlib -static -T $(BOXLIB_DIR)/user.ld
BOXLIB       = ../boxlib/libbox.a
BOXLIB_START = ../build/boxlib_start.o

.PHONY: all clean

all: display.elf

display.elf: $(BOXLIB_START) display.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) display.o $(BOXLIB)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.elf
