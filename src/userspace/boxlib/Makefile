UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
    CC = x86_64-elf-gcc
    AR = x86_64-elf-ar
else
    CC = gcc
    AR = ar
endif

CFLAGS = -nostdlib -fno-builtin -ffreestanding -mno-red-zone \
         -Iinclude -Wall -Wextra -O2 -fno-stack-protector -Wno-unused-parameter

SRCS = src/core/notify.c src/core/result.c src/core/string.c \
       src/core/io.c src/core/file.c src/core/system.c \
       src/core/yield.c src/core/result_wait_yield.c \
       src/core/result_wait_umwait.c src/core/result_wait_smart.c \
       src/core/ipc.c src/core/format.c src/core/convert.c \
       src/system.c src/proc.c \
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

src/system.o: src/system.c include/box/system.h include/box/error.h

clean:
	rm -f $(OBJS) libbox.a

.PHONY: all clean
