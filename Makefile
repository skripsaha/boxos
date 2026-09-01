# ===================================================================
# BoxOS Makefile - Cross-platform OS build system
# ===================================================================
# Builds: disk image, VDI, ELF with debug symbols
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
# Bootloader layout (LBA addressing). Sectors are absolute across the whole
# medium; the partition table added on 2026-08-24 describes this layout, it did
# not move it — which is why every LBA below is the one it always was.
#   Sector 0        : Stage1 (446 bytes of code, then the MBR partition table)
#   Sectors 1-16    : Stage2 (16 sectors = 8192 bytes)
#   Sectors 17-2047 : the rest of the first mebibyte — nothing, on purpose
#   Sector 2048+    : the BoxOS partition. Everything inside it is the volume's
#                     own business and is counted from HERE, not from sector 0:
#                     its Deed, its Ledger, its DiskBook, its bitmap, its data.
#                     The kernel finds it by reading this table, exactly as the
#                     tool that made it did.
#   Sector 51200+   : EFI System Partition (FAT32) — the UEFI half of the
#                     medium, appended AFTER the BoxOS partition on purpose
#
# The kernel is a FILE in the volume. It is not written to a sector of its own:
# the loaders find it through the Deed, which says which block it starts at.
# It used to be dd'd to sector 17 as well, where the volume then overwrote its
# tail — a copy nothing read and nothing could have booted.
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
# Where the loaders leave the Boarding Pass — what they say about the journey
# that produced this kernel, as opposed to boot_info, which describes the
# machine. One number, spelled here, threaded into the assembler, the UEFI
# loader and the kernel, all three of which assert it against this value rather
# than repeat it.
BOARDING_PASS_ADDR  = 0xA600
KERNEL_MAX_BYTES    = 33554432  # 32MB (sanity check; bootloader places page tables dynamically after kernel)

# Where the volume's ground begins. One mebibyte in: the step every
# partitioning tool has used for fifteen years, and the one every flash
# translation layer and 4Kn medium is built around. It is also what makes the
# volume's own 4096-byte grid line up with the medium's — the old layout put
# the superblock at sector 1034, three sectors into a physical block, so every
# metadata write cost the device a read, a patch and a write.
GROUND_START_SECTOR = 2048

# The whole BoxOS region: sector 0 through BOXOS_SECTORS-1. The volume is the
# part of it from GROUND_START_SECTOR on — 24 MiB exactly.
BOXOS_SECTORS       = 51200

# Where the ESP begins, immediately after the BoxOS partition and on the same
# 2048-sector step.
ESP_START_SECTOR    = 51200

ASM_INCLUDE    = -I$(SRCDIR)/kernel/arch/x86-64/gdt/
ASMFLAGS       =  -g -f bin
# Layout facts the image build owns, handed to the assembler rather than
# repeated inside it.
ASM_LAYOUT     = -DSTAGE2_SECTORS=$(STAGE2_SECTORS) -DBOOT_INFO_ADDR=$(BOOT_INFO_ADDR) -DBOARDING_PASS_ADDR=$(BOARDING_PASS_ADDR)
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

# Bring-up hold: the first user-mode fault prints in full and the machine
# stops, instead of the process being killed and the next one scheduled. On a
# running system the normal behaviour is right; on the first boot of a new
# machine it scrolls the only dump that mattered off a screen with no
# scrollback. `make BRINGUP=on` — never in a shipped build.
# Let the startup tests write to the mounted volume.
#
# Off by default, because the volume a running machine has mounted is the one
# somebody keeps their files on — and these tests create files, allocate blocks
# and checkpoint the journal on it, at every boot. That was invisible until a
# flash drive was read back after two boots on a real machine and had
# test_file, test_rw and stress_file_0..2 sitting among its owner's files.
#
# `make VOLUME_TESTS=on` — for a throwaway image, never on a medium that
# carries anything.
ifeq ($(VOLUME_TESTS),on)
CFLAGS += -DCONFIG_VOLUME_TESTS=1
endif

