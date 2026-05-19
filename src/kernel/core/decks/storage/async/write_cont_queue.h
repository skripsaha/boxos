#ifndef WRITE_CONT_QUEUE_H
#define WRITE_CONT_QUEUE_H

#include "ktypes.h"
#include "error.h"

/*
 * Write-Continuation Queue — historical thin wrapper around the
 * generic irq_defer subsystem.
 *
 * Originally an MPSC linked-list with per-node kmalloc, which was
 * the central instance of the "kmalloc-from-IRQ" deadlock pattern
 * the 2026-05-17 audit flagged. The implementation now delegates to
 * irq_defer (elastic chunked ring, IRQ-safe, no allocation on the
 * producer side). The old types and globals are gone — callers must
 * use the function API only.
 *
 * Kept as a separate translation unit so the storage subsystem can
 * grow a richer write-pipeline API later (priority lanes, fairness
 * quotas) without burdening the universal irq_defer layer.
 */

typedef void (*WriteContFn)(void *job);

/* No-op; preserved so existing init wiring compiles. The real ring
 * setup happens in irq_defer_init() called from main.c. */
void WriteContQueueInit(void);

/* Enqueue a continuation on the irq_defer ring of the *calling core*.
 * Safe to call from IRQ context. Returns OK on success, ERR_NO_MEMORY
 * only when the underlying ring is mid-init (which the audit-tested
 * boot order makes a non-issue in practice). */
error_t WriteContEnqueue(WriteContFn fn, void *job);

/* Consumer drain — calls irq_defer_pump for the given core. Returns
 * the number of continuations invoked. */
uint32_t WriteContPump(uint8_t kcore_idx);

/* Approximate pending count for routing / halt-drain. */
uint32_t WriteContDepth(uint8_t kcore_idx);

#endif /* WRITE_CONT_QUEUE_H */
