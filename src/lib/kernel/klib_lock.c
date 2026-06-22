/* klib_lock.c — IRQ-safe spinlock primitives for the kernel.
 *
 * Acquire path: save RFLAGS, disable IRQs, then CAS-acquire. Storing
 * saved_flags AFTER acquisition is mandatory — writing it before the
 * lock is owned races with a peer CPU that takes the lock and then
 * overwrites the slot from beneath us.
 *
 * Release path: snapshot saved_flags into a local BEFORE __sync_lock_release
 * so a peer that grabs the lock immediately cannot rewrite the slot we
 * were about to read.
 *
 * spin_force_release skips the saved-flags restore; it exists for fatal
 * IST recovery paths (kernel-stack overflow, panic) where the original
 * holder's frame is unreachable. */
#include "klib.h"

/* Hook drained by spin_lock() while it spins with IRQs disabled — see the
 * declaration in klib.h. RELEASE on publish / ACQUIRE on read so the function
 * pointer is visible to a peer core before it could be invoked. */
static spin_wait_service_fn g_spin_wait_service = 0;

void spin_set_wait_service(spin_wait_service_fn fn)
{
    __atomic_store_n(&g_spin_wait_service, fn, __ATOMIC_RELEASE);
}

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
    lock->saved_flags = 0;
}

void spin_lock(spinlock_t *lock)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");

    if (__sync_lock_test_and_set(&lock->locked, 1))
    {
        /* Contended. We spin with IRQs OFF, so a cross-core TLB shootdown
         * targeting this core cannot be ACKed via its IPI vector until we
         * acquire the lock and restore IRQs — stall long enough and the
         * initiator times out and panics (the M1 real-HW deadlock). Drain
         * shootdowns for this core inline each iteration; the serviced handler
         * is lock-free, generation-gated and idempotent, so it composes with
         * real IPI delivery and is safe to run while we still hold no lock. */
        spin_wait_service_fn svc =
            __atomic_load_n(&g_spin_wait_service, __ATOMIC_ACQUIRE);
        do
        {
            if (svc) svc();
            asm volatile("pause");
        } while (__sync_lock_test_and_set(&lock->locked, 1));
    }

    lock->saved_flags = flags;
}

void spin_unlock(spinlock_t *lock)
{
    uint64_t flags = lock->saved_flags;
    __sync_lock_release(&lock->locked);
    asm volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
}

bool spin_trylock(spinlock_t *lock)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");

    if (!__sync_lock_test_and_set(&lock->locked, 1))
    {
        lock->saved_flags = flags;
        return true;
    }

    asm volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
    return false;
}

void spin_force_release(spinlock_t *lock)
{
    /* Fatal-path-only: release without restoring IRQ state.
     * Caller must hold no other locks that depend on RFLAGS restoration
     * (panic / IST stack-overflow recovery are the legitimate use sites). */
    __sync_lock_release(&lock->locked);
}
