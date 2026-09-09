#include "serial.h"      /* WireDrain — the last words are heard */
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
#include "baton.h"
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
 * Drain in-flight async storage I/O before we kill cores. Three targets:
 *   1. Per-core irq_defer rings — SCI/GPE/ATA/AHCI-recovery bottom-halves.
 *   2. The never-drop storage completion queue — read/write state-machine
 *      continuations posted by the AHCI IRQ that haven't been consumed yet
 *      (a pending WRITE completion still owes its tagfs commit, so this
 *      must drain before we cut power or data is lost).
 *   3. AHCI port command issue (CI/SACT) — commands the controller is
 *      still executing.
 * We poll all until idle or the timeout elapses. Other cores are still
 * live at this point (cli not yet executed) so their guide loops keep
 * pumping continuations naturally.
 */
static void halt_drain_async_writes(void)
{
    kprintf("[HALT] Draining async writes...\n");
    uint8_t  me       = amp_get_core_index();
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(3000);
    while (rdtsc() < deadline) {
        bool any = false;

        /* Pump THIS core's own queues before polling. system_halt runs in
         * guide-loop context on whichever K-Core picked up the reboot/poweroff
         * pocket — and that can be the BSP, which is the SOLE consumer of the
         * never-drop storage completion queue (all completions route there).
         * While we spin here the BSP is no longer in kcore_run_loop, so nothing
         * else can drain its queue: a completion that lands during the drain
         * (interrupts are still on before the cli below) would be held forever
         * and its write's tagfs-commit continuation lost. Draining our own
         * index each pass is a valid single-consumer pump (me == this core) and
         * a harmless empty early-out on a non-drain core; other cores keep
         * pumping their own queues from their guide loops. irq_defer is
         * multi-consumer, but our own ring is likewise stranded here, so pump
         * it too. */
        BatonPump(me);
        irq_defer_pump(me);

        /* Pending irq_defer bottom-halves AND unconsumed never-drop storage
         * completions, across all cores. BatonOutstanding is
         * counter-based, so it is safe to poll from this (possibly non-owning)
         * core while the drain core keeps pumping. */
        for (uint8_t i = 0; i < g_amp.total_cores; i++) {
            if (irq_defer_pending(i) > 0 || BatonOutstanding(i)) {
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
        /* Release store pairs with amp_core_online's acquire load on the
         * other side, so peers that still iterate g_amp.cores[] see this
         * core leave the "online" set with proper cross-CPU visibility. */
        __atomic_store_n(&g_amp.cores[c].online, (uint8_t)0, __ATOMIC_RELEASE);
    }

    /* Every AP is now in cli;hlt (online=0) and will never run again. If one
     * was stopped while holding a process-subsystem lock (e.g. mid strand-
     * reaper), reclaim those locks now so the shutdown walk below cannot spin
     * on them forever. Safe precisely because no other core is alive to race. */
    process_force_release_locks_for_shutdown();

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
    tagfs_flush_cache();

    kprintf("[HALT] Storage sync complete\n");
}

static void halt_stop_hardware(void)
{
    for (uint8_t ci = 0; ci < xhci_controller_count(); ci++)
    {
        xhci_controller_t *ctrl = xhci_controller_at(ci);
        if (!ctrl || !ctrl->initialized)
        {
            continue;
        }
        kprintf("[HALT] Stopping USB controller %u...\n", ci);
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
    kprintf(reboot ? "[HALT] Rebooting...\n" : "[HALT] Powering off...\n");

    /* The last words onto the wire before the machine goes: interrupts are
     * off, so the line cannot drive itself from here. */
    WireDrain();

    if (reboot) acpi_reboot();
    else        acpi_shutdown();

    __builtin_unreachable();
}
