#include "box_hash.h"
#include "../../lib/kernel/klib.h"

// ============================================================================
// Secrets — nothing-up-my-sleeve constants (fractional parts of sqrt of small
// primes). Used as the wyhash-class mixing secrets and SHA-256 round constants.
// ============================================================================
static const uint64_t SECRET[5] = {
    0x9e3779b97f4a7c15ULL,  // sqrt(3)
    0xbf58476d1ce4e5b9ULL,  // sqrt(5)
    0x94d049bb133111ebULL,  // sqrt(7)
    0x4cf5ad432745937fULL,  // sqrt(11)
    0x3a478be4ac9e0d17ULL,  // sqrt(13)
};

// 64x64 -> 128 multiply, folded to 64 bits (the wyhash mixer). __uint128_t is a
// native GCC type on x86-64 (no libgcc call for the multiply).
static inline uint64_t WyMix(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

// Little-endian loads (x86-64 is LE; memcpy avoids alignment UB).
static inline uint64_t Read64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t Read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return (uint64_t)v; }

// Read 1..3 trailing bytes without reading past the buffer (wyhash _wyr3).
static inline uint64_t ReadSmall(const uint8_t *p, uint32_t k) {
    return ((uint64_t)p[0] << 16) | ((uint64_t)p[k >> 1] << 8) | (uint64_t)p[k - 1];
}

// wyhash (final v4) — strong 64-bit hash, seeded. Handles any length with no
// out-of-bounds reads.
static uint64_t WyHash64(const uint8_t *p, uint32_t len, uint64_t seed) {
    seed ^= WyMix(seed ^ SECRET[0], SECRET[1]);
    uint64_t a, b;

    if (len <= 16) {
        if (len >= 4) {
            uint32_t off = (len >> 3) << 2;             // 0 for 4..7, 4 for 8..16
            a = (Read32(p) << 32)            | Read32(p + off);
            b = (Read32(p + len - 4) << 32)  | Read32(p + len - 4 - off);
        } else if (len > 0) {
            a = ReadSmall(p, len);
            b = 0;
        } else {
            a = 0;
            b = 0;
        }
    } else {
        uint32_t i = len;
        const uint8_t *q = p;
        if (i > 48) {
            uint64_t s1 = seed, s2 = seed;
            do {
                seed = WyMix(Read64(q)      ^ SECRET[1], Read64(q + 8)  ^ seed);
                s1   = WyMix(Read64(q + 16) ^ SECRET[2], Read64(q + 24) ^ s1);
                s2   = WyMix(Read64(q + 32) ^ SECRET[3], Read64(q + 40) ^ s2);
                q += 48;
                i -= 48;
            } while (i > 48);
            seed ^= s1 ^ s2;
        }
        while (i > 16) {
            seed = WyMix(Read64(q) ^ SECRET[1], Read64(q + 8) ^ seed);
            q += 16;
            i -= 16;
        }
        // Last 16 bytes (overlap with the tail just consumed — standard wyhash).
        a = Read64(q + i - 16);
        b = Read64(q + i - 8);
    }

    a ^= SECRET[1];
    b ^= seed;
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    a = (uint64_t)r;
    b = (uint64_t)(r >> 64);
    return WyMix(a ^ SECRET[0] ^ (uint64_t)len, b ^ SECRET[1]);
}

// ============================================================================
// Public: deterministic seed derivation
// ============================================================================
void BoxHashInit(BoxHashContext *ctx, const void *seed, uint32_t seed_len) {
    if (!ctx) return;
    uint64_t base = (seed && seed_len)
                    ? WyHash64((const uint8_t *)seed, seed_len, SECRET[0])
                    : SECRET[0];
    for (int j = 0; j < 4; j++) {
        base = WyMix(base ^ SECRET[j % 5], SECRET[(j + 1) % 5]);
        ctx->s[j] = base;
    }
}

// ============================================================================
// Public: 64-bit integrity checksum
// ============================================================================
uint64_t BoxHashIntegrity(const void *data, uint32_t size, const BoxHashContext *ctx) {
    if (!data || size == 0)
        return 0;
    uint64_t seed = ctx ? ctx->s[0] : SECRET[0];
    return WyHash64((const uint8_t *)data, size, seed);
}

