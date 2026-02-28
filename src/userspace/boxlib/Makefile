UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
    CC = x86_64-elf-gcc
    AR = x86_64-elf-ar
else
    CC = gcc
    AR = ar
endif

CFLAGS = -nostdlib -fno-builtin -ffreestanding -mno-red-zone \
         -Iinclude -I../../include -Wall -Wextra -O2 -fno-stack-protector -Wno-unused-parameter

SRCS = src/core/notify.c src/core/chain.c src/core/result.c src/core/string.c \
       src/core/io.c src/core/file.c src/core/system.c \
       src/core/yield.c src/core/result_wait.c \
       src/core/ipc.c src/core/format.c src/core/convert.c \
       src/core/io_mode.c \
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
	nasm -f elf64 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) libbox.a

.PHONY: all clean
