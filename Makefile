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

# Physical address of the boot_info handoff block. ONE number, four consumers:
# src/boot/stage2/stage2.asm (BIOS loader writes it), src/boot/uefi/tagboot.c
# (UEFI loader writes it), src/kernel/entry/kernel_entry.asm (reads the boot
# stack out of it before any C runs) and src/include/boot_info.h (everything
# else). It was spelled out separately in all four until 2026-08-23, when
# moving it turned up the copy in kernel_entry.asm — which would have taken
# the kernel's stack pointer from whatever happened to be at the old address.
# The two headers _Static_assert against this value, so a drift is a build
# error rather than a boot that gets as far as its first push.
BOOT_INFO_ADDR      = 0xA000
KERNEL_MAX_BYTES    = 33554432  # 32MB (sanity check; bootloader places page tables dynamically after kernel)
KERNEL_START_SECTOR = 17

ASM_INCLUDE    = -I$(SRCDIR)/kernel/arch/x86-64/gdt/
ASMFLAGS       =  -g -f bin
# Layout facts the image build owns, handed to the assembler rather than
# repeated inside it.
ASM_LAYOUT     = -DSTAGE2_SECTORS=$(STAGE2_SECTORS) -DBOOT_INFO_ADDR=$(BOOT_INFO_ADDR)
ASMFLAGS_ELF   = -g -f elf64 $(ASM_INCLUDE) $(ASM_LAYOUT)
# ─── Kernel CFLAGS — production-grade real-HW hardening ────────────────────
#
# Core ABI:
#   -Os                 minimize code size (kernel images are dd-cat'd onto
#                       disk images, smaller is better)
#   -m64                64-bit code generation
#   -ffreestanding      no hosted-environment assumptions (no libc)
#   -nostdlib           no linker stdlib startup
#   -mno-red-zone       interrupt-safe: SysV red-zone overlaps IRQ frames
#   -mno-sse -mno-mmx -mno-avx
#                       kernel avoids SIMD; ISRs don't save SIMD state
#   -mcmodel=kernel     RIP-relative within the high-half kernel image
#   -fno-PIC            kernel linked at fixed VA, no PIC tables needed
#   -fno-stack-protector
#                       kernel has no __stack_chk_guard symbol; canaries
#                       would link-fail
#   -fno-omit-frame-pointer
#                       reliable stack traces from panic/debug paths
#   -fcf-protection=full
#                       emit ENDBR64 at every address-taken function (IBT)
#                       and shadow-stack-aware prologue/epilogue (SHSTK).
#                       NOP without CR4.CET=1.
#
# Real-HW UB containment — every one of these closes a CLASS of UB that
# would mis-compile silently. See Linux Makefile, https://lwn.net/Articles/342330
# and CVE-2009-1897 (NULL-deref optimization bug):
#   -fno-delete-null-pointer-checks
#                       MUST: GCC otherwise treats post-deref NULL checks
#                       as UB and removes them. Linux kernel forces this.
#   -fno-strict-aliasing
#                       MUST: kernel casts heavily between pointer types
#                       (opaque void*, struct ↔ uint8_t*).
#   -fwrapv             MUST: signed overflow → 2's-complement wrap
#                       (well-defined) instead of UB → no GCC removal of
#                       "impossible" bounds checks
#   -fno-asynchronous-unwind-tables -fno-unwind-tables
#                       no C++ EH machinery; saves ~10-20% of binary size
#
# Compile-time bug catching — promote latent bugs to build errors:
#   -Werror=implicit-function-declaration
#                       missing #include → silent ABI mismatch → kernel
#                       memory corruption. Fail at compile.
#   -Werror=incompatible-pointer-types
#                       passing a void** where char** expected etc. silently
#                       miscompiles on cross-arch. Fail at compile.
#   -Werror=return-type
#                       missing `return` in non-void → reads RAX garbage
#   -Werror=int-conversion
#                       int↔ptr cast without explicit (uintptr_t) cast
#   -Wstack-usage=8192
#                       warn at >8 KB stack frame; per-cpu/IST stacks are
#                       16 KB so this leaves headroom for nested IRQs
#
# Real-HW security hardening (GCC ≥ 11 / ≥ 12):
#   -fzero-call-used-regs=used
#                       zero caller-saved regs on function return — no
#                       data leak through register spill on context switch
#                       or syscall return
#   -mharden-sls=all    Straight-Line Speculation hardening: INT3 after
#                       RET / indirect JMP — closes a Spectre-v1 cousin
#                       on speculative execution past return
CFLAGS         = -Os -m64 -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-mmx -mno-avx \
                 -mcmodel=kernel -fno-PIC -fno-stack-protector \
                 -fno-omit-frame-pointer -fcf-protection=full \
                 -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
                 -fno-asynchronous-unwind-tables -fno-unwind-tables \
                 -fzero-call-used-regs=used -mharden-sls=all \
                 -Wall -Wextra \
                 -Wno-type-limits \
                 -Werror=implicit-function-declaration \
                 -Werror=incompatible-pointer-types \
                 -Werror=return-type \
                 -Werror=int-conversion \
                 -Wstack-usage=8192
# -Wno-type-limits — defensive `if (core < MAX_CORES)` checks on uint8_t
# values look "always true" to GCC because uint8_t maxes at 255 < 256.
# We keep the checks as type-widening defence (amp_get_core_index could
# return uint32_t in a future refactor) and accept the dead-by-type-system
# branches as a feature, not a compromise. The other -W categories from
# -Wextra remain enabled.
# Kernel directories first to ensure correct header resolution
INCLUDE_DIRS   := src $(shell find src/kernel -type d) $(shell find src/lib -type d) $(shell find src/boot -type d) $(shell find src/include -type d) $(shell find src/userspace -type d)
CFLAGS         += $(addprefix -I,$(INCLUDE_DIRS))

# Debug configuration (must be after CFLAGS definition)
ifeq ($(DEBUG),on)
CFLAGS += -DCONFIG_DEBUG_ENABLED=1 -DCONFIG_DEBUG_MODE=1
endif

