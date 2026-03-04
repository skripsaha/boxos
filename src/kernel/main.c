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
#include "route_test.h"
#include "kernel_config.h"
#include "event_ring.h"
#include "xhci.h"
#include "acpi.h"
#include "ahci.h"
#include "cabin_layout.h"
#include "event_ring_test.h"
#include "tagfs.h"
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
    kprintf("Model: Snowball command pipeline (BoxOS)\n");
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

    debug_printf("[INIT] CPU Feature Detection...\n");
    cpu_detect_features();

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
        uint32_t ahci_active = ahci_get_active_port_mask();
        for (uint8_t p = 0; p < 32 && ahci_active; p++) {
            if (ahci_active & (1U << p)) {
                ahci_port_enable_irq(p);
                ahci_active &= ~(1U << p);
            }
        }
        debug_printf("[INIT] AHCI NCQ enabled (%u port(s))\n", ahci_get_active_port_count());
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
    debug_printf("[INIT] Running Tag Wildcard test...\n");
    test_tag_wildcard();
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

    // ============================================================
    // AUTOSTART: Query TagFS for files tagged "autostart" and
    // spawn them automatically, as specified in the BoxOS tag spec.
    // ============================================================
    debug_printf("[AUTOSTART] Scanning TagFS for autostart files...\n");

    TagFSState* tfs_state = tagfs_get_state();
    uint32_t autostart_max = tfs_state ? tfs_state->max_files : TAGFS_MAX_FILES;
    uint32_t* file_ids = kmalloc(sizeof(uint32_t) * autostart_max);
    if (!file_ids) {
        debug_printf("[AUTOSTART] Failed to allocate file_ids buffer\n");
        autostart_max = 0;
    }
    int file_count = file_ids ? tagfs_list_all_files(file_ids, autostart_max) : 0;
    process_t *initial_proc = NULL;
    int autostart_count = 0;

    for (int i = 0; i < file_count; i++) {
        TagFSMetadata *meta = tagfs_get_metadata(file_ids[i]);
        if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE))
            continue;

        bool has_autostart = false;
        bool has_exec_tag = false;

        for (uint8_t t = 0; t < meta->tag_count; t++) {
            if (meta->tags[t].type != TAGFS_TAG_SYSTEM)
                continue;
            const char *key = meta->tags[t].key;
            if (strcmp(key, "autostart") == 0)
                has_autostart = true;
            if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0)
                has_exec_tag = true;
        }

        if (!has_autostart || !has_exec_tag)
            continue;

        debug_printf("[AUTOSTART] Found: '%s' (file_id=%u)\n",
                     meta->filename, file_ids[i]);

        // Collect all system tags for the new process
        char found_tags[PROCESS_TAG_SIZE];
        size_t pos = 0;
        for (uint8_t t = 0; t < meta->tag_count; t++) {
            if (meta->tags[t].type != TAGFS_TAG_SYSTEM)
                continue;
            const char *key = meta->tags[t].key;
            size_t klen = strlen(key);
            if (pos + klen + 2 > PROCESS_TAG_SIZE)
                break;
            if (pos > 0)
                found_tags[pos++] = ',';
            memcpy(found_tags + pos, key, klen);
            pos += klen;
        }
        found_tags[pos] = '\0';

        // Load binary from TagFS
        uint64_t file_size = meta->size;
        if (file_size == 0 || file_size > CONFIG_PROC_MAX_BINARY_SIZE) {
            debug_printf("[AUTOSTART] Skip '%s': invalid size %lu\n",
                         meta->filename, file_size);
            continue;
        }

        size_t pages_needed = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        void *phys_buf = pmm_alloc_zero(pages_needed);
        if (!phys_buf) {
            debug_printf("[AUTOSTART] Skip '%s': memory allocation failed\n",
                         meta->filename);
            continue;
        }

        void *virt_buf = vmm_phys_to_virt((uintptr_t)phys_buf);

        TagFSFileHandle *fh = tagfs_open(file_ids[i], TAGFS_HANDLE_READ);
        if (!fh) {
            pmm_free(phys_buf, pages_needed);
            debug_printf("[AUTOSTART] Skip '%s': tagfs_open failed\n",
                         meta->filename);
            continue;
        }

        int read_result = tagfs_read(fh, virt_buf, file_size);
        tagfs_close(fh);

        if (read_result < 0) {
            pmm_free(phys_buf, pages_needed);
            debug_printf("[AUTOSTART] Skip '%s': tagfs_read failed (%d)\n",
                         meta->filename, read_result);
            continue;
        }

        // Create process with all file tags
        process_t *proc = process_create(found_tags);
        if (!proc) {
            pmm_free(phys_buf, pages_needed);
            debug_printf("[AUTOSTART] Skip '%s': process_create failed\n",
                         meta->filename);
            continue;
        }

        int load_result = process_load_binary(proc, virt_buf, (size_t)file_size);
        pmm_free(phys_buf, pages_needed);

        if (load_result != 0) {
            process_destroy(proc);
            debug_printf("[AUTOSTART] Skip '%s': load_binary failed (%d)\n",
                         meta->filename, load_result);
            continue;
        }

        proc->state = PROC_WORKING;
        autostart_count++;

        kprintf("[AUTOSTART] Started '%s' (PID %u, tags: %s)\n",
                meta->filename, proc->pid, found_tags);

        // First autostart process becomes the initial process
        if (!initial_proc)
            initial_proc = proc;
    }

    if (file_ids) {
        kfree(file_ids);
    }

    // Fallback: if no autostart files found, use embedded shell binary
    if (!initial_proc) {
        kprintf("[AUTOSTART] No autostart files found, falling back to embedded shell\n");

        process_t *shell_proc = process_create("shell");
        if (!shell_proc) {
            panic("Failed to create fallback shell process");
        }

        process_add_tag(shell_proc, "system");
        process_add_tag(shell_proc, "utility");
        process_add_tag(shell_proc, "app");
        process_add_tag(shell_proc, "hw_vga");
        process_add_tag(shell_proc, "hw_keyboard");
        process_add_tag(shell_proc, "storage");

        extern uint8_t _binary_shell_stripped_elf_start[];
        extern uint8_t _binary_shell_stripped_elf_end[];
        size_t shell_size = (size_t)(_binary_shell_stripped_elf_end - _binary_shell_stripped_elf_start);

        if (shell_size == 0 || shell_size > CONFIG_PROC_MAX_BINARY_SIZE) {
            panic("Invalid embedded shell binary size");
        }

        int load_result = process_load_binary(shell_proc, _binary_shell_stripped_elf_start, shell_size);
        if (load_result != 0) {
            panic("Failed to load embedded shell binary");
        }

        shell_proc->state = PROC_WORKING;
        initial_proc = shell_proc;
        kprintf("[AUTOSTART] Fallback shell ready (PID %u)\n", shell_proc->pid);
    }

    kprintf("[AUTOSTART] %d process(es) launched\n", autostart_count);
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
