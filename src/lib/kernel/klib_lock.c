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

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
    lock->saved_flags = 0;
}

void spin_lock(spinlock_t *lock)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");

    while (__sync_lock_test_and_set(&lock->locked, 1))
    {
        asm volatile("pause");
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
