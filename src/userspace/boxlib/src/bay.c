#include "box/bay.h"
#include "box/core/manifest.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"

/* Opcodes must mirror src/kernel/core/decks/system/system_deck.h */
#define SYSTEM_OP_BAY_OPEN     0x70
#define SYSTEM_OP_BAY_RELEASE  0x71
#define SYSTEM_OP_BAY_SIZE     0x72

void *bay_open(const char *tag, uint64_t size, uint32_t flags)
{
    if (!tag || tag[0] == '\0') return 0;

    /* params: [u64 size][u32 flags]  (12 bytes) */
    uint8_t params[12];
    memcpy(params,     &size,  sizeof(uint64_t));
    memcpy(params + 8, &flags, sizeof(uint32_t));

    /* Out: [u64 user_va][u64 actual_size] */
    uint8_t out[16] = {0};

    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_BAY_OPEN,
                     params, sizeof(params),
                     tag, (uint32_t)(strlen(tag) + 1),
                     out, sizeof(out), 0,
                     30000, 0);
    if (rc != 0) return 0;

    uint64_t user_va = 0;
    memcpy(&user_va, out, sizeof(uint64_t));
    return (void *)(uintptr_t)user_va;
}

int bay_release(void *ptr)
{
    if (!ptr) return -ERR_INVALID_ARGS;
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    uint8_t params[8];
    memcpy(params, &va, sizeof(uint64_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_BAY_RELEASE,
                   params, sizeof(params),
                   0, 0, 0, 0, 0,
                   30000, 0);
}

uint64_t bay_size(void *ptr)
{
    if (!ptr) return 0;
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    uint8_t params[8];
    memcpy(params, &va, sizeof(uint64_t));

    uint8_t out[8] = {0};
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_BAY_SIZE,
                     params, sizeof(params),
                     0, 0,
                     out, sizeof(out), 0,
                     30000, 0);
    if (rc != 0) return 0;
    uint64_t size = 0;
    memcpy(&size, out, sizeof(uint64_t));
    return size;
}