# The handoff address the image build chose, handed to the C side so the two
# headers that name it can _Static_assert against it. Unconditional on
# purpose: it first went in under `ifeq ($(DEBUG),on)`, where DEBUG defaults
# to off, so the assertion that exists to catch a drift was compiled in
# exactly the builds nobody ships.
CFLAGS += -DBOOT_INFO_ADDR_FROM_BUILD=$(BOOT_INFO_ADDR)
# On-demand BMIDE watchdog TIER-2 (wedge->SRST) diagnostic. Off unless requested
# (it SRSTs the boot drive) — `make WEDGETEST=on` for a verification build.
WEDGETEST ?= off
ifeq ($(WEDGETEST),on)
CFLAGS += -DCONFIG_BMIDE_WEDGE_SELFTEST=1
endif
# LDFLAGS — kernel link
#   -z max-page-size=0x1000    align segments to page (kernel mappings)
#   -z noexecstack             explicit no-exec stack (default but
#                              explicit suppresses "missing .note.GNU-stack"
#                              warnings on some asm objects)
#   --oformat=binary           kernel.bin is dd'd directly onto the disk
#                              image; no ELF wrapper at runtime
LDFLAGS        = -g -T $(ENTRYDIR)/linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --oformat=binary

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
MTEST_BIN = $(APPS_DIR)/mtest.elf
CHAIN_BIN = $(APPS_DIR)/chain.elf
DECKS_BIN = $(APPS_DIR)/decks.elf
BENCH_BIN        = $(APPS_DIR)/bench.elf
TOUCH_TEST_BIN   = $(APPS_DIR)/touch_test.elf
TOUCH_STRESS_BIN = $(APPS_DIR)/touch_stress.elf
LIFECYCLE_BIN    = $(APPS_DIR)/lifecycle.elf
PERSIST_BIN      = $(APPS_DIR)/persist.elf
WRITE_STRESS_BIN = $(APPS_DIR)/write_stress.elf
WRITE_CONC_BIN   = $(APPS_DIR)/write_concurrent.elf
WRITE_OBS_BIN    = $(APPS_DIR)/write_observer.elf
COW_TEST_BIN     = $(APPS_DIR)/cow_test.elf
ANCHOR_TEST_BIN  = $(APPS_DIR)/anchor_test.elf
BAY_TEST_BIN     = $(APPS_DIR)/bay_test.elf
HTEST_BIN        = $(APPS_DIR)/htest.elf
BROOK_TEST_BIN   = $(APPS_DIR)/brook_test.elf
CXXTEST_BIN      = $(APPS_DIR)/cxxtest.elf
CURRENT_TEST_BIN = $(APPS_DIR)/current_test.elf
STRANDTEST_BIN   = $(APPS_DIR)/strandtest.elf
BROOKSTRAND_BIN  = $(APPS_DIR)/brookstrand.elf
BROOKEXEC_BIN    = $(APPS_DIR)/brookexec.elf
CURRENTEXEC_BIN  = $(APPS_DIR)/currentexec.elf
STRANDPARK_BIN   = $(APPS_DIR)/strandpark.elf
CHILDSPIN_BIN    = $(APPS_DIR)/childspin.elf
PRINT_STRESS_BIN = $(APPS_DIR)/print_stress.elf
EXITPATHS_BIN    = $(APPS_DIR)/exitpaths.elf

# Display server ELF
DISPLAY_DIR = $(USERSPACE_DIR)/display
DISPLAY_BIN = $(DISPLAY_DIR)/display.elf

# Utility ELF binaries
UTILS_DIR = $(USERSPACE_DIR)/utils
UTIL_NAMES = help create show files tag untag name trash erase \
             me info say reboot bye defrag fsck ipc_test memtag hw \
             timezone
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

# ==== UEFI BOOTLOADER ====
TAGBOOT_SRC    = src/boot/uefi/tagboot.c
TAGBOOT_JUMP   = src/boot/uefi/tagboot_jump.asm
TAGBOOT_LD     = src/boot/uefi/tagboot.ld
TAGBOOT_EFI    = $(BUILDDIR)/BOOTX64.EFI
TAGBOOT_SO     = $(BUILDDIR)/tagboot.so
TAGBOOT_OBJ    = $(BUILDDIR)/tagboot.o
TAGBOOT_JUMP_OBJ = $(BUILDDIR)/tagboot_jump.o

# UEFI compile/link flags
# gcc ELF path: compile with -fpic, link as .so, objcopy to PE32+
UEFI_CC      = x86_64-elf-gcc
UEFI_CFLAGS_GCC = -ffreestanding -nostdlib -nostdinc \
                  -mno-red-zone -mno-sse -mno-mmx -mno-avx \
                  -fpic -fshort-wchar -fno-stack-protector \
                  -Wall -Wextra -Os \
                  -DBOOT_INFO_ADDR_FROM_BUILD=$(BOOT_INFO_ADDR) \
                  -I$(SRCDIR)/boot/uefi

# clang direct-to-PE path: -fpic is invalid on MSVC target; PE handles
# relocations natively via the .reloc section.
UEFI_CFLAGS_CLANG = -ffreestanding -nostdlib -nostdinc \
                    -mno-red-zone -mno-sse -mno-mmx -mno-avx \
                    -fshort-wchar -fno-stack-protector \
                    -Wall -Wextra -Os \
                    -target x86_64-unknown-windows \
                    -DBOOT_INFO_ADDR_FROM_BUILD=$(BOOT_INFO_ADDR) \
                    -I$(SRCDIR)/boot/uefi

# Detect whether lld-link is available for direct PE output via clang.
# Apple clang does not ship lld-link, so we fall through to the gcc+objcopy path
# even when 'clang' is in PATH.  Only enable the clang path when lld-link exists.
CLANG_AVAILABLE := $(shell command -v lld-link 2>/dev/null)

.PHONY: all clean run run-bg run-stop bochs-bg bochs-stop debug info check-deps install-deps uefi usb check-endbr64

# ==== MAIN TARGET ====
all: check-deps check-error-parity check-no-exit-sentinel $(IMAGE) $(KERNEL_ELF) $(FLOPPY_IMG) $(ISO) $(VBOX_VDI) uefi check-endbr64

# ==== CET / IBT POST-LINK AUDIT ====
# Verifies every globally-visible function in the kernel ELF begins with
# ENDBR64 (F3 0F 1E FA). Required for IBT under CR4.CET=1 +
# IA32_S_CET.ENDBR_EN=1 — without ENDBR64 the first indirect-call to a
# function would trigger #CP on Tiger Lake+ / Zen 4+ real silicon.
# Runs after the kernel ELF exists and depends on `nm` + `objdump`
# (already required by the toolchain). Fast (~1s).
check-endbr64: $(KERNEL_ELF)
	@./tools/check_endbr64.sh $(KERNEL_ELF)

# ==== ERROR-CODE PARITY GUARD (Ф23) ====
# boxlib box/error.h is the single userspace source of truth for error codes
# (one X-macro row → boxlib ERR_*, box::errc, box_error.cpp tables). This guard
# enforces the one seam the X-macro cannot reach — the kernel boundary — so a
# new kernel ERR_* cannot silently re-open the subset-drift Ф23 closed. Pure
# source diff, instant; fails the build on any name/value mismatch.
check-error-parity:
	@./tools/check_error_parity.sh

# ==== EXIT-SENTINEL DRIFT GUARD (Ф26d) ====
# The legacy 0xFE child-exit IPC sentinel is deleted — a child's exit is now
# observed on the kernel's process:died Touch (clean exit AND crash, carrying
# (pid, generation) on a ring separate from keyboard/args IPC). This guard fails
# the build if the sentinel is reintroduced in userspace. Pure source grep,
# instant.
check-no-exit-sentinel:
	@./tools/check_no_exit_sentinel.sh

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
	@command -v bochs   >/dev/null || echo "WARNING: bochs not found (needed for 'make bochs')"
	@command -v xorriso >/dev/null || echo "WARNING: xorriso not found (needed for ISO)"
	@command -v VBoxManage >/dev/null || (echo "WARNING: VBoxManage not found" && sleep 2)
	@echo "All dependencies OK."

# ==== BUILD RULES ====
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

# Cross-platform build for create_tagfs (host tool, not target)
# Store OS signature to detect when to rebuild
# OS-specific signature path: switching host OS picks a new (absent) file, which
# its recipe creates fresh, re-triggering the tool compile below — so the cross-
# platform rebuild is handled by prerequisites, not an in-body guard.
TAGFS_OS_SIGNATURE = $(BUILDDIR)/.tagfs_host_os.$(UNAME_S).$(UNAME_M)

# Prerequisites — the .c source, the shared reserved-vocabulary header, and the
# OS signature — decide WHEN this recipe runs, so the compile is unconditional.
# No in-body skip guard: that previously kept a stale tool after a
# create_tagfs.c / tagfs_reserved.h edit, which silently formats a divergent
# on-disk tag-id layout (the new kernel then panics: reserved vocab not 0..11).
$(TAGFS_TOOL): tools/create_tagfs.c $(SRCDIR)/include/tagfs_reserved.h $(TAGFS_OS_SIGNATURE) | $(BUILDDIR)
	@echo "Compiling TagFS tool for $(UNAME_S) ($(UNAME_M))..."
	@$(CC_HOST) -I$(SRCDIR)/include -o $@ $< -Wall -Wextra
	@echo "TagFS tool built: $@"

