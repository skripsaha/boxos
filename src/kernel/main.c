#include "vga.h"
#include "klib.h"
#include "serial.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "pic.h"
#include "pit.h"
#include "rtc.h"
#include "e820.h"
#include "fpu.h"
#include "process.h"
#include "guide.h"
#include "scheduler.h"
#include "operations_test.h"
#include "hardware_test.h"
#include "system_test.h"
#include "keyboard.h"
#include "ata.h"
#include "ata_dma.h"
#include "async_io.h"
#include "pci.h"
#include "storage_deck.h"
#include "storage_test.h"
#include "journal_test.h"
#include "guide/route_test.h"
#include "kernel_config.h"
#include "event_ring_dynamic.h"
#include "xhci.h"
#include "acpi.h"
#include "ahci.h"
#include "cabin_layout.h"
#include "core/test_event_ring_dynamic.h"
#include "cpuid.h"
#include "cpu_caps_page.h"

extern char __bss_start[];
extern char __bss_end[];

void kernel_main(void)
{
    volatile uint64_t *bss_ptr = (volatile uint64_t *)__bss_start;
    volatile uint64_t *bss_end_ptr = (volatile uint64_t *)__bss_end;
    while (bss_ptr < bss_end_ptr)
    {
        *bss_ptr++ = 0;
    }

    vga_init();
    serial_init();

    kprintf("====================================\n");
    kprintf("BoxOS - Clean Foundation\n");
    kprintf("====================================\n");
    kprintf("Architecture: x86-64 (64-bit)\n");
    kprintf("Model: Event-driven workflow (BoxOS)\n");
    kprintf("Syscall: kernel_notify (INT 0x80)\n");
    kprintf("Memory Model: Cabin (0x%lx Notify, 0x%lx Result, 0x%lx Code)\n",
            CABIN_NOTIFY_PAGE_ADDR, CABIN_RESULT_PAGE_ADDR, CABIN_CODE_START_ADDR);
    kprintf("\n");

    debug_printf("[INIT] FPU/SSE...\n");
    enable_fpu();

    debug_printf("[INIT] GDT...\n");
    gdt_init();

    debug_printf("[INIT] TSS...\n");
    tss_init();

    debug_printf("[INIT] IDT...\n");
    idt_init();

    debug_printf("[INIT] PIC...\n");
    pic_init();

    debug_printf("[INIT] E820 Memory Map...\n");
    e820_entry_t *e820_map = (e820_entry_t *)0x500;
    volatile uint16_t *e820_count_ptr = (volatile uint16_t *)0x4FE;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    uint16_t e820_count = *e820_count_ptr;
#pragma GCC diagnostic pop

    #define E820_MAX_ENTRIES 128
    if (e820_count == 0) {
        kprintf("[PANIC] E820: No memory map entries\n");
        while (1) { asm volatile("cli; hlt"); }
    }
    if (e820_count > E820_MAX_ENTRIES) {
        kprintf("[PANIC] E820: Invalid count %u (max %u)\n", e820_count, E820_MAX_ENTRIES);
        while (1) { asm volatile("cli; hlt"); }
    }

    debug_printf("[E820] Entries at 0x500, count = %u\n", e820_count);
    e820_set_entries(e820_map, (size_t)e820_count);

    debug_printf("[INIT] PMM...\n");
    pmm_init();

    debug_printf("[INIT] Kernel Heap...\n");
    mem_init();

    debug_printf("[INIT] VMM...\n");
    vmm_init();

    // Detect CPU features
    debug_printf("[INIT] CPU Feature Detection...\n");
    cpu_detect_features();

    // Initialize CPU capabilities page
    debug_printf("[INIT] CPU Capabilities Page...\n");
    cpu_caps_page_init();

    debug_printf("[INIT] PIT...\n");
    pit_init(100);

    debug_printf("[INIT] RTC...\n");
    rtc_init();

    debug_printf("[INIT] CPU Calibration...\n");
    extern void cpu_calibrate_tsc(void);
    cpu_calibrate_tsc();

    // Initialize idle process (PID 0) before process system
    kprintf("[INIT] Idle Process...\n");
    extern void idle_process_init(void);
    idle_process_init();

    debug_printf("[INIT] Process Management...\n");
    process_init();

    debug_printf("[INIT] Scheduler...\n");
    scheduler_init();

    debug_printf("[INIT] Guide Dispatcher...\n");
    guide_init();

    // Run Dynamic EventRing unit tests
    kprintf("\n");
    kprintf("========================================================================\n");
    kprintf("RUNNING UNIT TESTS: Dynamic EventRing\n");
    kprintf("========================================================================\n");

    int test_result = test_event_ring_dynamic_all();
    if (test_result != 0) {
        kprintf("\n");
        kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        kprintf("CRITICAL ERROR: Unit tests FAILED!\n");
        kprintf("System halted to prevent boot with broken EventRing implementation.\n");
        kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        panic("Dynamic EventRing unit tests failed - aborting boot");
    }

    kprintf("\n");
    kprintf("========================================================================\n");
    kprintf("ALL UNIT TESTS PASSED - Continuing boot\n");
    kprintf("========================================================================\n");
    kprintf("\n");

    debug_printf("[INIT] PCI Subsystem...\n");
    pci_init();

#ifdef CONFIG_AHCI_DRIVER
    debug_printf("[INIT] AHCI Driver...\n");
    if (ahci_init() == 0) {
        debug_printf("[INIT] AHCI initialized successfully\n");

        debug_printf("[INIT] AHCI IRQ setup...\n");
        ahci_init_irq();
        ahci_port_enable_irq(0);
        debug_printf("[INIT] AHCI NCQ enabled\n");
    } else {
        debug_printf("[INIT] AHCI not available, using legacy ATA\n");
    }
#endif

    debug_printf("[INIT] ATA Driver...\n");
    ata_init();

    debug_printf("[INIT] ATA DMA...\n");
    if (ata_dma_init() == 0) {
        debug_printf("[INIT] ATA DMA enabled successfully\n");
        pic_enable_irq(14);  // Enable IRQ14 (ATA Primary DMA)
    } else {
        debug_printf("[INIT] ATA DMA not available, using PIO mode\n");
    }

    debug_printf("[INIT] Async I/O Queue...\n");
    async_io_init();

    debug_printf("[INIT] Storage Deck & TagFS...\n");
    storage_deck_init();

    debug_printf("[INIT] Keyboard...\n");
    keyboard_init();

    debug_printf("[INIT] USB xHCI Driver...\n");
    int xhci_result = xhci_init();
    if (xhci_result == 0) {
        debug_printf("[INIT] xHCI controller initialized successfully\n");
    } else {
        debug_printf("[INIT] xHCI controller not found or initialization failed\n");
    }

    debug_printf("[INIT] ACPI Subsystem...\n");
    acpi_error_t acpi_err = acpi_init();
    if (acpi_err == ACPI_OK) {
        debug_printf("[INIT] ACPI initialized successfully\n");
    } else {
        debug_printf("[INIT] ACPI initialization failed (error %d), shutdown may use fallback methods\n", acpi_err);
    }

    kprintf("\n");
    kprintf("====================================\n");
    kprintf("Kernel initialization complete!\n");
    kprintf("====================================\n");
    kprintf("\n");

#if CONFIG_RUN_STARTUP_TESTS
    kprintf("\n");
    debug_printf("[INIT] Running BoxOS full cycle test...\n");
    test_full_cycle();
    kprintf("\n");
    debug_printf("[INIT] Running Operations Deck test...\n");
    test_operations_deck();
    kprintf("\n");
    debug_printf("[INIT] Running Hardware Deck test...\n");
    test_hardware_deck();
    kprintf("\n");
    debug_printf("[INIT] Running System Deck security test...\n");
    test_system_deck_security();
    kprintf("\n");
    debug_printf("[INIT] Running System Deck process management test...\n");
    test_system_deck_process_management();
    kprintf("\n");
    debug_printf("[INIT] Running System Deck tag management test...\n");
    test_system_deck_tag_management();
    kprintf("\n");
    debug_printf("[INIT] Running System Deck buffer management test...\n");
    test_system_deck_buffer_management();
    kprintf("\n");
    debug_printf("[INIT] Running System Deck CTX_USE test...\n");
    test_system_deck_ctx_use();
    kprintf("\n");
    debug_printf("[INIT] Running Listen Table test...\n");
    test_listen_table();
    kprintf("\n");
    debug_printf("[INIT] Running Route Direct test...\n");
    test_route_direct();
    kprintf("\n");
    debug_printf("[INIT] Running Route Tag test...\n");
    test_route_tag();
    kprintf("\n");
    debug_printf("[INIT] Running Listen via System Deck test...\n");
    test_listen_via_system_deck();
    kprintf("\n");
    debug_printf("[INIT] Running Route Security test...\n");
    test_route_security();
    kprintf("\n");
    debug_printf("[INIT] Running PMM tests...\n");
    pmm_run_tests();
    kprintf("\n");
    debug_printf("[INIT] Running Journal tests...\n");
    run_journal_tests();
    kprintf("\n");
    debug_printf("[INIT] Running Storage Deck & TagFS test...\n");
    test_storage_deck();
#endif

    kprintf("STATUS: Guide Dispatcher OPERATIONAL\n");
    kprintf("STATUS: Smart Score Scheduler OPERATIONAL\n");
    kprintf("\n");

#if CONFIG_START_USERSPACE
    kprintf("====================================\n");
    kprintf("STARTING USERSPACE EXECUTION\n");
    kprintf("====================================\n");
    kprintf("\n");

    debug_printf("[USERSPACE] Creating shell process...\n");
    process_t *shell_proc = process_create("shell");
    if (!shell_proc)
    {
        debug_printf("[USERSPACE] PANIC: Failed to create shell process\n");
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    process_add_tag(shell_proc, "system");
    process_add_tag(shell_proc, "utility");
    process_add_tag(shell_proc, "app");
    process_add_tag(shell_proc, "shell");
    process_add_tag(shell_proc, "hw_vga");
    process_add_tag(shell_proc, "hw_keyboard");
    process_add_tag(shell_proc, "storage");

    debug_printf("[USERSPACE] Loading shell binary...\n");

    extern uint8_t _binary_shell_stripped_elf_start[];
    extern uint8_t _binary_shell_stripped_elf_end[];
    size_t shell_size = (size_t)(_binary_shell_stripped_elf_end - _binary_shell_stripped_elf_start);

    debug_printf("[USERSPACE] Shell binary: %zu bytes (embedded at %p)\n",
                 shell_size, _binary_shell_stripped_elf_start);

    if (shell_size == 0 || shell_size > 53248)  // 52 KB = 0x10000 - 0x3000
    {
        debug_printf("[USERSPACE] PANIC: Invalid shell binary size\n");
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    int load_result = process_load_binary(shell_proc, _binary_shell_stripped_elf_start, shell_size);
    if (load_result != 0)
    {
        debug_printf("[USERSPACE] PANIC: Failed to load shell binary (error %d)\n", load_result);
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    debug_printf("[USERSPACE] Binary loaded successfully\n");
    debug_printf("[USERSPACE]   Code start: 0x%lx\n", shell_proc->code_start);
    debug_printf("[USERSPACE]   Entry RIP: 0x%lx\n", shell_proc->context.rip);
    debug_printf("[USERSPACE]   Stack RSP: 0x%lx\n", shell_proc->context.rsp);
    debug_printf("[USERSPACE]   CR3: 0x%lx\n", shell_proc->context.cr3);

    shell_proc->state = PROC_READY;

    debug_printf("[USERSPACE] Shell process ready (PID %u)\n", shell_proc->pid);
    debug_printf("[KERNEL] Starting shell process - jumping to Ring 3...\n");

    process_start_initial(shell_proc);

    // Should NEVER return from above call
    debug_printf("[KERNEL] ERROR: Returned from userspace! This should never happen!\n");
    panic("Returned from process_start_initial()");
#else
    kprintf("Userspace execution disabled (CONFIG_START_USERSPACE=0)\n");
    kprintf("System idle. Guide will process events on demand.\n");
    kprintf("\n");

    kprintf("====================================\n");
    kprintf("KERNEL EVENT LOOP ACTIVE\n");
    kprintf("====================================\n");
    kprintf("Timer: 100Hz (IRQ0 triggers scheduler)\n");
    kprintf("Guide: Processes events from EventRing\n");
    kprintf("Status: Interrupts enabled, entering idle loop\n");
    kprintf("\n");

    asm volatile("sti");

    uint64_t loop_count = 0;
    while (1)
    {
        guide_run();

        if ((loop_count % 100) == 0)
        {
            extern EventRingBuffer* kernel_event_ring;
            size_t pending = event_ring_count(kernel_event_ring);
            if (pending > 0)
            {
                debug_printf("[MAIN] EventRing has %zu pending events\n", pending);
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
