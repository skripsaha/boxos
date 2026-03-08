#ifndef DECK_UTILS_H
#define DECK_UTILS_H

#include "ktypes.h"

static inline uint16_t deck_read_u16(const uint8_t *buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static inline uint32_t deck_read_u32(const uint8_t *buf, size_t offset) {
    return ((uint32_t)buf[offset]     << 24) |
           ((uint32_t)buf[offset + 1] << 16) |
           ((uint32_t)buf[offset + 2] << 8)  |
            (uint32_t)buf[offset + 3];
}

static inline uint64_t deck_read_u64(const uint8_t *buf) {
    return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[6] << 8)  |  (uint64_t)buf[7];
}

static inline void deck_write_u16(uint8_t *buf, uint16_t value) {
    buf[0] = (value >> 8) & 0xFF;
    buf[1] =  value       & 0xFF;
}

static inline void deck_write_u32(uint8_t *buf, size_t offset, uint32_t value) {
    buf[offset]     = (value >> 24) & 0xFF;
    buf[offset + 1] = (value >> 16) & 0xFF;
    buf[offset + 2] = (value >> 8)  & 0xFF;
    buf[offset + 3] =  value        & 0xFF;
}

static inline void deck_write_u64(uint8_t *buf, uint64_t value) {
    buf[0] = (value >> 56) & 0xFF;
    buf[1] = (value >> 48) & 0xFF;
    buf[2] = (value >> 40) & 0xFF;
    buf[3] = (value >> 32) & 0xFF;
    buf[4] = (value >> 24) & 0xFF;
    buf[5] = (value >> 16) & 0xFF;
    buf[6] = (value >> 8)  & 0xFF;
    buf[7] =  value        & 0xFF;
}

#endif // DECK_UTILS_H
