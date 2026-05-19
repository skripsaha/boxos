#include "system_halt.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"
#include "irq_defer.h"
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
#include "touch.h"
#include "write_cont_queue.h"
#include "ahci.h"
#include "amp.h"

static void halt_delay_ms(uint32_t ms)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while (rdtsc() < deadline)
    {
        __asm__ volatile("pause");
    }
}

/*
 * Drain in-flight async storage I/O before we kill cores. Two targets:
 *   1. Per-K-Core WriteContQueue — state-machine continuations posted
 *      by the AHCI IRQ that haven't been pumped yet.
 *   2. AHCI port command issue (CI/SACT) — commands the controller is
 *      still executing.
 * We poll both until they're idle or the timeout elapses. Other cores
 * are still live at this point (cli not yet executed) so their guide
 * loops keep pumping continuations naturally.
 */
static void halt_drain_async_writes(void)
{
    kprintf("[HALT] Draining async writes...\n");
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(3000);
    while (rdtsc() < deadline) {
        bool any = false;

        /* Check pending continuations via the irq_defer accessor —
         * the old WriteContQueue global is gone. */
        for (uint8_t i = 0; i < g_amp.total_cores; i++) {
            if (irq_defer_pending(i) > 0) {
                any = true;
                break;
            }
        }

        if (!any && ahci_is_initialized()) {
            uint32_t mask = ahci_get_active_port_mask();
            for (uint8_t p = 0; p < 32 && mask; p++) {
                if (!(mask & (1u << p))) continue;
                mask &= ~(1u << p);
                volatile ahci_port_regs_t *regs = ahci_get_port_regs_pub(p);
                if (regs && (regs->ci != 0 || regs->sact != 0)) {
                    any = true;
                    break;
                }
            }
        }

        if (!any) break;
        __asm__ volatile("pause");
    }
    kprintf("[HALT] Async writes drained\n");
}

static void halt_all_ap_cores(void)
{
    if (!g_amp.multicore_active)
        return;

    kprintf("[HALT] Stopping all AP cores...\n");

    lapic_send_ipi_all_excluding_self(IPI_PANIC_VECTOR);

    halt_delay_ms(5);

    uint8_t my_index = amp_get_core_index();
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        if (c == my_index)
            continue;
        g_amp.cores[c].online = false;
    }

    kprintf("[HALT] All AP cores stopped\n");
}

static void halt_detach_all_schedulers(void)
{
    // Clear current_process on every core's scheduler.
    // AP cores are already in cli;hlt — they will never touch these pointers again.
    // This prevents process_destroy() from refusing to destroy "current" processes.
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        scheduler_state_t *s = scheduler_get_core(c);
        s->current_process = NULL;
    }
}

static void halt_terminate_all_processes(void)
{
    kprintf("[HALT] Terminating all processes...\n");

    // Detach all processes from scheduler before destruction
    halt_detach_all_schedulers();

    process_cleanup_queue_flush();

    uint32_t pids[256];
    uint32_t pid_count = 0;

    process_list_lock();
    process_t *proc = process_get_first();
    while (proc && pid_count < 256)
    {
        pids[pid_count++] = proc->pid;
        proc = proc->next;
    }
    process_list_unlock();

    uint32_t killed = 0;
    for (uint32_t i = 0; i < pid_count; i++)
    {
        process_t *p = process_find(pids[i]);
        if (p)
        {
            process_set_state(p, PROC_DONE);
            process_destroy(p);
            killed++;
        }
    }

    process_cleanup_queue_flush();

    kprintf("[HALT] %u processes terminated\n", killed);
}

static void halt_sync_storage(void)
{
    kprintf("[HALT] Syncing TagFS...\n");
    tagfs_shutdown();

    kprintf("[HALT] Flushing disk cache...\n");
    ata_flush_cache(1);

    kprintf("[HALT] Storage sync complete\n");
}

static void halt_stop_hardware(void)
{
    xhci_controller_t *ctrl = xhci_get_controller();
    if (ctrl && ctrl->initialized)
    {
        kprintf("[HALT] Stopping USB controller...\n");
        for (uint8_t p = 1; p <= ctrl->max_ports; p++)
        {
            xhci_disable_port(ctrl, p);
        }
    }

    kprintf("[HALT] Masking all IRQs...\n");
    for (uint8_t irq = 0; irq < irqchip_max_irqs(); irq++)
    {
        irqchip_disable_irq(irq);
    }
}

void system_halt(bool reboot)
{
    /* Publish system:shutdown/system:reboot BEFORE we cli, so subscribers
     * (e.g. user daemons holding open files / dirty caches) get woken on
     * their App-Cores while interrupts and the scheduler still work. The
     * grace_ms hint is currently nominal — the actual halt path doesn't
     * await acks, but a future scheduler tick can be added between
     * publish and `cli` to give listeners a chance to run. */
    {
        struct __attribute__((packed)) {
            uint32_t reason;
            uint32_t grace_ms;
        } hint = { 0u, 50u };
        TouchPublish(reboot ? "system:reboot" : "system:shutdown",
                     &hint, sizeof(hint));
    }

    /* Drain any pending Touch deliveries and let App-Cores run subscribers
     * for a brief window before we kill interrupts. 50 ms is a hint — apps
     * that need more should checkpoint on every state change, not on
     * shutdown alone. */
    halt_delay_ms(50);

    /* Drain BEFORE cli — other cores need their guide loops alive to
     * pump pending continuations from the AHCI IRQ. */
    halt_drain_async_writes();

    __asm__ volatile("cli");

    kprintf("%s initiated\n", reboot ? "reboot" : "shutdown");

    halt_all_ap_cores();
    halt_terminate_all_processes();
    halt_sync_storage();
    halt_stop_hardware();

    kprintf("[HALT] Cleanup complete.\n");

    if (reboot)
    {
        kprintf("[HALT] Rebooting...\n");
        acpi_reboot();
    }
    else
    {
        kprintf("[HALT] Powering off...\n");
        acpi_shutdown();
    }

    __builtin_unreachable();
}
