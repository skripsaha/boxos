#include "box/timeouts.h"
#include "box/bay.h"
#include "box/core/manifest.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"  /* SYSTEM_OP_BAY_* opcodes — single source */

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
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return 0;

    uint64_t user_va = 0;
    memcpy(&user_va, out, sizeof(uint64_t));
    return (void *)(uintptr_t)user_va;
}

/* The kernel's flags (bay.h) a Bay made for one program is created with:
 * create, only create, and for that program alone. Nobody passes them to
 * bay_open — such a Bay is made by naming its program, here. */
#define BAY_EXCLUSIVE   0x08u
#define BAY_FOR         0x10u

int bay_create_for(const char *tag, uint64_t size, uint32_t program_pid,
                   uint32_t program_generation, void **out)
{
    if (!out) return -ERR_NULL_POINTER;
    *out = 0;
    if (!tag || tag[0] == '\0' || size == 0 || program_pid == 0)
        return -ERR_INVALID_ARGUMENT;

    /* params: [u64 size][u32 flags][u32 program pid][u32 program generation] */
    uint32_t flags = BAY_CREATE | BAY_EXCLUSIVE | BAY_FOR;
    uint8_t  params[20];
    memcpy(params,      &size,               sizeof(uint64_t));
    memcpy(params + 8,  &flags,              sizeof(uint32_t));
    memcpy(params + 12, &program_pid,        sizeof(uint32_t));
    memcpy(params + 16, &program_generation, sizeof(uint32_t));

    /* Out: [u64 user_va][u64 actual_size] */
    uint8_t answer[16] = {0};

    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_BAY_OPEN,
                     params, sizeof(params),
                     tag, (uint32_t)(strlen(tag) + 1),
                     answer, sizeof(answer), 0,
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return box_fail(rc);

    uint64_t user_va = 0;
    memcpy(&user_va, answer, sizeof(uint64_t));
    *out = (void *)(uintptr_t)user_va;
    return OK;
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
                   BOX_ANSWER_GUARANTEED, 0);
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
                     BOX_ANSWER_GUARANTEED, 0);
    if (rc != 0) return 0;
    uint64_t size = 0;
    memcpy(&size, out, sizeof(uint64_t));
    return size;
}