// ============================================================================
// Public: 256-bit content digest — four independent wyhash lanes (distinct
// seeds). A collision requires all four 64-bit lanes to collide simultaneously,
// which is astronomically unlikely for any realistic block count.
// ============================================================================
BoxHash BoxHashContent(const void *data, uint32_t size, const BoxHashContext *ctx) {
    BoxHash h;
    memset(&h, 0, sizeof(h));
    if (!data || size == 0)
        return h;

    uint64_t base = ctx ? ctx->s[0] : SECRET[0];
    uint64_t lanes[4];
    for (int j = 0; j < 4; j++) {
        uint64_t seed = (ctx ? ctx->s[j] : SECRET[j % 5]) ^ SECRET[(j + 2) % 5];
        lanes[j] = WyHash64((const uint8_t *)data, size, seed);
    }
    memcpy(h.bytes, lanes, sizeof(lanes));
    (void)base;
    return h;
}

// ============================================================================
// SHA-256 (FIPS 180-4) — the cryptographic option. Unseeded, standard output.
// ============================================================================
static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t Rotr32(uint32_t x, uint8_t r) { return (x >> r) | (x << (32 - r)); }
static inline uint32_t Sha_Ch(uint32_t x, uint32_t y, uint32_t z)  { return (x & y) ^ (~x & z); }
static inline uint32_t Sha_Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t Sha_S0(uint32_t x) { return Rotr32(x, 2) ^ Rotr32(x, 13) ^ Rotr32(x, 22); }
static inline uint32_t Sha_S1(uint32_t x) { return Rotr32(x, 6) ^ Rotr32(x, 11) ^ Rotr32(x, 25); }
static inline uint32_t Sha_s0(uint32_t x) { return Rotr32(x, 7) ^ Rotr32(x, 18) ^ (x >> 3); }
static inline uint32_t Sha_s1(uint32_t x) { return Rotr32(x, 17) ^ Rotr32(x, 19) ^ (x >> 10); }

static void Sha256Block(uint32_t h[8], const uint8_t chunk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)chunk[i*4] << 24) | ((uint32_t)chunk[i*4+1] << 16) |
               ((uint32_t)chunk[i*4+2] << 8) | (uint32_t)chunk[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = Sha_s1(w[i-2]) + w[i-7] + Sha_s0(w[i-15]) + w[i-16];

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + Sha_S1(e) + Sha_Ch(e, f, g) + SHA256_K[i] + w[i];
        uint32_t t2 = Sha_S0(a) + Sha_Maj(a, b, c);
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

BoxHash BoxHashSecure(const void *data, uint32_t size) {
    BoxHash hash;
    memset(&hash, 0, sizeof(hash));

    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    const uint8_t *bytes = (const uint8_t *)data;

    uint32_t pos = 0;
    if (bytes) {
        while (pos + 64 <= size) { Sha256Block(h, bytes + pos); pos += 64; }
    }

    // Final padding: 0x80, zeros, 8-byte BE bit length (two blocks if needed).
    uint8_t chunk[64];
    uint32_t remaining = bytes ? size - pos : 0;
    uint64_t bit_len = (uint64_t)size * 8;
    memset(chunk, 0, 64);
    if (remaining > 0) memcpy(chunk, bytes + pos, remaining);
    chunk[remaining] = 0x80;
    if (remaining >= 56) {
        Sha256Block(h, chunk);
        memset(chunk, 0, 64);
    }
    for (int i = 0; i < 8; i++)
        chunk[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
    Sha256Block(h, chunk);

    for (int i = 0; i < 8; i++) {
        hash.bytes[i*4]   = (uint8_t)(h[i] >> 24);
        hash.bytes[i*4+1] = (uint8_t)(h[i] >> 16);
        hash.bytes[i*4+2] = (uint8_t)(h[i] >> 8);
        hash.bytes[i*4+3] = (uint8_t)(h[i]);
    }
    return hash;
}

// ============================================================================
// Helpers
// ============================================================================
bool BoxHashEqual(const BoxHash *a, const BoxHash *b) {
    if (!a || !b) return false;
    volatile uint8_t diff = 0;
    for (uint32_t i = 0; i < BOX_HASH_BYTES; i++)
        diff |= (uint8_t)(a->bytes[i] ^ b->bytes[i]);
    return diff == 0;
}

void BoxHashToHex(const BoxHash *hash, char *out, uint32_t out_size) {
    if (!hash || !out || out_size < BOX_HASH_BYTES * 2 + 1)
        return;
    static const char hex[] = "0123456789abcdef";
    uint32_t j = 0;
    for (uint32_t i = 0; i < BOX_HASH_BYTES && j + 2 < out_size; i++) {
        out[j++] = hex[(hash->bytes[i] >> 4) & 0xF];
        out[j++] = hex[hash->bytes[i] & 0xF];
    }
    out[j] = '\0';
}
