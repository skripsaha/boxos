#ifndef KRYPTO_H
#define KRYPTO_H

#include "ktypes.h"

// ============================================================================
// BoxOS Cryptographic Utilities
// Common hash and checksum functions used across the kernel
// ============================================================================

// CRC32 (ISO 3309 polynomial)
// Used for: superblock integrity, disk entries, self-heal mirrors
uint32_t KCrc32(const uint8_t *data, uint32_t len);

// CRC16 (CCITT-FALSE)
// Used for: metadata pool record integrity
uint16_t KCrc16(const uint8_t *data, uint32_t len);

// SHA-256
// Used for: secure hashing, future cryptographic needs
void KSha256(const uint8_t *data, uint32_t len, uint8_t *out_hash);

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
