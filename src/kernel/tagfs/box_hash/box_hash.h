#ifndef BOX_HASH_H
#define BOX_HASH_H

#include "../../lib/kernel/ktypes.h"


#define BOX_HASH_BYTES 32

typedef struct {
    uint8_t bytes[BOX_HASH_BYTES];
} BoxHash;

typedef struct {
    uint64_t s[4];
} BoxHashContext;

void     BoxHashInit(BoxHashContext *ctx, const void *seed, uint32_t seed_len);

uint64_t BoxHashIntegrity(const void *data, uint32_t size, const BoxHashContext *ctx);

BoxHash  BoxHashContent(const void *data, uint32_t size, const BoxHashContext *ctx);

BoxHash  BoxHashSecure(const void *data, uint32_t size);

bool     BoxHashEqual(const BoxHash *a, const BoxHash *b);

void     BoxHashToHex(const BoxHash *hash, char *out, uint32_t out_size);

#endif