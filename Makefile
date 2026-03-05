# ===================================================================
# BoxOS Makefile - Cross-platform OS build system
# ===================================================================
# Builds: disk image, floppy image, ISO, VDI, ELF with debug symbols
# Supports: Linux, macOS, Windows (Cygwin/MSYS2)
# ===================================================================

UNAME_S := $(shell uname -s)

# OS-specific toolchain selection
ifeq ($(UNAME_S),Darwin)  # macOS
    CC       = x86_64-elf-gcc
    LD       = x86_64-elf-ld
    OBJCOPY  = x86_64-elf-objcopy
else ifeq ($(UNAME_S),Linux)  # Linux
    CC       = gcc
    LD       = ld
    OBJCOPY  = objcopy
else  # Windows (Cygwin/MSYS2) or other
    CC       = gcc
    LD       = ld
    OBJCOPY  = objcopy
endif

# ==== TOOLS ====
ASM      = nasm
QEMU     = qemu-system-x86_64
TAGFS_TOOL = tools/create_tagfs

# ==== FLAGS ====
# === BUILD CONFIGURATION ===
# Bootloader layout (LBA addressing):
#   Sector 0        : Stage1 (512 bytes, MBR)
#   Sectors 1-16    : Stage2 (16 sectors = 8192 bytes)
#   Sectors 17+     : Kernel (dynamic size, loaded via TagFS + Unreal Mode)
#   Sector 1034     : TagFS Superblock
#   Sectors 1035-2058 : TagFS Metadata (1024 entries)
#   Sectors 2059-2060 : Journal Superblock (primary + backup)
#   Sectors 2061-3085 : Journal Entries (512 entries * 2 sectors)
#   Sector 3086+    : TagFS Data
STAGE2_SECTORS      = 16
KERNEL_MAX_BYTES    = 13631488  # 13MB (must fit below PAGE_TABLE_BASE at 0xE00000)
KERNEL_START_SECTOR = 17

ASMFLAGS       =  -g -f bin
ASMFLAGS_ELF   = -g -f elf64
CFLAGS         = -Os -m64 -ffreestanding -nostdlib -Wall -Wextra -fstack-protector-strong
INCLUDE_DIRS   := $(shell find src -type d)
CFLAGS         += $(addprefix -I,$(INCLUDE_DIRS))
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
# Include test files since CONFIG_RUN_STARTUP_TESTS is enabled
C_SRCS      := $(shell find $(SRCDIR) -name '*.c' ! -name '._*' ! -path '*/userspace/*')
ASM_SRCS    := $(shell find $(SRCDIR) -name '*.asm' ! -name '._*' ! -path '*/userspace/*')

# ==== EXCLUSIONS ====
BOOT_ASM_SRCS     := $(STAGE1_SRC) $(STAGE2_SRC)
# USER_ASM_SRCS     := $(SRCDIR)/userspace/user_test.asm $(SRCDIR)/userspace/user_storage_test.asm $(SRCDIR)/userspace/concurrent_test.asm
EXCLUDED_ASM_SRCS := $(BOOT_ASM_SRCS) $(KERNEL_ENTRY_SRC) $(USER_ASM_SRCS)
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
	@command -v $(ASM) >/dev/null || (echo "ERROR: nasm not found" && exit 1)
	@command -v $(CC) >/dev/null || (echo "ERROR: gcc not found" && exit 1)
	@command -v $(LD) >/dev/null || (echo "ERROR: ld not found" && exit 1)
	@command -v $(QEMU) >/dev/null || echo "WARNING: qemu not found (needed for 'make run')"
	@command -v xorriso >/dev/null || echo "WARNING: xorriso not found (needed for ISO)"
	@command -v VBoxManage >/dev/null || (echo "WARNING: VBoxManage not found" && sleep 2)
	@echo "All dependencies OK."

