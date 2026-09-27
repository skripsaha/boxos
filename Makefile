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
#   Sector 2048+    : EFI System Partition (FAT32) — as long as the loader it
#                     carries needs, rounded up to the mebibyte
#   after the ESP   : the BoxOS partition, LAST on the medium. Everything
#                     inside it is the volume's own business and is counted
#                     from HERE, not from sector 0: its Deed, its Ledger, its
#                     DiskBook, its bitmap, its data. The kernel finds it by
#                     reading this table, exactly as the tool that made it did.
#
# Neither of the last two is a number in this file. The ESP is as long as its
# own image; the volume is as long as its content plus the room a running
# machine works in, and create_tagfs --ground is asked how long that is before
# the medium is made. The volume comes last so that a bigger device can be
# given to it later without moving anything in front of it.
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

# ‼ THE ONLY NUMBERS OF THE MEDIUM'S SHAPE THAT ARE TYPED. BOTH ARE GRAINS.
#
# One mebibyte in: the step every partitioning tool has used for fifteen years,
# and the one every flash translation layer and 4Kn medium is built around. It
# is also what makes the volume's own 4096-byte grid line up with the medium's
# — the old layout put the superblock at sector 1034, three sectors into a
# physical block, so every metadata write cost the device a read, a patch and
# a write.
#
# Everything else about the shape is DERIVED, in the image rule below:
#
#   sector 0            stage1, and the partition table inside it
#   sectors 1..16       stage2
#   sector 2048         the ESP — as long as the loader it carries needs,
#                       rounded up to the grain
#   after the ESP       the volume's ground, as far as its CONTENT needs plus
#                       the room a running machine works in (create_tagfs
#                       --ground says how far, and says why)
#
# ‼ THE VOLUME IS LAST ON THE MEDIUM, AND THAT IS THE WHOLE POINT.
#
# It used to be first, with the ESP behind it at a typed sector — so the volume
# could not be made larger without moving the ESP, and a 24 MiB volume written
# to a 64 GB stick stayed 24 MiB for ever. A partition that is last is one that
# can be grown into whatever the device turns out to be, without moving
# anything, which is what lets the volume take the ground it is given.
MEDIUM_GRAIN_SECTORS = 2048
ESP_START_SECTOR     = 2048

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
#   -fcallgraph-info=su
#                       one .ci beside every object: each function's frame and
#                       every direct call GCC emitted. Changes no byte of code
#                       (.text measured identical). Read by the stack guard
#                       beside the kernel link rules.
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
                 -Wstack-usage=8192 -fcallgraph-info=su
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
# ‼ The key whose absence is hardest to see. With DEBUG off every debug_printf
# compiles to nothing at all (klib.h: `do { if (0) kprintf(...); } while (0)`),
# so on an already-built tree `make DEBUG=on` linked the objects it already had
# and booted a kernel that stayed silent in exactly the place the person asking
# for DEBUG was looking. The marker is what carries the key to the compiler;
# see BUILD_KEY_MARKS further down for why a marker and not a variable.
DEBUG_MARK = $(BUILDDIR)/.debug.$(if $(filter on,$(DEBUG)),on,off)

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

# WHICH PCI SLOT the second controller sits in. Empty means "wherever QEMU
# puts it", which is what twoctrl wants: that scenario cares about there being
# two controllers, not about where they are.
#
# The intx scenario cares about exactly that. Two functions share an INTx wire
# when the board's routing table sends their (slot, pin) to the same interrupt
# link, and on this machine type that is a function of the slot number — so
# the only way to ASK for a shared line rather than hope for one is to name
# the slot. `make run-bg USB=on XHCI2=on XHCI2ADDR=0x08`
XHCI2ADDR ?=

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

# HARDWARE THIS FILE DOES NOT NAME.
#
# Every key above adds a piece of emulated hardware this tree has a reason to
# ask for by name, and each one is a line in the run recipes. That works right
# up to the first piece of hardware nobody has a key for — and the IOMMU was
# exactly that: the whole DMA-translation path shipped without ever having
# executed, because QEMU publishes no DMAR table unless it is asked to and
# there was no way to ask from here. `[IOMMU] ACPI describes neither DMAR nor
# IVRS` was the entire result of every run this tree has ever done.
#
# So: whatever is put here is appended verbatim to the END of the QEMU command
# line, after everything the keys above built. Verbatim, because a device
# string is QEMU's language and not this file's, and anything that parsed it
# here would only be a second parser to be wrong.
#
#   make run-bg UEFI=on QEMUEXTRA="-device intel-iommu"
#
# ‼ No rebuild marker, and that is not an omission. The markers above exist
# for keys that change CFLAGS, where a stale object file would silently boot
# the kernel you did not build. This changes nothing about the kernel — it is
# a key of the same kind as USB, AHCI, XHCI2 and ISOCH, which describe the
# MACHINE the already-built image is booted on, and none of them has one
# either. The build key for the IOMMU side is IOMMU=on|off, and that one does
# have its marker.
QEMUEXTRA ?=

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
BRINGUP_MARK = $(BUILDDIR)/.bringup.$(if $(filter on,$(BRINGUP)),on,off)

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
# `make PRINTTOFILE=on` — the log ring itself is always in the kernel (it is
# the serial line's source); this switch adds the carry-over window, the copy
# of the ring that survives a warm reset for `lastsaid`.
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

# ── INTX=on ────────────────────────────────────────────────────────────────
#
# Make the xHCI driver decline MSI-X and MSI, so the boot arms the PCI INTx
# line instead.
#
# The Summons work — a line that arrives described, with its polarity, its
# trigger, its shareability and the PROVENANCE of each of those — exists for
# INTx and only for INTx. And on every machine this kernel is tested on, that
# path is dead code: QEMU's xHCI offers MSI-X, QEMU's AHCI offers MSI, and a
# controller that takes a message-signalled interrupt never touches a wire.
# Measured on the `dd871b4` run: exactly ONE line was armed in the whole boot,
# the ACPI SCI, and not one PCI function reached PciSummons.
#
# So the four defects that work closed — an open-drain level-sensitive wire
# programmed edge/active-high from the ISA default, one handler per line on a
# bus where sharing is mandatory, a GSI taken from the byte firmware wrote for
# the 8259 instead of from _PRT, and a suppressed EOI broadcast that would let
# a level line fire exactly once — could all still be there and every test
# would be green.
#
# This key is not a reproduction of a bug: it is the machine being asked to
# use the wire it has. Real boards do — an xHCI behind a bridge that does not
# forward MSI, a chipset function with the capability fused off, a firmware
# that leaves MSI unusable — and this is the only way to be one of them here.
#
# ‼ It costs nothing but the delivery mechanism: with the line armed the
# keyboard still types, which is exactly the claim the intx scenario makes.
#
# `make INTX=on` / `tools/logcheck.sh intx`
ifeq ($(INTX),on)
CFLAGS += -DCONFIG_XHCI_INTX_PROOF=1
endif

