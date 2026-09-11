#ifndef BOXOS_CRATE_H
#define BOXOS_CRATE_H


#ifndef __packed
#define __packed __attribute__((packed))
#endif


#define CRATE_MAGIC 0x43525441u

typedef enum {
    CRATE_KIND_INPUT  = 0,
    CRATE_KIND_OUTPUT = 1,
    CRATE_KIND_INOUT  = 2
} CrateKind;

#define CRATE_FLAG_GROWABLE  (1u << 0)
#define CRATE_FLAG_DMA_OK    (1u << 1)
#define CRATE_FLAG_ZERO_COPY (1u << 2)
#define CRATE_FLAG_PINNED    (1u << 3)

typedef struct __packed {
    uint32_t magic;
    uint32_t flags;
    uint64_t addr;
    uint64_t size;
    uint64_t capacity;
    uint16_t kind;
    uint16_t _pad0;
    uint32_t _pad1;
} Crate;

#if !defined(__cplusplus) && !defined(static_assert) && \
    (!defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L))
#define static_assert _Static_assert
#endif

static_assert(sizeof(Crate) == 40, "Crate descriptor must be 40 bytes");

static inline bool CrateIsValid(const Crate *c)
{
    if (!c) return false;
    if (c->magic != CRATE_MAGIC) return false;
    if (c->size > c->capacity) return false;
    if (c->capacity > 0 && c->addr == 0) return false;
    if (c->kind > CRATE_KIND_INOUT) return false;
    return true;
}

#endif