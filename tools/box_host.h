#ifndef BOX_HOST_H
#define BOX_HOST_H

/*
 * A box's SHA-256 on the build machine, for the host tools that read and write
 * boxes (tools/pack, tools/box_stand): BoxLib's own sha256.c, built for the
 * host beside the tool, handed to the reader and the pen as a BoxHasher.
 *
 * BoxLib's headers are not included — they define NULL and the integer types
 * their own way, and a host tool has the host's — so its one-shot entry is
 * declared here, and the hasher gathers what it is given and hashes it whole
 * at the end.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box_reader.h"

void sha256(const void *bytes, size_t length, uint8_t digest[32]);

typedef struct {
    uint8_t *Bytes;
    uint64_t Used;
    uint64_t Room;
} BoxHostHash;

static inline void BoxHostHashBegin(void *state)
{
    ((BoxHostHash *)state)->Used = 0;
}

static inline void BoxHostHashAdd(void *state, const uint8_t *bytes, uint64_t count)
{
    BoxHostHash *hash = state;
    if (count > hash->Room - hash->Used) {
        hash->Room  = (hash->Used + count) * 2 + 64;
        hash->Bytes = realloc(hash->Bytes, hash->Room);
        if (!hash->Bytes) {
            fprintf(stderr, "no memory for %llu bytes to hash\n", (unsigned long long)hash->Room);
            exit(2);
        }
    }
    memcpy(hash->Bytes + hash->Used, bytes, count);
    hash->Used += count;
}

static inline void BoxHostHashEnd(void *state, uint8_t digest[BOX_HASH_BYTES])
{
    BoxHostHash *hash = state;
    sha256(hash->Bytes, hash->Used, digest);
}

/* A hasher over `state`, which starts zeroed and is freed by BoxHostHashFree. */
static inline BoxHasher BoxHostHasher(BoxHostHash *state)
{
    BoxHasher hasher = { state, BoxHostHashBegin, BoxHostHashAdd, BoxHostHashEnd };
    return hasher;
}

static inline void BoxHostHashFree(BoxHostHash *state)
{
    free(state->Bytes);
    state->Bytes = NULL;
    state->Used = state->Room = 0;
}

/* Whether the host's SHA-256 is FIPS 180-4's, on its first vector. */
static inline bool BoxHostHashTrue(void)
{
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t digest[32];
    sha256("abc", 3, digest);
    return memcmp(digest, abc, 32) == 0;
}

#endif /* BOX_HOST_H */
