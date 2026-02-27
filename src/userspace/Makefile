# ============================================================================
# BoxOS User Programs Build System
# ============================================================================

# Detect OS for cross-compilation
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
    CC = x86_64-elf-gcc
    LD = x86_64-elf-ld
    OBJCOPY = x86_64-elf-objcopy
    AS = nasm
else ifeq ($(UNAME),Linux)
    CC = gcc
    LD = ld
    OBJCOPY = objcopy
    AS = nasm
else
    CC = x86_64-elf-gcc
    LD = x86_64-elf-ld
    OBJCOPY = x86_64-elf-objcopy
    AS = nasm
endif

# Compiler flags
CFLAGS = -std=c11 -ffreestanding -nostdlib -mno-red-zone -Wall -Wextra \
         -I boxlib/include -O2 -mcmodel=large

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
