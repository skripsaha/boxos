# BoxOS Apps Makefile - builds proca and procb

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    CC      = x86_64-elf-gcc
else
    CC      = gcc
endif

CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone -fno-stack-protector \
         -fno-pic -static -Wall -Wextra -g \
         -I../boxlib/include \
         -I../../include

BOXLIB_DIR = ../boxlib
LDFLAGS    = -nostdlib -static -T $(BOXLIB_DIR)/user.ld
BOXLIB     = ../boxlib/libbox.a
BOXLIB_START = ../build/boxlib_start.o

.PHONY: all clean

all: proca.elf procb.elf today.elf

# ---- ELF binaries (stored in TagFS) ----
proca.elf: $(BOXLIB_START) proca.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) proca.o $(BOXLIB)

procb.elf: $(BOXLIB_START) procb.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) procb.o $(BOXLIB)

today.elf: $(BOXLIB_START) today.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) today.o $(BOXLIB)

# ---- object files ----
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- boxlib_start entry (shared with shell) ----
$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.elf
