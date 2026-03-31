# ============================================================================
# BoxOS User Programs Build System
# ============================================================================
# Uses x86_64-elf-gcc cross-compiler on ALL platforms for consistency.
# Native system gcc is NEVER used for kernel/userspace compilation.
# ============================================================================

# Cross-compiler toolchain - SAME on all platforms
CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy
AS      = nasm

# Compiler flags
# -mcmodel=large: Allows code to be placed anywhere in 64-bit address space
# -mno-red-zone: Required for interrupt context compatibility
CFLAGS = -std=c11 -ffreestanding -nostdlib -mno-red-zone \
         -fno-stack-protector \
         -Wall -Wextra -I boxlib/include -O2 -mcmodel=large

# Assembler flags
ASFLAGS = -f elf64

# Linker flags
LDFLAGS = -T boxlib/user.ld -nostdlib

# Paths
BUILD = build
BOXLIB = boxlib/libbox.a

# Default target
all: dirs

# Create build directory
dirs:
	@mkdir -p $(BUILD)

#==============================================================================
# Utilities
#==============================================================================

clean:
	rm -rf $(BUILD)

info:
	@echo "BoxOS User Programs Build System"
	@echo "CC:     $(CC)"
	@echo "LD:     $(LD)"
	@echo "AS:     $(AS)"
	@echo "BOXLIB: $(BOXLIB)"

.PHONY: all dirs clean info