# Create OS signature file on first build
$(TAGFS_OS_SIGNATURE): | $(BUILDDIR)
	@echo "$(UNAME_S):$(UNAME_M)" > $@

# -MMD -MP: header dependencies. Without them a kernel header edit rebuilt only
# the .c files that changed, and objects compiled against two different layouts
# of the same struct linked cleanly and hung on the first schedule() — measured
# in Ф41-e-2, three matrix runs spent bisecting it. `make clean` was the only
# remedy and it is not one anybody remembers every time.
$(BUILDDIR)/kernel/drivers/usb/%.o: $(SRCDIR)/kernel/drivers/usb/%.c | $(BUILDDIR)
	@echo "Compiling USB driver $<..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -Os -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	@echo "Compiling $<..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.asm | $(BUILDDIR)
	@echo "Assembling $<..."
	@mkdir -p $(@D)
	@$(ASM) $(ASMFLAGS_ELF) $< -o $@

# Dash-prefixed: the .d files do not exist on the first build, and BUILDDIR is
# removed wholesale by clean, which takes them with it.
-include $(C_OBJS:.o=.d)

$(STAGE1_BIN): $(STAGE1_SRC) | $(BUILDDIR)
	@echo "Building Stage1..."
	@$(ASM) $(ASMFLAGS) $(ASM_LAYOUT) $< -o $@
	@sz=$$(stat -f%z $@ 2>/dev/null || stat -c%s $@); \
	if [ "$$sz" != "512" ]; then \
	    echo "ERROR: stage1.bin is $$sz bytes; the boot sector is exactly 512."; \
	    rm -f $@; exit 1; \
	fi

$(STAGE2_BIN): $(STAGE2_SRC) | $(BUILDDIR)
	@echo "Building Stage2..."
	@$(ASM) $(ASMFLAGS) $(ASM_LAYOUT) $< -o $@
	@sz=$$(stat -f%z $@ 2>/dev/null || stat -c%s $@); \
	max=$$(( $(STAGE2_SECTORS) * 512 )); \
	if [ "$$sz" -gt "$$max" ]; then \
	    echo "ERROR: stage2.bin is $$sz bytes, over its $(STAGE2_SECTORS)-sector window ($$max)."; \
	    echo "       dd would write the overflow across sector $(KERNEL_START_SECTOR), where the kernel begins,"; \
	    echo "       and stage1 would load a truncated stage2 that still passes its signature check."; \
	    rm -f $@; exit 1; \
	fi
	@if [ $$(( 1 + $(STAGE2_SECTORS) )) -gt $(KERNEL_START_SECTOR) ]; then \
	    echo "ERROR: stage2 occupies sectors 1..$$(( $(STAGE2_SECTORS) )) but the kernel starts at $(KERNEL_START_SECTOR)."; \
	    exit 1; \
	fi

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

# Build boxcxx (C++ runtime + stdlib; needs boxlib headers only)
.PHONY: $(USERSPACE_DIR)/boxcxx/libboxcxx.a
$(USERSPACE_DIR)/boxcxx/libboxcxx.a:
	@echo "Building boxcxx..."
	@cd $(USERSPACE_DIR)/boxcxx && $(MAKE)

# Build shell
$(SHELL_BIN): $(USERSPACE_DIR)/boxlib/libbox.a
	@echo "Building shell process..."
	@cd $(SHELL_DIR) && $(MAKE)
	@echo "Shell binary: $@ ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes)"

# Build apps (proca, procb, today, memtest)
$(PROCA_BIN) $(PROCB_BIN) $(TODAY_BIN) $(MEMTEST_BIN) $(MTEST_BIN) $(CHAIN_BIN) $(DECKS_BIN) $(BENCH_BIN) $(TOUCH_TEST_BIN) $(TOUCH_STRESS_BIN) $(LIFECYCLE_BIN) $(PERSIST_BIN) $(WRITE_STRESS_BIN) $(WRITE_CONC_BIN) $(WRITE_OBS_BIN) $(COW_TEST_BIN) $(ANCHOR_TEST_BIN) $(BAY_TEST_BIN) $(BROOK_TEST_BIN) $(CURRENT_TEST_BIN) $(HTEST_BIN) $(CXXTEST_BIN) $(STRANDTEST_BIN) $(BROOKSTRAND_BIN) $(BROOKEXEC_BIN) $(CURRENTEXEC_BIN) $(STRANDPARK_BIN) $(CHILDSPIN_BIN) $(PRINT_STRESS_BIN) $(EXITPATHS_BIN): $(USERSPACE_DIR)/boxlib/libbox.a $(USERSPACE_DIR)/boxcxx/libboxcxx.a
	@echo "Building apps..."
	@cd $(APPS_DIR) && $(MAKE)
	@echo "proca.elf: $$(stat -f%z $(PROCA_BIN) 2>/dev/null || stat -c%s $(PROCA_BIN) 2>/dev/null) bytes"
	@echo "procb.elf: $$(stat -f%z $(PROCB_BIN) 2>/dev/null || stat -c%s $(PROCB_BIN) 2>/dev/null) bytes"
	@echo "today.elf: $$(stat -f%z $(TODAY_BIN) 2>/dev/null || stat -c%s $(TODAY_BIN) 2>/dev/null) bytes"
	@echo "memtest.elf: $$(stat -f%z $(MEMTEST_BIN) 2>/dev/null || stat -c%s $(MEMTEST_BIN) 2>/dev/null) bytes"
	@echo "mtest.elf: $$(stat -f%z $(MTEST_BIN) 2>/dev/null || stat -c%s $(MTEST_BIN) 2>/dev/null) bytes"
	@echo "chain.elf: $$(stat -f%z $(CHAIN_BIN) 2>/dev/null || stat -c%s $(CHAIN_BIN) 2>/dev/null) bytes"
	@echo "decks.elf: $$(stat -f%z $(DECKS_BIN) 2>/dev/null || stat -c%s $(DECKS_BIN) 2>/dev/null) bytes"

# Build display server
$(DISPLAY_BIN): $(USERSPACE_DIR)/boxlib/libbox.a
	@echo "Building display server..."
	@cd $(DISPLAY_DIR) && $(MAKE)
	@echo "display.elf: $$(stat -f%z $(DISPLAY_BIN) 2>/dev/null || stat -c%s $(DISPLAY_BIN) 2>/dev/null) bytes"

