#ifndef KRYPTO_H
#define KRYPTO_H

#include "ktypes.h"


uint32_t KCrc32(const uint8_t *data, uint32_t len);

uint16_t KCrc16(const uint8_t *data, uint32_t len);

void KSha256(const uint8_t *data, uint32_t len, uint8_t *out_hash);

typedef struct {
    uint32_t state[8];
    uint64_t total_bits;
    uint32_t buffer_len;
    uint8_t  buffer[64];
} KSha256Ctx;

void KSha256Init(KSha256Ctx *ctx);
void KSha256Update(KSha256Ctx *ctx, const uint8_t *data, uint32_t len);
void KSha256Final(KSha256Ctx *ctx, uint8_t *out_hash);

uint8_t KChecksum8(const uint8_t *data, uint32_t len);

uint16_t KChecksum16(const uint8_t *data, uint32_t len);

uint32_t KChecksum32(const uint8_t *data, uint32_t len);

uint32_t KFnv1a(const uint8_t *data, uint32_t len);

uint32_t KMurmur3Finalize(uint32_t h);

#endif