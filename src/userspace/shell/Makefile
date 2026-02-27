# BoxOS Shell Makefile

# Detect OS for cross-compilation
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS: use cross-compiler
    CC = x86_64-elf-gcc
    AR = x86_64-elf-ar
else
    # Linux: use system compiler
    CC = gcc
    AR = ar
endif

# Compiler flags
CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone -fno-stack-protector \
         -fno-pic -static -Wall -Wextra -g \
         -I../boxlib/include \
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
CMD_SRCS = commands/cmd_help.c commands/cmd_exit.c \
           commands/cmd_create.c commands/cmd_show.c commands/cmd_files.c \
           commands/cmd_tag.c commands/cmd_untag.c commands/cmd_name.c \
           commands/cmd_trash.c commands/cmd_erase.c commands/cmd_use.c \
           commands/cmd_me.c commands/cmd_info.c commands/cmd_say.c \
           commands/cmd_reboot.c commands/cmd_bye.c \
           commands/cmd_defrag.c commands/cmd_fsck.c \
           commands/cmd_ipc_test.c

SRCS = $(CORE_SRCS) $(CMD_SRCS)
OBJS = $(SRCS:.c=.o)

# Output binary
TARGET_ELF = shell.elf
TARGET = shell.bin
OBJCOPY = x86_64-elf-objcopy

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
