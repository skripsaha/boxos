#ifndef KRYPTO_H
#define KRYPTO_H

#include "ktypes.h"

// ============================================================================
// BoxOS Cryptographic Utilities
// Common hash and checksum functions used across the kernel
// ============================================================================

// CRC32 (ISO 3309 polynomial)
// Used for: the Ledger, the boarding pass, GPT, DiskBook records, EFI tables
uint32_t KCrc32(const uint8_t *data, uint32_t len);

// The same CRC32 carried on over more bytes: KCrc32(a ++ b) is
// KCrc32Continue(KCrc32(a), b), and KCrc32Continue(0, x) is KCrc32(x). A text
// longer than one piece of memory, or than a u32 length, is summed a piece at
// a time.
uint32_t KCrc32Continue(uint32_t crc, const uint8_t *data, uint32_t len);

// CRC16 (CCITT-FALSE)
// Used for: metadata pool record integrity
uint16_t KCrc16(const uint8_t *data, uint32_t len);

// The same CRC16 carried on over more bytes: KCrc16(a ++ b) is
// KCrc16Continue(KCrc16(a), b). The sum takes no final step, so a record can
// be summed in pieces — a field it stores zeroed, its sum's own, fed as zeros —
// without copying it into one buffer first.
uint16_t KCrc16Continue(uint16_t crc, const uint8_t *data, uint32_t len);

// SHA-256
// Used for: secure hashing, future cryptographic needs
void KSha256(const uint8_t *data, uint32_t len, uint8_t *out_hash);

// SHA-256 streaming API. Use Init → Update*N → Final to hash data that
// arrives in chunks (e.g. PE Authenticode walk over disjoint regions of
// a large image without copying the image into one contiguous buffer).
//
//   KSha256Init   — reset the context to initial state.
//   KSha256Update — absorb `len` bytes; may be called any number of times.
//                   No allocation; the internal 64-byte buffer holds at
//                   most one block of leftover bytes between updates.
//   KSha256Final  — produce the 32-byte digest and reset the context.
//
// All functions are no-allocation and IRQ-safe; the only kernel
// dependency is memcpy/memset via klib.
typedef struct {
    uint32_t state[8];        // 256-bit running hash
    uint64_t total_bits;      // length in bits (per FIPS 180-4)
    uint32_t buffer_len;      // bytes currently in `buffer` (< 64)
    uint8_t  buffer[64];      // partial block
} KSha256Ctx;

void KSha256Init(KSha256Ctx *ctx);
void KSha256Update(KSha256Ctx *ctx, const uint8_t *data, uint32_t len);
void KSha256Final(KSha256Ctx *ctx, uint8_t *out_hash);

// Simple checksum8 (8-bit additive checksum)
// Used for: lightweight integrity checks
uint8_t KChecksum8(const uint8_t *data, uint32_t len);

// Simple checksum16 (16-bit additive checksum)
// Used for: medium integrity checks
uint16_t KChecksum16(const uint8_t *data, uint32_t len);

// Simple checksum32 (32-bit additive checksum)
// Used for: general purpose checksums
uint32_t KChecksum32(const uint8_t *data, uint32_t len);

// FNV-1a hash (fast, non-cryptographic)
// Used for: hash tables, bloom filters
uint32_t KFnv1a(const uint8_t *data, uint32_t len);

// MurmurHash3 finalizer (for mixing)
// Used for: additional hash needs
uint32_t KMurmur3Finalize(uint32_t h);

#endif // KRYPTO_H
