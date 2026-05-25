#ifndef BOX_HASH_H
#define BOX_HASH_H

#include "../../lib/kernel/ktypes.h"

// ============================================================================
// BoxHash — integrity & content hashing for BoxOS
//
//   BoxHashIntegrity — fast 64-bit checksum: silent bit-rot / torn-write
//                      detection on data blocks. wyhash-class.
//   BoxHashContent   — 256-bit content digest: dedup keys, block checksums.
//   BoxHashSecure    — standard SHA-256. The ONLY cryptographic option here;
//                      use it where tamper-resistance is actually required.
//
// Integrity/Content are DETERMINISTIC given a seed (set via BoxHashInit) — seed
// them with the volume UUID so digests are stable across reboots and unique per
// volume. They have strong avalanche and are excellent against random
// corruption, but are NOT collision-proof against an adversary (that is what
// BoxHashSecure is for). No per-boot randomness: a digest written now must
// re-verify after a reboot.
// ============================================================================

#define BOX_HASH_BYTES 32   // 256-bit content digest

typedef struct {
    uint8_t bytes[BOX_HASH_BYTES];
} BoxHash;

// Deterministic seed state derived from a caller seed (e.g. fs_uuid).
typedef struct {
    uint64_t s[4];
} BoxHashContext;

// Derive the seed state from arbitrary seed bytes (volume UUID recommended).
// Deterministic: same seed -> same state, every boot, every machine.
void     BoxHashInit(BoxHashContext *ctx, const void *seed, uint32_t seed_len);

// 64-bit integrity checksum (bit-rot / torn-write detection).
uint64_t BoxHashIntegrity(const void *data, uint32_t size, const BoxHashContext *ctx);

// 256-bit content digest (dedup keys, block checksums).
BoxHash  BoxHashContent(const void *data, uint32_t size, const BoxHashContext *ctx);

// SHA-256 (cryptographic; unseeded; standard FIPS 180-4).
BoxHash  BoxHashSecure(const void *data, uint32_t size);

// Constant-time 256-bit compare.
bool     BoxHashEqual(const BoxHash *a, const BoxHash *b);

// Hex string (2*BOX_HASH_BYTES + 1 bytes needed).
void     BoxHashToHex(const BoxHash *hash, char *out, uint32_t out_size);

#endif // BOX_HASH_H
