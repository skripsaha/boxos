#include "box/sync.h"
#include "box/core/manifest.h"
#include "box/error.h"
#include "box/string.h"
#include "boxos_decks.h"
#include "box/timeouts.h"

error_t addr_park(const volatile void *addr, uint64_t expected,
                  uint32_t timeout_ms)
{
    if (!addr) return ERR_INVALID_ARGUMENT;

    uint64_t user_va = (uint64_t)(uintptr_t)addr;

    uint8_t params[20];
    memcpy(params,      &user_va,    sizeof(uint64_t));
    memcpy(params + 8,  &expected,   sizeof(uint64_t));
    memcpy(params + 16, &timeout_ms, sizeof(uint32_t));

    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_ADDR_PARK,
                     params, sizeof(params),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);

    if (rc == 0) return OK;
    if (rc < 0)  return (error_t)(-rc);
    return (error_t)rc;
}

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