# ==== BUILD RULES ====
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(TAGFS_TOOL): tools/create_tagfs.c
	@echo "Compiling TagFS tool..."
	@gcc -o $@ $< -Wall -Wextra

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
	echo "  Max allowed: $$MAX_SIZE bytes (13MB, Unreal Mode)"; \
	if [ $$KERNEL_SIZE -gt $$MAX_SIZE ]; then \
		echo "ERROR: Kernel binary exceeds 13MB limit (page tables at 0xE00000)!"; \
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
	@echo "  Creating TagFS (superblock=1034, metadata=1035, data=3086)..."
	@$(TAGFS_TOOL) $@ 1034 1035 3086 \
		$(KERNEL_BIN)   "system" \
		$(SHELL_BIN)    "utility,system,app,hw_vga,hw_keyboard,storage,autostart" \
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

# ==== UTILITIES ====
FULLSCREEN ?= on

run-xhci: $(IMAGE)
	@echo "Running BoxOS in QEMU with xHCI..."
	@$(QEMU) -drive file=$<,format=raw,index=0,media=disk -m 512M -serial stdio -device qemu-xhci -device usb-kbd

run: $(IMAGE)
	@echo "Running BoxOS in QEMU..."
	@$(QEMU) -drive format=raw,file=$< -m 512M -serial stdio

run-fullscreen: $(IMAGE)
	@echo "Running BoxOS in QEMU (Fullscreen=$(FULLSCREEN))..."
	@$(QEMU) -drive format=raw,file=$< \
		-m 512M \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-display cocoa,full-screen=$(FULLSCREEN),zoom-to-fit=on

runlog: $(IMAGE)
	@echo "Running BoxOS in QEMU with full logging..."
	@$(QEMU) -drive format=raw,file=$< -m 512M -serial stdio \
		-d int,cpu_reset -no-reboot -no-shutdown \
		-D boxos_qemu.log

CORES = 4
MEM = 4096M

run1: $(IMAGE)
	@echo "Running BoxOS in QEMU with $(CORES) cores and $(MEM) memory size"
	@$(QEMU) -drive format=raw,file=$< -m $(MEM) -serial stdio \
		-smp $(CORES),cores=$(CORES),threads=1,sockets=1 \
		-d int,cpu_reset -no-reboot -no-shutdown \
		-D boxos_qemu.log

debug: $(IMAGE)
	@echo "Running BoxOS in QEMU with debugger..."
	@$(QEMU) -drive format=raw,file=$< -m 512M -serial stdio -s -S

run-ahci: $(IMAGE)
	@echo "Running BoxOS in QEMU with AHCI controller..."
	@$(QEMU) -drive id=ahci0,file=$<,format=raw,if=none \
		-device ahci,id=ahci \
		-device ide-hd,drive=ahci0,bus=ahci.0 \
		-m 512M -serial stdio

clean:
	@echo "Cleaning build..."
	@rm -rf $(BUILDDIR)
	@cd $(USERSPACE_DIR) && $(MAKE) clean
	@cd $(SHELL_DIR) && $(MAKE) clean
	@cd $(APPS_DIR) && $(MAKE) clean
	@cd $(DISPLAY_DIR) && $(MAKE) clean
	@cd $(UTILS_DIR) && $(MAKE) clean
	@cd $(USERSPACE_DIR)/boxlib && $(MAKE) clean

install-deps:
	@echo "Installing dependencies..."
	@sudo apt update
	@sudo apt install -y nasm gcc binutils qemu-system-x86 xorriso virtualbox make

info:
	@echo $(QEMU) -drive format=raw,file=$(IMAGE) -m 512M -serial stdio -no-reboot -no-shutdown
	@echo "BoxOS Makefile Info"
	@echo "Targets:"
	@echo "  all        — full build (img, iso, elf)"
	@echo "  run        — run BoxOS in QEMU"
	@echo "  debug      — run QEMU with gdb waiting"
	@echo "  clean      — clean build directory"
	@echo "  install-deps — install required packages"