INTX_MARK = $(BUILDDIR)/.intx.$(if $(filter on,$(INTX)),on,off)

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

# The way back from DMA translation.
#
# Translation is ON by default: straight after pci_init, every PCI function on
# the machine is given an identity-map entry and then every remap unit is
# switched on — or none of them is, and the kernel says why. That is the
# owner's decision, taken knowing the risk it carries: the first board to run
# this is the first machine on which all DMA has ever gone through tables this
# kernel wrote, and if one of them is wrong the symptom is a screen that never
# comes back rather than a message.
#
# `make IOMMU=off` builds the kernel that does not throw that switch.
# iommu_take_the_wheel declines out loud and every device reaches memory
# untranslated exactly as it did before the wheel existed. The units still come
# up, the tables are still written, the fault channel is still read — only the
# last step is skipped, so the difference between a board that boots and a
# board that does not is one build key and nothing else.
#
# Not a reproduction and not a test key: this is the way back.
ifeq ($(IOMMU),off)
CFLAGS += -DCONFIG_IOMMU_TRANSLATION_OFF=1
endif

IOMMU_MARK = $(BUILDDIR)/.iommu.$(if $(filter off,$(IOMMU)),off,on)

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

# ── SETTLECUT=<point> ──────────────────────────────────────────────────────
#
# The power goes at a named point of the metadata sync. The first sync of the
# boot to reach it — an `anchor`, or the shutdown; nothing syncs during boot —
# stops the whole machine right there, for the host to read the medium back
# (tools/volume-read.py check). The medium then holds every write issued before
# the point and none after it: the best case of a power cut there, the one in
# which the drive's cache survived. A real cut can also lose any of the writes
# since the last cache flush; those states are PowerCut's (tools/powercut.sh,
# `tools/volume-read.py cut`), never this key's. The points are named by the
# record ABOUT to be written, so a scenario keeps its meaning when the sync
# becomes Settle at step 1.4:
#
#   registry pool table retires integrity bitmap ledger flush done
#
# tagfs/settle_cut/settle_cut.h is the one list of them; a value that is not on
# it names an undeclared point and does not compile. Its mark lives apart from
# the unset one (at-<value> against unset), so no value — `off` included — can
# find the plain kernel's objects already built and skip that compile.
#
# Not with VOLUME_TESTS=on: the startup tests sync the volume themselves, so the
# cut would land inside a test instead of in the scenario it names.
SETTLECUT ?=
ifneq ($(SETTLECUT),)
ifeq ($(VOLUME_TESTS),on)
$(error SETTLECUT=$(SETTLECUT) cannot be built with VOLUME_TESTS=on — the startup tests sync the volume themselves, so the cut would land inside a test instead of in the scenario it names)
endif
CFLAGS += -DCONFIG_TAGFS_SETTLE_CUT_POINT=SETTLE_CUT_POINT_$(shell printf '%s' '$(SETTLECUT)' | tr '[:lower:]' '[:upper:]')
endif

SETTLECUT_MARK = $(BUILDDIR)/.settlecut.$(if $(SETTLECUT),at-$(SETTLECUT),unset)

# The handoff address the image build chose, handed to the C side so the two
# headers that name it can _Static_assert against it. Unconditional on
# purpose: it first went in under `ifeq ($(DEBUG),on)`, where DEBUG defaults
# to off, so the assertion that exists to catch a drift was compiled in
# exactly the builds nobody ships.
CFLAGS += -DBOOT_INFO_ADDR_FROM_BUILD=$(BOOT_INFO_ADDR)
CFLAGS += -DBOARDING_PASS_ADDR_FROM_BUILD=$(BOARDING_PASS_ADDR)
# On-demand BMIDE watchdog TIER-2 (wedge->SRST) diagnostic. Off unless requested
# (it SRSTs the boot drive) — `make WEDGETEST=on` for a verification build.
# The BMIDE channel exists only on the i440fx machine, and the proof skips on
# one core: boot such a build with CORES>=2 and without STRICT=on — STRICT is
# q35, which has no legacy IDE — or with QEMUEXTRA='-machine pc' after it.
WEDGETEST ?= off
ifeq ($(WEDGETEST),on)
CFLAGS += -DCONFIG_BMIDE_WEDGE_SELFTEST=1
endif
WEDGETEST_MARK = $(BUILDDIR)/.wedgetest.$(if $(filter on,$(WEDGETEST)),on,off)

# The silence road of USB enumeration, made reachable on the stand.
#
# Under emulation every control request is answered at once, so the road this
# driver spent eight seconds of every real boot on cannot be produced at all —
# which is why its defect survived eight green scenarios. `make ENUMSILENCE=on`
# withholds the doorbell for ONE descriptor request, once, on the first device
# that asks: the controller then never starts the transfer, exactly as both
# boards reported. Never in a shipped build.
ENUMSILENCE ?= off
ifeq ($(ENUMSILENCE),on)
CFLAGS += -DCONFIG_XHCI_ENUM_SILENCE_PROOF=1
endif
ENUMSILENCE_MARK = $(BUILDDIR)/.enumsilence.$(if $(filter on,$(ENUMSILENCE)),on,off)

# ‼ THE ONE KEY THAT MUST NOT REACH THE COMPILER. `make HARNESS=on` says who is
# asking — the test harness, which has the tree deliberately mutated — and it
# lets the pack gate stamp the image UNDER TEST instead of refusing it. It adds
# no -D and so has no marker: a key that changed CFLAGS would change the tree
# fingerprint the plate carries, and the plate would then be answering a
# question about itself.
HARNESS ?= off

