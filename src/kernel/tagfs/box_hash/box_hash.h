#ifndef BOX_HASH_H
#define BOX_HASH_H

#include "../../lib/kernel/ktypes.h"

// BoxHash — Cryptographically Secure Hash for BoxOS
// Features:
//   • 256-bit output (collision resistant like SHA-256)
//   • Salt + Key support (HMAC-style security)
//   • Fast for 4KB blocks (optimized for filesystem)
//   • Unique to BoxOS (our custom algorithm)
//
// Security Level:
//   • BoxHashCompute: Integrity (bit rot detection)
//   • BoxHashComputeSecure: Security (attack resistance)
//   • BoxHashComputeSHA256: Maximum security (standard SHA-256)

#define BOX_HASH_SIZE      256
#define BOX_HASH_BYTES     (BOX_HASH_SIZE / 8)  // 32 bytes
#define BOX_HASH_SALT_SIZE 16
#define BOX_HASH_KEY_SIZE  32

typedef struct {
    uint8_t bytes[BOX_HASH_BYTES];
} BoxHash;

typedef struct {
    uint8_t salt[BOX_HASH_SALT_SIZE];
    uint8_t key[BOX_HASH_KEY_SIZE];
    bool    key_initialized;
} BoxHashContext;

// Initialize context with random salt and optional key
void BoxHashInit(BoxHashContext *ctx);
void BoxHashInitWithKey(BoxHashContext *ctx, const uint8_t *key, uint32_t key_size);

// Compute hash (fast path - integrity checking)
BoxHash BoxHashCompute(const void *data, uint32_t size, const BoxHashContext *ctx);

// Compute hash (secure path - attack resistance)
BoxHash BoxHashComputeSecure(const void *data, uint32_t size, const BoxHashContext *ctx);

// Compute SHA-256 hash (maximum security, slower)
BoxHash BoxHashComputeSHA256(const void *data, uint32_t size);

// Compare two hashes (constant-time to prevent timing attacks)
bool BoxHashEqual(const BoxHash *a, const BoxHash *b);

// Convert to hex string (for debugging)
void BoxHashToHex(const BoxHash *hash, char *out, uint32_t out_size);

// Verify hash (returns true if valid)
bool BoxHashVerify(const void *data, uint32_t size, const BoxHash *expected_hash, const BoxHashContext *ctx);

#endif // BOX_HASH_H