# `timezone` carries the zone database, which is C++. Stated BEFORE the rule
# below and not inside it: a target line between a rule and its recipe ends
# that rule, which would leave every other utility with no recipe at all.
$(UTILS_DIR)/timezone.elf: $(USERSPACE_DIR)/boxcxx/libboxcxx.a

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
$(IMAGE): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(PROCA_BIN) $(PROCB_BIN) $(TODAY_BIN) $(MEMTEST_BIN) $(MTEST_BIN) $(CHAIN_BIN) $(DECKS_BIN) $(BENCH_BIN) $(TOUCH_TEST_BIN) $(TOUCH_STRESS_BIN) $(LIFECYCLE_BIN) $(PERSIST_BIN) $(WRITE_STRESS_BIN) $(WRITE_CONC_BIN) $(WRITE_OBS_BIN) $(COW_TEST_BIN) $(ANCHOR_TEST_BIN) $(BAY_TEST_BIN) $(BROOK_TEST_BIN) $(CURRENT_TEST_BIN) $(HTEST_BIN) $(CXXTEST_BIN) $(STRANDTEST_BIN) $(BROOKSTRAND_BIN) $(BROOKEXEC_BIN) $(CURRENTEXEC_BIN) $(STRANDPARK_BIN) $(CHILDSPIN_BIN) $(PRINT_STRESS_BIN) $(EXITPATHS_BIN) $(DISPLAY_BIN) $(UTIL_ELFS) $(TAGFS_TOOL)
	@echo "Creating disk image (24MB)..."
	@dd if=/dev/zero of=$@ bs=512 count=49152 status=none
	@echo "  Writing Stage1 (sector 0, 512 bytes)..."
	@dd if=$(STAGE1_BIN) of=$@ bs=512 conv=notrunc status=none
	@echo "  Writing Stage2 (sectors 1-9, $(STAGE2_SECTORS) sectors)..."
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "  Writing Kernel (sector $(KERNEL_START_SECTOR)+, dynamic size via TagFS)..."
	@dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	@echo "  Creating TagFS v1..."
	@$(TAGFS_TOOL) $@ \
		$(KERNEL_BIN)   "system" \
		$(DISPLAY_BIN)  "display,system,utility,autostart" \
		$(SHELL_BIN)    "utility,system,app,autostart" \
		$(PROCA_BIN)    "app" \
		$(PROCB_BIN)    "app" \
		$(TODAY_BIN)    "app,utility" \
		$(MEMTEST_BIN)  "app,utility" \
		$(MTEST_BIN)    "utility,memory" \
		$(CHAIN_BIN)    "app,utility" \
		$(DECKS_BIN)    "app,utility" \
		$(BENCH_BIN)         "app,utility,bench" \
		$(TOUCH_TEST_BIN)    "app,utility,test" \
		$(TOUCH_STRESS_BIN)  "app,utility,test" \
		$(LIFECYCLE_BIN)     "app,utility,test" \
		$(PERSIST_BIN)       "app,utility,test" \
		$(WRITE_STRESS_BIN)  "app,utility,test" \
		$(WRITE_CONC_BIN)    "app,utility,test" \
		$(WRITE_OBS_BIN)     "app,utility,test" \
		$(COW_TEST_BIN)      "app,utility,test" \
		$(ANCHOR_TEST_BIN)   "app,utility,test" \
		$(BAY_TEST_BIN)      "app,utility,test" \
		$(BROOK_TEST_BIN)    "app,utility,test" \
		$(CURRENT_TEST_BIN)  "app,utility,test" \
		$(HTEST_BIN)         "app,utility,test" \
		$(CXXTEST_BIN)       "app,utility,test,cxx" \
		$(STRANDTEST_BIN)    "app,utility,test" \
		$(BROOKSTRAND_BIN)   "app,utility,test" \
		$(BROOKEXEC_BIN)     "app,utility,test,cxx" \
		$(CURRENTEXEC_BIN)   "app,utility,test,cxx" \
		$(STRANDPARK_BIN)    "app,utility,test" \
		$(CHILDSPIN_BIN)     "app,utility,test" \
		$(PRINT_STRESS_BIN)  "app,utility,test" \
		$(EXITPATHS_BIN)     "app,utility,test,cxx" \
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
		$(UTILS_DIR)/timezone.elf "utility,system,clock" \
		$(USERSPACE_DIR)/hello.txt   "message,text" \
		$(USERSPACE_DIR)/file.txt    "file,info,message,text" \
		$(USERSPACE_DIR)/testbin.bin "binary, test:forerror, emptyfile" \
		$(UTILS_DIR)/ipc_test.elf "utility" \
		$(UTILS_DIR)/memtag.elf  "utility,memory,system" \
		$(UTILS_DIR)/hw.elf      "utility,system,hardware"
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
	@rm -f $@
	@VBoxManage convertfromraw $< $@ --format VDI

# ==== UEFI BOOTLOADER BUILD ====
# Build strategy:
#   If clang is available → compile directly to PE32+ using -target x86_64-unknown-windows
#   Otherwise             → compile to ELF .so with x86_64-elf-gcc + objcopy to PE
#
# The EFI binary is placed at build/BOOTX64.EFI.  To boot it with OVMF:
#   make run UEFI=on   (requires OVMF.fd in the project root or /usr/share/ovmf/)

uefi: $(TAGBOOT_EFI)

ifdef CLANG_AVAILABLE

# ---- clang path: compile directly to PE32+ EFI application ----
# Assemble tagboot_jump.asm to ELF64 first; clang links both objects into PE.
$(TAGBOOT_JUMP_OBJ): $(TAGBOOT_JUMP) | $(BUILDDIR)
	@echo "Assembling TagBoot jump trampoline..."
	@$(ASM) -f elf64 $< -o $@

$(TAGBOOT_EFI): $(TAGBOOT_SRC) $(TAGBOOT_JUMP_OBJ) $(TAGBOOT_LD) src/boot/uefi/uefi.h src/boot/uefi/tagfs_boot.h | $(BUILDDIR)
	@echo "Building TagBoot UEFI (clang/lld → PE32+)..."
	@clang $(UEFI_CFLAGS_CLANG) \
		-Wl,-entry:TagBootMain \
		-Wl,-subsystem:efi_application \
		-Wl,-nodefaultlib \
		-o $@ $< $(TAGBOOT_JUMP_OBJ)
	@echo "TagBoot EFI: $@ ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes)"

else

# ---- gcc path: compile to ELF PIC .so, then objcopy to PE32+ ----
$(TAGBOOT_JUMP_OBJ): $(TAGBOOT_JUMP) | $(BUILDDIR)
	@echo "Assembling TagBoot jump trampoline..."
	@$(ASM) -f elf64 $< -o $@

$(TAGBOOT_OBJ): $(TAGBOOT_SRC) src/boot/uefi/uefi.h src/boot/uefi/tagfs_boot.h | $(BUILDDIR)
	@echo "Building TagBoot UEFI object (gcc ELF)..."
	@$(UEFI_CC) $(UEFI_CFLAGS_GCC) -c $< -o $@

$(TAGBOOT_SO): $(TAGBOOT_OBJ) $(TAGBOOT_JUMP_OBJ) $(TAGBOOT_LD) | $(BUILDDIR)
	@echo "Linking TagBoot ELF shared object..."
	@$(LD) \
		-nostdlib \
		-shared \
		-Bsymbolic \
		-T $(TAGBOOT_LD) \
		-o $@ $(TAGBOOT_OBJ) $(TAGBOOT_JUMP_OBJ)

$(TAGBOOT_EFI): $(TAGBOOT_SO) | $(BUILDDIR)
	@echo "Converting TagBoot ELF → PE32+ EFI binary (make_efi.py)..."
	@python3 tools/make_efi.py $< $@

endif

# Comma helper for $(if ...) inside recipes
comma := ,