# A SECOND xHCI controller, because a machine with two of them is a different
# machine and we had no way to be one.
#
# The board has an Intel 00:14.0 and an NVIDIA 01:00.2, and the driver
# registers the NVIDIA one FIRST — so the stick lives on controller INDEX 1.
# Every defect that turns on "which controller is this" was invisible here
# until now: QEMU's default has exactly one, and one controller is always
# index 0. `make run-bg USB=on XHCI2=on` adds a second, and a device can be
# hot-plugged onto it with bus=xhci2.0.
XHCI2 ?= off

# An isochronous device on the bus. This kernel configures no isochronous
# endpoint — it has nothing that would read one — and the point of being able
# to attach one is to prove that it SAYS so instead of dropping that half of
# the device in silence. `make run-bg USB=on ISOCH=on`
ISOCH ?= off

# HOW MANY ROOT PORTS the emulated controller has.
#
# MaxPorts in HCSPARAMS1 is an eight-bit field, so a controller may report up
# to 255 root ports — and this driver had a survey that stopped at the
# thirty-first and a bookkeeping word thirty-two bits wide to match. Every
# device in a socket above that was invisible to the boot.
#
# QEMU's xHCI allows fifteen ports per protocol and no more, so the cliff
# itself cannot be reached here. What CAN be reached is the invariant that
# broke on it: every port the controller says it has must be looked at and
# described. `make run-bg USB=on XHCIPORTS=15` gives thirty of them, which is
# enough for that check to be worth something and enough to catch the cliff
# coming back at any bound this machine can express.
#
# Empty means "whatever QEMU defaults to", which is four of each.
XHCIPORTS ?=
XHCI_PORT_ARGS = $(if $(XHCIPORTS),$(comma)p2=$(XHCIPORTS)$(comma)p3=$(XHCIPORTS))

# A switch that changes CFLAGS has to change what gets rebuilt, or it does
# nothing on a tree that is already built — `make VOLUME_TESTS=on` would print
# nothing, link the objects it already had, and boot a kernel with the tests
# still skipped. Measured, first try.
#
# Same trick this file already uses for the host OS signature: the marker's
# NAME carries the setting, so switching it names a file that does not exist,
# whose recipe touches it, and every object depending on it recompiles.
VOLUME_TESTS_MARK = $(BUILDDIR)/.volume_tests.$(if $(filter on,$(VOLUME_TESTS)),on,off)

ifeq ($(BRINGUP),on)
CFLAGS += -DCONFIG_BRINGUP_HOLD_ON_FIRST_FAULT=1
endif

# Keep what the kernel says, so a machine with no serial cable can be asked
# afterwards. Every byte kputchar() emits also lands in a ring in kernel
# memory, and the `logsave` command writes that ring to a file on the volume
# through Current. See src/lib/kernel/klib_logring.h for why a ring in memory
# and not a file written as it goes.
#
# Off by default and deliberately not "always on": a board run where this is
# wanted is not a constant activity, and a build that carries a megabyte of
# buffer for a boot nobody is going to read is a cost with no reader. Built
# once with it, every run of that image keeps its log — first, second, third —
# until the tree is rebuilt without it.
#
# `make PRINTTOFILE=on`
ifeq ($(PRINTTOFILE),on)
CFLAGS += -DCONFIG_PRINTTOFILE=1
endif

# Same marker trick as VOLUME_TESTS below: a switch that changes CFLAGS has to
# change what gets rebuilt, or `make PRINTTOFILE=on` on a built tree links the
# objects it already had and boots a kernel with no ring in it.
PRINTTOFILE_MARK = $(BUILDDIR)/.printtofile.$(if $(filter on,$(PRINTTOFILE)),on,off)

# Prove, on the machine, that the hardware deck can put a USB controller back in
# service — the whole eleven-step repair the driver keeps for a controller that
# has stopped itself, asked for by name through hw.usb.reset.
#
# That repair is production code that had never once run in any test: nothing in
# an emulator makes a controller halt itself, so error_state was never set and
# xhci_recover_if_needed never had anything to do. This is the switch that makes
# a boot go through it deliberately, once, with a keyboard and a volume on the
# bus — which is also the only way to find out whether the machine survives it.
#
# Off in every ordinary build: it costs the machine every USB device it has.
#
# `make USBRECOVER=on`
ifeq ($(USBRECOVER),on)
CFLAGS += -DCONFIG_USB_RECOVER_PROOF=1
endif

