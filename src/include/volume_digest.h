#ifndef VOLUME_DIGEST_H
#define VOLUME_DIGEST_H

/*
 * The digest of a volume's block — the 64-bit hash BoxHashIntegrity answers,
 * as one text for every build that computes or checks one.
 *
 * A digest is written by whoever writes the block and checked by whoever
 * reads it: the kernel (tagfs/box_hash/box_hash.c), mkfs writing a volume's
 * DIGESTS and the UEFI loader checking the kernel's blocks before it jumps.
 * If any two of them disagreed
 * by one bit, every block the other wrote would read as rot. So the hash is
 * spelled once, here, and the kernel's startup suite and volume-read's
 * self-test both hold it to one known answer (VOLUME_DIGEST_GOLDEN_4096).
 *
 *
 * ‼ NOT wyhash's OWN CONSTANTS
 *
 * The shape is wyhash final v4, but the secrets below are not wyhash's, and
 * they are not what the old comment over them said either (square roots of
 * small primes): the first is 2^64 / phi and the next two are splitmix64's
 * multipliers; where the last two came from was not written down. They are
 * this format's constants now. Published wyhash vectors do not apply, which is
 * why this tree carries its own — and why changing any of them changes every
 * digest on every volume ever made.
 *
 *
 * ‼ NO LIBRARY, ANY BYTE ORDER OF HOST
 *
 * The loads are assembled from bytes, little-endian, instead of memcpy into
 * an integer: the UEFI loader has no memcpy, and the host that builds mkfs is
 * whatever the desk is. On x86-64 it is the same value memcpy gave. Each is
 * one expression of its bytes, which the compiler can merge into one load (a
 * loop of shifts it left as a loop), and always inlined: the kernel's -Os
 * inliner counts the eight loads before they are merged and kept a call for
 * every eight bytes hashed. __uint128_t is a native type of every 64-bit
 * target this is built for; the multiply calls nothing.
 *
 * Every name here begins Wy, because every includer — the kernel, the UEFI
 * loader, a host tool — gets all of them in its own scope.
 *
 * Integer types come from the includer, as volume_deed.h says.
 */

static const uint64_t WySecret[5] = {
    0x9e3779b97f4a7c15ULL,
    0xbf58476d1ce4e5b9ULL,
    0x94d049bb133111ebULL,
    0x4cf5ad432745937fULL,
    0x3a478be4ac9e0d17ULL,
};

/* 64x64 -> 128 multiply, folded to 64 bits (the wyhash mixer). */
static inline uint64_t WyMix(uint64_t a, uint64_t b)
{
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

static inline __attribute__((always_inline)) uint64_t WyRead32(const uint8_t *p)
{
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24;
}

static inline __attribute__((always_inline)) uint64_t WyRead64(const uint8_t *p)
{
    return (uint64_t)p[0]       | (uint64_t)p[1] << 8  | (uint64_t)p[2] << 16 |
           (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

/* Read 1..3 trailing bytes without reading past the buffer (wyhash _wyr3). */
static inline uint64_t WyReadSmall(const uint8_t *p, uint32_t k)
{
    return ((uint64_t)p[0] << 16) | ((uint64_t)p[k >> 1] << 8) | (uint64_t)p[k - 1];
}

/* The hash of `len` bytes under `seed`. Any length; nothing read past it. */
static inline uint64_t WyHash64(const uint8_t *p, uint32_t len, uint64_t seed)
{
    seed ^= WyMix(seed ^ WySecret[0], WySecret[1]);
    uint64_t a, b;

    if (len <= 16) {
        if (len >= 4) {
            uint32_t off = (len >> 3) << 2;             // 0 for 4..7, 4 for 8..16
            a = (WyRead32(p) << 32)            | WyRead32(p + off);
            b = (WyRead32(p + len - 4) << 32)  | WyRead32(p + len - 4 - off);
        } else if (len > 0) {
            a = WyReadSmall(p, len);
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
                seed = WyMix(WyRead64(q)      ^ WySecret[1], WyRead64(q + 8)  ^ seed);
                s1   = WyMix(WyRead64(q + 16) ^ WySecret[2], WyRead64(q + 24) ^ s1);
                s2   = WyMix(WyRead64(q + 32) ^ WySecret[3], WyRead64(q + 40) ^ s2);
                q += 48;
                i -= 48;
            } while (i > 48);
            seed ^= s1 ^ s2;
        }
        while (i > 16) {
            seed = WyMix(WyRead64(q) ^ WySecret[1], WyRead64(q + 8) ^ seed);
            q += 16;
            i -= 16;
        }
        // Last 16 bytes (overlap with the tail just consumed — standard wyhash).
        a = WyRead64(q + i - 16);
        b = WyRead64(q + i - 8);
    }

    a ^= WySecret[1];
    b ^= seed;
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    a = (uint64_t)r;
    b = (uint64_t)(r >> 64);
    return WyMix(a ^ WySecret[0] ^ (uint64_t)len, b ^ WySecret[1]);
}

/*
 * The four seed lanes a seed's bytes give — a volume's uuid, for a volume's
 * digests. Lane 0 seeds BoxHashIntegrity: it is the lane every writer and
 * reader of a block digest must derive alike. All four seed BoxHashContent.
 * No seed bytes at all start from the first secret.
 */
static inline void WySeedLanes(const uint8_t *seed, uint32_t len, uint64_t lanes[4])
{
    uint64_t base = (seed && len) ? WyHash64(seed, len, WySecret[0]) : WySecret[0];
    for (uint32_t j = 0; j < 4u; j++) {
        base = WyMix(base ^ WySecret[j % 5u], WySecret[(j + 1u) % 5u]);
        lanes[j] = base;
    }
}

/*
 * The golden block's digest: the volume uuid 00 01 .. 0F gives the lanes,
 * and the block is 4096 bytes whose byte i is (i * 13 + 5) & 0xFF, hashed under
 * lane 0. The kernel's boxhash suite and volume-read's self-test reach it each
 * by their own code.
 */
#define VOLUME_DIGEST_GOLDEN_4096  0x0911A376FAC2E968ull

#endif /* VOLUME_DIGEST_H */
