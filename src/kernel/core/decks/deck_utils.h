#ifndef DECK_UTILS_H
#define DECK_UTILS_H

#include "ktypes.h"
#include "klib.h"


static inline uint16_t deck_read_u16(const void *buf)
{
    uint16_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
}

static inline uint32_t deck_read_u32(const void *buf)
{
    uint32_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
}

static inline uint64_t deck_read_u64(const void *buf)
{
    uint64_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
}

static inline void deck_write_u16(void *buf, uint16_t v)
{
    memcpy(buf, &v, sizeof(v));
}

static inline void deck_write_u32(void *buf, uint32_t v)
{
    memcpy(buf, &v, sizeof(v));
}

static inline void deck_write_u64(void *buf, uint64_t v)
{
    memcpy(buf, &v, sizeof(v));
}

#endif