# THE BUILD KEYS, AS GIVEN, FOR THE PLATE TO CARRY.
#
# Only what is NOT at its default is named, and the plate says "all defaults"
# when nothing is. Spelling out all fourteen would take about two hundred
# characters of the plate's two hundred and eight, and a key list that has to be
# truncated is a key list that lies — the tool refuses a long one rather than
# cutting it. Two keys default to ON, so it is their OFF that is worth saying.
BUILD_KEYS_SET := $(strip \
  $(if $(filter on,$(DEBUG)),DEBUG=on,) \
  $(if $(filter on,$(BRINGUP)),BRINGUP=on,) \
  $(if $(filter on,$(WEDGETEST)),WEDGETEST=on,) \
  $(if $(filter on,$(VOLUME_TESTS)),VOLUME_TESTS=on,) \
  $(if $(filter on,$(PRINTTOFILE)),PRINTTOFILE=on,) \
  $(if $(filter on,$(USBRECOVER)),USBRECOVER=on,) \
  $(if $(filter on,$(CTRLGIVEUP)),CTRLGIVEUP=on,) \
  $(if $(filter on,$(INTX)),INTX=on,) \
  $(if $(filter on,$(EARLYIRQ)),EARLYIRQ=on,) \
  $(if $(filter on,$(MOUNTFAIL)),MOUNTFAIL=on,) \
  $(if $(filter on,$(HOLDGROUND)),HOLDGROUND=on,) \
  $(if $(filter on,$(RETURNFAIL)),RETURNFAIL=on,) \
  $(if $(SETTLECUT),SETTLECUT=$(SETTLECUT),) \
  $(if $(filter on,$(ENUMSILENCE)),ENUMSILENCE=on,) \
  $(if $(filter off,$(NOEXEC)),NOEXEC=off,) \
  $(if $(filter off,$(IOMMU)),IOMMU=off,))
BUILD_KEYS := $(if $(BUILD_KEYS_SET),$(BUILD_KEYS_SET),all defaults)
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
QUIETPRINT_BIN   = $(APPS_DIR)/quietprint.elf
EXITPATHS_BIN    = $(APPS_DIR)/exitpaths.elf
QUILLTEST_BIN    = $(APPS_DIR)/quilltest.elf

# Display server ELF
DISPLAY_DIR = $(USERSPACE_DIR)/display
DISPLAY_BIN = $(DISPLAY_DIR)/display.elf

# Utility ELF binaries
UTILS_DIR = $(USERSPACE_DIR)/utils
UTIL_NAMES = help create show files tag untag name trash erase \
             me info say reboot bye defrag fsck ipc_test memtag hw \
             timezone logsave lastsaid rgbtest usetest luggagetest headcount \
             draft playtime lavalamp finish
UTIL_ELFS = $(addprefix $(UTILS_DIR)/,$(addsuffix .elf,$(UTIL_NAMES)))

# ==== FINAL BINARIES ====
KERNEL_BIN   = $(BUILDDIR)/kernel.bin
KERNEL_ELF   = $(BUILDDIR)/kernel.elf
STAGE1_BIN   = $(BUILDDIR)/stage1.bin
STAGE2_BIN   = $(BUILDDIR)/stage2.bin
IMAGE        = $(BUILDDIR)/boxos.img

# An empty file the volume carries as a fixture (tags: emptyfile, test:forerror).
# It is zero bytes and therefore not a source file: the repository ignores *.bin,
# and a fresh clone used to fail with "Failed to open: src/userspace/testbin.bin".
# Made where every other build product is made.
TESTBIN_EMPTY = $(BUILDDIR)/testbin.bin
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

.PHONY: all clean run run-bg run-stop bochs-bg bochs-stop debug info check-deps install-deps uefi usb check-endbr64 logs logs-list

# A recipe that fails leaves its target on disk otherwise, and the next make
# takes that half-written file for a finished one. Measured on a fresh clone:
# the volume tool failed, a partial boxos.img stayed behind, and QEMU booted it
# into SeaBIOS with an empty serial log — the matrix then blamed "keystrokes
# dropped". Delete what a failed recipe left, always.
.DELETE_ON_ERROR:

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

