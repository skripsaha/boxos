# ===================================================================
# BoxOS Makefile - Cross-platform OS build system
# ===================================================================
# Builds: disk image, floppy image, ISO, VDI, ELF with debug symbols
# Supports: Linux, macOS, Windows (Cygwin/MSYS2)
# ===================================================================
# IMPORTANT: Uses x86_64-elf-gcc cross-compiler on ALL platforms for
# consistent, reproducible builds. Native system gcc is NEVER used for
# kernel/userspace compilation (only for host tools like create_tagfs).
# ===================================================================

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Host compiler for tools (native to current OS)
CC_HOST = gcc

# Cross-compiler toolchain - SAME on all platforms
# This ensures identical code generation regardless of host OS
CC       = x86_64-elf-gcc
LD       = x86_64-elf-ld
OBJCOPY  = x86_64-elf-objcopy
AR       = x86_64-elf-ar
AS       = x86_64-elf-as
NM       = x86_64-elf-nm

# ==== TOOLS ====
ASM      = nasm
QEMU     = qemu-system-x86_64
TAGFS_TOOL = tools/create_tagfs

# ==== DEBUG CONFIGURATION ====
DEBUG    ?= off

# ==== FLAGS ====
# === BUILD CONFIGURATION ===
# Bootloader layout (LBA addressing):
#   Sector 0        : Stage1 (512 bytes, MBR)
#   Sectors 1-16    : Stage2 (16 sectors = 8192 bytes)
#   Sectors 17+     : Kernel (dynamic size, loaded via TagFS + Unreal Mode)
#   Sector 1034     : TagFS Superblock (primary)
#   Sector 1035     : TagFS Superblock (backup)
#   Sector 1036-1037: Journal Superblock (primary + backup)
#   Sectors 1038-2061: Journal Entries (512 entries * 2 sectors)
#   Sector 2062+    : Block Bitmap (dynamic size)
#   After bitmap    : Data Blocks (block 0=registry, 1=ftable, 2=mpool, 3+=files)
STAGE2_SECTORS      = 16
KERNEL_MAX_BYTES    = 33554432  # 32MB (sanity check; bootloader places page tables dynamically after kernel)
KERNEL_START_SECTOR = 17

ASM_INCLUDE    = -I$(SRCDIR)/kernel/arch/x86-64/gdt/
ASMFLAGS       =  -g -f bin
ASMFLAGS_ELF   = -g -f elf64 $(ASM_INCLUDE)
CFLAGS         = -Os -m64 -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-mmx -mno-avx \
                 -mcmodel=kernel -fno-PIC -fno-stack-protector -Wall -Wextra -fno-omit-frame-pointer
# Kernel directories first to ensure correct header resolution
INCLUDE_DIRS   := src $(shell find src/kernel -type d) $(shell find src/lib -type d) $(shell find src/boot -type d) $(shell find src/include -type d) $(shell find src/userspace -type d)
CFLAGS         += $(addprefix -I,$(INCLUDE_DIRS))

# Debug configuration (must be after CFLAGS definition)
ifeq ($(DEBUG),on)
CFLAGS += -DCONFIG_DEBUG_ENABLED=1 -DCONFIG_DEBUG_MODE=1
endif
LDFLAGS        = -g -T $(ENTRYDIR)/linker.ld -nostdlib -z max-page-size=0x1000 --oformat=binary

# ==== DIRECTORIES ====
SRCDIR      = src
BUILDDIR    = build
BOOTDIR     = $(SRCDIR)/boot
KERNELDIR   = $(SRCDIR)/kernel
ENTRYDIR    = $(KERNELDIR)/entry

# ==== SOURCES ====
STAGE1_SRC        = $(BOOTDIR)/stage1/stage1.asm
STAGE2_SRC        = $(BOOTDIR)/stage2/stage2.asm
KERNEL_ENTRY_SRC  = $(ENTRYDIR)/kernel_entry.asm

# ==== DISCOVER FILES ====
# Exclude macOS metadata files (._*) and userspace
C_SRCS      := $(shell find $(SRCDIR) -name '*.c' ! -name '._*' ! -path '*/userspace/*')
ASM_SRCS    := $(shell find $(SRCDIR) -name '*.asm' ! -name '._*' ! -path '*/userspace/*')

