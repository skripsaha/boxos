#include "crypto.h"
#include "klib.h"

// ============================================================================
// CRC32 (ISO 3309 polynomial)
// Used for: superblock integrity, disk entries, self-heal mirrors
// ============================================================================

#define CRC32_POLYNOMIAL 0xEDB88320  // ISO 3309 reversed polynomial
#define CRC32_INIT_VALUE 0xFFFFFFFF

uint32_t KCrc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = CRC32_INIT_VALUE;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            crc = (crc >> 1) ^ (CRC32_POLYNOMIAL & (-(crc & 1)));
        }
    }

    return ~crc;
}

// ============================================================================
// CRC16 (CCITT-FALSE)
// Used for: metadata pool record integrity
// ============================================================================

uint16_t KCrc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
        {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }

    return crc;
}

// ============================================================================
// SHA-256
// Used for: secure hashing, future cryptographic needs
// ============================================================================

// SHA-256 constants (first 32 bits of the fractional parts of the cube roots
// of the first 64 primes 2..311)
static const uint32_t K_SHA256_K[64] = {
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

// Right rotate for SHA-256
#define K_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

// SHA-256 functions
#define K_CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define K_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define K_SIGMA0(x)    (K_ROTR(x, 2) ^ K_ROTR(x, 13) ^ K_ROTR(x, 22))
#define K_SIGMA1(x)    (K_ROTR(x, 6) ^ K_ROTR(x, 11) ^ K_ROTR(x, 25))
#define K_SIGMA2(x)    (K_ROTR(x, 7) ^ K_ROTR(x, 18) ^ ((x) >> 3))
#define K_SIGMA3(x)    (K_ROTR(x, 17) ^ K_ROTR(x, 19) ^ ((x) >> 10))

static void KSha256Transform(uint32_t *state, const uint8_t *block)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;

    // Prepare message schedule
    for (int i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }

    for (int i = 16; i < 64; i++)
    {
        w[i] = K_SIGMA3(w[i - 2]) + w[i - 7] + K_SIGMA2(w[i - 15]) + w[i - 16];
    }

    // Initialize working variables
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    // Compression function main loop
    for (int i = 0; i < 64; i++)
    {
        t1 = h + K_SIGMA1(e) + K_CH(e, f, g) + K_SHA256_K[i] + w[i];
        t2 = K_SIGMA0(a) + K_MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    // Add compressed chunk to current hash value
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void KSha256(const uint8_t *data, uint32_t len, uint8_t *out_hash)
{
    // Initial hash values (first 32 bits of the fractional parts of the square
    // roots of the first 8 primes 2..19)
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    uint64_t total_len = len;
    uint8_t buffer[64];

    // Process full blocks
    while (len >= 64)
    {
        KSha256Transform(state, data);
        data += 64;
        len -= 64;
    }

    // Pad remaining data
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, data, len);

    // Append bit '1'
    buffer[len] = 0x80;

    if (len >= 56)
    {
        // Need two blocks for padding
        KSha256Transform(state, buffer);
        memset(buffer, 0, sizeof(buffer));
    }

    // Append length in bits as 64-bit big-endian
    uint64_t bit_len = total_len * 8;
    buffer[56] = (bit_len >> 56) & 0xFF;
    buffer[57] = (bit_len >> 48) & 0xFF;
    buffer[58] = (bit_len >> 40) & 0xFF;
    buffer[59] = (bit_len >> 32) & 0xFF;
    buffer[60] = (bit_len >> 24) & 0xFF;
    buffer[61] = (bit_len >> 16) & 0xFF;
    buffer[62] = (bit_len >> 8) & 0xFF;
    buffer[63] = bit_len & 0xFF;

    KSha256Transform(state, buffer);

    // Produce final hash value (big-endian)
    for (int i = 0; i < 8; i++)
    {
        out_hash[i * 4]     = (state[i] >> 24) & 0xFF;
        out_hash[i * 4 + 1] = (state[i] >> 16) & 0xFF;
        out_hash[i * 4 + 2] = (state[i] >> 8) & 0xFF;
        out_hash[i * 4 + 3] = state[i] & 0xFF;
    }
}

// ============================================================================
// Simple Checksums
// ============================================================================

uint8_t KChecksum8(const uint8_t *data, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return sum;
}

uint16_t KChecksum16(const uint8_t *data, uint32_t len)
{
    uint16_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return sum;
}

uint32_t KChecksum32(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return sum;
}

// ============================================================================
// FNV-1a Hash (fast, non-cryptographic)
// ============================================================================

#define FNV1A_OFFSET_BASIS 2166136261u
#define FNV1A_PRIME        16777619u

uint32_t KFnv1a(const uint8_t *data, uint32_t len)
{
    uint32_t hash = FNV1A_OFFSET_BASIS;

    for (uint32_t i = 0; i < len; i++)
    {
        hash ^= data[i];
        hash *= FNV1A_PRIME;
    }

    return hash;
}

// ============================================================================
// MurmurHash3 Finalizer
// ============================================================================

uint32_t KMurmur3Finalize(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}
