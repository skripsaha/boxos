#ifndef ENDIAN_H
#define ENDIAN_H

#include "ktypes.h"

// x86-64 only (little-endian); these macros exist for future portability
#define LITTLE_ENDIAN 1
#define BIG_ENDIAN    0

#if LITTLE_ENDIAN
    #define cpu_to_le16(x) (x)
    #define cpu_to_le32(x) (x)
    #define cpu_to_le64(x) (x)
    #define le16_to_cpu(x) (x)
    #define le32_to_cpu(x) (x)
    #define le64_to_cpu(x) (x)

    #define cpu_to_be16(x) bswap16(x)
    #define cpu_to_be32(x) bswap32(x)
    #define cpu_to_be64(x) bswap64(x)
    #define be16_to_cpu(x) bswap16(x)
    #define be32_to_cpu(x) bswap32(x)
    #define be64_to_cpu(x) bswap64(x)
#else
    #error "Big-endian platforms not yet supported"
#endif

static inline uint16_t bswap16(uint16_t x) {
    return (x >> 8) | (x << 8);
}

static inline uint32_t bswap32(uint32_t x) {
    return ((x >> 24) & 0x000000FFUL) |
           ((x >>  8) & 0x0000FF00UL) |
           ((x <<  8) & 0x00FF0000UL) |
           ((x << 24) & 0xFF000000UL);
}

static inline uint64_t bswap64(uint64_t x) {
    return ((x >> 56) & 0x00000000000000FFULL) |
           ((x >> 40) & 0x000000000000FF00ULL) |
           ((x >> 24) & 0x0000000000FF0000ULL) |
           ((x >>  8) & 0x00000000FF000000ULL) |
           ((x <<  8) & 0x000000FF00000000ULL) |
           ((x << 24) & 0x0000FF0000000000ULL) |
           ((x << 40) & 0x00FF000000000000ULL) |
           ((x << 56) & 0xFF00000000000000ULL);
}

#define htons(x)  cpu_to_be16(x)
#define htonl(x)  cpu_to_be32(x)
#define htonll(x) cpu_to_be64(x)
#define ntohs(x)  be16_to_cpu(x)
#define ntohl(x)  be32_to_cpu(x)
#define ntohll(x) be64_to_cpu(x)

#endif // ENDIAN_H