# ==== EXCLUSIONS ====
BOOT_ASM_SRCS     := $(STAGE1_SRC) $(STAGE2_SRC)
EXCLUDED_ASM_SRCS := $(BOOT_ASM_SRCS) $(KERNEL_ENTRY_SRC)
ASM_SRCS          := $(filter-out $(EXCLUDED_ASM_SRCS),$(ASM_SRCS))

# ==== OBJECT FILES ====
C_OBJS       := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
ASM_OBJS     := $(patsubst $(SRCDIR)/%.asm,$(BUILDDIR)/%.o,$(ASM_SRCS))
KERNEL_ENTRY_OBJ := $(patsubst $(SRCDIR)/%.asm,$(BUILDDIR)/%.o,$(KERNEL_ENTRY_SRC))

# ==== USERSPACE BINARIES ====
USERSPACE_DIR = $(SRCDIR)/userspace
USERSPACE_BUILD = $(USERSPACE_DIR)/build

# Shell binary
SHELL_DIR = $(USERSPACE_DIR)/shell
SHELL_BIN = $(SHELL_DIR)/shell.bin
SHELL_ELF = $(SHELL_DIR)/shell.elf
SHELL_EMBED = $(BUILDDIR)/shell_embed.o

# App ELF binaries (proca, procb)
APPS_DIR  = $(USERSPACE_DIR)/apps
PROCA_BIN = $(APPS_DIR)/proca.elf
PROCB_BIN = $(APPS_DIR)/procb.elf
TODAY_BIN   = $(APPS_DIR)/today.elf
MEMTEST_BIN = $(APPS_DIR)/memtest.elf

# Display server ELF
DISPLAY_DIR = $(USERSPACE_DIR)/display
DISPLAY_BIN = $(DISPLAY_DIR)/display.elf

# Utility ELF binaries
UTILS_DIR = $(USERSPACE_DIR)/utils
UTIL_NAMES = help create show files tag untag name trash erase \
             me info say reboot bye defrag fsck ipc_test
UTIL_ELFS = $(addprefix $(UTILS_DIR)/,$(addsuffix .elf,$(UTIL_NAMES)))

# ==== FINAL BINARIES ====
KERNEL_BIN   = $(BUILDDIR)/kernel.bin
KERNEL_ELF   = $(BUILDDIR)/kernel.elf
STAGE1_BIN   = $(BUILDDIR)/stage1.bin
STAGE2_BIN   = $(BUILDDIR)/stage2.bin
IMAGE        = $(BUILDDIR)/boxos.img
FLOPPY_IMG   = $(BUILDDIR)/boxos_floppy.img
ISO          = $(BUILDDIR)/boxos.iso
ISO_DIR      = $(BUILDDIR)/isofiles
VBOX_VDI     = $(BUILDDIR)/boxos.vdi

.PHONY: all clean run debug info check-deps install-deps

# ==== MAIN TARGET ====
all: check-deps $(IMAGE) $(KERNEL_ELF) $(FLOPPY_IMG) $(ISO) $(VBOX_VDI)

# ==== DEP CHECK ====
check-deps:
	@echo "Checking dependencies..."
	@echo "  Using cross-compiler: $(CC)"
	@command -v $(ASM) >/dev/null || (echo "ERROR: nasm not found" && exit 1)
	@command -v $(CC) >/dev/null || (echo "ERROR: x86_64-elf-gcc not found" && echo "" && \
		echo "Install cross-compiler toolchain:" && \
		echo "  Debian/Ubuntu: sudo apt install binutils-x86-64-linux-gnu gcc-x86-64-linux-gnu" && \
		echo "  Then create symlinks:" && \
		echo "    sudo ln -s /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc" && \
		echo "    sudo ln -s /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld" && \
		echo "    sudo ln -s /usr/bin/x86_64-linux-gnu-objcopy /usr/local/bin/x86_64-elf-objcopy" && \
		echo "    sudo ln -s /usr/bin/x86_64-linux-gnu-ar /usr/local/bin/x86_64-elf-ar" && \
		echo "" && \
		exit 1)
	@command -v $(LD) >/dev/null || (echo "ERROR: x86_64-elf-ld not found" && exit 1)
	@command -v $(OBJCOPY) >/dev/null || (echo "ERROR: x86_64-elf-objcopy not found" && exit 1)
	@command -v $(QEMU) >/dev/null || echo "WARNING: qemu-system-x86_64 not found (needed for 'make run')"
	@command -v xorriso >/dev/null || echo "WARNING: xorriso not found (needed for ISO)"
	@command -v VBoxManage >/dev/null || (echo "WARNING: VBoxManage not found" && sleep 2)
	@echo "All dependencies OK."

