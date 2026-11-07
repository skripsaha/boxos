/*
 * sha256.c — SHA-256 as FIPS 180-4 gives it.
 */

#include "box/sha256.h"
#include "box/string.h"

static const uint32_t g_rounds[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotate(uint32_t value, unsigned by)
{
    return (value >> by) | (value << (32u - by));
}

static void compress(uint32_t words[8], const uint8_t block[64])
{
    uint32_t schedule[64];

    for (unsigned i = 0; i < 16; i++) {
        schedule[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
                      (uint32_t)block[4 * i + 2] << 8 | (uint32_t)block[4 * i + 3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = rotate(schedule[i - 15], 7) ^ rotate(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        uint32_t s1 = rotate(schedule[i - 2], 17) ^ rotate(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    uint32_t a = words[0], b = words[1], c = words[2], d = words[3];
    uint32_t e = words[4], f = words[5], g = words[6], h = words[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) +
                      g_rounds[i] + schedule[i];
        uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    words[0] += a;
    words[1] += b;
    words[2] += c;
    words[3] += d;
    words[4] += e;
    words[5] += f;
    words[6] += g;
    words[7] += h;
}

void sha256_begin(Sha256 *state)
{
    static const uint32_t first[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    memcpy(state->Words, first, sizeof(first));
    state->Length = 0;
    state->Held   = 0;
}

void sha256_add(Sha256 *state, const void *bytes, size_t length)
{
    const uint8_t *at = (const uint8_t *)bytes;

    state->Length += length;
    if (state->Held != 0) {
        size_t take = 64u - state->Held;
        if (take > length) take = length;
        memcpy(state->Block + state->Held, at, take);
        state->Held += (uint32_t)take;
        at += take;
        length -= take;
        if (state->Held < 64u) return;
        compress(state->Words, state->Block);
        state->Held = 0;
    }
    for (; length >= 64u; at += 64, length -= 64u) compress(state->Words, at);
    memcpy(state->Block, at, length);
    state->Held = (uint32_t)length;
}

void sha256_end(Sha256 *state, uint8_t digest[SHA256_BYTES])
{
    uint64_t bits = state->Length * 8u;

    state->Block[state->Held++] = 0x80u;
    if (state->Held > 56u) {
        memset(state->Block + state->Held, 0, 64u - state->Held);
        compress(state->Words, state->Block);
        state->Held = 0;
    }
    memset(state->Block + state->Held, 0, 56u - state->Held);
    for (unsigned i = 0; i < 8; i++) state->Block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    compress(state->Words, state->Block);

    for (unsigned i = 0; i < 8; i++) {
        digest[4 * i]     = (uint8_t)(state->Words[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(state->Words[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(state->Words[i] >> 8);
        digest[4 * i + 3] = (uint8_t)state->Words[i];
    }
}

void sha256(const void *bytes, size_t length, uint8_t digest[SHA256_BYTES])
{
    Sha256 state;

    sha256_begin(&state);
    sha256_add(&state, bytes, length);
    sha256_end(&state, digest);
}