USBRECOVER_MARK = $(BUILDDIR)/.usbrecover.$(if $(filter on,$(USBRECOVER)),on,off)

# Run, once, the one control transfer no emulated device will ever fail to
# answer: posted and not rung for, so xhci_ep_wait genuinely gives up. Proves
# the transfer is taken back off the endpoint rather than merely forgotten —
# see xhci_ctrl_giveup_proof. Off in every ordinary build.
#
# `make CTRLGIVEUP=on`
ifeq ($(CTRLGIVEUP),on)
CFLAGS += -DCONFIG_XHCI_CTRL_GIVEUP_PROOF=1
endif

CTRLGIVEUP_MARK = $(BUILDDIR)/.ctrlgiveup.$(if $(filter on,$(CTRLGIVEUP)),on,off)

# Boot the way both real boards booted: with IA32_EFER.NXE off.
#
# OVMF leaves NXE set, which is exactly why the emulator never showed this and
# the desk did. `make NOEXEC=off` makes CpuTakeUpNoExecute decline — the same
# state a firmware that leaves the bit clear hands the kernel — so the whole
# chain downstream of it can be measured here instead of on a monitor with a
# phone camera. A kernel built this way must still boot: bit 63 stays out of
# every page table entry, the hardening is lost, nothing else is.
#
# ‼ It is a REPRODUCTION, not a bug: with the fix removed and this key on, the
# machine dies inside SetVirtualAddressMap with err=0x9 exactly as the boards
# did. That is what makes the noexec scenario an oracle rather than a claim.
#
# `make NOEXEC=off`
ifeq ($(NOEXEC),off)
CFLAGS += -DCONFIG_NO_EXECUTE_REFUSED=1
endif

NOEXEC_MARK = $(BUILDDIR)/.noexec.$(if $(filter off,$(NOEXEC)),off,on)

# Let an interrupt in before the kernel is ready for one — the way the board did.
#
# On an i5-9400F booting UEFI, RFLAGS.IF was set by something outside this
# kernel (a loader, or firmware returning from a runtime call in breach of
# UEFI 2.10 §8.1). The first HPET tick then arrived inside cpu_calibrate_tsc,
# thirty-six lines of kernel_main before scheduler_init() allocated any state,
# and irq_handler wrote through the NULL it got back: #PF at 0x18, dead.
#
# `make EARLYIRQ=on` reproduces that machine state exactly — an sti straight
# after the HPET tick is armed — so the window can be measured here instead of
# on a monitor. A kernel built this way MUST still boot: the tick is real, it
# simply has nowhere to be recorded yet, and everything else on that path is
# safe without a scheduler.
#
# ‼ It is a REPRODUCTION, not a bug. Remove the NULL guard in irq_handler and
# this key kills the machine at 0x18, exactly as the board did.
ifeq ($(EARLYIRQ),on)
CFLAGS += -DCONFIG_EARLY_INTERRUPTS_PROOF=1
endif

EARLYIRQ_MARK = $(BUILDDIR)/.earlyirq.$(if $(filter on,$(EARLYIRQ)),on,off)

# Fail the FIRST mount of the volume, once, halfway through.
#
# tagfs_init brings up eleven things in order, and until this key existed no
# emulator could reach the case where one of them refuses: QEMU's disk answers
# every read. So the whole failure path — and everything that follows from it —
# was unreachable from the desk, and it was on that path that the owner's board
# lost its filesystem: a mount that failed left five subsystems standing,
# nothing took them down, and every later mount died at TagFS_CowInit with
# ERR_ALREADY_INITIALIZED, reported by a debug_printf that compiles to nothing.
# The room seated the medium, the deed was described, "58 files" was printed,
# and the shell answered `Unknown command` to every one of them.
#
# `make MOUNTFAIL=on` fails the first mount at the worst point there is, with
# all of those subsystems up. The kernel MUST then say so out loud, clear the
# ground, and mount successfully on the catch-up attempt storage_deck_init makes
# straight afterwards.
#
# ‼ It is a REPRODUCTION, not a bug. Take tagfs_clear_the_ground out of the top
# of tagfs_init and this key leaves the machine exactly as the board was.
ifeq ($(MOUNTFAIL),on)
CFLAGS += -DCONFIG_TAGFS_MOUNT_FAIL_ONCE=1
endif