# ==== BUILD RULES ====
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

# Cross-platform build for create_tagfs (host tool, not target)
# Store OS signature to detect when to rebuild
TAGFS_OS_SIGNATURE = $(BUILDDIR)/.tagfs_host_os

$(TAGFS_TOOL): tools/create_tagfs.c $(TAGFS_OS_SIGNATURE) | $(BUILDDIR)
	@if [ ! -f "$(TAGFS_TOOL)" ] || [ "$$(cat $(TAGFS_OS_SIGNATURE) 2>/dev/null)" != "$(UNAME_S):$(UNAME_M)" ]; then \
		echo "OS changed or first build - compiling TagFS tool for $(UNAME_S) ($(UNAME_M))..."; \
		$(CC_HOST) -o $@ $< -Wall -Wextra; \
		echo "$(UNAME_S):$(UNAME_M)" > $(TAGFS_OS_SIGNATURE); \
		echo "TagFS tool built: $@"; \
	else \
		echo "TagFS tool already built for $(UNAME_S), skipping..."; \
	fi

# Create OS signature file on first build
$(TAGFS_OS_SIGNATURE): | $(BUILDDIR)
	@echo "$(UNAME_S):$(UNAME_M)" > $@

$(BUILDDIR)/kernel/drivers/usb/%.o: $(SRCDIR)/kernel/drivers/usb/%.c | $(BUILDDIR)
	@echo "Compiling USB driver $<..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -Os -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	@echo "Compiling $<..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.asm | $(BUILDDIR)
	@echo "Assembling $<..."
	@mkdir -p $(@D)
	@$(ASM) $(ASMFLAGS_ELF) $< -o $@

$(STAGE1_BIN): $(STAGE1_SRC) | $(BUILDDIR)
	@echo "Building Stage1..."
	@$(ASM) $(ASMFLAGS) $< -o $@

$(STAGE2_BIN): $(STAGE2_SRC) | $(BUILDDIR)
	@echo "Building Stage2..."
	@$(ASM) $(ASMFLAGS) $< -o $@

$(KERNEL_ENTRY_OBJ): $(KERNEL_ENTRY_SRC) | $(BUILDDIR)
	@echo "Assembling kernel entry..."
	@mkdir -p $(@D)
	@$(ASM) $(ASMFLAGS_ELF) $< -o $@

# ==== USERSPACE BUILD RULES ====
# Build boxlib (must be built before shell)
# PHONY so make always delegates to boxlib's own Makefile for dependency checking
.PHONY: $(USERSPACE_DIR)/boxlib/libbox.a
$(USERSPACE_DIR)/boxlib/libbox.a:
	@echo "Building boxlib..."
	@cd $(USERSPACE_DIR)/boxlib && $(MAKE)

# Build shell
$(SHELL_BIN): $(USERSPACE_DIR)/boxlib/libbox.a
	@echo "Building shell process..."
	@cd $(SHELL_DIR) && $(MAKE)
	@echo "Shell binary: $@ ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes)"

# Build apps (proca, procb, today, memtest)
$(PROCA_BIN) $(PROCB_BIN) $(TODAY_BIN) $(MEMTEST_BIN): $(USERSPACE_DIR)/boxlib/libbox.a
	@echo "Building apps..."
	@cd $(APPS_DIR) && $(MAKE)
	@echo "proca.elf: $$(stat -f%z $(PROCA_BIN) 2>/dev/null || stat -c%s $(PROCA_BIN) 2>/dev/null) bytes"
	@echo "procb.elf: $$(stat -f%z $(PROCB_BIN) 2>/dev/null || stat -c%s $(PROCB_BIN) 2>/dev/null) bytes"
	@echo "today.elf: $$(stat -f%z $(TODAY_BIN) 2>/dev/null || stat -c%s $(TODAY_BIN) 2>/dev/null) bytes"
	@echo "memtest.elf: $$(stat -f%z $(MEMTEST_BIN) 2>/dev/null || stat -c%s $(MEMTEST_BIN) 2>/dev/null) bytes"

