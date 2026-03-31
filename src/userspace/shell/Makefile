# ============================================================================
# BoxOS Shell Makefile
# ============================================================================
# Uses x86_64-elf-gcc cross-compiler on ALL platforms for consistency.
# ============================================================================

# Cross-compiler toolchain - SAME on all platforms
CC      = x86_64-elf-gcc
AR      = x86_64-elf-ar
OBJCOPY = x86_64-elf-objcopy

# Compiler flags
# -fno-stack-protector: No libc in userspace, canary support unavailable
# -fno-pic: Position-independent code not needed for static binary
# -mno-red-zone: Required for interrupt context compatibility
CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static \
         -Wall -Wextra -g \
         -I../boxlib/include \
         -I../../include \
         -I.

# Linker flags
BOXLIB_DIR = ../boxlib
LDFLAGS = -nostdlib -static -T $(BOXLIB_DIR)/user.ld

# BoxLib path
BOXLIB = ../boxlib/libbox.a

# Entry point object
BOXLIB_START = ../build/boxlib_start.o

# Source files
CORE_SRCS = shell.c parser.c executor.c
CMD_SRCS = commands/cmd_use.c commands/cmd_exit.c commands/cmd_clear.c

SRCS = $(CORE_SRCS) $(CMD_SRCS)
OBJS = $(SRCS:.c=.o)

# Output binary
TARGET_ELF = shell.elf
TARGET = shell.bin

# Build rules
all: $(TARGET)

$(TARGET): $(TARGET_ELF)
	@echo "Converting ELF to flat binary..."
	$(OBJCOPY) -O binary --set-section-flags .bss=alloc,load,contents $< $@
	@echo "Shell binary created: $(TARGET)"
	@ls -lh $(TARGET)

$(TARGET_ELF): $(BOXLIB_START) $(OBJS) $(BOXLIB)
	@echo "Linking $@..."
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) $(OBJS) $(BOXLIB)
	@echo "Shell ELF created: $(TARGET_ELF)"

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Build entry point from assembly
$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@echo "Assembling entry point..."
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f $(OBJS) $(TARGET_ELF) $(TARGET)
	@echo "Cleaned shell build artifacts"

.PHONY: all clean