MOUNTFAIL_MARK = $(BUILDDIR)/.mountfail.$(if $(filter on,$(MOUNTFAIL)),on,off)

# ── HOLDGROUND=on ──────────────────────────────────────────────────────────
#
# A volume whose medium leaves is dropped and read again, and everything the
# mount allocated goes back to the allocator at that moment. On a desk that is
# always safe, because the medium comes back while the machine is idle and
# there is nobody inside the volume to be protected from it — so every check
# about the barrier that stops it would be green by construction.
#
# This key puts somebody there: at the moment the medium is noticed gone one
# caller steps into the volume and stays for two seconds, and a file is held
# open across the whole departure. It is a REPRODUCTION of the situation, not
# a bug; see the block over TAGFS_HOLD_GROUND_MS in tagfs.c for the three
# separate lines it makes falsifiable.
ifeq ($(HOLDGROUND),on)
CFLAGS += -DCONFIG_TAGFS_HOLD_GROUND=1
endif

HOLDGROUND_MARK = $(BUILDDIR)/.holdground.$(if $(filter on,$(HOLDGROUND)),on,off)

# ── RETURNFAIL=on ──────────────────────────────────────────────────────────
#
# Mounting is driven by ARRIVALS, and a medium that is already seated does not
# arrive twice. So a mount that begins on a volume's RETURN and fails leaves
# the machine with no filesystem and nothing that would ever ask again — the
# stick sitting in its socket, answering, ignored for the rest of the boot.
#
# Measured on the owner's board 2026-08-30: three returns of fifty-five failed,
# and all three came back ONLY because the hand at the machine kept replugging.
#
# This key fails the first RE-mount once, with the medium left exactly where it
# is, so the only road back is the machine asking again by itself.
ifeq ($(RETURNFAIL),on)
CFLAGS += -DCONFIG_TAGFS_RETURN_FAIL_ONCE=1
endif

RETURNFAIL_MARK = $(BUILDDIR)/.returnfail.$(if $(filter on,$(RETURNFAIL)),on,off)

# The handoff address the image build chose, handed to the C side so the two
# headers that name it can _Static_assert against it. Unconditional on
# purpose: it first went in under `ifeq ($(DEBUG),on)`, where DEBUG defaults
# to off, so the assertion that exists to catch a drift was compiled in
# exactly the builds nobody ships.
CFLAGS += -DBOOT_INFO_ADDR_FROM_BUILD=$(BOOT_INFO_ADDR)
CFLAGS += -DBOARDING_PASS_ADDR_FROM_BUILD=$(BOARDING_PASS_ADDR)
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
             timezone logsave lastsaid rgbtest
UTIL_ELFS = $(addprefix $(UTILS_DIR)/,$(addsuffix .elf,$(UTIL_NAMES)))

# ==== FINAL BINARIES ====
KERNEL_BIN   = $(BUILDDIR)/kernel.bin
KERNEL_ELF   = $(BUILDDIR)/kernel.elf
STAGE1_BIN   = $(BUILDDIR)/stage1.bin
STAGE2_BIN   = $(BUILDDIR)/stage2.bin
IMAGE        = $(BUILDDIR)/boxos.img
# Defined here, with the other image paths, and not down beside the UEFI build
# rules: Make expands a prerequisite list when it READS the rule, so a variable
# defined below the rule that names it expands to nothing and the dependency
# quietly disappears. That is exactly what happened when the ESP first became
# part of the image — the build got as far as `dd if=` an empty filename.
UEFI_ESP_IMG = $(BUILDDIR)/esp.img
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
                  -DBOARDING_PASS_ADDR_FROM_BUILD=$(BOARDING_PASS_ADDR) \
                  -I$(SRCDIR)/boot/uefi -I$(SRCDIR)/include

