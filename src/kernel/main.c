#include "screen.h"
#include "klib.h"
#include "klib_logring.h"
#include "serial.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "pic.h"
#include "irqchip.h"
#include "pit.h"
#include "hpet.h"
#include "iommu.h"
#include "aml.h"
#include "efi.h"
#include "efi_esrt.h"
#include "efi_secureboot.h"
#include "efi_selftest.h"
#include "rtc.h"
#include "clockboard.h"
#include "nightwatch.h"
#include "e820.h"
#include "fpu.h"
#include "process.h"
#include "autostart.h"
#include "rise.h"
#include "guide.h"
#include "scheduler.h"
#include "keyboard.h"
#include "ata.h"
#include "pci.h"
#include "storage_deck.h"
#include "kernel_config.h"
#include "ready_queue.h"
#include "xhci.h"
#include "xhci_interrupt.h"
#include "boardroom.h"
#include "acpi.h"
#include "ahci.h"
#include "cabin_layout.h"
#include "tagfs.h"
#include "deed.h"
#include "cpuid.h"
#include "cpu_caps_page.h"
#include "boarding.h"
#include "boot_info.h"
#include "notify.h"
#include "amp.h"
#include "per_core.h"
#include "kcore.h"
#include "baton.h"
#include "lapic.h"
#include "per_core.h"
#include "idle.h"
#include "cpu_calibrate.h"
#include "hypervisor.h"
#include "pvclock.h"
#include "hvclock.h"
#include "aslr.h"
#include "memtag.h"
#include "mce.h"
#include "op_registry.h"
#include "manifest.h"
#include "manifest_selftest.h"
#include "crate_io_selftest.h"
#include "auth_decouple_selftest.h"
#include "slab.h"   /* slab_identity_selftest */
#include "operations_deck.h"
#include "hardware_deck.h"
#include "system_deck.h"
#include "buildplate.h"   /* BuildPlateSay — the first line of the log */

/*
 * RFLAGS as the loader left them, captured by _start before its very first
 * cli. Bit 9 (IF) is the whole point: it is the difference between a kernel
 * that is interrupted only when it says so and one that inherits whatever the
 * firmware was doing. Written once, from assembly, after BSS is zeroed.
 */
uint64_t g_entry_rflags = 0;

