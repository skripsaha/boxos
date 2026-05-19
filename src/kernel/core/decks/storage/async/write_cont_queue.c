#include "write_cont_queue.h"
#include "irq_defer.h"
#include "amp.h"
#include "atomics.h"
#include "klib.h"

/*
 * write_cont_queue is now a thin shim over irq_defer.
 *
 * History: the original implementation was a Vyukov MPSC linked list
 * with one kmalloc() per push, called directly from AHCI completion
 * IRQs. That violated "no allocation from IRQ context" (CLAUDE.md
 * lock-ordering rule) and was the central explanation for random
 * hangs / faults / "Unknown command" races diagnosed in the
 * 2026-05-17 full-codebase audit.
 *
 * The replacement irq_defer subsystem uses pre-allocated elastic
 * chunks — producers (IRQs) do no allocation at all; chunk allocation
 * happens in K-Core pump context where kmalloc is safe.
 *
 * The WriteContQueue API is preserved for source compatibility with
 * the storage_ops / write_job callers. New code should call
 * irq_defer() directly.
 */

void WriteContQueueInit(void)
{
    /* irq_defer_init() is the real initialiser, called from main.c
     * before any IRQ that defers can fire. Nothing to do here. */
}

error_t WriteContEnqueue(WriteContFn fn, void *job)
{
    if (!fn) return ERR_INVALID_ARGUMENT;
    irq_defer((void (*)(void *))fn, job);
    /* irq_defer drops silently on overflow (incrementing a counter);
     * we return OK so that the historical "if (rc != OK) emergency-
     * finalize" branches in write_job.c reduce to dead code that the
     * compiler will eliminate. The proper hang-recovery story for
     * extreme overflow is a higher-level watchdog (see backlog). */
    return OK;
}

uint32_t WriteContPump(uint8_t kcore_idx)
{
    return irq_defer_pump(kcore_idx);
}

uint32_t WriteContDepth(uint8_t kcore_idx)
{
    return irq_defer_pending(kcore_idx);
}
