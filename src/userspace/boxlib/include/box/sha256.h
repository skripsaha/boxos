#ifndef BOX_SHA256_H
#define BOX_SHA256_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

/*
 * SHA-256 (FIPS 180-4): the digest of bytes, the same on every machine and
 * every volume. A Recipe keeps the digest of every file a program was made
 * from, so it knows when one of them is no longer what it was.
 *
 * In one call, or fed in pieces of any size:
 *
 *     Sha256 state;
 *     sha256_begin(&state);
 *     sha256_add(&state, piece, length);    as often as there are pieces
 *     sha256_end(&state, digest);
 */

#define SHA256_BYTES 32u

typedef struct {
    uint32_t Words[8];
    uint64_t Length;           /* bytes added so far */
    uint8_t  Block[64];
    uint32_t Held;             /* bytes of Block waiting for the rest of it */
} Sha256;

void sha256_begin(Sha256 *state);
void sha256_add(Sha256 *state, const void *bytes, size_t length);
void sha256_end(Sha256 *state, uint8_t digest[SHA256_BYTES]);
void sha256(const void *bytes, size_t length, uint8_t digest[SHA256_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* BOX_SHA256_H */
