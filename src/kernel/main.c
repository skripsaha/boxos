#include "video.h"
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
#include "guide.h"
#include "scheduler.h"
#include "keyboard.h"
#include "ata.h"
#include "async_io.h"
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
#include "linker_symbols.h"
#include "memtag.h"
#include "mce.h"
#include "op_registry.h"
#include "manifest.h"
#include "manifest_selftest.h"
#include "crate_io_selftest.h"
#include "auth_decouple_selftest.h"
#include "slab.h"
#include "operations_deck.h"
#include "hardware_deck.h"
#include "system_deck.h"

uint64_t g_entry_rflags = 0;

void kernel_main(void)
{
    LogKeepInit();

    VideoInit();
    serial_init();

    kprintf(" Cabin (0x%lx Info, 0x%lx PocketRing, 0x%lx ResultRing, 0x%lx Code)\n",
            CABIN_INFO_ADDR, CABIN_POCKET_RING_ADDR, CABIN_RESULT_RING_ADDR, CABIN_CODE_START_ADDR);
    kprintf("\n");

    kprintf("[BOOT] the loader handed over RFLAGS=0x%lx (interrupts were %s; "
            "they are off now, and stay off until this kernel says otherwise)\n",
            (unsigned long)g_entry_rflags,
            (g_entry_rflags & (1ULL << 9)) ? "ENABLED" : "disabled");

    if (LogKeepWindow(NULL, NULL)) {
        uint64_t carried = LogKeepPreviousBytes();
        if (carried) {
            kprintf("[BOOT] the previous run left %lu byte(s) behind (boot %u) "
                    "— `lastsaid` reads them\n",
                    (unsigned long)carried, LogKeepPreviousBoot());
        } else {
            kprintf("[BOOT] nothing came through the last reset — a cold start, "
                    "a first one, or firmware that scrubs memory\n");
        }
    }

    debug_printf("[INIT] CPU Feature Detection (early)...\n");
    cpu_detect_features();

    vmm_note_no_execute(CpuTakeUpNoExecute());

    cpu_log_identity("BSP");

    debug_printf("[INIT] Hypervisor Detection...\n");
    hypervisor_detect();
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

    bi = boot_info_get();

    if (bi->version >= BOOT_INFO_VERSION2 && bi->fb_addr) {
        VideoInitFramebuffer(bi->fb_addr, bi->fb_width, bi->fb_height,
                             bi->fb_stride, bi->fb_format);
        if (VideoGetMode() == DISPLAY_GOP_FB) {
            kprintf("[DISPLAY] GOP framebuffer active: %ux%u (stride=%u fmt=%u)\n",
                    bi->fb_width, bi->fb_height, bi->fb_stride, bi->fb_format);
        }
    }

    BoardingPassInit();

    debug_printf("[INIT] MemTag...\n");
    error_t memtag_err = MemTagInit();
    if (memtag_err != OK) {
        debug_printf("[INIT] WARNING: MemTag init failed: %s (non-fatal)\n", ErrorString(memtag_err));
    }

    (void)MemTagVerifyPteMetadataBits();

    (void)MemTagVerifyPatMsr();
    MemTagDumpMtrrLayout();

    mce_init();

    vmm_pku_init();

    vmm_lam_probe();

    vmm_tme_probe();

    {
        extern error_t tme_init_bsp(void);
        (void)tme_init_bsp();
    }

    vmm_cet_probe();

    pmm_test_high_memory();
    MemTagStressTest();

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
    { int p = 0, f = 0; TmeRunTests(&p, &f); (void)p; (void)f; }
    AddrWaitSelfTest();
    XhciRingSelfTest();
    slab_identity_selftest();

    debug_printf("[INIT] TSS Dynamic Stacks...\n");
    tss_setup_dynamic_stacks();

    debug_printf("[INIT] CPU Capabilities Page...\n");
    cpu_caps_page_init();

    debug_printf("[INIT] EFI crypto self-test...\n");
    if (!efi_selftest_run()) {
        kprintf("[BOOT] EFI self-test FAILED — Secure Boot disabled\n");
    }

    debug_printf("[INIT] EFI runtime services...\n");
    if (efi_runtime_init()) {
        efi_runtime_print_info();

        if (efi_esrt_init()) {
            efi_esrt_print();
        }

        if (efi_secureboot_init()) {
            efi_secureboot_print();
        }
    } else {
        debug_printf("[INIT] EFI runtime services not available\n");
    }

    PmmReleaseBootServicesMemory();

    debug_printf("[INIT] ACPI Subsystem (early)...\n");
    acpi_error_t acpi_err = acpi_init();
    if (acpi_err == ACPI_OK)
    {
        debug_printf("[INIT] ACPI initialized successfully\n");
        acpi_apei_consume();
        pmm_log_numa_topology();
        iommu_init();
        IommuPresenceTest();
        iommu_audit_dump();
        aml_init();
    }
    else
    {
        debug_printf("[INIT] ACPI initialization failed (error %d), shutdown may use fallback methods\n", acpi_err);
    }

    debug_printf("[INIT] Interrupt Controller...\n");
    irqchip_init();

    if (acpi_err == ACPI_OK) acpi_sci_register();

    debug_printf("[INIT] AMP Core Detection...\n");
    amp_init();

    debug_printf("[INIT] Baton queues (never-drop, per core)...\n");
    BatonInit();

    debug_printf("[INIT] Per-core GDT/TSS/Notify (BSP)...\n");
    per_core_init_bsp();

    debug_printf("[INIT] ClockBoard...\n");
    clockboard_init();

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
    pit_init(CONFIG_SCHED_DEFAULT_TICK_HZ);

    debug_printf("[INIT] RTC...\n");
    rtc_init();
    clockboard_set_boot_unix_secs(rtc_get_unix64());

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
    kprintf("[BOOT] EARLYIRQ: opening the interrupt flag for the length of "
            "TSC calibration, which is where the board took its first tick\n");
    asm volatile("sti");
    cpu_calibrate_tsc();
    asm volatile("cli");
#else
    cpu_calibrate_tsc();
#endif
    clockboard_set_tsc_freq_khz(cpu_get_tsc_freq_khz());

    cpu_umwait_control_init(cpu_get_tsc_freq_khz());

    cpu_test_ctl_init();

    per_core_record_bsp_tsc_anchor(pit_get_uptime_us());

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

    { extern volatile bool g_kernel_ready_for_interrupts;
      g_kernel_ready_for_interrupts = true; }

    debug_printf("[INIT] Guide Dispatcher...\n");
    guide_init();

    { extern void TouchWatchSelfTest(void); TouchWatchSelfTest(); }

    VideoNotifyReady();

    MemTagEnableTouchPublish();

    {
        extern void mce_migrate_init(void);
        mce_migrate_init();
        extern void McMigrationTest(void);
        McMigrationTest();
        extern void PkuStampTest(void);
        PkuStampTest();

        extern void apei_ghes_runtime_init(void);
        apei_ghes_runtime_init();
        extern void ApeiGhesTest(void);
        ApeiGhesTest();

        if (acpi_err == ACPI_OK) acpi_sci_arm();

        extern void uaccess_init(void);
        uaccess_init();

        extern int cet_lifecycle_init_bsp(void);
        (void)cet_lifecycle_init_bsp();

        extern error_t cet_lifecycle_init_supervisor_ssp(uint8_t);
        (void)cet_lifecycle_init_supervisor_ssp(amp_get_core_index());

        extern void CetLifecycleTest(void);
        CetLifecycleTest();
    }

    if (efi_runtime_available()) {
        if (efi_esrt_available())       efi_esrt_publish_touch();
        if (efi_secureboot_available()) efi_secureboot_publish_touch();
    }

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


    if (g_amp.total_cores > 1)
    {
        debug_printf("[INIT] K-Core Queues...\n");
        kcore_init();

        debug_printf("[INIT] Syscall Mode: ASYNC...\n");
        idt_set_syscall_mode(true);

        debug_printf("[INIT] Booting Application Processors...\n");
        amp_boot_aps();

        nightwatch_init();
    }
    else
    {
        idt_set_syscall_mode(false);

        nightwatch_init();
    }

    debug_printf("[INIT] PCI Subsystem...\n");
    pci_init();

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

    debug_printf("[INIT] Async I/O Queue...\n");
    async_io_init();

    debug_printf("[INIT] USB xHCI Driver...\n");
    if (xhci_init() != 0)
    {
        debug_printf("[INIT] xHCI controller not found or initialization failed\n");
    }

    debug_printf("[INIT] Boardroom (media)...\n");
    BoardroomInit();

    debug_printf("[INIT] Storage Deck & TagFS...\n");
    storage_deck_init();

    BoardroomProveUnattendedRead(tagfs_get_seat());

    DeedSurveyAll(tagfs_get_seat(), tagfs_get_volume_base());

    debug_printf("[INIT] Storage Deck register (Manifest path)...\n");
    error_t storage_reg_err = StorageDeckRegister();
    if (storage_reg_err != OK)
    {
        kprintf("[WARN] Storage Deck register failed: %s\n", ErrorString(storage_reg_err));
    }

    xhci_interrupt_touch_init();

    debug_printf("[INIT] Keyboard...\n");
    keyboard_init();

    debug_printf("[INIT] Serial console (COM1 RX)...\n");
    serial_console_init();

    kprintf("Kernel initialization complete!\n");
    kprintf("\n");

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

    {
        extern error_t bmide_watchdog_selftest(void);
        (void)bmide_watchdog_selftest();
    }

#if CONFIG_BMIDE_WEDGE_SELFTEST
    {
        extern error_t bmide_wedge_selftest(void);
        (void)bmide_wedge_selftest();
    }
#endif

#if CONFIG_START_USERSPACE
    kprintf("Starting userspace...\n");
    kprintf("\n");

    process_t *initial_proc = NULL;
    int autostart_count = AutostartLaunchFromVolume(&initial_proc, false);

    if (!initial_proc)
    {
        kprintf("[AUTOSTART] No autostart files found, falling back to embedded shell\n");

        process_t *shell_proc = process_create("shell");
        if (!shell_proc)
        {
            panic("Failed to create fallback shell process");
        }

        process_add_tag(shell_proc, "system");
        process_add_tag(shell_proc, "utility");
        process_add_tag(shell_proc, "app");

        size_t shell_size = (size_t)(_binary_shell_stripped_elf_end - _binary_shell_stripped_elf_start);

        if (shell_size == 0 || shell_size > CONFIG_PROC_MAX_BINARY_SIZE)
        {
            panic("Invalid embedded shell binary size");
        }

        int load_result = process_load_binary(shell_proc, _binary_shell_stripped_elf_start, shell_size);
        if (load_result != 0)
        {
            panic("Failed to load embedded shell binary");
        }

        shell_proc->state = PROC_WORKING;
        initial_proc = shell_proc;
        kprintf("[AUTOSTART] Fallback shell ready (PID %u)\n", shell_proc->pid);

        AutostartNoteStandIn(shell_proc->pid);
    }
    else
    {
        AutostartNoteVolumeLaunched();
    }

    AutostartWatchVolume();

    kprintf("[AUTOSTART] %d process(es) launched\n", autostart_count);

    if (g_amp.multicore_active)
    {
        kprintf("[KERNEL] Multi-core mode: %u K-Core(s), %u App Core(s)\n",
                g_amp.k_count, g_amp.app_count);

        process_list_lock();
        process_t *p = process_get_first();
        while (p)
        {
            if (p->magic == PROCESS_MAGIC && p->state == PROC_WORKING &&
                !process_is_idle(p))
            {
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

        for (uint8_t c = 0; c < g_amp.total_cores; c++)
        {
            if (!g_amp.cores[c].is_kcore && amp_core_online(&g_amp.cores[c]))
            {
                lapic_send_ipi(g_amp.cores[c].lapic_id, IPI_WAKE_VECTOR);
            }
        }

        kprintf("[KERNEL] BSP entering K-Core guide loop...\n");
        extern void cet_supv_shstk_activate_and_jump(void (*)(void));
        cet_supv_shstk_activate_and_jump(kcore_run_loop);
    }

    kprintf("[KERNEL] Single-core mode: BSP scheduling all processes\n");

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