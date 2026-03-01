# BoxOS Utilities Makefile

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

UTILS = help create show files tag untag name trash erase \
        me info say reboot bye defrag fsck ipc_test

.PHONY: all clean

all: $(addsuffix .elf,$(UTILS))

%.elf: $(BOXLIB_START) %.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) $*.o $(BOXLIB)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.elf
