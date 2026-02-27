#ifndef ENDIAN_H
#define ENDIAN_H

#include "ktypes.h"

// ============================================================================
// BOXOS ENDIANNESS SUPPORT
// ============================================================================
// BoxOS is currently little-endian only (x86-64 architecture)
// These helpers provide future portability for other architectures

// Platform endianness
#define BOXOS_LITTLE_ENDIAN 1
#define BOXOS_BIG_ENDIAN    0

#if BOXOS_LITTLE_ENDIAN
    // No-op conversions for little-endian (x86-64)
    #define cpu_to_le16(x) (x)
    #define cpu_to_le32(x) (x)
    #define cpu_to_le64(x) (x)
    #define le16_to_cpu(x) (x)
    #define le32_to_cpu(x) (x)
    #define le64_to_cpu(x) (x)

    // Big-endian conversions (need byte swap)
    #define cpu_to_be16(x) bswap16(x)
    #define cpu_to_be32(x) bswap32(x)
    #define cpu_to_be64(x) bswap64(x)
    #define be16_to_cpu(x) bswap16(x)
    #define be32_to_cpu(x) bswap32(x)
    #define be64_to_cpu(x) bswap64(x)
#else
    // TODO: Implement for big-endian platforms
    #error "Big-endian platforms not yet supported"
#endif

// Explicit byte swap functions (for debugging/testing)
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

// Network byte order helpers (network is big-endian)
#define htons(x)  cpu_to_be16(x)  // Host to network short
#define htonl(x)  cpu_to_be32(x)  // Host to network long
#define htonll(x) cpu_to_be64(x)  // Host to network long long
#define ntohs(x)  be16_to_cpu(x)  // Network to host short
#define ntohl(x)  be32_to_cpu(x)  // Network to host long
#define ntohll(x) be64_to_cpu(x)  // Network to host long long

#endif // ENDIAN_H