# ==== RUN CONFIGURATION ====
# Usage: make run [EMU=qemu|bochs] [CORES=N] [MEM=size] [FULLSCREEN=on]
#                 [USB=on|off] [AHCI=on] [LOG=on] [GDB=on] [DEBUG=on] [UEFI=on]
#                 [BOCHSDBG=on] [BOCHS_DISPLAY=sdl2|x|term|nogui]
#                 [BOCHS_CPU=name] [BOCHS_IPS=N]
#
# Examples:
#   make run                          — QEMU, 1 core, 512M, USB kbd (MBR boot)
#   make run EMU=bochs                — Bochs (SDL2 on macOS, X11 on Linux)
#   make run EMU=bochs BOCHSDBG=on    — Bochs + internal text debugger
#   make run EMU=bochs GDB=on         — Bochs gdb stub on tcp:1234
#   make run UEFI=on                  — QEMU via OVMF + TagBoot EFI
#   make run UEFI=on EMU=bochs        — Bochs via OVMF (only on Bochs builds with
#                                       extended ROM area; stock builds reject 3.5MB OVMF)
#   make run EMU=bochs CORES=4        — only works on Bochs built --enable-smp
#   make run EMU=bochs BOCHS_CPU=ryzen — pick a different CPU profile
#   make run CORES=4 MEM=4G           — 4 cores, 4GB RAM (any EMU)
#   make run FULLSCREEN=on            — QEMU cocoa fullscreen
#   make run LOG=on                   — verbose logging (QEMU IRQs / Bochs info)
#   make run GDB=on                   — QEMU: pause for gdb / Bochs: gdbstub
#   make run AHCI=on USB=off          — AHCI disk controller, no USB (QEMU only)
#   make run DEBUG=on                 — kernel CONFIG_DEBUG_ENABLED at compile
#   make run UEFI=on CORES=4 MEM=4G

EMU        ?= qemu
CORES      ?= 1
MEM        ?= 512M
FULLSCREEN ?= off
USB        ?= on
AHCI       ?= off
LOG        ?= off
GDB        ?= off
DEBUG      ?= off
UEFI       ?= off
# STRICT=on — make QEMU emulate real-HW behaviour without softening:
#   • -cpu max               — exposes every feature TCG can emulate
#                              (RDRAND/RDSEED/AVX2/AES-NI/SMEP/SMAP/UMIP/+invtsc).
#                              Catches "kernel uses CPU feature without CPUID
#                              guard" bugs that default qemu64 silently masks.
#   • -machine q35           — modern PCH chipset (PCIe, AHCI, MSI-X, ACPI 6.x).
#                              Default i440fx is a 1996 PIIX board no longer
#                              shipping on real hardware.
#   • -d guest_errors,unimp,cpu_reset — log every undefined / unimplemented
#                              guest behaviour QEMU normally absorbs silently.
#   STRICT deliberately does NOT set -no-reboot or -no-shutdown:
#     - Real hardware reboots after reset (and triple-fault); STRICT mirrors
#       that — `reboot` actually restarts the VM, matching production.
#     - `bye` (ACPI S5) cleanly exits QEMU without -no-shutdown blocking.
#     - For triple-fault diagnostics (halt + log) use LOG=on instead, which
#       keeps both flags set and writes boxos_qemu.log.
#   • -overcommit cpu-pm=on   — match real CPU power-management semantics
#                              (HLT/MWAIT actually descheduled, not no-op).
STRICT     ?= off
BOCHSDBG      ?= off
BOCHS_DISPLAY ?= auto

# Bochs runtime — auto-discover BIOS / VGA BIOS (only used when EMU=bochs).
# Keymap is selected at config-gen time inside tools/make_bochsrc.sh based on
# BOCHS_DISP_LIB (sdl2 vs x vs term) so we don't feed an X11 keymap to SDL2.
# Use the BIOS-bochs-latest pair shipped with Bochs — QEMU's SeaBIOS is built
# with CONFIG_QEMU=y and pulls fwcfg/PCI quirks Bochs doesn't emulate, so it
# bricks boot. Override via BOCHS_BIOS=/your/bios.bin if you have a custom one.
BOCHS         ?= bochs
BOCHS_DATA_DIRS := /opt/homebrew/share/bochs \
                   /usr/local/share/bochs \
                   /usr/share/bochs \
                   $(wildcard /opt/homebrew/Cellar/bochs/*/share/bochs)
BOCHS_BIOS    ?= $(firstword $(foreach d,$(BOCHS_DATA_DIRS),$(wildcard $(d)/BIOS-bochs-latest)))
BOCHS_VGABIOS ?= $(firstword $(foreach d,$(BOCHS_DATA_DIRS),$(wildcard $(d)/VGABIOS-lgpl-latest.bin)))
BOCHS_RC      := $(BUILDDIR)/bochsrc.txt
BOCHS_LOG     := $(BUILDDIR)/bochs.log

# CPU emulation knobs — pass any Bochs-supported model + ips override.
# `BOCHS_CPU` accepts anything from `bochs --help cpu` (corei7_haswell_4770,
# corei7_skylake_x, ryzen, atom_x5_z8350, etc.). `BOCHS_IPS` is instructions
# per second — affects realtime alignment / wall-clock fidelity.
BOCHS_CPU     ?= corei7_haswell_4770
BOCHS_IPS     ?= 50000000

# Resolve display library: auto -> SDL2 on macOS, X11 on Linux/BSD.
ifeq ($(BOCHS_DISPLAY),auto)
ifeq ($(UNAME_S),Darwin)
BOCHS_DISP_LIB := sdl2
else
BOCHS_DISP_LIB := x
endif
else
BOCHS_DISP_LIB := $(BOCHS_DISPLAY)
endif

# OVMF firmware search paths (common locations on macOS/Linux)
OVMF_PATHS := /usr/share/ovmf/OVMF.fd \
              /usr/share/qemu/OVMF.fd \
              /usr/local/share/ovmf/OVMF.fd \
              /opt/homebrew/share/ovmf/OVMF.fd \
              $(wildcard /opt/homebrew/Cellar/qemu/*/share/qemu/edk2-x86_64-code.fd) \
              $(wildcard /usr/local/Cellar/qemu/*/share/qemu/edk2-x86_64-code.fd) \
              OVMF.fd
OVMF_FD := $(firstword $(foreach p,$(OVMF_PATHS),$(wildcard $(p))))

# For UEFI boot: create a minimal ESP (EFI System Partition) FAT image that
# holds EFI/BOOT/BOOTX64.EFI, then pass it as a second drive alongside the
# BoxOS disk image (which still holds the TagFS data).
UEFI_ESP_IMG = $(BUILDDIR)/esp.img

# UEFI NVRAM variables image — writable copy of the OVMF vars template.
# Searched in common locations; falls back to an empty 256 KB file if no
# template is found (OVMF will initialise it on first boot).
OVMF_VARS_PATHS := \
    /opt/homebrew/share/qemu/edk2-x86_64-vars.fd \
    $(wildcard /opt/homebrew/Cellar/qemu/*/share/qemu/edk2-x86_64-vars.fd) \
    /usr/share/OVMF/OVMF_VARS.fd \
    /usr/share/qemu/OVMF_VARS.fd \
    /usr/local/share/ovmf/OVMF_VARS.fd \
    edk2-x86_64-vars.fd
OVMF_VARS_TEMPLATE := $(firstword $(foreach p,$(OVMF_VARS_PATHS),$(wildcard $(p))))

$(BUILDDIR)/edk2-vars.fd: | $(BUILDDIR)
	@echo "Creating UEFI NVRAM vars image..."
	@if [ -n "$(OVMF_VARS_TEMPLATE)" ]; then \
		cp "$(OVMF_VARS_TEMPLATE)" $@; \
		echo "  Copied NVRAM template: $(OVMF_VARS_TEMPLATE)"; \
	elif [ -n "$(OVMF_FD)" ]; then \
		CODE_SZ=$$(stat -f%z "$(OVMF_FD)" 2>/dev/null || stat -c%s "$(OVMF_FD)" 2>/dev/null); \
		dd if=/dev/zero of=$@ bs=$$CODE_SZ count=1 status=none; \
		echo "  Created zeroed NVRAM ($$CODE_SZ bytes, matches $(OVMF_FD))"; \
	else \
		dd if=/dev/zero of=$@ bs=1024 count=256 status=none; \
		echo "  Created empty 256 KB NVRAM (fallback)"; \
	fi

