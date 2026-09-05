#include "box/sync.h"
#include "box/core/manifest.h"
#include "box/error.h"
#include "box/string.h"
#include "boxos_decks.h"  /* SYSTEM_OP_ADDR_PARK, SYSTEM_OP_ADDR_WAKE */
#include "box/timeouts.h" /* BOX_ANSWER_GUARANTEED */

/*
 * addr_park — park until *addr != expected, or timeout.
 *
 * params: [u64 user_va][u64 expected][u32 timeout_ms]  (20 bytes)
 * Returns kernel error code translated to error_t.
 * ERR_WOULD_BLOCK from the kernel means successfully parked and woken —
 * guide.c delivers this as a result with error_code==ERR_WOULD_BLOCK only
 * while parked; on actual wake the scheduler reschedules and the caller
 * gets back OK from the result ring.  We treat OK from MfCall1 as success.
 */
error_t addr_park(const volatile void *addr, uint64_t expected,
                  uint32_t timeout_ms)
{
    if (!addr) return ERR_INVALID_ARGUMENT;

    uint64_t user_va = (uint64_t)(uintptr_t)addr;

    uint8_t params[20];
    memcpy(params,      &user_va,    sizeof(uint64_t));
    memcpy(params + 8,  &expected,   sizeof(uint64_t));
    memcpy(params + 16, &timeout_ms, sizeof(uint32_t));

    /* A wait without a deadline is exactly that: the answer is the wake,
     * whenever it comes, and the kernel arms no timer for it. What stood here
     * was 30 s "for forever" — a re-arm every half minute that woke the
     * sleeper for nothing and blanketed a lost wake, which Nightwatch now
     * names on facts instead.
     *
     * A deadline the caller DID set is the kernel's to keep: SysAddrPark arms
     * it and delivers ERR_TIMEOUT as this submit's own Result when it passes
     * (SyncTimeoutDeliver). The call budget below only backstops that
     * delivery, which rides one irq_defer hop that admits it can drop; it
     * saturates instead of wrapping, so a near-UINT32_MAX budget (std::mutex's
     * RemainingMs clamps there for a far-future deadline) parks once instead
     * of re-entering the kernel ten times a second. */
    uint32_t call_timeout;
    if (timeout_ms == 0)                     call_timeout = BOX_ANSWER_GUARANTEED;
    else if (timeout_ms > 0xFFFFFFFFu - 100) call_timeout = 0xFFFFFFFFu;
    else                                     call_timeout = timeout_ms + 100;

    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_ADDR_PARK,
                     params, sizeof(params),
                     NULL, 0, NULL, 0, NULL,
                     call_timeout, NULL);

    if (rc == 0) return OK;
    if (rc < 0)  return (error_t)(-rc);
    return (error_t)rc;
}

/*
 * addr_wake — wake up to `count` processes parked on `addr`.
 *
 * params: [u64 user_va][u32 count]  (12 bytes)
 */
error_t addr_wake(const volatile void *addr, uint32_t count)
{
    if (!addr) return ERR_INVALID_ARGUMENT;

    uint64_t user_va = (uint64_t)(uintptr_t)addr;

    uint8_t params[12];
    memcpy(params,     &user_va, sizeof(uint64_t));
    memcpy(params + 8, &count,   sizeof(uint32_t));

    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_ADDR_WAKE,
                     params, sizeof(params),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);

    if (rc == 0) return OK;
    if (rc < 0)  return (error_t)(-rc);
    return (error_t)rc;
}
