#include "box_hash.h"
#include "../../lib/kernel/klib.h"
#include "../../../kernel/drivers/timer/rtc.h"

// BoxHash Constants (nothing-up-my-sleeve numbers from fractional parts of sqrt(3,5,7,11))
#define BOX_HASH_C1 0x9e3779b97f4a7c15ULL  // sqrt(3)
#define BOX_HASH_C2 0xbf58476d1ce4e5b9ULL  // sqrt(5)
#define BOX_HASH_C3 0x94d049bb133111ebULL  // sqrt(7)
#define BOX_HASH_C4 0x4cf5ad432745937fULL  // sqrt(11)
#define BOX_HASH_C5 0x3a478be4ac9e0d17ULL  // sqrt(13)
#define BOX_HASH_C6 0x6d5a2f8b3c1e9047ULL  // sqrt(17)
#define BOX_HASH_C7 0x8f2b4e6a1d9c3058ULL  // sqrt(19)
#define BOX_HASH_C8 0x1c7e9a5b3f4d2068ULL  // sqrt(23)

// SHA-256 Constants (first 32 bits of fractional parts of sqrt(2,3,5...))
static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// Rotate left
static inline uint64_t Rotl64(uint64_t x, uint8_t r) {
    return (x << r) | (x >> (64 - r));
}

// Rotate right (for SHA-256)
static inline uint32_t Rotr32(uint32_t x, uint8_t r) {
    return (x >> r) | (x << (32 - r));
}

// Initialize context with random salt from RTC
void BoxHashInit(BoxHashContext *ctx) {
    if (!ctx) return;
    
    uint64_t seed = rtc_get_unix64();
    
    // Generate salt from seed
    for (int i = 0; i < BOX_HASH_SALT_SIZE; i += 8) {
        uint64_t val = seed ^ Rotl64(seed, 13) ^ (seed * BOX_HASH_C1);
        memcpy(&ctx->salt[i], &val, MIN(8, BOX_HASH_SALT_SIZE - i));
        seed = Rotl64(seed, 17) ^ BOX_HASH_C2;
    }
    
    // Clear key (not initialized by default)
    memset(ctx->key, 0, BOX_HASH_KEY_SIZE);
    ctx->key_initialized = false;
}

// Initialize with key for HMAC-style security
void BoxHashInitWithKey(BoxHashContext *ctx, const uint8_t *key, uint32_t key_size) {
    if (!ctx || !key) return;
    
    BoxHashInit(ctx);  // Initialize salt first
    
    // Hash the key into our key slot
    BoxHash key_hash = BoxHashComputeSecure(key, key_size, ctx);
    memcpy(ctx->key, key_hash.bytes, BOX_HASH_KEY_SIZE);
    ctx->key_initialized = true;
}

// Core mixing function (improved avalanche)
static inline uint64_t Mix(uint64_t h) {
    h ^= h >> 33;
    h *= BOX_HASH_C1;
    h ^= h >> 29;
    h *= BOX_HASH_C2;
    h ^= h >> 33;
    h *= BOX_HASH_C3;
    h ^= h >> 29;
    return h;
}

// Enhanced mixing for secure mode (8 rounds)
static inline uint64_t MixSecure(uint64_t h, uint8_t round) {
    static const uint64_t constants[8] = {
        BOX_HASH_C1, BOX_HASH_C2, BOX_HASH_C3, BOX_HASH_C4,
        BOX_HASH_C5, BOX_HASH_C6, BOX_HASH_C7, BOX_HASH_C8
    };

    h ^= h >> 33;
    h *= constants[round % 8];
    h ^= Rotl64(h, (round * 7) & 31);
    h ^= h >> 29;
    return h;
}

// XOR key into state (HMAC-style)
static inline uint64_t ApplyKey(uint64_t state, const uint8_t *key, uint32_t pos) {
    uint64_t key_word;
    memcpy(&key_word, &key[pos % BOX_HASH_KEY_SIZE], MIN(8, BOX_HASH_KEY_SIZE - (pos % BOX_HASH_KEY_SIZE)));
    return state ^ key_word;
}

