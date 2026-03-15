#include "system_halt.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"
#include "process.h"
#include "scheduler.h"
#include "tagfs.h"
#include "ata.h"
#include "acpi.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "xhci.h"
#include "xhci_port.h"

static void halt_delay_ms(uint32_t ms) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while (rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
}

static void halt_all_ap_cores(void) {
    if (!g_amp.multicore_active) return;

    kprintf("[HALT] Stopping all AP cores...\n");

    lapic_send_ipi_all_excluding_self(IPI_PANIC_VECTOR);

    halt_delay_ms(5);

    uint8_t my_index = amp_get_core_index();
    for (uint8_t c = 0; c < g_amp.total_cores; c++) {
        if (c == my_index) continue;
        g_amp.cores[c].online = false;
    }

    kprintf("[HALT] All AP cores stopped\n");
}

static void halt_terminate_all_processes(void) {
    kprintf("[HALT] Terminating all processes...\n");

    process_cleanup_queue_flush();

    uint32_t pids[256];
    uint32_t pid_count = 0;

    process_list_lock();
    process_t* proc = process_get_first();
    while (proc && pid_count < 256) {
        pids[pid_count++] = proc->pid;
        proc = proc->next;
    }
    process_list_unlock();

    uint32_t killed = 0;
    for (uint32_t i = 0; i < pid_count; i++) {
        process_t* p = process_find(pids[i]);
        if (p) {
            // Force state to PROC_DONE — AP cores are halted, processes are frozen
            process_set_state(p, PROC_DONE);
            process_destroy(p);
            killed++;
        }
    }

    process_cleanup_queue_flush();

    kprintf("[HALT] %u processes terminated\n", killed);
}

static void halt_sync_storage(void) {
    kprintf("[HALT] Syncing TagFS...\n");
    tagfs_shutdown();

    kprintf("[HALT] Flushing disk cache...\n");
    ata_flush_cache(1);

    kprintf("[HALT] Storage sync complete\n");
}

static void halt_stop_hardware(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (ctrl && ctrl->initialized) {
        kprintf("[HALT] Stopping USB controller...\n");
        for (uint8_t p = 1; p <= ctrl->max_ports; p++) {
            xhci_disable_port(ctrl, p);
        }
    }

    kprintf("[HALT] Masking all IRQs...\n");
    for (uint8_t irq = 0; irq < irqchip_max_irqs(); irq++) {
        irqchip_disable_irq(irq);
    }
}

void system_halt(bool reboot) {
    __asm__ volatile("cli");

    kprintf("\n========================================\n");
    kprintf("  SYSTEM %s INITIATED\n", reboot ? "REBOOT" : "SHUTDOWN");
    kprintf("========================================\n");

    halt_all_ap_cores();
    halt_terminate_all_processes();
    halt_sync_storage();
    halt_stop_hardware();

    kprintf("[HALT] Cleanup complete.\n");

    if (reboot) {
        kprintf("[HALT] Rebooting...\n");
        acpi_reboot();
    } else {
        kprintf("[HALT] Powering off...\n");
        acpi_shutdown();
    }

    __builtin_unreachable();
}