$(UEFI_ESP_IMG): $(TAGBOOT_EFI) $(BUILDDIR)/edk2-vars.fd | $(BUILDDIR)
	@echo "Creating UEFI ESP image (FAT32, 34 MB)..."
	@python3 tools/make_esp.py $(TAGBOOT_EFI) $@

run: $(IMAGE)
ifeq ($(EMU),bochs)
	@echo "=== BoxOS Bochs ==="
	@echo "  Cores: $(CORES) | RAM: $(MEM) | Display: $(BOCHS_DISP_LIB) | UEFI: $(UEFI)"
	@echo "  CPU: $(BOCHS_CPU) | IPS: $(BOCHS_IPS)"
	@echo "  Internal Debugger: $(BOCHSDBG) | GDB stub: $(GDB) | Log: $(LOG)"
	@echo "  Config: $(BOCHS_RC)  Log: $(BOCHS_LOG)"
	@echo "==================="
	@command -v $(BOCHS) >/dev/null || \
	    (echo "ERROR: bochs not found. Install: brew install bochs / apt install bochs"; exit 1)
	@if [ -z "$(BOCHS_VGABIOS)" ]; then \
	    echo "ERROR: VGABIOS-lgpl-latest.bin not found in any of: $(BOCHS_DATA_DIRS)"; \
	    exit 1; \
	fi
	@if [ "$(UEFI)" = "on" ]; then \
	    if [ -z "$(OVMF_FD)" ]; then \
	        echo "ERROR: UEFI=on requires OVMF.fd. Install ovmf or place OVMF.fd at project root."; \
	        exit 1; \
	    fi; \
	elif [ -z "$(BOCHS_BIOS)" ]; then \
	    echo "ERROR: BIOS-bochs-latest not found."; \
	    echo "  Install:  brew install bochs / sudo apt install bochs"; \
	    echo "  Searched: $(BOCHS_DATA_DIRS)"; \
	    exit 1; \
	fi
	@$(MAKE) --no-print-directory $(if $(filter on,$(UEFI)),$(UEFI_ESP_IMG))
	@BOCHS_RC="$(BOCHS_RC)" \
	  IMAGE="$<" \
	  CORES="$(CORES)" \
	  MEM="$(MEM)" \
	  BOCHS_CPU="$(BOCHS_CPU)" \
	  BOCHS_IPS="$(BOCHS_IPS)" \
	  BOCHS_DISP_LIB="$(BOCHS_DISP_LIB)" \
	  BOCHS_DATA_DIRS="$(BOCHS_DATA_DIRS)" \
	  GDB="$(GDB)" \
	  LOG="$(LOG)" \
	  UEFI="$(UEFI)" \
	  OVMF_FD="$(OVMF_FD)" \
	  UEFI_ESP_IMG="$(UEFI_ESP_IMG)" \
	  BOCHS_BIOS="$(BOCHS_BIOS)" \
	  BOCHS_VGABIOS="$(BOCHS_VGABIOS)" \
	  BOCHS_LOG="$(BOCHS_LOG)" \
	  bash tools/make_bochsrc.sh
	@$(BOCHS) -q -unlock -f $(BOCHS_RC) $(if $(filter on,$(BOCHSDBG)),-debugger)
else
	@echo "=== BoxOS QEMU ==="
	@echo "  Cores: $(CORES) | RAM: $(MEM) | USB: $(USB) | AHCI: $(AHCI) | UEFI: $(UEFI)"
	@echo "  Fullscreen: $(FULLSCREEN) | Log: $(LOG) | GDB: $(GDB) | Debug: $(DEBUG) | Strict: $(STRICT)"
	@echo "==================="
	$(if $(filter on,$(UEFI)), \
		$(if $(OVMF_FD),, \
			$(error UEFI=on requires OVMF.fd. Install ovmf package or place OVMF.fd in project root.)))
	@$(MAKE) --no-print-directory $(if $(filter on,$(UEFI)),$(UEFI_ESP_IMG))
	@$(QEMU) \
		$(if $(filter on,$(UEFI)), \
			-machine q35 \
			-drive if=pflash$(comma)format=raw$(comma)readonly=on$(comma)file=$(OVMF_FD) \
			-drive if=pflash$(comma)format=raw$(comma)file=$(BUILDDIR)/edk2-vars.fd \
			-drive format=raw$(comma)file=$(UEFI_ESP_IMG)$(comma)if=ide$(comma)index=0 \
			-drive format=raw$(comma)file=$<$(comma)if=ide$(comma)index=1, \
			$(if $(filter on,$(STRICT)),-machine q35) \
			$(if $(filter on,$(AHCI)), \
				-drive id=disk0$(comma)file=$<$(comma)format=raw$(comma)if=none \
				-device ahci$(comma)id=ahci -device ide-hd$(comma)drive=disk0$(comma)bus=ahci.0, \
				-drive format=raw$(comma)file=$<$(comma)index=0$(comma)media=disk)) \
		$(if $(filter on,$(STRICT)), \
		    -cpu max$(comma)+invtsc$(comma)+rdrand$(comma)+rdseed \
		    -overcommit cpu-pm=on \
		    -d guest_errors$(comma)unimp$(comma)cpu_reset, \
		    -cpu qemu64$(comma)+fsgsbase) \
		-m $(MEM) \
		-serial stdio \
		$(if $(filter-out 1,$(CORES)),-smp $(CORES)$(comma)cores=$(CORES)$(comma)threads=1$(comma)sockets=1) \
		$(if $(filter on,$(FULLSCREEN)),$(if $(filter Darwin,$(UNAME_S)),-display cocoa$(comma)full-screen=on$(comma)zoom-to-fit=on,-full-screen)) \
		$(if $(filter on,$(USB)),-device qemu-xhci -device usb-kbd) \
		$(if $(filter on,$(LOG)),-d int$(comma)cpu_reset -no-reboot -no-shutdown -D boxos_qemu.log) \
		$(if $(filter on,$(GDB)),-s -S)
endif

