#ifndef BOXOS_CRATE_H
#define BOXOS_CRATE_H

/*
 * Shared kernel/userspace ABI header.
 *
 * Includer must provide uint*_t and bool BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/defs.h"
 */

#ifndef __packed
#define __packed __attribute__((packed))
#endif

/*
 * Crate — variable-size buffer descriptor.
 *
 * Replaces every fixed-size syscall payload struct (obj_*_request_t,
 * obj_*_response_t, etc.). A Crate is a single descriptor pointing at a region
 * of the cabin's user heap. Operations read from INPUT crates and write into
 * OUTPUT crates; sizes are runtime, never compile-time.
 *
 * Lifetime:
 *   - User allocates payload at addr (via SYSTEM_OP_BUF_ALLOC or local heap).
 *   - User builds a Crate descriptor pointing at addr.
 *   - Kernel translates addr via vmm_translate_user_addr() and reads/writes.
 *   - User reclaims payload after the Pocket completes.
 *
 * The kernel never copies payload bytes into bounded stack buffers — this is
 * the central architectural fix that removes 168/176-byte limits.
 */

#define CRATE_MAGIC 0x43525441u  /* 'CRTA' */

typedef enum {
    CRATE_KIND_INPUT  = 0,  /* op reads only */
    CRATE_KIND_OUTPUT = 1,  /* op writes only; op may set size to actual produced bytes */
    CRATE_KIND_INOUT  = 2   /* op reads then writes (in-place transform) */
} CrateKind;

#define CRATE_FLAG_GROWABLE  (1u << 0)  /* capacity may auto-grow on write overflow */
#define CRATE_FLAG_DMA_OK    (1u << 1)  /* pinned and physically contiguous, suitable for DMA */
#define CRATE_FLAG_ZERO_COPY (1u << 2)  /* shared mapping with another cabin */
#define CRATE_FLAG_PINNED    (1u << 3)  /* kernel must not relocate during op */

typedef struct __packed {
    uint32_t magic;     /* CRATE_MAGIC */
    uint32_t flags;     /* CRATE_FLAG_* */
    uint64_t addr;      /* user vaddr of payload */
    uint64_t size;      /* valid bytes currently at addr (op may update) */
    uint64_t capacity;  /* total allocated bytes at addr */
    uint16_t kind;      /* CrateKind */
    uint16_t _pad0;
    uint32_t _pad1;
} Crate;

_Static_assert(sizeof(Crate) == 40, "Crate descriptor must be 40 bytes");

static inline bool CrateIsValid(const Crate *c)
{
    if (!c) return false;
    if (c->magic != CRATE_MAGIC) return false;
    if (c->size > c->capacity) return false;
    if (c->capacity > 0 && c->addr == 0) return false;
    if (c->kind > CRATE_KIND_INOUT) return false;
    return true;
}

#endif /* BOXOS_CRATE_H */