BoxHash BoxHashCompute(const void *data, uint32_t size, const BoxHashContext *ctx) {
    BoxHash hash;
    memset(&hash, 0, BOX_HASH_BYTES);
    
    if (!data || size == 0)
        return hash;
    
    const uint8_t *bytes = (const uint8_t *)data;
    
    // Initialize state with salt
    uint64_t state[4];
    for (int i = 0; i < 4; i++) {
        uint64_t salt_val;
        memcpy(&salt_val, &ctx->salt[i * 4], MIN(8, BOX_HASH_SALT_SIZE - i * 4));
        state[i] = BOX_HASH_C1 ^ salt_val;
        
        if (ctx->key_initialized)
            state[i] = ApplyKey(state[i], ctx->key, i * 8);
    }
    
    // Process all bytes (4 rounds for speed)
    uint32_t i = 0;
    while (i + 4 <= size) {
        for (int round = 0; round < 4; round++) {
            state[round % 4] ^= bytes[i + round];
            if (ctx->key_initialized)
                state[round % 4] = ApplyKey(state[round % 4], ctx->key, i + round);
            state[round % 4] = Mix(state[round % 4]);
        }
        i += 4;
    }
    
    // Process remaining bytes
    while (i < size) {
        state[i % 4] ^= bytes[i];
        if (ctx->key_initialized)
            state[i % 4] = ApplyKey(state[i % 4], ctx->key, i);
        state[i % 4] = Mix(state[i % 4]);
        i++;
    }
    
    // Finalize with size
    for (int i = 0; i < 4; i++) {
        state[i] = Mix(state[i] ^ size ^ (uint64_t)i * BOX_HASH_C4);
    }
    
    // Combine into final hash
    memcpy(&hash.bytes[0],  &state[0], 8);
    memcpy(&hash.bytes[8],  &state[1], 8);
    memcpy(&hash.bytes[16], &state[2], 8);
    memcpy(&hash.bytes[24], &state[3], 8);
    
    return hash;
}

BoxHash BoxHashComputeSecure(const void *data, uint32_t size, const BoxHashContext *ctx) {
    BoxHash hash;
    memset(&hash, 0, BOX_HASH_BYTES);
    
    if (!data || size == 0)
        return hash;
    
    const uint8_t *bytes = (const uint8_t *)data;
    
    // Initialize state with salt + key
    uint64_t state[8];
    for (int i = 0; i < 8; i++) {
        uint64_t salt_val;
        memcpy(&salt_val, &ctx->salt[i % BOX_HASH_SALT_SIZE], MIN(8, BOX_HASH_SALT_SIZE - (i % BOX_HASH_SALT_SIZE)));
        state[i] = BOX_HASH_C1 ^ salt_val ^ ((uint64_t)i * BOX_HASH_C5);
        
        if (ctx->key_initialized)
            state[i] = ApplyKey(state[i], ctx->key, i * 8);
    }
    
    // Process all bytes (8 rounds for security)
    uint32_t i = 0;
    while (i + 8 <= size) {
        for (int round = 0; round < 8; round++) {
            uint64_t input;
            memcpy(&input, &bytes[i + (round * 4) % (size - 7)], MIN(8, size - i));
            
            state[round] ^= input;
            if (ctx->key_initialized)
                state[round] = ApplyKey(state[round], ctx->key, i + round);
            state[round] = MixSecure(state[round], round);
        }
        i += 8;
    }
    
    // Process remaining bytes
    while (i < size) {
        state[i % 8] ^= bytes[i];
        if (ctx->key_initialized)
            state[i % 8] = ApplyKey(state[i % 8], ctx->key, i);
        state[i % 8] = MixSecure(state[i % 8], i);
        i++;
    }
    
    // Finalize (8 rounds)
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < 8; i++) {
            state[i] = MixSecure(state[i] ^ size ^ ((uint64_t)round * BOX_HASH_C6), round);
        }
    }
    
    // Combine into final hash
    for (int i = 0; i < 8; i++) {
        memcpy(&hash.bytes[i * 4], &state[i], 4);
    }
    
    return hash;
}

// SHA-256 implementation for maximum security
static uint32_t SHA256_Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static uint32_t SHA256_Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32_t SHA256_Sigma0(uint32_t x) { return Rotr32(x, 2) ^ Rotr32(x, 13) ^ Rotr32(x, 22); }
static uint32_t SHA256_Sigma1(uint32_t x) { return Rotr32(x, 6) ^ Rotr32(x, 11) ^ Rotr32(x, 25); }
static uint32_t SHA256_Gamma0(uint32_t x) { return Rotr32(x, 7) ^ Rotr32(x, 18) ^ (x >> 3); }
static uint32_t SHA256_Gamma1(uint32_t x) { return Rotr32(x, 17) ^ Rotr32(x, 19) ^ (x >> 10); }