# Build display server
$(DISPLAY_BIN): $(USERSPACE_DIR)/boxlib/libbox.a
	@echo "Building display server..."
	@cd $(DISPLAY_DIR) && $(MAKE)
	@echo "display.elf: $$(stat -f%z $(DISPLAY_BIN) 2>/dev/null || stat -c%s $(DISPLAY_BIN) 2>/dev/null) bytes"

# Build utilities
$(UTIL_ELFS): $(USERSPACE_DIR)/boxlib/libbox.a
	@echo "Building utilities..."
	@cd $(UTILS_DIR) && $(MAKE)

$(SHELL_ELF): $(SHELL_BIN)
	@echo "Shell ELF: $@ ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes)"

$(BUILDDIR)/shell.bin: $(SHELL_BIN)
	@echo "Copying shell binary to build directory..."
	@cp $< $@
	@echo "Shell binary: $@ ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes)"

$(BUILDDIR)/shell.elf: $(SHELL_ELF)
	@echo "Copying shell ELF to build directory..."
	@cp $< $@
	@echo "Shell ELF: $@ ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes)"

$(SHELL_EMBED): $(BUILDDIR)/shell.elf
	@echo "Stripping debug symbols from shell..."
	@$(OBJCOPY) --strip-debug $(BUILDDIR)/shell.elf $(BUILDDIR)/shell_stripped.elf
	@echo "Embedding shell ELF into kernel (stripped: $$(stat -f%z $(BUILDDIR)/shell_stripped.elf 2>/dev/null || stat -c%s $(BUILDDIR)/shell_stripped.elf 2>/dev/null) bytes)..."
	@cd $(BUILDDIR) && \
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    shell_stripped.elf shell_embed.o
	@echo "Embedded object: $@"

# ==== KERNEL BUILD RULES ====
$(KERNEL_BIN): $(KERNEL_ENTRY_OBJ) $(C_OBJS) $(ASM_OBJS) $(SHELL_EMBED)
	@echo "Linking kernel (raw binary)..."
	@$(LD) $(LDFLAGS) -o $@ $^
	@echo "Validating kernel size..."
	@KERNEL_SIZE=$$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null); \
	MAX_SIZE=$(KERNEL_MAX_BYTES); \
	echo "  Kernel size: $$KERNEL_SIZE bytes"; \
	echo "  Max allowed: $$MAX_SIZE bytes (32MB sanity limit)"; \
	if [ $$KERNEL_SIZE -gt $$MAX_SIZE ]; then \
		echo "ERROR: Kernel binary exceeds 32MB sanity limit!"; \
		echo "  Overflow: $$((KERNEL_SIZE - MAX_SIZE)) bytes"; \
		exit 1; \
	else \
		REMAINING=$$((MAX_SIZE - KERNEL_SIZE)); \
		echo "  Remaining space: $$REMAINING bytes"; \
	fi

$(KERNEL_ELF): $(KERNEL_ENTRY_OBJ) $(C_OBJS) $(ASM_OBJS) $(SHELL_EMBED)
	@echo "Linking kernel ELF (with embedded shell binary)..."
	@$(LD) -g -nostdlib -T $(ENTRYDIR)/linker.ld -o $@ $^


