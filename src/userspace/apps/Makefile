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
# -fno-ipa-ra:   Disable IPRA. Without it GCC parks live values in caller-
#                saved registers across calls. Any clobber (kernel
#                save/restore quirk, IRQ stack-trash, scheduler hiccup) then
#                yields a wild pointer that surfaces as a far-away page
#                fault inside boxlib (memcpy/ManifestBuilderAddOp). Same
#                fix the boxlib Makefile already carries — must be on every
#                userspace binary that links libbox.a.
CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static -fno-ipa-ra \
         -Wall -Wextra -g \
         -I../boxlib/include \
         -I../../include

BOXLIB_DIR = ../boxlib
LDFLAGS    = -nostdlib -static -T $(BOXLIB_DIR)/user.ld
BOXLIB     = ../boxlib/libbox.a
BOXLIB_START = ../build/boxlib_start.o

.PHONY: all clean

all: proca.elf procb.elf today.elf memtest.elf mtest.elf chain.elf decks.elf bench.elf touch_test.elf touch_stress.elf lifecycle.elf persist.elf

# ---- ELF binaries (stored in TagFS) ----
proca.elf: $(BOXLIB_START) proca.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) proca.o $(BOXLIB)

procb.elf: $(BOXLIB_START) procb.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) procb.o $(BOXLIB)

today.elf: $(BOXLIB_START) today.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) today.o $(BOXLIB)

memtest.elf: $(BOXLIB_START) memtest.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) memtest.o $(BOXLIB)

mtest.elf: $(BOXLIB_START) mtest.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) mtest.o $(BOXLIB)

chain.elf: $(BOXLIB_START) chain.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) chain.o $(BOXLIB)

decks.elf: $(BOXLIB_START) decks.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) decks.o $(BOXLIB)

bench.elf: $(BOXLIB_START) bench.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) bench.o $(BOXLIB)

touch_test.elf: $(BOXLIB_START) touch_test.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) touch_test.o $(BOXLIB)

touch_stress.elf: $(BOXLIB_START) touch_stress.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) touch_stress.o $(BOXLIB)

lifecycle.elf: $(BOXLIB_START) lifecycle.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) lifecycle.o $(BOXLIB)

persist.elf: $(BOXLIB_START) persist.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) persist.o $(BOXLIB)

# ---- object files ----
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- boxlib_start entry (shared with shell) ----
$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.elf