$(IOMMU_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.iommu.*
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

$(SETTLECUT_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.settlecut.*
	@touch $@

$(INTX_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.intx.*
	@touch $@

$(DEBUG_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.debug.*
	@touch $@

$(BRINGUP_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.bringup.*
	@touch $@

$(WEDGETEST_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.wedgetest.*
	@touch $@

$(ENUMSILENCE_MARK): | $(BUILDDIR)
	@rm -f $(BUILDDIR)/.enumsilence.*
	@touch $@

# EVERY KEY THAT REACHES THE COMPILER, NAMED ONCE.
#
# ‼ This list used to be written out twice — in the USB object rule and in the
# general one — and that is exactly how three keys came to have no marker at
# all. DEBUG, BRINGUP and WEDGETEST each change CFLAGS and each was missing
# from both copies, so for as long as they have existed `make DEBUG=on` on an
# already-built tree changed nothing and said nothing about changing nothing.
# A list a person has to remember to extend in two places is a list that will
# be extended in one.
#
# Adding a key: a *_MARK beside its ifeq, a rule above, and a name HERE.
# Nowhere else.
BUILD_KEY_MARKS := $(VOLUME_TESTS_MARK) $(PRINTTOFILE_MARK) $(USBRECOVER_MARK) \
                   $(CTRLGIVEUP_MARK) $(NOEXEC_MARK) $(EARLYIRQ_MARK) \
                   $(MOUNTFAIL_MARK) $(HOLDGROUND_MARK) $(RETURNFAIL_MARK) \
                   $(SETTLECUT_MARK) $(IOMMU_MARK) $(INTX_MARK) $(DEBUG_MARK) \
                   $(BRINGUP_MARK) $(WEDGETEST_MARK) $(ENUMSILENCE_MARK)

$(BUILDDIR)/kernel/drivers/usb/%.o: $(SRCDIR)/kernel/drivers/usb/%.c $(BUILD_KEY_MARKS) | $(BUILDDIR)
	@echo "Compiling USB driver $<..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -Os -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(BUILD_KEY_MARKS) | $(BUILDDIR)
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
	@if [ $$(( 1 + $(STAGE2_SECTORS) )) -gt $(ESP_START_SECTOR) ]; then \
	    echo "ERROR: stage2 occupies sectors 1..$$(( $(STAGE2_SECTORS) )) and the first"; \
	    echo "       partition begins at $(ESP_START_SECTOR) — the loader would be inside it,"; \
	    echo "       and would be written over."; \
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
#
# ONE recipe, ONE target — and that is the whole point. GNU make reads
# "a b c: deps" followed by a recipe as "run this recipe FOR EACH of a, b, c",
# so under -j8 it launched this same sub-make up to thirty times at once,
# against the same intermediate .o / .elf.pass1 / .np.o files. The failures it
# produced read exactly like failures of the tree — "nameplate: not an ELF
# file", "undefined reference to `main'", "bytes 446..509 of the boot sector are
# not empty" — which is the worst kind of defect a build can have: a race that
# lies about the source. It cost two agents in one session a false hunt each.
#
# The grouped-target form (`&:`) says this directly, but it arrived in GNU make
# 4.3 and this machine's make is 3.81. So the recipe hangs on a single stamp and
# every binary simply waits for it. The stamp is distrusted whenever any of the
# binaries it claims to have built is missing, so deleting one by hand heals
# itself at the next parse instead of silently doing nothing.
APP_ELFS := $(PROCA_BIN) $(PROCB_BIN) $(TODAY_BIN) $(MEMTEST_BIN) $(MTEST_BIN) $(CHAIN_BIN) $(DECKS_BIN) $(BENCH_BIN) $(TOUCH_TEST_BIN) $(TOUCH_STRESS_BIN) $(LIFECYCLE_BIN) $(PERSIST_BIN) $(WRITE_STRESS_BIN) $(WRITE_CONC_BIN) $(WRITE_OBS_BIN) $(COW_TEST_BIN) $(ANCHOR_TEST_BIN) $(BAY_TEST_BIN) $(BROOK_TEST_BIN) $(CURRENT_TEST_BIN) $(HTEST_BIN) $(CXXTEST_BIN) $(STRANDTEST_BIN) $(BROOKSTRAND_BIN) $(BROOKEXEC_BIN) $(CURRENTEXEC_BIN) $(STRANDPARK_BIN) $(CHILDSPIN_BIN) $(PRINT_STRESS_BIN) $(QUIETPRINT_BIN) $(EXITPATHS_BIN) $(QUILLTEST_BIN)

APPS_STAMP  := $(APPS_DIR)/.apps.stamp
APPS_INPUTS := $(wildcard $(APPS_DIR)/*.c) $(wildcard $(APPS_DIR)/*.cpp) \
               $(wildcard $(APPS_DIR)/*.h) $(APPS_DIR)/Makefile

ifneq ($(words $(wildcard $(APP_ELFS))),$(words $(APP_ELFS)))
$(shell rm -f $(APPS_STAMP))
endif

$(APPS_STAMP): $(USERSPACE_DIR)/boxlib/libbox.a $(USERSPACE_DIR)/boxcxx/libboxcxx.a $(APPS_INPUTS)
	@echo "Building apps..."
	@cd $(APPS_DIR) && $(MAKE)
	@echo "proca.elf: $$(stat -f%z $(PROCA_BIN) 2>/dev/null || stat -c%s $(PROCA_BIN) 2>/dev/null) bytes"
	@echo "procb.elf: $$(stat -f%z $(PROCB_BIN) 2>/dev/null || stat -c%s $(PROCB_BIN) 2>/dev/null) bytes"
	@echo "today.elf: $$(stat -f%z $(TODAY_BIN) 2>/dev/null || stat -c%s $(TODAY_BIN) 2>/dev/null) bytes"
	@echo "memtest.elf: $$(stat -f%z $(MEMTEST_BIN) 2>/dev/null || stat -c%s $(MEMTEST_BIN) 2>/dev/null) bytes"
	@echo "mtest.elf: $$(stat -f%z $(MTEST_BIN) 2>/dev/null || stat -c%s $(MTEST_BIN) 2>/dev/null) bytes"
	@echo "chain.elf: $$(stat -f%z $(CHAIN_BIN) 2>/dev/null || stat -c%s $(CHAIN_BIN) 2>/dev/null) bytes"
	@echo "decks.elf: $$(stat -f%z $(DECKS_BIN) 2>/dev/null || stat -c%s $(DECKS_BIN) 2>/dev/null) bytes"
	@touch $@

$(APP_ELFS): $(APPS_STAMP)
	@:

# Build display server
$(DISPLAY_BIN): $(USERSPACE_DIR)/boxlib/libbox.a
	@echo "Building display server..."
	@cd $(DISPLAY_DIR) && $(MAKE)
	@echo "display.elf: $$(stat -f%z $(DISPLAY_BIN) 2>/dev/null || stat -c%s $(DISPLAY_BIN) 2>/dev/null) bytes"

# `timezone` carries the zone database, which is C++. Stated BEFORE the rule
# below and not inside it: a target line between a rule and its recipe ends
# that rule, which would leave every other utility with no recipe at all.
$(UTILS_DIR)/timezone.elf: $(USERSPACE_DIR)/boxcxx/libboxcxx.a

# Build utilities — same single-recipe rule as the apps above, and the same
# reason: one sub-make, not one per utility. This is the rule whose parallel
# copies produced "undefined reference to `main'" in timezone.elf while three
# other utilities were being linked.
UTILS_STAMP  := $(UTILS_DIR)/.utils.stamp
UTILS_INPUTS := $(wildcard $(UTILS_DIR)/*.c) $(wildcard $(UTILS_DIR)/*.cpp) \
                $(wildcard $(UTILS_DIR)/*.h) $(UTILS_DIR)/Makefile

ifneq ($(words $(wildcard $(UTIL_ELFS))),$(words $(UTIL_ELFS)))
$(shell rm -f $(UTILS_STAMP))
endif

$(UTILS_STAMP): $(USERSPACE_DIR)/boxlib/libbox.a $(USERSPACE_DIR)/boxcxx/libboxcxx.a $(UTILS_INPUTS)
	@echo "Building utilities..."
	@cd $(UTILS_DIR) && $(MAKE)
	@touch $@

$(UTIL_ELFS): $(UTILS_STAMP)
	@:

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

# ---- The stack guard -------------------------------------------------------
# A block buffer belongs to the job that moves the block, or to its OpContext —
# not to the kernel stack: a K-Core and every process stand on 16 KiB, and
# every interrupt lands on the same 16 KiB (no IST for vectors 32..255). GCC's
# -Wstack-usage has one threshold and no memory, so it cannot say "these
# twenty-three, until their step, and nothing new". tools/check_stack_usage.py
# can: it reads the .ci of every C object, holds what the held file names as
# Caveats, and refuses anything else. It stands in front of the link, so a
# kernel that puts a new block on the stack is never linked, and `make run-bg`
# cannot boot one either.
#
# The held numbers are the default build's. Under a build key the guard judges
# only what no key excuses — a new offender, the budget — and under HARNESS=on,
# a tree mutated on purpose, it says what it would refuse and writes no stamp,
# so the next build without the harness judges the same objects in full.
#
# Order-only below, and not only for tidiness: the ELF recipe links `$^`, and
# a stamp in that list would be handed to the linker.
STACK_CHECK       = tools/check_stack_usage.py
STACK_HELD        = tools/stack_usage_held.txt
STACK_USAGE_STAMP = $(BUILDDIR)/.stack_usage.ok

$(STACK_USAGE_STAMP): $(C_OBJS) $(STACK_CHECK) $(STACK_HELD) | $(BUILDDIR)
	@python3 $(STACK_CHECK) --build $(BUILDDIR) --held $(STACK_HELD) \
		--keys "$(BUILD_KEYS)" $(if $(filter on,$(HARNESS)),--harness,) \
		--stamp $@ $(C_OBJS)

$(NAMEPLATE_TOOL): tools/nameplate.c src/include/nameplate_format.h
	@echo "Compiling Nameplate tool..."
	@$(CC_HOST) -O2 -Wall -Wextra -Isrc/include -o $@.$$$$ $< && mv -f $@.$$$$ $@

# ---- The BuildPlate tool ---------------------------------------------------
# Computes the tree's fingerprint, rivets it into the flat kernel, and refuses
# to stamp a tree that carries a harness injection unless HARNESS=on says so
# deliberately. See src/include/buildplate_format.h for why the image has to
# carry this at all.
BUILDPLATE_TOOL = tools/buildplate

$(BUILDPLATE_TOOL): tools/buildplate.c src/include/buildplate_format.h src/include/boxos_magic.h
	@echo "Compiling BuildPlate tool..."
	@$(CC_HOST) -O2 -Wall -Wextra -Isrc/include -o $@.$$$$ $< && mv -f $@.$$$$ $@

$(KERNEL_NP_OBJ): $(KERNEL_LINK_IN) $(NAMEPLATE_TOOL) $(ENTRYDIR)/linker.ld | $(STACK_USAGE_STAMP)
	@echo "Nameplate pass 1 (kernel)..."
	@$(LD) -g -nostdlib -T $(ENTRYDIR)/linker.ld -o $(KERNEL_PASS1) $(KERNEL_LINK_IN)
	@$(NAMEPLATE_TOOL) build $(KERNEL_PASS1) $@

$(KERNEL_BIN): $(KERNEL_LINK_IN) $(KERNEL_NP_OBJ) $(BUILDPLATE_TOOL)
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
	@echo "Riveting the BuildPlate..."
	@$(BUILDPLATE_TOOL) stamp $@ --root . --keys "$(BUILD_KEYS)" \
		$(if $(filter on,$(HARNESS)),--allow-harness,)

$(KERNEL_ELF): $(KERNEL_ENTRY_OBJ) $(C_OBJS) $(ASM_OBJS) $(SHELL_EMBED) | $(STACK_USAGE_STAMP)
	@echo "Linking kernel ELF (with embedded shell binary)..."
	@$(LD) -g -nostdlib -T $(ENTRYDIR)/linker.ld -o $@ $^


# ==== DISK IMAGES ====
$(TESTBIN_EMPTY):
	@mkdir -p $(@D)
	@: > $@

# ‼ WHAT GOES ON THE VOLUME — named once, and asked about twice.
#
# The image rule asks create_tagfs --ground how far the ground must run to
# hold exactly this, and then asks it to lay exactly this down. Two questions
# about one list, so the medium can never be made too small for what is about
# to go in it. A second copy of the list is how those two answers would come
# to disagree.
VOLUME_CONTENT = \
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
	$(QUIETPRINT_BIN)    "app,utility,test" \
	$(EXITPATHS_BIN)     "app,utility,test,cxx" \
	$(QUILLTEST_BIN)     "app,utility,test,system" \
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
	$(TESTBIN_EMPTY)         "binary, test:forerror, emptyfile" \
	$(UTILS_DIR)/ipc_test.elf "utility" \
	$(UTILS_DIR)/memtag.elf  "utility,memory,system" \
	$(UTILS_DIR)/hw.elf      "utility,system,hardware" \
	$(UTILS_DIR)/usetest.elf "utility,system,test" \
	$(UTILS_DIR)/luggagetest.elf "utility,test" \
	$(UTILS_DIR)/logsave.elf "utility,system,log" \
	$(UTILS_DIR)/lastsaid.elf "utility,system,log" \
	$(UTILS_DIR)/rgbtest.elf "utility,test,display" \
	$(UTILS_DIR)/rollcall.elf "utility,test" \
	$(UTILS_DIR)/handset.elf "utility,test" \
	$(UTILS_DIR)/headcount.elf "utility,test" \
	$(UTILS_DIR)/draft.elf    "utility,storage" \
	$(UTILS_DIR)/playtime.elf "utility,display" \
	$(UTILS_DIR)/lavalamp.elf "utility,display" \
	$(UTILS_DIR)/finish.elf   "utility,system"

$(IMAGE): $(UEFI_ESP_IMG) $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(PROCA_BIN) $(PROCB_BIN) $(TODAY_BIN) $(MEMTEST_BIN) $(MTEST_BIN) $(CHAIN_BIN) $(DECKS_BIN) $(BENCH_BIN) $(TOUCH_TEST_BIN) $(TOUCH_STRESS_BIN) $(LIFECYCLE_BIN) $(PERSIST_BIN) $(WRITE_STRESS_BIN) $(WRITE_CONC_BIN) $(WRITE_OBS_BIN) $(COW_TEST_BIN) $(ANCHOR_TEST_BIN) $(BAY_TEST_BIN) $(BROOK_TEST_BIN) $(CURRENT_TEST_BIN) $(HTEST_BIN) $(CXXTEST_BIN) $(STRANDTEST_BIN) $(BROOKSTRAND_BIN) $(BROOKEXEC_BIN) $(CURRENTEXEC_BIN) $(STRANDPARK_BIN) $(CHILDSPIN_BIN) $(PRINT_STRESS_BIN) $(QUIETPRINT_BIN) $(EXITPATHS_BIN) $(QUILLTEST_BIN) $(DISPLAY_BIN) $(UTIL_ELFS) $(TESTBIN_EMPTY) $(TAGFS_TOOL)
	@GROUND=$$($(TAGFS_TOOL) --ground $(VOLUME_CONTENT)); \
	ESP_RAW=$$(( ( $$(stat -f%z $(UEFI_ESP_IMG) 2>/dev/null || stat -c%s $(UEFI_ESP_IMG)) + 511 ) / 512 )); \
	ESP=$$(( ( $$ESP_RAW + $(MEDIUM_GRAIN_SECTORS) - 1 ) / $(MEDIUM_GRAIN_SECTORS) * $(MEDIUM_GRAIN_SECTORS) )); \
	START=$$(( $(ESP_START_SECTOR) + $$ESP )); \
	TOTAL=$$(( $$START + $$GROUND )); \
	echo "Creating disk image: ESP $$(( $$ESP / 2048 )) MiB at sector $(ESP_START_SECTOR), volume $$(( $$GROUND / 2048 )) MiB at sector $$START..."; \
	dd if=/dev/zero of=$@ bs=512 count=$$TOTAL status=none; \
	echo "  Writing Stage1 (sector 0, 512 bytes)..."; \
	dd if=$(STAGE1_BIN) of=$@ bs=512 conv=notrunc status=none; \
	echo "  Writing Stage2 (sectors 1-$(STAGE2_SECTORS), $(STAGE2_SECTORS) sectors)..."; \
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none; \
	echo "  Embedding EFI System Partition at sector $(ESP_START_SECTOR)..."; \
	dd if=$(UEFI_ESP_IMG) of=$@ bs=512 seek=$(ESP_START_SECTOR) conv=notrunc status=none; \
	echo "  Writing the partition table (the ESP, then the volume's ground)..."; \
	python3 tools/make_mbr.py $@ \
	    $$START,$$GROUND,7f,boot \
	    $(ESP_START_SECTOR),$$ESP,ef
	@echo "  Making the volume on it..."
	@$(TAGFS_TOOL) $@ $(VOLUME_CONTENT)
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
#                              (RDRAND/RDSEED/AVX2/AES-NI/SMEP/SMAP/UMIP). The
#                              +invtsc asked for below is dropped by TCG — the
#                              guest reports invtsc=0 — it takes effect on KVM.
#                              Catches "kernel uses CPU feature without CPUID
#                              guard" bugs that default qemu64 silently masks.
#   • -machine q35           — modern PCH chipset (PCIe, AHCI, MSI-X, ACPI 6.x).
#                              Default i440fx is a 1996 PIIX board no longer
#                              shipping on real hardware. Asked for by run AND
#                              run-bg. Until 2026-09-27 only `run` asked, so a
#                              BIOS check by hand wrote through AHCI while the
#                              matrix — every config STRICT, every one run-bg —
#                              wrote through the legacy ATA channel, which is
#                              always synchronous: two stands, two write paths,
#                              and nothing said so. A -machine in QEMUEXTRA
#                              comes later on the line and wins (QEMU keeps the
#                              last machine type — measured). That is how the
#                              intx scenario and the matrix's BIOS PIIX STRICT
#                              4c config stay on i440fx, the one board with the
#                              legacy channel and its BMIDE watchdog; a plain
#                              BIOS run (STRICT=off) is on it as well.
#   • -d guest_errors,unimp,cpu_reset — log every undefined / unimplemented
#                              guest behaviour QEMU normally absorbs silently.
#   STRICT deliberately does NOT set -no-reboot or -no-shutdown:
#     - Real hardware reboots after reset (and triple-fault); STRICT mirrors
#       that — `reboot` actually restarts the VM, matching production.
#     - `bye` (ACPI S5) cleanly exits QEMU without -no-shutdown blocking.
#     - For triple-fault diagnostics (halt + log) use LOG=on instead, which
#       keeps both flags set and writes boxos_qemu.log.
#   • -overcommit cpu-pm=on   — KVM only: lets the guest's HLT/MWAIT reach the
#                              host CPU. Under TCG (every QEMU on this desk) it
#                              does nothing: HLT already parks the vCPU thread,
#                              and MWAIT on more than one vCPU runs as PAUSE —
#                              which is why cpu_idle asks CPUID.05H first.
STRICT     ?= off

# THE POWER GOES — AND EVERY WRITE BEFORE IT IS ON THE RECORD.
#
# `make run-bg POWERCUT=on` hands the boot disk to QEMU through blklogwrites:
# every write the guest makes and every flush it asks for is appended, in the
# order the disk finished them, to POWERCUT_LOG (the dm-log-writes format).
# tools/powercut.sh replays that record onto a copy of the disk as it stood
# before the run and so builds the medium a power cut would have left — at
# every flush, and at every subset of the writes between two of them. The
# emulator cannot pull a plug; this is how one run pulls it a thousand times.
#
#   · The record is emptied before QEMU starts: without log-append blklogwrites
#     starts again at sector 1 and truncates nothing (blklogwrites.c 255-260),
#     so a longer earlier run would leave entries behind the new ones.
#   · log-super-update-interval=1: the superblock is otherwise written only on
#     a flush or every 4096 entries (402-404), and a whole boot with no flush
#     left sector 0 all zeros — a record that counted nothing.
#   · cache.no-flush=on on the two FILE nodes only. The guest's flush still
#     reaches blklogwrites and is recorded (measured, 212 flushes under
#     write_stress); what is skipped is the host's fsync of two files on a
#     machine that is not the one losing power. Never on the blklogwrites node.
#
# A MACHINE key, like USB, AHCI and QEMUEXTRA: nothing about the kernel changes,
# so it has no marker and the plate does not carry it.
POWERCUT     ?= off
POWERCUT_LOG ?= $(BUILDDIR)/powercut.log

# THE DISK A RUN BOOTS, WHEN IT IS NOT THE ONE THIS TREE JUST MADE.
#
# The kernel is a file ON the volume, so a disk already carries the kernel
# that will boot from it. A disk handed in by name — a power-cut image, a copy
# of a volume that has lived — is booted as it stands, and run-bg then does
# not depend on $(IMAGE): rebuilding build/boxos.img first changes nothing
# about that boot and can lay a fresh volume under a harness that believed it
# had the old one (tools/logcheck.sh: stick_from_boot_image, grow_stood_on).
# ‼ Not IMAGE=: IMAGE is the target of the rule that lays the volume down, and
# pointing it at a disk worth keeping hands that disk to a recipe that rewrites
# it from nothing whenever make thinks it is out of date.
BOOTDISK ?= $(IMAGE)
RUN_BG_NEEDS = $(if $(filter $(IMAGE),$(BOOTDISK)),$(IMAGE))

# The chain, bottom to top: the disk's file and the record's file; blklogwrites
# over both — it writes the disk, THEN appends the write to the record
# (blklogwrites.c 511-512), which is why powercut.sh checks that the record
# accounts for every byte the disk holds; raw on top, as -drive format=raw was.
POWERCUT_CHAIN = \
	-blockdev driver=file$(comma)node-name=pc-disk-file$(comma)filename=$(BOOTDISK)$(comma)cache.no-flush=on \
	-blockdev driver=file$(comma)node-name=pc-log-file$(comma)filename=$(POWERCUT_LOG)$(comma)cache.no-flush=on \
	-blockdev driver=blklogwrites$(comma)node-name=pc-log$(comma)file=pc-disk-file$(comma)log=pc-log-file$(comma)log-sector-size=512$(comma)log-super-update-interval=1 \
	-blockdev driver=raw$(comma)node-name=pc-disk$(comma)file=pc-log
# ide.0 unit 0 is the PIIX primary master on pc and AHCI port 0 on q35 — where
# `-drive index=0` (if=ide by default on both) has always put the disk.
POWERCUT_IDE  = $(POWERCUT_CHAIN) -device ide-hd$(comma)drive=pc-disk$(comma)bus=ide.0$(comma)unit=0$(comma)bootindex=0
POWERCUT_AHCI = $(POWERCUT_CHAIN) -device ahci$(comma)id=ahci -device ide-hd$(comma)drive=pc-disk$(comma)bus=ahci.0$(comma)bootindex=0

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

# The two run-bg keys, refused here before anything is built: an interactive
# run would boot build/boxos.img unrecorded and let the key pass unheard.
ifneq ($(filter run,$(MAKECMDGOALS)),)
ifeq ($(POWERCUT),on)
$(error POWERCUT=on is a run-bg key: tools/powercut.sh reads the record, and an interactive run has nobody to read it)
endif
ifneq ($(BOOTDISK),$(IMAGE))
$(error BOOTDISK is a run-bg key: `make run` boots the image this tree makes)
endif
endif

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
		$(if $(filter on,$(GDB)),-s -S) \
		$(QEMUEXTRA)
endif

# ===================================================================
# Headless QEMU for automated key-injection testing.
# - Monitor on a Unix socket (sendkey, screendump, info ...)
# - Serial output to a file (kernel debug_printf, panic markers)
# - VGA framebuffer dumpable via tools/qemu-input.sh shot
# - Drive interactively from the shell with tools/qemu-input.sh.
# Same vars as `run`: UEFI=on, CORES=N, MEM=size, USB=on, AHCI=on; and the
# two of its own, BOOTDISK=path and POWERCUT=on (see RUN CONFIGURATION).
# The sub-make below names no goal without UEFI, and no goal is `all`: for the
# image this tree makes that is how it always ran. A disk handed in by name is
# booted as it stands, so it gets the UEFI vars file and nothing else — measured:
# with `all` run for it, a touched tagfs.c was compiled and build/boxos.img
# repacked under a BOOTDISK boot.
# ===================================================================
run-bg: $(RUN_BG_NEEDS)
	$(if $(RUN_BG_NEEDS)$(filter on,$(UEFI)),@$(MAKE) --no-print-directory $(if $(filter on,$(UEFI)),$(BUILDDIR)/edk2-vars.fd))
	@if [ -f $(BUILDDIR)/qemu.pid ] && kill -0 $$(cat $(BUILDDIR)/qemu.pid) 2>/dev/null; then \
		echo "[run-bg] QEMU already running (pid $$(cat $(BUILDDIR)/qemu.pid)). Use 'make run-stop' first."; \
		exit 1; \
	fi
	@[ -f "$(BOOTDISK)" ] || { echo "[run-bg] there is no disk at $(BOOTDISK)"; exit 1; }
	$(if $(filter on,$(POWERCUT)),@: > "$(POWERCUT_LOG)")
	@rm -f $(BUILDDIR)/qemu.mon $(BUILDDIR)/qemu.qmp $(BUILDDIR)/serial.log $(BUILDDIR)/qemu.pid
	@touch $(BUILDDIR)/serial.log
	@echo "=== BoxOS QEMU (background) ==="
	@echo "  Monitor : $(BUILDDIR)/qemu.mon"
	@echo "  Serial  : $(BUILDDIR)/serial.log"
	@echo "  Pidfile : $(BUILDDIR)/qemu.pid"
	$(if $(filter on,$(POWERCUT)),@echo "  Record  : $(POWERCUT_LOG)")
	@echo "==============================="
	@$(QEMU) \
		$(if $(filter on,$(UEFI)), \
			-machine q35 \
			-drive if=pflash$(comma)format=raw$(comma)readonly=on$(comma)file=$(OVMF_FD) \
			-drive if=pflash$(comma)format=raw$(comma)file=$(BUILDDIR)/edk2-vars.fd \
			$(if $(filter on,$(POWERCUT)),$(POWERCUT_IDE),-drive format=raw$(comma)file=$(BOOTDISK)$(comma)if=ide$(comma)index=0), \
			$(if $(filter on,$(STRICT)),-machine q35) \
			$(if $(filter on,$(AHCI)), \
				$(if $(filter on,$(POWERCUT)),$(POWERCUT_AHCI), \
					-drive id=disk0$(comma)file=$(BOOTDISK)$(comma)format=raw$(comma)if=none \
					-device ahci$(comma)id=ahci \
					-device ide-hd$(comma)drive=disk0$(comma)bus=ahci.0$(comma)bootindex=0), \
				$(if $(filter on,$(POWERCUT)),$(POWERCUT_IDE),-drive format=raw$(comma)file=$(BOOTDISK)$(comma)index=0$(comma)media=disk))) \
		$(if $(filter on,$(STRICT)), \
		    -cpu max$(comma)+invtsc$(comma)+rdrand$(comma)+rdseed \
		    -overcommit cpu-pm=on \
		    -d guest_errors$(comma)unimp$(comma)cpu_reset, \
		    $(if $(filter off,$(FSGSBASE)), \
		        -cpu qemu64, \
		        -cpu qemu64$(comma)+fsgsbase)) \
		-m $(MEM) \
		-monitor unix:$(BUILDDIR)/qemu.mon$(comma)server$(comma)nowait \
		-qmp unix:$(BUILDDIR)/qemu.qmp$(comma)server$(comma)nowait \
		-serial file:$(BUILDDIR)/serial.log \
		-display none \
		$(if $(filter-out 1,$(CORES)),-smp $(CORES)$(comma)cores=$(CORES)$(comma)threads=1$(comma)sockets=1) \
		$(if $(filter on,$(USB)),-device qemu-xhci$(comma)id=xhci1$(XHCI_PORT_ARGS) -device usb-kbd$(comma)bus=xhci1.0) \
		$(if $(filter on,$(XHCI2)),-device qemu-xhci$(comma)id=xhci2$(if $(XHCI2ADDR),$(comma)addr=$(XHCI2ADDR))) \
		$(if $(filter on,$(ISOCH)),-audiodev none$(comma)id=isoa -device usb-audio$(comma)audiodev=isoa) \
		-pidfile $(BUILDDIR)/qemu.pid \
		$(QEMUEXTRA) \
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
usb: $(BUILDPLATE_TOOL)
	@if [ ! -f "$(IMAGE)" ]; then \
		echo "ERROR: $(IMAGE) does not exist — run 'make' first"; \
		echo "  (and 'make PRINTTOFILE=on' if you want the machine to keep its log)"; \
		exit 1; \
	fi
	@# ‼ AND IT IS THE IMAGE THIS TREE MADE, OR IT IS NOT FLASHED.
	@#
	@# The cure for "do not rebuild" is not "do not look". This target still
	@# never builds — it reads the plate inside the image and recomputes the
	@# tree's fingerprint here and now, and refuses on three answers: no plate,
	@# a plate that does not match the tree, or a plate that says UNDER TEST.
	@#
	@# The second of those is the 2026-09-22 run exactly: the harness had put
	@# its injected source back, so the TREE was correct and the IMAGE was not,
	@# and nothing anywhere could say so. The third is the same run before the
	@# put-back. An UNDER TEST image can still be flashed on purpose, with
	@# FLASH_UNDER_TEST=on, because sometimes that is the measurement — but it
	@# is never flashed by accident.
	@$(BUILDPLATE_TOOL) verify $(IMAGE) --root . ; \
		rc=$$?; \
		if [ $$rc = 4 ] && [ "$(FLASH_UNDER_TEST)" = "on" ]; then \
			echo "  flashing an UNDER TEST image because FLASH_UNDER_TEST=on says so"; \
		elif [ $$rc != 0 ]; then \
			echo "REFUSED: see above. Run 'make' and try again"; \
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

# ---------------------------------------------------------------------------
# What the machine wrote down, off the stick and onto this desk.
#
# `make logs`            the stick finds itself; watch.log lands in build/board/
# `make logs FILE=x.log` some other file off the same volume
# `make logs-list`       what is on the stick at all
#
# No device to remember and no sudo, once you are in the 'operator' group —
# the tool's own refusal says how to join it, once. Reading a raw device is
# all this needs; `make usb` still asks for sudo, because WRITING one is a
# different question and should stay a deliberate act.
# ---------------------------------------------------------------------------
# ‼ NOT under $(BUILDDIR): `make clean` removes that whole tree, and a board
# log is the one artefact in this project that CANNOT BE REBUILT. Two were
# lost that way on 2026-09-25 — pulled off the stick, then destroyed by a
# clean before they were read a second time, with the stick already reflashed.
BOARD_DIR ?= boardlogs
STICK     ?= auto
FILE      ?= watch.log

.PHONY: logs logs-list
logs:
	@mkdir -p $(BOARD_DIR)
	@python3 tools/volume-read.py $(STICK) get $(FILE) -o $(BOARD_DIR)/$(FILE)
	@echo "[logs] $(BOARD_DIR)/$(FILE) — $$(wc -c < $(BOARD_DIR)/$(FILE) | tr -d ' ') bytes, $$(wc -l < $(BOARD_DIR)/$(FILE) | tr -d ' ') lines"

logs-list:
	@python3 tools/volume-read.py $(STICK) list

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
	@rm -f $(APPS_STAMP) $(UTILS_STAMP)
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
	@echo "  POWERCUT=on   — run-bg: record every write and flush of the boot disk in POWERCUT_LOG (tools/powercut.sh)"
	@echo "  BOOTDISK=path — run-bg: boot this disk as it stands, without rebuilding the image"
	@echo ""
	@echo "Bochs-only flags:"
	@echo "  BOCHSDBG=on        — launch with -debugger (internal text debugger)"
	@echo "  BOCHS_DISPLAY=...  — sdl2 (default macOS), x, term, nogui, win32"
	@echo ""
	@echo "Test-stand build keys (never in a shipped build):"
	@echo "  SETTLECUT=<point>  — the first sync to reach <point> stops the machine there, as a"
	@echo "                       power cut that kept the drive's cache would: registry|pool|"
	@echo "                       table|retires|integrity|bitmap|ledger|flush|done"
	@echo "                       (not with VOLUME_TESTS=on)"

debug: $(IMAGE)
	@$(MAKE) run GDB=on IMAGE=$(IMAGE) CORES=$(CORES) MEM=$(MEM) FULLSCREEN=$(FULLSCREEN) USB=$(USB) AHCI=$(AHCI) LOG=$(LOG) DEBUG=$(DEBUG)