# clang direct-to-PE path: -fpic is invalid on MSVC target; PE handles
# relocations natively via the .reloc section.
UEFI_CFLAGS_CLANG = -ffreestanding -nostdlib -nostdinc \
                    -mno-red-zone -mno-sse -mno-mmx -mno-avx \
                    -fshort-wchar -fno-stack-protector \
                    -Wall -Wextra -Os \
                    -target x86_64-unknown-windows \
                    -DBOOT_INFO_ADDR_FROM_BUILD=$(BOOT_INFO_ADDR) \
                    -DBOARDING_PASS_ADDR_FROM_BUILD=$(BOARDING_PASS_ADDR) \
                    -I$(SRCDIR)/boot/uefi -I$(SRCDIR)/include

# Detect whether lld-link is available for direct PE output via clang.
# Apple clang does not ship lld-link, so we fall through to the gcc+objcopy path
# even when 'clang' is in PATH.  Only enable the clang path when lld-link exists.
CLANG_AVAILABLE := $(shell command -v lld-link 2>/dev/null)

.PHONY: all clean run run-bg run-stop bochs-bg bochs-stop debug info check-deps install-deps uefi usb check-endbr64

# ==== MAIN TARGET ====
all: check-deps check-error-parity check-no-exit-sentinel $(IMAGE) $(KERNEL_ELF) $(VBOX_VDI) uefi check-endbr64

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
$(TAGFS_TOOL): tools/create_tagfs.c $(SRCDIR)/include/tagfs_reserved.h \
               $(SRCDIR)/include/volume_deed.h $(SRCDIR)/include/volume_ledger.h \
               $(TAGFS_OS_SIGNATURE) | $(BUILDDIR)
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
$(VOLUME_TESTS_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.volume_tests.*
	@touch $@

$(PRINTTOFILE_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.printtofile.*
	@touch $@

$(USBRECOVER_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.usbrecover.*
	@touch $@

$(CTRLGIVEUP_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.ctrlgiveup.*
	@touch $@

$(NOEXEC_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.noexec.*
	@touch $@

$(EARLYIRQ_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.earlyirq.*
	@touch $@

$(MOUNTFAIL_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.mountfail.*
	@touch $@

$(HOLDGROUND_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.holdground.*
	@touch $@

$(RETURNFAIL_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.returnfail.*
	@touch $@

$(BUILDDIR)/kernel/drivers/usb/%.o: $(SRCDIR)/kernel/drivers/usb/%.c $(VOLUME_TESTS_MARK) $(PRINTTOFILE_MARK) $(USBRECOVER_MARK) $(CTRLGIVEUP_MARK) $(NOEXEC_MARK) $(EARLYIRQ_MARK) $(MOUNTFAIL_MARK) $(HOLDGROUND_MARK) $(RETURNFAIL_MARK) | $(BUILDDIR)
	@echo "Compiling USB driver $<..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -Os -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(VOLUME_TESTS_MARK) $(PRINTTOFILE_MARK) $(USBRECOVER_MARK) $(CTRLGIVEUP_MARK) $(NOEXEC_MARK) $(EARLYIRQ_MARK) $(MOUNTFAIL_MARK) $(HOLDGROUND_MARK) $(RETURNFAIL_MARK) | $(BUILDDIR)
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
	    echo "       stage1 would load a truncated stage2 that still passes its signature check."; \
	    rm -f $@; exit 1; \
	fi
	@if [ $$(( 1 + $(STAGE2_SECTORS) )) -gt $(GROUND_START_SECTOR) ]; then \
	    echo "ERROR: stage2 occupies sectors 1..$$(( $(STAGE2_SECTORS) )) and the volume's"; \
	    echo "       ground begins at $(GROUND_START_SECTOR) — the loader would be inside the volume,"; \
	    echo "       and the volume's Deed would be written over it."; \
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
# ---- Kernel Nameplate ------------------------------------------------------
# The kernel carries its own address->name table, for the same reason every
# userspace image does: a panic is exactly when you want names, and exactly
# when you cannot go and look them up. A photograph of a screen reading
# "efi_runtime_init+0x39" is a diagnosis; one reading "ffffffff8014bae9" is a
# request for the matching kernel.elf, which the person holding the camera
# does not have and which the next build will overwrite.
#
# Two passes, because the table names addresses and so cannot exist before
# them. Pass one produces the addresses, the tool turns them into a table,
# pass two links it in — and since .nameplate sits before .data in the linker
# script, pass two moves data and not one function. `nameplate verify` checks
# that claim against the binary's own symbols on every build.
KERNEL_NP_OBJ   = $(BUILDDIR)/kernel.np.o
KERNEL_PASS1    = $(BUILDDIR)/kernel.pass1.elf
NAMEPLATE_TOOL  = tools/nameplate
KERNEL_LINK_IN  = $(KERNEL_ENTRY_OBJ) $(C_OBJS) $(ASM_OBJS) $(SHELL_EMBED)

$(NAMEPLATE_TOOL): tools/nameplate.c src/include/nameplate_format.h
	@echo "Compiling Nameplate tool..."
	@$(CC_HOST) -O2 -Wall -Wextra -Isrc/include -o $@.$$$$ $< && mv -f $@.$$$$ $@

$(KERNEL_NP_OBJ): $(KERNEL_LINK_IN) $(NAMEPLATE_TOOL) $(ENTRYDIR)/linker.ld
	@echo "Nameplate pass 1 (kernel)..."
	@$(LD) -g -nostdlib -T $(ENTRYDIR)/linker.ld -o $(KERNEL_PASS1) $(KERNEL_LINK_IN)
	@$(NAMEPLATE_TOOL) build $(KERNEL_PASS1) $@

$(KERNEL_BIN): $(KERNEL_LINK_IN) $(KERNEL_NP_OBJ)
	@echo "Linking kernel (raw binary)..."
	@$(LD) $(LDFLAGS) -o $@ $(KERNEL_LINK_IN) $(KERNEL_NP_OBJ)
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
$(IMAGE): $(UEFI_ESP_IMG) $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(PROCA_BIN) $(PROCB_BIN) $(TODAY_BIN) $(MEMTEST_BIN) $(MTEST_BIN) $(CHAIN_BIN) $(DECKS_BIN) $(BENCH_BIN) $(TOUCH_TEST_BIN) $(TOUCH_STRESS_BIN) $(LIFECYCLE_BIN) $(PERSIST_BIN) $(WRITE_STRESS_BIN) $(WRITE_CONC_BIN) $(WRITE_OBS_BIN) $(COW_TEST_BIN) $(ANCHOR_TEST_BIN) $(BAY_TEST_BIN) $(BROOK_TEST_BIN) $(CURRENT_TEST_BIN) $(HTEST_BIN) $(CXXTEST_BIN) $(STRANDTEST_BIN) $(BROOKSTRAND_BIN) $(BROOKEXEC_BIN) $(CURRENTEXEC_BIN) $(STRANDPARK_BIN) $(CHILDSPIN_BIN) $(PRINT_STRESS_BIN) $(EXITPATHS_BIN) $(DISPLAY_BIN) $(UTIL_ELFS) $(TAGFS_TOOL)
	@echo "Creating disk image ($$(( $(BOXOS_SECTORS) / 2048 ))MB BoxOS region)..."
	@dd if=/dev/zero of=$@ bs=512 count=$(BOXOS_SECTORS) status=none
	@echo "  Writing Stage1 (sector 0, 512 bytes)..."
	@dd if=$(STAGE1_BIN) of=$@ bs=512 conv=notrunc status=none
	@echo "  Writing Stage2 (sectors 1-$(STAGE2_SECTORS), $(STAGE2_SECTORS) sectors)..."
	@dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "  Embedding EFI System Partition at sector $(ESP_START_SECTOR)..."
	@dd if=$(UEFI_ESP_IMG) of=$@ bs=512 seek=$(ESP_START_SECTOR) conv=notrunc status=none
	@echo "  Writing the partition table (the volume's ground, then the ESP)..."
	@ESP_SECTORS=$$(( ( $$(stat -f%z $(UEFI_ESP_IMG) 2>/dev/null || stat -c%s $(UEFI_ESP_IMG)) + 511 ) / 512 )); \
	python3 tools/make_mbr.py $@ \
	    $(GROUND_START_SECTOR),$$(( $(BOXOS_SECTORS) - $(GROUND_START_SECTOR) )),7f,boot \
	    $(ESP_START_SECTOR),$$ESP_SECTORS,ef
	@echo "  Making the volume on it..."
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
		$(UTILS_DIR)/hw.elf      "utility,system,hardware" \
		$(UTILS_DIR)/logsave.elf "utility,system,log" \
		$(UTILS_DIR)/lastsaid.elf "utility,system,log" \
		$(UTILS_DIR)/rgbtest.elf "utility,test,display"
	@echo "Disk image created: $(IMAGE)"

# There is no floppy image and no ISO here any more.
#
# Both were stage1 + stage2 + a raw copy of the kernel, and NO VOLUME — so
# stage2 reached them, looked for a partition table, and had nowhere to go.
# They could not boot before this layout either: they carried no superblock at
# 1034 the loader could read. They were two build targets that produced two
# files nobody could start a machine from, and `all` waited on both.

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

# The ESP is built to be embedded, not to stand alone: its BPB records the LBA
# it will start at, because the FAT specification defines that field as the
# partition's offset and firmware that reads it computes absolute addresses
# from it. It no longer depends on edk2-vars.fd — NVRAM belongs to running a
# virtual machine, not to building a filesystem, and the image needs this even
# when no OVMF is installed.
$(UEFI_ESP_IMG): $(TAGBOOT_EFI) | $(BUILDDIR)
	@echo "Creating UEFI ESP image (FAT32) for sector $(ESP_START_SECTOR)..."
	@python3 tools/make_esp.py $(TAGBOOT_EFI) $@ $(ESP_START_SECTOR)

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
	@$(MAKE) --no-print-directory $(if $(filter on,$(UEFI)),$(BUILDDIR)/edk2-vars.fd)
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
	@$(MAKE) --no-print-directory $(if $(filter on,$(UEFI)),$(BUILDDIR)/edk2-vars.fd)
	@$(QEMU) \
		$(if $(filter on,$(UEFI)), \
			-machine q35 \
			-drive if=pflash$(comma)format=raw$(comma)readonly=on$(comma)file=$(OVMF_FD) \
			-drive if=pflash$(comma)format=raw$(comma)file=$(BUILDDIR)/edk2-vars.fd \
			-drive format=raw$(comma)file=$<$(comma)if=ide$(comma)index=0, \
			$(if $(filter on,$(STRICT)),-machine q35) \
			$(if $(filter on,$(AHCI)), \
				-drive id=disk0$(comma)file=$<$(comma)format=raw$(comma)if=none \
				-device ahci$(comma)id=ahci -device ide-hd$(comma)drive=disk0$(comma)bus=ahci.0, \
				-drive format=raw$(comma)file=$<$(comma)index=0$(comma)media=disk)) \
		$(if $(filter on,$(STRICT)), \
		    -cpu max$(comma)+invtsc$(comma)+rdrand$(comma)+rdseed \
		    -overcommit cpu-pm=on \
		    -d guest_errors$(comma)unimp$(comma)cpu_reset, \
		    $(if $(filter off,$(FSGSBASE)), \
		        -cpu qemu64, \
		        -cpu qemu64$(comma)+fsgsbase)) \
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
	@$(MAKE) --no-print-directory $(if $(filter on,$(UEFI)),$(BUILDDIR)/edk2-vars.fd)
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
			-drive format=raw$(comma)file=$<$(comma)if=ide$(comma)index=0, \
			$(if $(filter on,$(AHCI)), \
				-drive id=disk0$(comma)file=$<$(comma)format=raw$(comma)if=none \
				-device ahci$(comma)id=ahci \
				-device ide-hd$(comma)drive=disk0$(comma)bus=ahci.0$(comma)bootindex=0, \
				-drive format=raw$(comma)file=$<$(comma)index=0$(comma)media=disk)) \
		$(if $(filter on,$(STRICT)), \
		    -cpu max$(comma)+invtsc$(comma)+rdrand$(comma)+rdseed \
		    -overcommit cpu-pm=on \
		    -d guest_errors$(comma)unimp$(comma)cpu_reset, \
		    $(if $(filter off,$(FSGSBASE)), \
		        -cpu qemu64, \
		        -cpu qemu64$(comma)+fsgsbase)) \
		-m $(MEM) \
		-monitor unix:$(BUILDDIR)/qemu.mon$(comma)server$(comma)nowait \
		-serial file:$(BUILDDIR)/serial.log \
		-display none \
		$(if $(filter-out 1,$(CORES)),-smp $(CORES)$(comma)cores=$(CORES)$(comma)threads=1$(comma)sockets=1) \
		$(if $(filter on,$(USB)),-device qemu-xhci$(comma)id=xhci1$(XHCI_PORT_ARGS) -device usb-kbd$(comma)bus=xhci1.0) \
		$(if $(filter on,$(XHCI2)),-device qemu-xhci$(comma)id=xhci2) \
		$(if $(filter on,$(ISOCH)),-audiodev none$(comma)id=isoa -device usb-audio$(comma)audiodev=isoa) \
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
# Writes the raw image (build/boxos.img) sector-by-sector to a USB stick or
# external disk. One medium, both firmware paths — which is the only
# arrangement a real user ever holds:
#   • Sector 0        = MBR: stage1's boot code and the partition table.
#     A stick WITHOUT that table is one the firmware has to guess about, and
#     the guess is commonly USB-FDD — a 1.44 MB floppy emulation, 2880
#     sectors, while the volume begins at 2048 and the kernel inside it lives
#     past that, so the read runs off the end of the emulation partway
#     through loading.
#   • Sectors 1..16   = stage2 (BIOS legacy path)
#   • Partition 1     = the volume, marked bootable, starting one mebibyte in.
#                       Its Deed is the first block of it, and every number
#                       inside the volume is counted from there.
#   • Partition 2     = an EFI System Partition (FAT32) holding
#                       EFI/BOOT/BOOTX64.EFI, which UEFI firmware finds by
#                       itself. TagBoot then reads this same table for the
#                       BoxOS partition, so both paths reach the same volume
#                       by asking the same question.
#
# SAFETY: refuses to write to /dev/disk0 / /dev/sda (likely host system),
# requires explicit DEV=, asks for confirmation before destroying data.
# ===================================================================

# Writes the image; does NOT build it. `make` builds, `make usb` flashes, and
# the two stay separate on purpose.
#
# It used to depend on $(IMAGE), which was harmless while nothing else could
# change what a build produces. PRINTTOFILE changed that: the switch's value is
# carried in the NAME of a marker every object depends on, so `make usb` with
# the switch left off renamed the marker, invalidated every object, recompiled
# the whole tree — and quietly produced an image WITHOUT the log ring, which is
# the one thing the person flashing it had built it for. A flash command that
# rebuilds is a flash command that can hand you something other than what you
# built.
usb:
	@if [ ! -f "$(IMAGE)" ]; then \
		echo "ERROR: $(IMAGE) does not exist — run 'make' first"; \
		echo "  (and 'make PRINTTOFILE=on' if you want the machine to keep its log)"; \
		exit 1; \
	fi
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
	@echo "[usb] Done. The stick boots BoxOS both ways from this one medium:"
	@echo "      BIOS/CSM  — MBR sector 0, partition 1 marked bootable"
	@echo "      UEFI      — partition 2, an EFI System Partition holding"
	@echo "                  EFI/BOOT/BOOTX64.EFI"
	@echo "      In the firmware setup pick the plain entry for BIOS/CSM, or the"
	@echo "      one prefixed UEFI: for the other. Both land in the same BoxOS."
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
		sudo apt install -y nasm qemu-system-x86 virtualbox bochs make \
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
		echo "  brew install nasm qemu x86_64-elf-gcc bochs"; \
		echo ""; \
		echo "Or using MacPorts:"; \
		echo "  sudo port install nasm qemu crossgcc-x86_64-elf bochs"; \
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