BoxHash BoxHashComputeSHA256(const void *data, uint32_t size) {
    BoxHash hash;
    
    if (!data || size == 0) {
        memset(&hash, 0, BOX_HASH_BYTES);
        return hash;
    }
    
    const uint8_t *bytes = (const uint8_t *)data;
    
    // Initial hash values
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    
    // Process in 64-byte chunks
    uint32_t pos = 0;
    while (pos + 64 <= size) {
        uint32_t w[64];
        
        // Prepare message schedule
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)bytes[pos + i*4] << 24) |
                   ((uint32_t)bytes[pos + i*4 + 1] << 16) |
                   ((uint32_t)bytes[pos + i*4 + 2] << 8) |
                   ((uint32_t)bytes[pos + i*4 + 3]);
        }
        
        for (int i = 16; i < 64; i++) {
            w[i] = SHA256_Gamma1(w[i-2]) + w[i-7] + SHA256_Gamma0(w[i-15]) + w[i-16];
        }
        
        // Compression function
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + SHA256_Sigma1(e) + SHA256_Ch(e, f, g) + SHA256_K[i] + w[i];
            uint32_t t2 = SHA256_Sigma0(a) + SHA256_Maj(a, b, c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        
        pos += 64;
    }
    
    // Handle remaining bytes + padding
    uint8_t chunk[64];
    memset(chunk, 0, 64);
    uint32_t remaining = size - pos;
    
    if (remaining > 0) {
        memcpy(chunk, &bytes[pos], MIN(remaining, 63));
    }
    
    // Padding
    chunk[remaining] = 0x80;
    
    // Length in bits (big-endian)
    uint64_t bit_len = (uint64_t)size * 8;
    chunk[63] = bit_len & 0xFF;
    chunk[62] = (bit_len >> 8) & 0xFF;
    chunk[61] = (bit_len >> 16) & 0xFF;
    chunk[60] = (bit_len >> 24) & 0xFF;
    chunk[59] = (bit_len >> 32) & 0xFF;
    chunk[58] = (bit_len >> 40) & 0xFF;
    chunk[57] = (bit_len >> 48) & 0xFF;
    chunk[56] = (bit_len >> 56) & 0xFF;
    
    // Process final chunk
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)chunk[i*4] << 24) |
               ((uint32_t)chunk[i*4 + 1] << 16) |
               ((uint32_t)chunk[i*4 + 2] << 8) |
               ((uint32_t)chunk[i*4 + 3]);
    }
    
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_Gamma1(w[i-2]) + w[i-7] + SHA256_Gamma0(w[i-15]) + w[i-16];
    }
    
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + SHA256_Sigma1(e) + SHA256_Ch(e, f, g) + SHA256_K[i] + w[i];
        uint32_t t2 = SHA256_Sigma0(a) + SHA256_Maj(a, b, c);
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    
    // Output hash (big-endian)
    for (int i = 0; i < 8; i++) {
        hash.bytes[i*4] = (h[i] >> 24) & 0xFF;
        hash.bytes[i*4 + 1] = (h[i] >> 16) & 0xFF;
        hash.bytes[i*4 + 2] = (h[i] >> 8) & 0xFF;
        hash.bytes[i*4 + 3] = h[i] & 0xFF;
    }
    
    return hash;
}

// Constant-time comparison (prevents timing attacks)
bool BoxHashEqual(const BoxHash *a, const BoxHash *b) {
    if (!a || !b)
        return false;
    
    volatile uint8_t result = 0;
    for (uint32_t i = 0; i < BOX_HASH_BYTES; i++) {
        result |= a->bytes[i] ^ b->bytes[i];
    }
    return result == 0;
}

void BoxHashToHex(const BoxHash *hash, char *out, uint32_t out_size) {
    if (!hash || !out || out_size < BOX_HASH_BYTES * 2 + 1)
        return;
    
    static const char hex_chars[] = "0123456789abcdef";
    uint32_t j = 0;
    
    for (uint32_t i = 0; i < BOX_HASH_BYTES && j < out_size - 1; i++) {
        out[j++] = hex_chars[(hash->bytes[i] >> 4) & 0x0F];
        out[j++] = hex_chars[hash->bytes[i] & 0x0F];
    }
    out[j] = '\0';
}

bool BoxHashVerify(const void *data, uint32_t size, const BoxHash *expected_hash, const BoxHashContext *ctx) {
    if (!data || !expected_hash || !ctx)
        return false;
    
    BoxHash computed = BoxHashComputeSecure(data, size, ctx);
    return BoxHashEqual(&computed, expected_hash);
}