void kernel_main(void)
{
    /* Before a single byte is said, because a byte said before this is a byte
     * the next boot will not find. See klib_logring.h for what the window
     * survives — a warm reset, not a power cut. */
    LogKeepInit();

    ScreenInit();

    /*
     * ‼ THE FIRST THING THIS KERNEL SAYS ABOUT ITSELF, AND IT SAYS WHAT IT WAS
     * MADE FROM.
     *
     * The slot is exact and there is only one. After ScreenInit, so the line is
     * on the glass of a machine with no cable; before serial_init, so it is in
     * the log ring before the UART complains about not being there — the ring
     * keeps it either way and the Wire drains it the moment it exists.
     *
     * Why a kernel says this at all: an image built over a test-harness
     * injection was flashed to a stick and booted on the owner's laptop on
     * 2026-09-22. It printed 2274 lines, 1228 of them from code that was no
     * longer in the tree by the time anybody read them, and nothing in the log
     * could say so. Every timing conclusion from that run had to be thrown
     * away. A run is evidence about a kernel only if the log can name the
     * kernel; see src/include/buildplate_format.h.
     */
    BuildPlateSay();

    serial_init();

    kprintf(" Cabin (0x%lx Info, 0x%lx PocketRing, 0x%lx ResultRing, 0x%lx Code)\n",
            CABIN_INFO_ADDR, CABIN_POCKET_RING_ADDR, CABIN_RESULT_RING_ADDR, CABIN_CODE_START_ADDR);
    kprintf("\n");

    /* What state the loader handed over, said out loud.
     *
     * IF set here means the loader left interrupts enabled and the kernel has
     * been running interruptible since its entry point — which is what the
     * UEFI path did, because TagBootJump never clears IF while stage2.asm
     * clears it ten times over. _start now clears it unconditionally; this
     * line reports which of the two the machine came from, because a fix that
     * makes both paths silent teaches nothing about either. */
    kprintf("[BOOT] the loader handed over RFLAGS=0x%lx (interrupts were %s; "
            "they are off now, and stay off until this kernel says otherwise)\n",
            (unsigned long)g_entry_rflags,
            (g_entry_rflags & (1ULL << 9)) ? "ENABLED" : "disabled");

    /* WHETHER THIS IMAGE KEEPS ANYTHING AT ALL, said out loud and once.
     *
     * The carry-over window is behind PRINTTOFILE, and a default image
     * carried nothing while saying nothing — which is how a board run gets
     * thrown away: the machine dies, and only afterwards does anybody find
     * out that this image was never going to keep the record. So the three
     * cases are told apart by name.
     *
     * Early, so it is on the screen even when the machine does not get much
     * further, and so an operator who just watched a machine die knows
     * whether `lastsaid` has something for them. The bank is NOT dumped
     * here — it is named, and lastsaid reads it. */
#ifdef CONFIG_PRINTTOFILE
    if (LogKeepWindow(NULL, NULL)) {
        uint64_t carried = LogKeepPreviousBytes();
        if (carried) {
            kprintf("[BOOT] LogKeep is armed (written back with %s); the "
                    "previous run left %lu byte(s) behind (boot %u) — "
                    "`lastsaid` reads them\n",
                    LogKeepWriteBackName(),
                    (unsigned long)carried, LogKeepPreviousBoot());
        } else {
            kprintf("[BOOT] LogKeep is armed (written back with %s); nothing "
                    "came through the last reset — a cold start, a first one, "
                    "or firmware that scrubs memory\n",
                    LogKeepWriteBackName());
        }
    } else {
        kprintf("[BOOT] %[W]LogKeep is compiled in but this machine's E820 map "
                "does not offer the window — NOTHING will survive a reset%[D]\n");
    }
#else
    kprintf("[BOOT] %[W]LogKeep is COMPILED OUT of this image (build it with "
            "`make PRINTTOFILE=on`) — nothing this kernel says will survive a "
            "reset, and `lastsaid` will have nothing to read%[D]\n");
#endif

    debug_printf("[INIT] CPU Feature Detection (early)...\n");
    cpu_detect_features();

    /* Bit 63 becomes a meaning here, and not one line later.
     *
     * Until IA32_EFER.NXE is 1 it is a RESERVED bit (Intel SDM Vol 3A §4.5):
     * an entry carrying it is malformed, and the first access through it —
     * read, write or fetch — is a #PF with RSVD set. So the kernel takes it
     * up before it builds a single page table, and tells the VMM whether it
     * may compose the bit at all. This used to be the loader's business on
     * two paths out of three (stage2 set it, the AP trampoline set it, the
     * UEFI loader deliberately did not) and the kernel's own write sat in
     * per_core_setup_notify_msrs, seventy-four lines after efi_runtime_init
     * had already mapped firmware data with bit 63 and called into firmware
     * through it. Both real boards BoxOS has booted died there. */
    vmm_note_no_execute(CpuTakeUpNoExecute());

    /* Log the BSP's identity + microcode revision once feature detection
     * is up. Doing it here (instead of inside cpu_detect_features) keeps
     * the helper purely functional; the operator-visible log lives with
     * the boot sequence. AP identity is logged from ap_entry_c so each
     * core's silicon + patch level shows up in the boot log. */
    cpu_log_identity("BSP");

    /* Hypervisor detection must follow cpu_detect_features (so we know
     * cpuid is usable) and precede everything that asks "are we on
     * KVM/TCG/Hyper-V?" — currently that's TSC calibration and the
     * para-virt clock drivers. Idempotent. */
    debug_printf("[INIT] Hypervisor Detection...\n");
    hypervisor_detect();
    /* User-visible banner so the boot log always carries the
     * environment ID. Lets the user (and bug reports) immediately
     * know "running on KVM" vs "QEMU/TCG" vs "bare metal" without
     * needing DEBUG=on. */
    kprintf("[CPU] Environment: %s%s%s\n",
            hv_vendor_name(),
            g_hypervisor.tsc_khz ? ", hv.tsc=" : "",
            "");
    if (g_hypervisor.tsc_khz) {
        kprintf("[CPU] Hypervisor CPUID.40000010h reports TSC=%u kHz, APIC bus=%u kHz\n",
                g_hypervisor.tsc_khz, g_hypervisor.apic_bus_khz);
    }

    debug_printf("[INIT] FPU/SSE/AVX...\n");
    enable_fpu();

    debug_printf("[INIT] GDT...\n");
    gdt_init();

    debug_printf("[INIT] TSS...\n");
    tss_init();

    debug_printf("[INIT] IDT...\n");
    idt_init();

    debug_printf("[INIT] Notify...\n");
    notify_init();

    debug_printf("[INIT] PIC (early init for boot)...\n");
    pic_init();

    debug_printf("[INIT] Boot Info...\n");
    boot_info_t *bi = boot_info_get();
    if (!boot_info_valid(bi))
    {
        panic("Boot info invalid! magic=0x%x version=%u\n", bi->magic, bi->version);
    }
    kprintf("[BOOT] Boot info v%u OK (drive=0x%x, kernel 0x%x-0x%x, pt=0x%x, stack=0x%x)\n",
            bi->version, bi->boot_drive, bi->kernel_start, bi->kernel_end,
            bi->page_table_base, bi->stack_base);

    debug_printf("[INIT] E820 Memory Map...\n");
    e820_entry_t *e820_map = (e820_entry_t *)(uintptr_t)bi->e820_map_addr;
    uint16_t e820_count = bi->e820_count;

    if (e820_count == 0)
    {
        panic("[PANIC] E820: No memory map entries\n");
    }

    debug_printf("[E820] Entries at 0x%x, count = %u\n", bi->e820_map_addr, e820_count);
    e820_set_entries(e820_map, (size_t)e820_count);

    debug_printf("[INIT] PMM...\n");
    error_t pmm_err = pmm_init();
    if (pmm_err != OK)
    {
        panic("[PANIC] PMM init failed: %s\n", ErrorString(pmm_err));
    }

    debug_printf("[INIT] Kernel Heap...\n");
    mem_init();

    debug_printf("[INIT] VMM...\n");
    vmm_init();

    /* vmm_init() removes the identity mapping and activates the pull map.
     * The earlier bi pointer was the identity address (0x9000) and is now
     * invalid.  Re-fetch via pull map so all further bi accesses are safe. */
    bi = boot_info_get();

    /* The UEFI loader's framebuffer becomes the glass. After vmm_init(), so
     * the kernel can map it. */
    if (bi->version >= BOOT_INFO_VERSION2 && bi->fb_addr)
        ScreenAdoptFramebuffer(bi->fb_addr, bi->fb_width, bi->fb_height,
                               bi->fb_stride, bi->fb_format);

    /* Said once, because a box on the screen is the first thing the owner
     * will ask about standing in front of the board — and because for three
     * years the same em dash came out of this kernel as three empty cells and
     * out of userspace as a '?'. */
    kprintf("[SCREEN] one cell of this glass holds one CP437 glyph, so UTF-8 is folded "
            "at the font and nowhere earlier — the log ring still keeps every byte it "
            "was handed, and a character this font cannot draw appears as one solid box "
            "rather than two or three wrong glyphs\n");

    /* The loader's account of its own journey — which volume this kernel was
     * read out of, off what medium, by which loader. Read here because the
     * block sits in memory the loaders own, below the first megabyte, and
     * because the Boardroom will want the answer long before there is a
     * filesystem to keep it in. */
    BoardingPassInit();

    debug_printf("[INIT] MemTag...\n");
    error_t memtag_err = MemTagInit();
    if (memtag_err != OK) {
        debug_printf("[INIT] WARNING: MemTag init failed: %s (non-fatal)\n", ErrorString(memtag_err));
    }

    /* MemTag Phase 2D M5 — verify PTE bits 52-58 are safely "Ignored"
     * on the BSP. APs run the same probe from per_core_init_ap after
     * cpu_intersect_features_ap so a hybrid CPU where the AP exposes
     * different CR4 state can still be caught. */
    (void)MemTagVerifyPteMetadataBits();

    /* MemTag Phase 2E — BSP-side PAT MSR self-test (sanity: BSP RDMSR
     * should match the value vmm_pat_init just programmed). APs run
     * the same probe from per_core_init_ap after their own vmm_pat_init
     * to catch heterogeneous PAT across coherent CPUs (Intel SDM Vol 3A
     * §11.12.4). MTRR audit dumps the firmware-programmed MTRR layout
     * so cache:wb tags can be reconciled with effective memory type
     * (Intel SDM §11.12.5). Both are non-fatal — informational. */
    (void)MemTagVerifyPatMsr();
    MemTagDumpMtrrLayout();

    /* Phase 2F — Machine Check Architecture bring-up on BSP. Enables
     * CR4.MCE, programs every reporting bank's IA32_MC<i>_CTL, clears
     * stale status, and (when supported) lights LMCE. Per-AP init runs
     * later from per_core_init_ap. The IDT vector 18 handler is already
     * registered with IST_MACHINE_CHECK by idt_init; mce_handle just
     * decodes banks + poisons phys pages now that the path is hot. */
    mce_init();

    /* Phase 2H — Protection Keys (PKU/PKS) bring-up on BSP. Sets CR4.PKE
     * (bit 22), CR4.PKS (bit 24) when supported, and XCR0.PKRU (bit 9)
     * so XSAVE covers per-thread PKRU. Default PKRU=0 = "all 16 keys
     * fully accessible" → backward-compatible until userspace writes
     * PKRU. PTE bits 62:59 (the PKEY field) can now be stamped by
     * MemTag policy. */
    vmm_pku_init();

    /* Phase 2I — Linear Address Masking BSP probe. Observe-only this
     * iteration: logs CR3.LAM_U48 / CR3.LAM_U57 / CR4.LAM_SUP state so
     * operators see the substrate snapshot at boot. Per-process LAM
     * opt-in (setting CR3.LAM_U48 on a specific process) is deferred
     * because it requires scheduler-side CR3 build modification —
     * Phase 2I lays the bit-layer foundation. */
    vmm_lam_probe();

    /* Phase 2J — TME / TME-MK BSP probe (observe-only). Reads firmware-
     * locked MSRs to log the encryption state: TME on/off, MK enable,
     * KeyID bit count. */
    vmm_tme_probe();

    /* TME pool init — runtime KeyID allocator on top of the probed
     * MSR state. Programs every usable KeyID with SET_KEY_RANDOM so
     * the per-region encryption pool is "warm" by the time any
     * consumer (encrypted Bay etc.) asks. No-op when TME-MK is
     * inactive (firmware didn't activate it, or hardware lacks it). */
    {
        extern error_t tme_init_bsp(void);
        (void)tme_init_bsp();
    }

    /* Phase 2K — CET (Control-flow Enforcement Technology) BSP probe.
     * Detects SHSTK + IBT, reads IA32_S_CET / IA32_U_CET when CR4.CET=1.
     * Observe-only — per-process shadow-stack allocation + SSP save/
     * restore via XSAVE component 11 is the lifecycle follow-up. */
    vmm_cet_probe();

    pmm_test_high_memory();
    MemTagStressTest();

    /* Subsystem-specific tests (split out of memtag_test.c in the
     * clean-slate audit so each test lives next to the code it covers). */
    extern void VmmHelperTest(void);
    extern void PmmPoisonTest(void);
    extern void McePresenceTest(void);
    extern void IommuPresenceTest(void);
    extern void TmeRunTests(int *out_pass, int *out_fail);
    extern void AddrWaitSelfTest(void);
    extern void XhciRingSelfTest(void);
    VmmHelperTest();
    PmmPoisonTest();
    McePresenceTest();
    /* IommuPresenceTest is NOT here. It used to be, seventy lines and several
     * subsystems ahead of iommu_init(), where the only answer it could give
     * was "dormant" — and it gave it as "no DMAR/IVRS", which is a statement
     * about the firmware's tables made before those tables had been read. On
     * the first real machine BoxOS booted, that line said the board had no
     * IOMMU while the board's DMAR was sitting in its RSDT, and the VT-d code
     * that later found it crashed. A presence test that runs before the thing
     * is present tests nothing and misleads twice. It now runs after
     * iommu_init(); see below. */
    { int p = 0, f = 0; TmeRunTests(&p, &f); (void)p; (void)f; }
    AddrWaitSelfTest();
    XhciRingSelfTest();
    slab_identity_selftest();
    KHeapLiveSelfTest();
    KHeapReallocSelfTest();

    debug_printf("[INIT] TSS Dynamic Stacks...\n");
    tss_setup_dynamic_stacks();

    debug_printf("[INIT] CPU Capabilities Page...\n");
    cpu_caps_page_init();

    /* EFI runtime services bring-up — must precede ACPI so the kernel can
     * fall back to EFI ResetSystem when the FADT lacks a reset register
     * (e.g. ACPI 1.0b firmware, legacy Bochs). Idempotent + non-fatal:
     * BIOS boots silently return false. Real-HW: maps every
     * EFI_MEMORY_RUNTIME descriptor at EFI_RT_VA_BASE+phys and calls
     * SetVirtualAddressMap (UEFI 2.10 §8.4) so subsequent RT calls
     * dispatch via virtual addresses. */
    /* Crypto + ASN.1 + RSA + streaming-SHA-256 self-test BEFORE we
     * start trusting any of those primitives for downstream policy
     * decisions. A regression in any one of them is fatal to Secure
     * Boot guarantees; surfacing it at boot beats discovering it
     * during a customer's first signed-image load. */
    debug_printf("[INIT] EFI crypto self-test...\n");
    if (!efi_selftest_run()) {
        kprintf("[BOOT] EFI self-test FAILED — Secure Boot disabled\n");
        /* Non-fatal: BoxOS still boots, but Authenticode verification
         * is unreliable. Production deployments should treat the boot
         * log as a hard fail. */
    }

    debug_printf("[INIT] EFI runtime services...\n");
    if (efi_runtime_init()) {
        efi_runtime_print_info();

        /* EFI System Resource Table — must come after efi_runtime_init
         * because efi_esrt_init reads boot_info's preserved ESRT copy
         * (allocated in EfiACPIMemoryNVS by TagBoot). Independent of
         * SVAM success: ESRT is a plain in-memory copy. Touch publish
         * matches the acpi_init pattern: emit at boot — late subscribers
         * can query via efi_esrt_get(). */
        efi_esrt_init();
        /* Printed whether or not it came up. A boot log with no [ESRT] line
         * cannot be told apart from a kernel whose ESRT reader never ran,
         * and on a UEFI machine "the firmware published no ESRT" is itself
         * one of the answers worth having. */
        efi_esrt_print();

        /* Secure Boot consumer — reads SecureBoot/SetupMode/PK/KEK/db/dbx
         * via Variable Services. Safe to call even when firmware reports
         * SB=0; populates state only and skips databases that are empty.
         *
         * NOTE: Touch publishing is deferred to after guide_init below
         * because TouchInit happens inside guide_init — publishing here
         * would land before the tag registry is ready, get dropped at
         * resolve_tag_pair, and userspace subscribers would never see
         * the boot-time state events. The kernel-internal state is
         * available immediately via efi_secureboot_get_state() /
         * efi_esrt_get() so the deferral is purely for Touch consumers. */
        efi_secureboot_init();
        /* Printed on both outcomes, for the same reason as ESRT above and
         * with more at stake: whether this machine authenticates what it
         * boots is the first fact anyone wants from a UEFI board, and a
         * refusal to serve Secure Boot state has to be as loud as a state. */
        efi_secureboot_print();
    } else {
        debug_printf("[INIT] EFI runtime services not available\n");
    }

    // ACPI must init early so irqchip_init can parse MADT for APIC detection
    debug_printf("[INIT] ACPI Subsystem (early)...\n");
    acpi_error_t acpi_err = acpi_init();
    if (acpi_err == ACPI_OK)
    {
        debug_printf("[INIT] ACPI initialized successfully\n");

        /* The console, asked of the machine instead of assumed.
         *
         * serial_init() ran at the top of this function, before a single
         * ACPI table existed, so the only address it could try was the
         * PC/AT one — and on a board with no Super-I/O nothing decodes it.
         * The firmware states where its own debug port is in DBG2 (and in
         * the older DBGP), and this is the first line of the boot at which
         * those tables can be read. It must also stay ahead of
         * serial_console_init() further down, which wires IRQ4 and needs to
         * know by then whether the line it is wiring is the legacy port.
         *
         * When the port moves, everything said so far goes out on the new
         * line at once — which is the point: a board whose console only
         * exists from here on would otherwise lose its whole early boot. */
        WireFollowTheFirmware();

        /* Surface any pre-boot hardware error captured by firmware. */
        acpi_apei_consume();
        /* Log NUMA topology now that both PMM and ACPI/SRAT are ready. */
        pmm_log_numa_topology();
        /* IOMMU — picks backend, brings the units up, does not enable
         * translation. It says nothing on Touch here and reads nothing out
         * of the units here: TouchInit is inside guide_init, still hundreds
         * of lines away, and a publish made now would be dropped at
         * resolve_tag_pair exactly as the Secure Boot note above describes.
         * Both happen in iommu_publish_touch, after guide_init. */
        iommu_init();
        IommuPresenceTest();  /* after init, which is the only time it means anything */
        iommu_audit_dump();   /* Phase 2G — log MemTag/Touch surface */
        /* AML interpreter skeleton — currently returns NOT_LOADED.
         * Hook here lets future implementation tie into boot. */
        aml_init();
    }
    else
    {
        debug_printf("[INIT] ACPI initialization failed (error %d), shutdown may use fallback methods\n", acpi_err);
    }

    // Detect and initialize interrupt controller (APIC or PIC fallback)
    debug_printf("[INIT] Interrupt Controller...\n");
    irqchip_init();

    /* The corrected-error interrupt, now that this core's local APIC is
     * software-enabled. mce_init armed the banks back at line ~299 and could
     * not arm the LVT: an unmask written to a software-disabled APIC is
     * ignored (Intel SDM Vol 3A, "Local APIC State After It Has Been Software
     * Disabled"). Without this line CMCI_P would be printed and nothing would
     * ever deliver one — which is the defect this session closed. */
    MceCmciArmLocal();

    /* Register the ACPI SCI handler now that IOAPIC routing is live. */
    if (acpi_err == ACPI_OK) acpi_sci_register();

    debug_printf("[INIT] AMP Core Detection...\n");
    amp_init();

    /* The batons, before the sti that opens the machine to interrupts, on
     * every core count: a park deadline is passed on one from the very first
     * timed wait, a keystroke knocks on one, and a one-core box has no K-Core
     * loop to make up for a queue that is not there. */
    debug_printf("[INIT] Baton queues (never-drop, per core)...\n");
    BatonInit();

    debug_printf("[INIT] Per-core GDT/TSS/Notify (BSP)...\n");
    per_core_init_bsp();

    /* ClockBoard MUST be initialised before pit_init so the IRQ handler
     * always finds a valid backing page. Late-boot setters (TSC freq,
     * RTC unix-secs) are called after their respective inits below. */
    debug_printf("[INIT] ClockBoard...\n");
    clockboard_init();

    /* HPET — high-precision timer.
     *
     * Two roles:
     *   1. Free-running 64-bit main counter — always read-available
     *      after hpet_init(), used by cpu_calibrate_tsc for the
     *      measurement window (independent of IRQ0 routing) and by
     *      pit_get_uptime_us() as the monotonic time source.
     *   2. Optionally takes over IRQ0 via LegacyReplacement. This is
     *      production-correct for modern hardware (many post-2018
     *      server boards ship without an 8254 PIT). Safe to enable
     *      now because cpu_calibrate_tsc no longer depends on PIT
     *      channel-0 — see commit history of cpu_calibrate.c.
     *
     * The earlier 2026-05-19 regression (TSC-calib mis-read +
     * bench corruption) was caused by the PIT-only calibration path
     * being silently broken once HPET stole IRQ0. With the new
     * CPUID.15h → HPET-counter → PIT preference order, that path
     * is unreachable. */
    debug_printf("[INIT] HPET...\n");
    if (hpet_init()) {
        if (hpet_start_legacy_tick(CONFIG_SCHED_DEFAULT_TICK_HZ)) {
            debug_printf("[INIT] HPET sourcing IRQ0 system tick @ %u Hz\n",
                         (unsigned)CONFIG_SCHED_DEFAULT_TICK_HZ);
        } else {
            debug_printf("[INIT] HPET counter available; PIT keeps IRQ0\n");
        }
    } else {
        debug_printf("[INIT] HPET unavailable — TSC/PIT only\n");
    }

    debug_printf("[INIT] PIT...\n");
    /* PIT and HPET legacy-replacement run at the same rate to keep the
     * scheduler tick math (idt.c uses SCHEDULER_DEFAULT_TICK_HZ
     * directly) in sync regardless of who owns IRQ0. */
    pit_init(CONFIG_SCHED_DEFAULT_TICK_HZ);

    debug_printf("[INIT] RTC...\n");
    rtc_init();
    clockboard_set_boot_unix_secs(rtc_get_unix64());

    /* Para-virtual clocksources — must come AFTER pmm/vmm (they need
     * a backing page and a phys→virt mapping) and BEFORE cpu_calibrate
     * (which prefers pvclock_tsc_khz() as its highest-trust input).
     * Each call no-ops on hosts that don't advertise the feature. */
    debug_printf("[INIT] kvmclock (pvclock)...\n");
    if (pvclock_init()) {
        debug_printf("[INIT] kvmclock active — TSC calib will use pvclock_tsc_khz\n");
    }

    debug_printf("[INIT] Hyper-V reference TSC page (hvclock)...\n");
    if (hvclock_init()) {
        debug_printf("[INIT] hvclock active — Hyper-V reference TSC available\n");
    }

    debug_printf("[INIT] CPU Calibration...\n");
#ifdef CONFIG_EARLY_INTERRUPTS_PROOF
    /* `make EARLYIRQ=on` — the board's window, exactly, and no wider.
     *
     * An i5-9400F booting UEFI took its first HPET tick right here, inside
     * TSC calibration, thirty-six lines before scheduler_init() had allocated
     * anything for irq_handler to count it into. The machine died writing
     * through that NULL: #PF at 0x18.
     *
     * The flag is opened for this call and closed again after it, which is
     * the board's failure and not a broader one: leaving interrupts on for
     * the rest of init is a DIFFERENT state, and a worse one — measured, the
     * kernel deadlocks later in TouchLogbookResolve, because early init is
     * not written to be re-entered from an interrupt. That is precisely why
     * the real fix is that this kernel owns RFLAGS.IF from _start and takes
     * it back after every firmware call, rather than merely surviving one
     * tick. See the note in the Makefile. */
    kprintf("[BOOT] EARLYIRQ: opening the interrupt flag for the length of "
            "TSC calibration, which is where the board took its first tick\n");
    asm volatile("sti");
    cpu_calibrate_tsc();
    asm volatile("cli");
#else
    cpu_calibrate_tsc();
#endif
    clockboard_set_tsc_freq_khz(cpu_get_tsc_freq_khz());

    /* IA32_UMWAIT_CONTROL on the BSP — Intel SDM Vol 4 §2.5.1.
     * Programs the OS-imposed UMWAIT/TPAUSE residency cap so wait
     * loops re-poll within a bounded interval even if a monitor wake
     * is dropped by silicon/microcode quirks. WAITPKG-gated inside;
     * no-op on AMD or pre-Tremont Intel. APs program their own copy
     * inside per_core_init_ap (each MSR is per-logical-processor;
     * see SDM Vol 4 Table 2-2 "Scope: Thread"). */
    cpu_umwait_control_init(cpu_get_tsc_freq_khz());

    /* TEST_CTL bit 29 — Intel SDM Vol 4 Table 2-2. Clear split-lock-#AC
     * enable on the BSP. Gated on g_cpu_caps.has_split_lock_detect so
     * we never write the MSR on a CPU that lacks IA32_CORE_CAPABILITIES
     * (would #GP). APs do the same in per_core_init_ap. Rationale lives
     * in cpuid.h cpu_test_ctl_init declaration. */
    cpu_test_ctl_init();

    /* Capture the BSP TSC anchor for per-AP TSC sync. Must be after
     * cpu_calibrate_tsc (we need tsc_freq_khz published) and before
     * amp_boot_aps (each AP reads the anchor in per_core_init_ap).
     * Uses the current pit_uptime_us as the wall-clock pinpoint. */
    per_core_record_bsp_tsc_anchor(pit_get_uptime_us());

    // Initialize idle process (PID 0) before process system
    kprintf("[INIT] Idle Process...\n");
    idle_process_init();

    debug_printf("[INIT] ASLR...\n");
    aslr_init();

    debug_printf("[INIT] Process Management...\n");
    process_init();

    debug_printf("[INIT] Scheduler...\n");
    error_t sched_err = scheduler_init();
    if (sched_err != OK)
    {
        panic("[PANIC] Scheduler init failed: %s\n", ErrorString(sched_err));
    }

    /* From here this kernel can be interrupted safely.
     *
     * The anchor is scheduler_init and not the sti at the bottom of
     * kernel_main, because the sti is not the moment interrupts become legal
     * — scheduling the first process turns them on earlier and legitimately,
     * every user context runs with RFLAGS.IF set. Measured: with the flag on
     * the sti, an ordinary boot reported a perfectly normal device interrupt
     * arriving "early", forty lines after userspace started. The question the
     * board actually raised is narrower and this is where it is answered:
     * an interrupt before THIS line finds irq_handler with no scheduler state
     * to record it into, which is what killed an i5-9400F at #PF 0x18. */
    { extern volatile bool g_kernel_ready_for_interrupts;
      g_kernel_ready_for_interrupts = true; }

    debug_printf("[INIT] Guide Dispatcher...\n");
    guide_init();

    /* The kernel's own ear, exercised here rather than later on purpose: no
     * process exists yet, and listening before there is any userspace to
     * listen for you is the entire reason TouchWatch exists. */
    { extern void TouchWatchSelfTest(void); TouchWatchSelfTest(); }

    /* MemTag's lifecycle publishes (memtag:region:*, memtag:tag:*) become
     * live once Touch resolves tags. Until now they silently no-op'd. */
    MemTagEnableTouchPublish();

    /* MCE page migration — Phase 2F shipped #MC bank decode + poison.
     * mce_migrate adds: after poison, defer a worker that copies the
     * affected page to a fresh phys + atomically swaps PTEs in every
     * cabin that mapped the poisoned page. Initialization is gated on
     * (a) MemTag being up so MemRegion reverse-map is queryable, and
     * (b) the batons being up so the slot's pass can be made from #MC
     * IST. Touch resolution happens here so the worker's publish path
     * uses cached handles. */
    {
        extern void mce_migrate_init(void);
        mce_migrate_init();
        /* Kernel-side test: exercises the full migrate path
         * synchronously (no baton hop) so we can observe side
         * effects on real vmm_context + MemRegion attaches.
         * Real-HW MCE injection (APEI EINJ) is the integration test;
         * this validates correctness without real silicon. */
        extern void McMigrationTest(void);
        McMigrationTest();
        /* Phase 2H+ — tag-driven PKU PTE stamping. Verifies
         * MemTagApply(rid, "pku:N") auto-stamps PTE bits 62:59 inside
         * MemRegionAttachCabin + MemTagApplyPkey sweep on tag change.
         * Userspace WRPKRU itself is unprivileged and tested via
         * boxlib box/pku.h. */
        extern void PkuStampTest(void);
        PkuStampTest();

        /* APEI/GHES runtime path — bridges firmware-delivered hardware
         * errors (Memory ECC via SMI → GHES, PCIe AER via GHES, etc.)
         * into the same mce_migrate + Touch pipeline that handles
         * architectural #MC events. Per-source registration ran during
         * acpi_parse_apei via the decode_ghes hook; this call only
         * resolves Touch tags + arms the periodic poll. Safe to skip
         * (becomes a no-op) when no GHES sources were discovered. */
        extern void apei_ghes_runtime_init(void);
        apei_ghes_runtime_init();
        extern void ApeiGhesTest(void);
        ApeiGhesTest();

        /* The SCI line opens only now: its handler says events under names
         * that exist since guide_init, and consults the APEI runtime that
         * exists since the line above. A level-triggered SCI raised before
         * this has been waiting in the IOAPIC. */
        if (acpi_err == ACPI_OK) acpi_sci_arm();

        /* Sort the .uaccess_fixup table so the #PF handler can
         * binary-search instead of linear-scan it. Must run before any
         * user process spawns + before interrupts are unmasked at the
         * LAPIC — both conditions hold at this point in main(). The
         * call is idempotent (re-entry no-ops) and the lookup path
         * falls back to linear scan if init didn't run, so no boot
         * ordering dependency is fatal. */
        extern void uaccess_init(void);
        uaccess_init();

        /* Phase 2K+ — CET shadow-stack + IBT lifecycle enable.
         * Programs CR4.CET=1, IA32_S_CET/IA32_U_CET MSRs, and
         * registers XSAVE components 11/12 so per-process SSP is
         * saved + restored on context switch. Per-process SSP
         * allocation is wired into process_create from inside this
         * call's after-effects (cet_process_create gates on
         * g_cpu_caps.has_shstk + CR4.CET=1). Safe to call on CPUs
         * without CET (becomes a no-op + returns ERR_NOT_SUPPORTED). */
        extern int cet_lifecycle_init_bsp(void);
        (void)cet_lifecycle_init_bsp();

        /* Per-CPU supervisor SSP infrastructure for the BSP. Allocates
         * PL0_SSP page + IA32_INTERRUPT_SSP_TABLE_ADDR + 5 per-IST SSP
         * pages, writes the supervisor tokens, programs the MSRs. The
         * S_CET.SH_STK_EN bit stays 0 — flipping it requires a kernel-
         * wide CALL/RET pair audit that's a follow-up. The infrastructure
         * is dormant until then, but the foundation is in place: a single
         * IA32_S_CET write activates supervisor SHSTK at that point. */
        extern error_t cet_lifecycle_init_supervisor_ssp(uint8_t);
        (void)cet_lifecycle_init_supervisor_ssp(amp_get_core_index());

        extern void CetLifecycleTest(void);
        CetLifecycleTest();
    }

    /* TouchInit has now run inside guide_init — replay every EFI boot-
     * time event so late subscribers (userspace daemons, fleet inventory
     * tools) actually observe the state instead of losing it to the pre-
     * TouchInit resolve_tag_pair no-op. Re-publishing is idempotent: the
     * payload identifies the source state snapshot. */
    if (efi_runtime_available()) {
        if (efi_esrt_available())       efi_esrt_publish_touch();
        if (efi_secureboot_available()) efi_secureboot_publish_touch();
    }

    /* The IOMMU is in the same position and for the same reason, with one
     * difference: it is not only a replay. iommu_init above chose the
     * backend and brought the units up but said nothing and read nothing,
     * because a publish before TouchInit reaches no subscriber. This is
     * where `iommu:ready` names the backend and where every unit's fault
     * channel is read, said and cleared -- what firmware left standing in
     * it, and on VT-d a gag on everything the unit would otherwise report
     * later (§11.4.7.1 PFO, §7.3). It runs once: the handover from firmware
     * is the fact it answers to, translation is still off, and nothing can
     * put a new record in that channel for a periodic look to find.
     *
     * Harmless on a machine with no DMAR and no IVRS, and it cannot refuse
     * a boot. */
    iommu_publish_touch();

    debug_printf("[INIT] OpRegistry...\n");
    error_t op_reg_err = OpRegistryInit();
    if (op_reg_err != OK)
    {
        panic("[PANIC] OpRegistry init failed: %s\n", ErrorString(op_reg_err));
    }

    debug_printf("[INIT] Manifest subsystem...\n");
    error_t manifest_err = ManifestSubsystemInit();
    if (manifest_err != OK)
    {
        panic("[PANIC] Manifest subsystem init failed: %s\n", ErrorString(manifest_err));
    }

    debug_printf("[INIT] Manifest self-test...\n");
    error_t selftest_err = ManifestSelfTest();
    if (selftest_err != OK)
    {
        kprintf("[WARN] Manifest self-test failed: %s\n", ErrorString(selftest_err));
    }

    debug_printf("[INIT] crate_io straddle self-test...\n");
    error_t crate_io_err = CrateIoSelfTest();
    if (crate_io_err != OK)
    {
        kprintf("[WARN] crate_io self-test failed: %s\n", ErrorString(crate_io_err));
    }

    debug_printf("[INIT] auth-decouple self-test...\n");
    error_t authdec_err = AuthDecoupleSelfTest();
    if (authdec_err != OK)
    {
        kprintf("[WARN] auth-decouple self-test failed: %s\n", ErrorString(authdec_err));
    }

    debug_printf("[INIT] proc-authority self-test...\n");
    error_t procauth_err = ProcAuthSelfTest();
    if (procauth_err != OK)
    {
        kprintf("[WARN] proc-authority self-test failed: %s\n", ErrorString(procauth_err));
    }

    debug_printf("[INIT] Baton never-drop self-test...\n");
    error_t baton_err = BatonSelfTest();
    if (baton_err != OK)
    {
        kprintf("[WARN] Baton self-test failed: %s\n", ErrorString(baton_err));
    }

    debug_printf("[INIT] Operations Deck register...\n");
    error_t ops_reg_err = OperationsDeckRegister();
    if (ops_reg_err != OK)
    {
        kprintf("[WARN] Operations Deck register failed: %s\n", ErrorString(ops_reg_err));
    }

    debug_printf("[INIT] Hardware Deck register...\n");
    error_t hw_reg_err = HardwareDeckRegister();
    if (hw_reg_err != OK)
    {
        kprintf("[WARN] Hardware Deck register failed: %s\n", ErrorString(hw_reg_err));
    }

    debug_printf("[INIT] System Deck register...\n");
    error_t sys_reg_err = SystemDeckRegister();
    if (sys_reg_err != OK)
    {
        kprintf("[WARN] System Deck register failed: %s\n", ErrorString(sys_reg_err));
    }

    /* Storage Deck registers AFTER storage_deck_init below (TagFS must be up). */

    if (g_amp.total_cores > 1)
    {
        debug_printf("[INIT] K-Core Queues...\n");
        kcore_init();

        debug_printf("[INIT] Syscall Mode: ASYNC...\n");
        idt_set_syscall_mode(true);

        debug_printf("[INIT] Booting Application Processors...\n");
        amp_boot_aps();

        /* Nightwatch last: it snapshots how many cores actually answered, and
         * a core the firmware promised but never delivered must not count. */
        nightwatch_init();
    }
    else
    {
        idt_set_syscall_mode(false);

        /* And the watch stands on one core too, now that one core can sleep.
         *
         * It used to be armed only for SMP, and the reason was sound at the
         * time: the verdict runs from cpu_idle, and a uniprocessor never
         * reached cpu_idle — the strand waiting for a keystroke held the only
         * core, spinning, so there was never an idle core to look from. Turn In
         * ended that. A single-core box now genuinely goes to sleep at its
         * prompt, which is exactly the moment the watch exists for, and leaving
         * it unarmed here would leave the newest way to sleep unwatched on the
         * configuration where it is easiest to see. */
        nightwatch_init();
    }

    debug_printf("[INIT] PCI Subsystem...\n");
    pci_init();

    /* The IOMMU takes the wheel HERE, and the position is the whole design.
     *
     * One line earlier there is no complete account of what is on the buses:
     * the walk that gives every present function a context entry (VT-d) or a
     * device-table entry (AMD-Vi) needs the ECAM windows pci_init has just
     * mapped, and a function with no entry either loses its DMA outright or
     * stays quietly outside the protection everything else just got.
     *
     * One line later is too late in the other direction: ahci_init, ata_init
     * and xhci_init all follow, and each of them starts real transfers. A
     * controller whose DMA is in flight when the tables under it change is
     * exactly the failure this ordering exists to prevent.
     *
     * All-or-nothing, and it never refuses a boot: on anything that does not
     * add up it leaves the machine as untranslated as it found it and says
     * through kprintf which thing it was. `make IOMMU=off` builds a kernel
     * where it declines on purpose. */
    iommu_take_the_wheel();

#ifdef CONFIG_AHCI_DRIVER
    debug_printf("[INIT] AHCI Driver...\n");
    if (ahci_init() == 0)
    {
        debug_printf("[INIT] AHCI initialized successfully\n");

        debug_printf("[INIT] AHCI IRQ setup...\n");
        ahci_init_irq();
        uint32_t ahci_active = ahci_get_active_port_mask();
        for (uint8_t p = 0; p < 32 && ahci_active; p++)
        {
            if (ahci_active & (1U << p))
            {
                ahci_port_enable_irq(p);
                ahci_active &= ~(1U << p);
            }
        }
        debug_printf("[INIT] AHCI NCQ enabled (%u port(s))\n", ahci_get_active_port_count());
    }
    else
    {
        debug_printf("[INIT] AHCI not available, using legacy ATA\n");
    }
#endif

    debug_printf("[INIT] ATA Driver...\n");
    ata_init();

    /* USB before storage, because a machine that boots from a flash drive has
     * its filesystem on the USB bus and cannot be asked to find it before the
     * bus has been asked what is on it. */
    debug_printf("[INIT] USB xHCI Driver...\n");
    if (xhci_init() != 0)
    {
        debug_printf("[INIT] xHCI controller not found or initialization failed\n");
    }

    debug_printf("[INIT] Boardroom (media)...\n");
    BoardroomInit();

    debug_printf("[INIT] Storage Deck & TagFS...\n");
    storage_deck_init();

    /* And whether the medium the volume landed on can be read with nobody
     * standing over the transfer — asked here, where the volume is known and
     * the answer is still cheap to act on. */
    BoardroomProveUnattendedRead(tagfs_get_seat());

    /* And what every medium's own ground says it carries. Reads only — and not
     * the ground the volume above is standing on, which has just been read,
     * checked and described by the mount that took it up. */
    DeedSurveyAll(tagfs_get_seat(), tagfs_get_volume_base());

    debug_printf("[INIT] Storage Deck register (Manifest path)...\n");
    error_t storage_reg_err = StorageDeckRegister();
    if (storage_reg_err != OK)
    {
        kprintf("[WARN] Storage Deck register failed: %s\n", ErrorString(storage_reg_err));
    }

    /* Now that the tag registry exists, resolve the USB Touch tags for real.
     *
     * They could not be resolved when the controller came up: tags live in
     * TagFS, and TagFS lives on a medium the controller had not yet been asked
     * about. Devices attached at power-on therefore enumerate before anyone can
     * subscribe to hearing about them — which is why every one of them is also
     * printed. Hot-plug from here on is announced properly. */
    xhci_interrupt_touch_init();

    /* The keyboard comes up AFTER the tag registry, and it has to.
     *
     * It resolves the "keyboard" tag once at init and caches the handle,
     * because resolving a tag takes the registry lock and the PS/2 interrupt
     * cannot. A tag resolved before the registry exists comes back invalid and
     * stays invalid — every keystroke is then published to nothing, and the
     * shell sits there receiving no input from a keyboard that is working
     * perfectly. Moving this above TagFS to make room for USB did exactly
     * that, on every boot path, with nothing anywhere saying why. */
    debug_printf("[INIT] Keyboard...\n");
    keyboard_init();

    /* COM1 serial console: route inbound serial bytes into the keyboard input
     * ring so a host-side console (headless QEMU/Bochs, or a real serial line)
     * can drive the shell. Must follow keyboard_init (fills the same ring). */
    debug_printf("[INIT] Serial console (COM1 RX)...\n");
    serial_console_init();

    /* Every controller on this machine has now come up and moved its first
     * bytes, and every one of those addresses was walked by the tables
     * iommu_take_the_wheel switched on above. This is the line at which the
     * units can be asked what that cost.
     *
     * It is the only read of the fault channel that can see a fault a DRIVER
     * caused: a hole in the identity map — an address above what it covers,
     * an RMRR the DMAR named and the map left out — is recorded in the units'
     * Fault Recording registers and nowhere else, and the two earlier reads
     * both happen before any driver has started a transfer. Without this one
     * the record stays in hardware, unsaid, and the machine looks alive. */
    iommu_sweep_faults();

    kprintf("Kernel initialization complete!\n");
    kprintf("\n");

#ifdef CONFIG_PANIC_STOP_PROOF /* in place of the startup tests; never returns */
    { extern __attribute__((noreturn)) void PanicStopProof(void); PanicStopProof(); }
#endif

#if CONFIG_RUN_STARTUP_TESTS
    kprintf("\n[TESTS] Running startup tests...\n");
    kprintf("========================================\n");
    TagFSState* fs = tagfs_get_state();
    kprintf("[TESTS] TagFS state: %s\n", fs ? (fs->initialized ? "initialized" : "NOT initialized") : "NULL");
    error_t test_result = TagFS_RunTests();
    kprintf("[TESTS] TagFS_RunTests returned: %d\n", test_result);
    if (test_result == OK) {
        kprintf("[TESTS] All tests PASSED\n");
    } else {
        kprintf("[TESTS] Some tests FAILED (error=%d: %s)\n", test_result, ErrorString(test_result));
    }
    kprintf("========================================\n\n");
#endif

    /* Ф26 BMIDE watchdog TIER-1 proof: mask a channel's IOAPIC pin so a real
     * disk completion latches BMISR.IRQ with no CPU IRQ, then verify
     * bmide_watchdog_scan recovers the "lost interrupt". Everything it needs is
     * up by here (multi-core, BMIDE); skips cleanly on single-core
     * or when AHCI owns block I/O. */
    {
        extern error_t bmide_watchdog_selftest(void);
        (void)bmide_watchdog_selftest();
    }

#if CONFIG_BMIDE_WEDGE_SELFTEST
    /* On-demand TIER-2 proof (WEDGETEST=on): drives a synthetic wedge through
     * the real scan -> K-Core SRST recovery. SRSTs the boot drive — never in a
     * production build. */
    {
        extern error_t bmide_wedge_selftest(void);
        (void)bmide_wedge_selftest();
    }
#endif

#if CONFIG_START_USERSPACE
    kprintf("Starting userspace...\n");
    kprintf("\n");

    /* The system's start set — display.box, hub.box, autostart.box — by name.
     * Extracted from here into autostart.c, because
     * it was a hundred and ninety lines executed once at whatever moment this
     * line happened to be reached — and a machine whose medium finished
     * enumerating a second later came up empty and stayed that way. */
    process_t *initial_proc = NULL;
    RiseInit();
    int autostart_count = AutostartLaunchFromVolume(&initial_proc);

    /* Nothing is started in its place: with none of the start set on a volume
     * the machine waits, saying for what, on the Emergency Screen. A volume
     * that comes starts its own (AutostartWatchVolume). */
    if (!initial_proc)
    {
        if (AutostartVolumeHere())
            kprintf("[AUTOSTART] the volume holds none of the start set; a volume that arrives "
                    "later starts its own\n");
        else
            kprintf("[AUTOSTART] no volume is here yet — its programs start when it arrives\n");
    }
    else
    {
        AutostartNoteVolumeLaunched();
    }

    /* Listen from here on. Set AFTER the boot has had its go, so it can only
     * ever catch a volume that arrived too late for it — the boot mount's own
     * announcement has already been made and gone unheard, deliberately. */
    AutostartWatchVolume();

    kprintf("[AUTOSTART] %d process(es) launched\n", autostart_count);

    /* Fill the clock page before anything in Ring 3 can read it.
     *
     * From here the BSP's IRQ0 handler keeps it current, but that handler does
     * not run until this core enables interrupts, which happens inside the
     * loop entered below — and by then the machine has been up for the whole
     * boot. Without this one call the first thing every process reads off the
     * page is zero while the system deck, asked the same question, answers
     * with the true uptime. */
    clockboard_publish_uptime();

    if (g_amp.multicore_active)
    {
        // ================================================================
        // Multi-core bootstrap: BSP is a K-Core, processes run on App Cores.
        // Enqueue all WORKING processes on their home App Core RunQueues,
        // wake App Cores via IPI, then BSP enters the K-Core guide loop.
        // ================================================================
        kprintf("[KERNEL] Multi-core mode: %u K-Core(s), %u App Core(s)\n",
                g_amp.k_count, g_amp.app_count);

        // Enqueue all created processes on their home App Core RunQueues
        process_list_lock();
        process_t *p = process_get_first();
        while (p)
        {
            if (p->magic == PROCESS_MAGIC && p->state == PROC_WORKING &&
                !process_is_idle(p))
            {
                /* sched_enqueue returns error_t: 0 = OK, non-zero = failure.
                 * Earlier `if (sched_enqueue())` was backwards and printed
                 * "FAILED to enqueue" on SUCCESS — confusing every boot log. */
                if (sched_enqueue(p) == OK)
                {
                    debug_printf("[KERNEL] Enqueued PID %u on App Core %u\n",
                                 p->pid, p->home_core);
                }
                else
                {
                    debug_printf("[KERNEL] FAILED to enqueue PID %u\n", p->pid);
                }
            }
            p = p->next;
        }
        process_list_unlock();

        // Wake all App Cores to start scheduling
        for (uint8_t c = 0; c < g_amp.total_cores; c++)
        {
            if (!g_amp.cores[c].is_kcore && amp_core_online(&g_amp.cores[c]))
            {
                lapic_send_ipi(g_amp.cores[c].lapic_id, IPI_WAKE_VECTOR);
            }
        }

        kprintf("[KERNEL] BSP entering K-Core guide loop...\n");
        /* Final step on BSP boot path: flip S_CET.SH_STK_EN=1 and JMP
         * into kcore_run_loop without returning. The activation function
         * is __noreturn because the current call chain has no matching
         * pushes on the shadow stack (built up before SH_STK_EN was on),
         * so any RET past activation would #CP. Dormant on TCG / CPUs
         * without SHSTK — degrades to a direct kcore_run_loop call. */
        extern void cet_supv_shstk_activate_and_jump(void (*)(void));
        cet_supv_shstk_activate_and_jump(kcore_run_loop);
        /* unreachable */
    }

    /* ----------------------------------------------------------------
     * Single-core path: BSP runs every userspace process itself.
     *
     * BUG (audit 2026-04-30): the previous code only `process_start_initial`'d
     * the *initial* autostart process and jumped straight to Ring 3,
     * leaving every other PROC_WORKING process unrouted into the
     * scheduler's runqueue. When the BSP timer fired and `schedule()`
     * looked for the next runnable process, the runqueue was empty —
     * so the shell, the second utility, etc. never got a slice and the
     * system silently sat on the initial process forever (display in
     * the autostart case, which only does `receive_wait` and produces
     * no output of its own). User-visible symptom: nothing happens
     * after `[AUTOSTART] launched`, regardless of BIOS or UEFI.
     *
     * Fix: enqueue every WORKING non-idle process on this BSP's
     * runqueue *before* the jump, and skip enqueuing `initial_proc`
     * itself (it becomes current_process in process_start_initial).
     * ---------------------------------------------------------------- */
    kprintf("[KERNEL] Single-core mode: BSP scheduling all processes\n");

    /* No program yet: the core waits for one. The timer that schedules every
     * process here hands it the volume's programs the moment the late launch
     * lets them run (autostart.c); until then it rests. */
    if (!initial_proc)
    {
        kprintf("[KERNEL] nothing to run until a volume's programs start; the core waits\n");
        asm volatile("sti");
        for (;;)
            asm volatile("hlt");
    }

    process_list_lock();
    process_t *sp = process_get_first();
    while (sp)
    {
        if (sp != initial_proc &&
            sp->magic == PROCESS_MAGIC &&
            sp->state == PROC_WORKING &&
            !process_is_idle(sp))
        {
            if (sched_enqueue(sp) == OK)
            {
                debug_printf("[KERNEL] Enqueued PID %u on BSP\n", sp->pid);
            }
            else
            {
                debug_printf("[KERNEL] FAILED to enqueue PID %u\n", sp->pid);
            }
        }
        sp = sp->next;
    }
    process_list_unlock();

    debug_printf("[KERNEL] Starting initial process (PID %u) - jumping to Ring 3...\n",
                 initial_proc->pid);

    process_start_initial(initial_proc);

    // Should NEVER return from above call
    debug_printf("[KERNEL] ERROR: Returned from userspace! This should never happen!\n");
    panic("Returned from process_start_initial()");
#else
    kprintf("Userspace execution disabled (CONFIG_START_USERSPACE=0)\n");
    kprintf("System idle. Guide will process events on demand.\n");
    kprintf("\n");

    kprintf("Kernel loop\n");
    kprintf("\n");

    asm volatile("sti");

    uint64_t loop_count = 0;
    while (1)
    {
        guide();

        if ((loop_count % 100) == 0)
        {
            uint32_t pending = ready_queue_count(&g_ready_queue);
            if (pending > 0)
            {
                debug_printf("[MAIN] ReadyQueue has %u pending processes\n", pending);
            }
        }
        loop_count++;

        asm volatile("hlt");
    }

    debug_printf("[KERNEL] PANIC: Main loop exited\n");
    while (1)
    {
        asm volatile("cli; hlt");
    }
#endif
}