# ===================================================================
# Headless QEMU for automated key-injection testing.
# - Monitor on a Unix socket (sendkey, screendump, info ...)
# - Serial output to a file (kernel debug_printf, panic markers)
# - VGA framebuffer dumpable via tools/qemu-input.sh shot
# - Drive interactively from the shell with tools/qemu-input.sh.
# Same vars as `run`: UEFI=on, CORES=N, MEM=size, USB=on, AHCI=on.
# ===================================================================
run-bg: $(IMAGE)
	@$(MAKE) --no-print-directory $(if $(filter on,$(UEFI)),$(UEFI_ESP_IMG))
	@if [ -f $(BUILDDIR)/qemu.pid ] && kill -0 $$(cat $(BUILDDIR)/qemu.pid) 2>/dev/null; then \
		echo "[run-bg] QEMU already running (pid $$(cat $(BUILDDIR)/qemu.pid)). Use 'make run-stop' first."; \
		exit 1; \
	fi
	@rm -f $(BUILDDIR)/qemu.mon $(BUILDDIR)/serial.log $(BUILDDIR)/qemu.pid
	@touch $(BUILDDIR)/serial.log
	@echo "=== BoxOS QEMU (background) ==="
	@echo "  Monitor : $(BUILDDIR)/qemu.mon"
	@echo "  Serial  : $(BUILDDIR)/serial.log"
	@echo "  Pidfile : $(BUILDDIR)/qemu.pid"
	@echo "==============================="
	@$(QEMU) \
		$(if $(filter on,$(UEFI)), \
			-machine q35 \
			-drive if=pflash$(comma)format=raw$(comma)readonly=on$(comma)file=$(OVMF_FD) \
			-drive if=pflash$(comma)format=raw$(comma)file=$(BUILDDIR)/edk2-vars.fd \
			-drive format=raw$(comma)file=$(UEFI_ESP_IMG)$(comma)if=ide$(comma)index=0 \
			-drive format=raw$(comma)file=$<$(comma)if=ide$(comma)index=1, \
			$(if $(filter on,$(AHCI)), \
				-drive id=disk0$(comma)file=$<$(comma)format=raw$(comma)if=none \
				-device ahci$(comma)id=ahci \
				-device ide-hd$(comma)drive=disk0$(comma)bus=ahci.0$(comma)bootindex=0, \
				-drive format=raw$(comma)file=$<$(comma)index=0$(comma)media=disk)) \
		$(if $(filter on,$(STRICT)), \
		    -cpu max$(comma)+invtsc$(comma)+rdrand$(comma)+rdseed \
		    -overcommit cpu-pm=on \
		    -d guest_errors$(comma)unimp$(comma)cpu_reset, \
		    -cpu qemu64$(comma)+fsgsbase) \
		-m $(MEM) \
		-monitor unix:$(BUILDDIR)/qemu.mon$(comma)server$(comma)nowait \
		-serial file:$(BUILDDIR)/serial.log \
		-display none \
		$(if $(filter-out 1,$(CORES)),-smp $(CORES)$(comma)cores=$(CORES)$(comma)threads=1$(comma)sockets=1) \
		$(if $(filter on,$(USB)),-device qemu-xhci -device usb-kbd) \
		-pidfile $(BUILDDIR)/qemu.pid \
		-daemonize
	@i=0; while [ ! -S $(BUILDDIR)/qemu.mon ] && [ $$i -lt 50 ]; do sleep 0.1; i=$$((i+1)); done
	@if [ -f $(BUILDDIR)/qemu.pid ]; then \
		echo "[run-bg] QEMU started (pid=$$(cat $(BUILDDIR)/qemu.pid))"; \
		echo "[run-bg] Send keys : tools/qemu-input.sh type \"files\"; tools/qemu-input.sh key ret"; \
		echo "[run-bg] View VGA  : tools/qemu-input.sh shot /tmp/screen.ppm"; \
		echo "[run-bg] Tail log  : tools/qemu-input.sh tail"; \
		echo "[run-bg] Stop      : make run-stop"; \
	else \
		echo "[run-bg] ERROR: QEMU failed to start"; exit 1; \
	fi

# ===================================================================
# === USB FLASHING (real hardware) ==================================
# ===================================================================
# Usage:  make usb DEV=/dev/diskN     (macOS — get from `diskutil list`)
#         sudo make usb DEV=/dev/sdX  (Linux — get from `lsblk`)
#
# Writes the raw image (build/boxos.img) sector-by-sector to a USB stick
# or external disk. Result: bootable on any x86_64 PC via BIOS legacy /
# CSM AND UEFI:
#   • Sector 0..16   = stage1 + stage2 (BIOS legacy boot path)
#   • Sectors 17+    = kernel + TagFS (data + apps + UEFI loader files)
#   • build/BOOTX64.EFI is embedded in the image too — UEFI firmware will
#     find it automatically if the disk has GPT+ESP layout (left for a
#     follow-up; current layout is raw + MBR signature, BIOS legacy only).
#
# SAFETY: refuses to write to /dev/disk0 / /dev/sda (likely host system),
# requires explicit DEV=, asks for confirmation before destroying data.
# ===================================================================

usb: $(IMAGE)
	@if [ -z "$(DEV)" ]; then \
		echo "ERROR: specify target device, e.g. make usb DEV=/dev/disk4"; \
		echo "  macOS: diskutil list  (look for the USB stick — never disk0!)"; \
		echo "  Linux: lsblk         (look for sdb/sdc — never sda!)"; \
		exit 1; \
	fi
	@case "$(DEV)" in \
		/dev/disk0|/dev/disk0s*|/dev/sda|/dev/sda[0-9]*|/dev/nvme0n1|/dev/nvme0n1p*) \
			echo "REFUSED: $(DEV) looks like the host system disk."; \
			echo "         Pick a USB stick: diskutil list / lsblk."; \
			exit 1 ;; \
	esac
	@if [ ! -e "$(DEV)" ]; then \
		echo "ERROR: $(DEV) does not exist."; exit 1; \
	fi
	@IMG_SIZE=$$(stat -f%z $(IMAGE) 2>/dev/null || stat -c%s $(IMAGE)); \
	IMG_MB=$$((IMG_SIZE / 1024 / 1024)); \
	echo "================================================================"; \
	echo "  About to overwrite $(DEV) with $(IMAGE) ($${IMG_MB} MB)"; \
	echo "  All data on $(DEV) will be LOST."; \
	echo "================================================================"; \
	printf "  Type 'yes' to continue: "; \
	read CONFIRM; \
	if [ "$$CONFIRM" != "yes" ]; then \
		echo "Aborted."; exit 1; \
	fi
	@if [ "$(UNAME_S)" = "Darwin" ]; then \
		RAW_DEV=$$(echo $(DEV) | sed 's|/dev/disk|/dev/rdisk|'); \
		echo "[usb] macOS detected — unmounting $(DEV) and writing to $${RAW_DEV} (raw, faster)"; \
		diskutil unmountDisk $(DEV) || true; \
		echo "[usb] sudo is for the dd ONLY — running the whole make as root"; \
		echo "      would leave root-owned objects in $(BUILDDIR) that a later"; \
		echo "      ordinary build cannot overwrite."; \
		sudo dd if=$(IMAGE) of=$${RAW_DEV} bs=4m status=progress conv=sync; \
		SYNC_RC=$$?; \
		diskutil eject $(DEV) || true; \
		exit $$SYNC_RC; \
	else \
		echo "[usb] Linux detected — writing to $(DEV) (requires root)"; \
		echo "  unmounting any mounted partitions on $(DEV)..."; \
		for part in $$(mount | awk -v d=$(DEV) '$$1 ~ d {print $$1}'); do \
			umount $$part 2>/dev/null || true; \
		done; \
		sudo dd if=$(IMAGE) of=$(DEV) bs=4M status=progress conv=fsync oflag=direct; \
		sync; \
	fi
	@echo "[usb] Done. The stick now boots BoxOS via BIOS legacy / CSM."
	@echo "[usb] To boot on real hardware:"
	@echo "       1) Plug the stick into the target PC"
	@echo "       2) Enter firmware setup (F2/F12/Del at power-on)"
	@echo "       3) Disable Secure Boot (we don't sign yet)"
	@echo "       4) Enable Legacy/CSM boot OR pick the stick in UEFI menu"
	@echo "       5) Boot from USB"

run-stop:
	@if [ -f $(BUILDDIR)/qemu.pid ]; then \
		PID=$$(cat $(BUILDDIR)/qemu.pid); \
		if kill -0 $$PID 2>/dev/null; then \
			kill $$PID 2>/dev/null && echo "[run-stop] killed pid=$$PID"; \
			sleep 0.3; kill -0 $$PID 2>/dev/null && kill -9 $$PID 2>/dev/null; \
		else \
			echo "[run-stop] pid $$PID not running"; \
		fi; \
		rm -f $(BUILDDIR)/qemu.pid $(BUILDDIR)/qemu.mon; \
	else \
		echo "[run-stop] no pidfile at $(BUILDDIR)/qemu.pid"; \
	fi

