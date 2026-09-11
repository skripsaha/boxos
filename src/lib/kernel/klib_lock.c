#include "klib.h"

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
    __sync_lock_release(&lock->locked);
}