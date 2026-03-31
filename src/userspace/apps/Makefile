# ============================================================================
# BoxOS Apps Makefile - builds proca, procb, today, memtest
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

BOXLIB_DIR = ../boxlib
LDFLAGS    = -nostdlib -static -T $(BOXLIB_DIR)/user.ld
BOXLIB     = ../boxlib/libbox.a
BOXLIB_START = ../build/boxlib_start.o

.PHONY: all clean

all: proca.elf procb.elf today.elf memtest.elf

# ---- ELF binaries (stored in TagFS) ----
proca.elf: $(BOXLIB_START) proca.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) proca.o $(BOXLIB)

procb.elf: $(BOXLIB_START) procb.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) procb.o $(BOXLIB)

today.elf: $(BOXLIB_START) today.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) today.o $(BOXLIB)

memtest.elf: $(BOXLIB_START) memtest.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) memtest.o $(BOXLIB)

# ---- object files ----
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- boxlib_start entry (shared with shell) ----
$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.elf