# Headless Bochs with COM1 on a TCP socket — drive the shell entirely over the
# serial console (kernel serial_console_init), no GUI. Bochs blocks until a
# client connects, then boots; connect + interact via tools/serial-console.py.
# Bochs is single-CPU here (homebrew build has no --enable-smp), so the multi-
# core BMIDE watchdog stays dormant — this target is for headless shell access
# and log capture, not SMP tests (use QEMU run-bg for those).
BOCHS_SERIAL_PORT ?= 14400
bochs-bg: $(IMAGE)
	@command -v $(BOCHS) >/dev/null || \
	    (echo "ERROR: bochs not found. Install: brew install bochs / apt install bochs"; exit 1)
	@if [ -z "$(BOCHS_BIOS)" ] || [ -z "$(BOCHS_VGABIOS)" ]; then \
	    echo "ERROR: Bochs BIOS/VGABIOS not found in: $(BOCHS_DATA_DIRS)"; exit 1; fi
	@$(MAKE) --no-print-directory bochs-stop >/dev/null 2>&1 || true
	@rm -f build/boxos.img.lock $(BUILDDIR)/bochs_serial.out
	@BOCHS_RC="$(BOCHS_RC)" IMAGE="$<" CORES="1" MEM="$(MEM)" \
	  BOCHS_CPU="$(BOCHS_CPU)" BOCHS_IPS="$(BOCHS_IPS)" \
	  BOCHS_DISP_LIB="nogui" BOCHS_DATA_DIRS="$(BOCHS_DATA_DIRS)" \
	  BOCHS_BIOS="$(BOCHS_BIOS)" BOCHS_VGABIOS="$(BOCHS_VGABIOS)" \
	  BOCHS_LOG="$(BOCHS_LOG)" \
	  BOCHS_COM1_MODE="socket-server" BOCHS_COM1_DEV="127.0.0.1:$(BOCHS_SERIAL_PORT)" \
	  bash tools/make_bochsrc.sh
	@( $(BOCHS) -q -unlock -f $(BOCHS_RC) >$(BUILDDIR)/bochs_serial.out 2>&1 & \
	   echo $$! >$(BUILDDIR)/bochs.pid )
	@echo "=== BoxOS Bochs (headless, COM1 -> tcp:127.0.0.1:$(BOCHS_SERIAL_PORT)) ==="
	@echo "  pid=$$(cat $(BUILDDIR)/bochs.pid) (waiting for a serial client, then boots)"
	@echo "  Run cmds : python3 tools/serial-console.py 127.0.0.1:$(BOCHS_SERIAL_PORT) files help"
	@echo "  Live     : python3 tools/serial-console.py 127.0.0.1:$(BOCHS_SERIAL_PORT)"
	@echo "  Stop     : make bochs-stop"

bochs-stop:
	@if [ -f $(BUILDDIR)/bochs.pid ]; then \
	    PID=$$(cat $(BUILDDIR)/bochs.pid); \
	    kill $$PID 2>/dev/null && echo "[bochs-stop] killed pid=$$PID"; \
	    sleep 0.3; kill -0 $$PID 2>/dev/null && kill -9 $$PID 2>/dev/null; \
	    rm -f $(BUILDDIR)/bochs.pid; \
	else echo "[bochs-stop] no pidfile"; fi
	@rm -f build/boxos.img.lock

clean:
	@echo "Cleaning build..."
	@rm -rf $(BUILDDIR)
	@rm -f $(TAGFS_TOOL)
	@rm -f tools/nameplate
	@rm -f $(TAGBOOT_SO) $(TAGBOOT_OBJ) $(TAGBOOT_JUMP_OBJ)
	@cd $(USERSPACE_DIR) && $(MAKE) clean
	@cd $(SHELL_DIR) && $(MAKE) clean
	@cd $(APPS_DIR) && $(MAKE) clean
	@cd $(DISPLAY_DIR) && $(MAKE) clean
	@cd $(UTILS_DIR) && $(MAKE) clean
	@cd $(USERSPACE_DIR)/boxlib && $(MAKE) clean
	# boxcxx was missing here, and its absence was not cosmetic: no Makefile in
	# userspace tracks header dependencies, so `make clean && make` is the ONLY
	# thing that rebuilds after a header changes — and for the whole C++ library
	# it was doing nothing. A boxcxx header edit left every object that included
	# it stale, which is how Ф41-e got a green run out of a mutation that never
	# reached the image, and how <iosfwd> could hold two different mbstate_t at
	# once. Found by a mutation that should have failed and did not.
	@cd $(USERSPACE_DIR)/boxcxx && $(MAKE) clean

install-deps:
	@echo "Installing dependencies for $(UNAME_S)..."
	@if [ "$(UNAME_S)" = "Linux" ]; then \
		echo "Linux detected (Debian/Ubuntu)..."; \
		sudo apt update; \
		sudo apt install -y nasm qemu-system-x86 xorriso virtualbox bochs make \
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
		echo "  brew install nasm qemu xorriso x86_64-elf-gcc bochs"; \
		echo ""; \
		echo "Or using MacPorts:"; \
		echo "  sudo port install nasm qemu xorriso crossgcc-x86_64-elf bochs"; \
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
	@echo "  all          — full build (img, iso, elf, BOOTX64.EFI)"
	@echo "  uefi         — build only the UEFI bootloader (build/BOOTX64.EFI)"
	@echo "  run          — run BoxOS (EMU=qemu by default; EMU=bochs for Bochs)"
	@echo "  run-bg       — run QEMU in background with monitor socket (QEMU only)"
	@echo "  run-stop     — stop background QEMU"
	@echo "  debug        — alias for run GDB=on"
	@echo "  usb DEV=...  — flash build/boxos.img to a USB stick (real-HW boot)"
	@echo "  clean        — clean build directory"
	@echo "  install-deps — install required packages"
	@echo ""
	@echo "Emulator selection:"
	@echo "  EMU=qemu      — (default) QEMU"
	@echo "  EMU=bochs     — Bochs (per-instruction tracing, internal debugger)"
	@echo ""
	@echo "Run flags (shared):"
	@echo "  DEBUG=on      — enable debug output (rebuilds with CONFIG_DEBUG_ENABLED=1)"
	@echo "  GDB=on        — QEMU: pause for gdb (-s -S); Bochs: enable gdbstub on :1234"
	@echo "  CORES=N       — number of CPU cores (Bochs SMP needs --enable-smp)"
	@echo "  MEM=size      — RAM size (e.g. 512M, 4G)"
	@echo "  LOG=on        — verbose logging (QEMU int/cpu_reset; Bochs info/debug)"
	@echo ""
	@echo "QEMU-only flags:"
	@echo "  FULLSCREEN=on — fullscreen mode"
	@echo "  USB=on|off    — enable/disable USB keyboard"
	@echo "  AHCI=on       — use AHCI disk controller"
	@echo "  UEFI=on       — boot via OVMF + TagBoot EFI (requires OVMF.fd)"
	@echo ""
	@echo "Bochs-only flags:"
	@echo "  BOCHSDBG=on        — launch with -debugger (internal text debugger)"
	@echo "  BOCHS_DISPLAY=...  — sdl2 (default macOS), x, term, nogui, win32"

debug: $(IMAGE)
	@$(MAKE) run GDB=on IMAGE=$(IMAGE) CORES=$(CORES) MEM=$(MEM) FULLSCREEN=$(FULLSCREEN) USB=$(USB) AHCI=$(AHCI) LOG=$(LOG) DEBUG=$(DEBUG)
