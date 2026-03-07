#include "vga.h"
#include "klib.h"
#include "serial.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "pic.h"
#include "irqchip.h"
#include "pit.h"
#include "rtc.h"
#include "e820.h"
#include "fpu.h"
#include "process.h"
#include "guide.h"
#include "scheduler.h"
#include "keyboard.h"
#include "ata.h"
#include "ata_dma.h"
#include "async_io.h"
#include "pci.h"
#include "storage_deck.h"
#include "kernel_config.h"
#include "ready_queue.h"
#include "xhci.h"
#include "acpi.h"
#include "ahci.h"
#include "cabin_layout.h"
#include "tagfs.h"
#include "cpuid.h"
#include "cpu_caps_page.h"
#include "boot_info.h"

void kernel_main(void)
{
    vga_init();
    serial_init();

    kprintf(" Cabin (0x%lx Info, 0x%lx PocketRing, 0x%lx ResultRing, 0x%lx Code)\n",
            CABIN_INFO_ADDR, CABIN_POCKET_RING_ADDR, CABIN_RESULT_RING_ADDR, CABIN_CODE_START_ADDR);
    kprintf("\n");

    debug_printf("[INIT] CPU Feature Detection (early)...\n");
    cpu_detect_features();

    // Randomize stack canary ASAP after CPUID is available
    extern void stack_canary_init(void);
    stack_canary_init();

    debug_printf("[INIT] FPU/SSE/AVX...\n");
    enable_fpu();

    debug_printf("[INIT] GDT...\n");
    gdt_init();

    debug_printf("[INIT] TSS...\n");
    tss_init();

    debug_printf("[INIT] IDT...\n");
    idt_init();

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
    pmm_init();

    debug_printf("[INIT] Kernel Heap...\n");
    mem_init();

    debug_printf("[INIT] VMM...\n");
    vmm_init();

    debug_printf("[INIT] TSS Dynamic Stacks...\n");
    tss_setup_dynamic_stacks();

    debug_printf("[INIT] CPU Capabilities Page...\n");
    cpu_caps_page_init();

    // ACPI must init early so irqchip_init can parse MADT for APIC detection
    debug_printf("[INIT] ACPI Subsystem (early)...\n");
    acpi_error_t acpi_err = acpi_init();
    if (acpi_err == ACPI_OK)
    {
        debug_printf("[INIT] ACPI initialized successfully\n");
    }
    else
    {
        debug_printf("[INIT] ACPI initialization failed (error %d), shutdown may use fallback methods\n", acpi_err);
    }

    // Detect and initialize interrupt controller (APIC or PIC fallback)
    debug_printf("[INIT] Interrupt Controller...\n");
    irqchip_init();

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

    debug_printf("[INIT] ASLR...\n");
    extern void aslr_init(void);
    aslr_init();

    debug_printf("[INIT] Process Management...\n");
    process_init();

    debug_printf("[INIT] Scheduler...\n");
    scheduler_init();

    debug_printf("[INIT] Guide Dispatcher...\n");
    guide_init();

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

    debug_printf("[INIT] ATA DMA...\n");
    if (ata_dma_init() == 0)
    {
        debug_printf("[INIT] ATA DMA enabled successfully\n");
        irqchip_enable_irq(14); // Enable IRQ14 (ATA Primary DMA)
    }
    else
    {
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
    if (xhci_result == 0)
    {
        debug_printf("[INIT] xHCI controller initialized successfully\n");
    }
    else
    {
        debug_printf("[INIT] xHCI controller not found or initialization failed\n");
    }

    kprintf("Kernel initialization complete!\n");
    kprintf("\n");

#if CONFIG_START_USERSPACE
    kprintf("Starting userspace...\n");
    kprintf("\n");

    // ============================================================
    // AUTOSTART: Query TagFS for files tagged "autostart" and
    // spawn them automatically, as specified in the BoxOS tag spec.
    // ============================================================
    debug_printf("[AUTOSTART] Scanning TagFS for autostart files...\n");

    TagFSState *tfs_state = tagfs_get_state();
    uint32_t autostart_max = tfs_state ? tfs_state->max_files : TAGFS_MAX_FILES;
    uint32_t *file_ids = kmalloc(sizeof(uint32_t) * autostart_max);
    if (!file_ids)
    {
        debug_printf("[AUTOSTART] Failed to allocate file_ids buffer\n");
        autostart_max = 0;
    }
    int file_count = file_ids ? tagfs_list_all_files(file_ids, autostart_max) : 0;
    process_t *initial_proc = NULL;
    int autostart_count = 0;

    for (int i = 0; i < file_count; i++)
    {
        TagFSMetadata *meta = tagfs_get_metadata(file_ids[i]);
        if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE))
            continue;

        bool has_autostart = false;
        bool has_exec_tag = false;

        for (uint8_t t = 0; t < meta->tag_count; t++)
        {
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
        for (uint8_t t = 0; t < meta->tag_count; t++)
        {
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
        if (file_size == 0 || file_size > CONFIG_PROC_MAX_BINARY_SIZE)
        {
            debug_printf("[AUTOSTART] Skip '%s': invalid size %lu\n",
                         meta->filename, file_size);
            continue;
        }

        size_t pages_needed = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        void *phys_buf = pmm_alloc_zero(pages_needed);
        if (!phys_buf)
        {
            debug_printf("[AUTOSTART] Skip '%s': memory allocation failed\n",
                         meta->filename);
            continue;
        }

        void *virt_buf = vmm_phys_to_virt((uintptr_t)phys_buf);

        TagFSFileHandle *fh = tagfs_open(file_ids[i], TAGFS_HANDLE_READ);
        if (!fh)
        {
            pmm_free(phys_buf, pages_needed);
            debug_printf("[AUTOSTART] Skip '%s': tagfs_open failed\n",
                         meta->filename);
            continue;
        }

        int read_result = tagfs_read(fh, virt_buf, file_size);
        tagfs_close(fh);

        if (read_result < 0)
        {
            pmm_free(phys_buf, pages_needed);
            debug_printf("[AUTOSTART] Skip '%s': tagfs_read failed (%d)\n",
                         meta->filename, read_result);
            continue;
        }

        // Create process with all file tags
        process_t *proc = process_create(found_tags);
        if (!proc)
        {
            pmm_free(phys_buf, pages_needed);
            debug_printf("[AUTOSTART] Skip '%s': process_create failed\n",
                         meta->filename);
            continue;
        }

        int load_result = process_load_binary(proc, virt_buf, (size_t)file_size);
        pmm_free(phys_buf, pages_needed);

        if (load_result != 0)
        {
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

    if (file_ids)
    {
        kfree(file_ids);
    }

    // Fallback: if no autostart files found, use embedded shell binary
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
        process_add_tag(shell_proc, "hw_vga");
        process_add_tag(shell_proc, "hw_kb");
        process_add_tag(shell_proc, "storage");

        extern uint8_t _binary_shell_stripped_elf_start[];
        extern uint8_t _binary_shell_stripped_elf_end[];
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

    kprintf("Kernel loop\n");
    kprintf("\n");

    asm volatile("sti");

    uint64_t loop_count = 0;
    while (1)
    {
        guide_run();

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