# ==== DISK IMAGES ====
$(IMAGE): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(PROCA_BIN) $(PROCB_BIN) $(TODAY_BIN) $(MEMTEST_BIN) $(DISPLAY_BIN) $(UTIL_ELFS) $(TAGFS_TOOL)
	@echo "Creating disk image (10MB)..."
	@dd if=/dev/zero of=$@ bs=512 count=20480 status=none
	@echo "  Writing Stage1 (sector 0, 512 bytes)..."
	@dd if=$(STAGE1_BIN) of=$@ bs=512 conv=notrunc status=none
	@echo "  Writing Stage2 (sectors 1-9, $(STAGE2_SECTORS) sectors)..."
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "  Writing Kernel (sector $(KERNEL_START_SECTOR)+, dynamic size via TagFS)..."
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	@echo "  Creating TagFS v1..."
	@$(TAGFS_TOOL) $@ \
		$(KERNEL_BIN)   "system" \
		$(SHELL_BIN)    "utility,system,app,autostart" \
		$(PROCA_BIN)    "app" \
		$(PROCB_BIN)    "app" \
		$(TODAY_BIN)    "app,utility" \
		$(MEMTEST_BIN)  "app,utility" \
		$(DISPLAY_BIN)  "display,system,utility" \
		$(UTILS_DIR)/help.elf    "utility" \
		$(UTILS_DIR)/create.elf  "utility,storage" \
		$(UTILS_DIR)/show.elf    "utility,storage" \
		$(UTILS_DIR)/files.elf   "utility,storage" \
		$(UTILS_DIR)/tag.elf     "utility,storage" \
		$(UTILS_DIR)/untag.elf   "utility,storage" \
		$(UTILS_DIR)/name.elf    "utility,storage" \
		$(UTILS_DIR)/trash.elf   "utility,storage" \
		$(UTILS_DIR)/erase.elf   "utility,storage" \
		$(UTILS_DIR)/me.elf      "utility" \
		$(UTILS_DIR)/info.elf    "utility,storage" \
		$(UTILS_DIR)/say.elf     "utility" \
		$(UTILS_DIR)/reboot.elf  "utility,system" \
		$(UTILS_DIR)/bye.elf     "utility,system" \
		$(UTILS_DIR)/defrag.elf  "utility,storage" \
		$(UTILS_DIR)/fsck.elf    "utility,storage" \
		$(USERSPACE_DIR)/hello.txt   "message,text" \
		$(UTILS_DIR)/ipc_test.elf "utility"
	@echo "Disk image created: $(IMAGE)"

