UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    CC      = x86_64-elf-gcc
else
    CC      = gcc
endif

CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone -fstack-protector-strong \
         -fno-pic -static -Wall -Wextra -g \
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