$(FLOPPY_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@echo "Creating floppy image (1.44MB)..."
	@dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	@dd if=$(STAGE1_BIN) of=$@ bs=512 conv=notrunc status=none
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	@echo "Floppy image created: $(FLOPPY_IMG)"

$(ISO): $(FLOPPY_IMG)
	@echo "Creating ISO image..."
	@mkdir -p $(ISO_DIR)
	@cp $< $(ISO_DIR)/boot.img
	@xorriso -as mkisofs -b boot.img -no-emul-boot -boot-load-size 2880 -boot-info-table -o $@ $(ISO_DIR)

$(VBOX_VDI): $(IMAGE)
	@echo "Creating VirtualBox VDI..."
	@VBoxManage convertfromraw $< $@ --format VDI

# Comma helper for $(if ...) inside recipes
comma := ,

# ==== RUN CONFIGURATION ====
# Usage: make run [CORES=N] [MEM=size] [FULLSCREEN=on] [USB=on|off] [AHCI=on] [LOG=on] [GDB=on] [DEBUG=on]
#
# Examples:
#   make run                          — 1 core, 512M, USB keyboard
#   make run CORES=4 MEM=4G          — 4 cores, 4GB RAM
#   make run FULLSCREEN=on           — cocoa fullscreen mode
#   make run LOG=on                  — enable QEMU interrupt/reset logging
#   make run GDB=on                  — pause for GDB attach (-s -S)
#   make run AHCI=on USB=off         — AHCI disk controller, no USB
#   make run CORES=8 MEM=4G LOG=on FULLSCREEN=on
#   make run DEBUG=on                — enable debug output (CONFIG_DEBUG_ENABLED)

CORES      ?= 1
MEM        ?= 512M
FULLSCREEN ?= off
USB        ?= on
AHCI       ?= off
LOG        ?= off
GDB        ?= off
DEBUG      ?= off

run: $(IMAGE)
	@echo "=== BoxOS QEMU ==="
	@echo "  Cores: $(CORES) | RAM: $(MEM) | USB: $(USB) | AHCI: $(AHCI)"
	@echo "  Fullscreen: $(FULLSCREEN) | Log: $(LOG) | GDB: $(GDB) | Debug: $(DEBUG)"
	@echo "==================="
	@$(QEMU) \
		$(if $(filter on,$(AHCI)), \
			-drive id=disk0$(comma)file=$<$(comma)format=raw$(comma)if=none \
			-device ahci$(comma)id=ahci -device ide-hd$(comma)drive=disk0$(comma)bus=ahci.0, \
			-drive format=raw$(comma)file=$<$(comma)index=0$(comma)media=disk) \
		-m $(MEM) \
		-serial stdio \
		$(if $(filter-out 1,$(CORES)),-smp $(CORES)$(comma)cores=$(CORES)$(comma)threads=1$(comma)sockets=1) \
		$(if $(filter on,$(FULLSCREEN)),$(if $(filter Darwin,$(UNAME_S)),-display cocoa$(comma)full-screen=on$(comma)zoom-to-fit=on,-full-screen)) \
		$(if $(filter on,$(USB)),-device qemu-xhci -device usb-kbd) \
		$(if $(filter on,$(LOG)),-d int$(comma)cpu_reset -no-reboot -no-shutdown -D boxos_qemu.log) \
		$(if $(filter on,$(GDB)),-s -S)

clean:
	@echo "Cleaning build..."
	@rm -rf $(BUILDDIR)
	@rm -f $(TAGFS_TOOL)
	@cd $(USERSPACE_DIR) && $(MAKE) clean
	@cd $(SHELL_DIR) && $(MAKE) clean
	@cd $(APPS_DIR) && $(MAKE) clean
	@cd $(DISPLAY_DIR) && $(MAKE) clean
	@cd $(UTILS_DIR) && $(MAKE) clean
	@cd $(USERSPACE_DIR)/boxlib && $(MAKE) clean

install-deps:
	@echo "Installing dependencies for $(UNAME_S)..."
	@if [ "$(UNAME_S)" = "Linux" ]; then \
		echo "Linux detected (Debian/Ubuntu)..."; \
		sudo apt update; \
		sudo apt install -y nasm qemu-system-x86 xorriso virtualbox make \
			binutils-x86-64-linux-gnu gcc-x86-64-linux-gnu; \
		echo "Creating symlinks for x86_64-elf-gcc toolchain..."; \
		sudo mkdir -p /usr/local/bin; \
		sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc; \
		sudo ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld; \
		sudo ln -sf /usr/bin/x86_64-linux-gnu-objcopy /usr/local/bin/x86_64-elf-objcopy; \
		sudo ln -sf /usr/bin/x86_64-linux-gnu-ar /usr/local/bin/x86_64-elf-ar; \
		sudo ln -sf /usr/bin/x86_64-linux-gnu-as /usr/local/bin/x86_64-elf-as; \
		sudo ln -sf /usr/bin/x86_64-linux-gnu-nm /usr/local/bin/x86_64-elf-nm; \
		echo "Cross-compiler toolchain installed and configured."; \
	elif [ "$(UNAME_S)" = "Darwin" ]; then \
		echo "macOS detected..."; \
		echo "Install dependencies using Homebrew:"; \
		echo "  brew install nasm qemu xorriso x86_64-elf-gcc"; \
		echo ""; \
		echo "Or using MacPorts:"; \
		echo "  sudo port install nasm qemu xorriso crossgcc-x86_64-elf"; \
	else \
		echo "Unsupported OS: $(UNAME_S)"; \
		echo "Please install dependencies manually:"; \
		echo "  - nasm (assembler)"; \
		echo "  - x86_64-elf-gcc (cross-compiler)"; \
		echo "  - qemu-system-x86_64 (emulator)"; \
	fi
	@echo "Run 'make check-deps' to verify installation."

info:
	@echo "BoxOS Makefile Info"
	@echo "Targets:"
	@echo "  all        — full build (img, iso, elf)"
	@echo "  run        — run BoxOS in QEMU"
	@echo "  debug      — run QEMU with GDB waiting (alias for GDB=on)"
	@echo "  clean      — clean build directory"
	@echo "  install-deps — install required packages"
	@echo ""
	@echo "Run flags:"
	@echo "  DEBUG=on   — enable debug output (rebuilds with CONFIG_DEBUG_ENABLED=1)"
	@echo "  GDB=on     — pause QEMU for GDB attach (-s -S)"
	@echo "  CORES=N    — number of CPU cores"
	@echo "  MEM=size   — RAM size (e.g. 512M, 4G)"
	@echo "  FULLSCREEN=on — fullscreen mode"
	@echo "  USB=on|off — enable/disable USB keyboard"
	@echo "  AHCI=on    — use AHCI disk controller"
	@echo "  LOG=on     — enable QEMU interrupt/reset logging"

debug: $(IMAGE)
	@$(MAKE) run GDB=on IMAGE=$(IMAGE) CORES=$(CORES) MEM=$(MEM) FULLSCREEN=$(FULLSCREEN) USB=$(USB) AHCI=$(AHCI) LOG=$(LOG) DEBUG=$(DEBUG)
