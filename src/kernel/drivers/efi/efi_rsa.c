/*
 * BoxOS — RSA-PKCS1-v1.5 SHA-256 signature verification.
 *
 * Algorithm reference: RFC 8017 §8.2.2 RSASSA-PKCS1-V1_5-VERIFY.
 *
 *   1. Convert signature byte string → integer s, 0 ≤ s < n.
 *   2. Compute m = s^e mod n (RSAVP1).
 *   3. Convert m → octet string EM of length k = mod_len.
 *   4. EMSA-PKCS1-v1.5 decode: EM should be
 *        0x00 0x01 0xFF...0xFF 0x00 || T
 *      where T = DigestInfo DER encoding for SHA-256 || the 32-byte hash.
 *
 * No private-key ops — verify-only. The bigint primitives are simple
 * schoolbook routines with 32-bit limbs; performance is fine because
 * RSA verifies are infrequent (boot-time image authentication, occasional
 * runtime capsule check). 2048-bit verification with e=65537 measures
 * around 1.5 ms on a 3 GHz Skylake.
 *
 * SECURITY NOTE: PKCS#1 v1.5 verify must validate the encoded message
 * byte-exact against the deterministic format above. Any tolerance
 * (e.g. accepting a longer DigestInfo than expected, or skipping the
 * padding length check) opens Bleichenbacher-class attacks. We do the
 * full byte-by-byte comparison.
 */

#include "efi_rsa.h"
#include "klib.h"

/* =========================================================================
 * Bigint primitives — 32-bit limbs, fixed-size array.
 *
 * BN_MAX_LIMBS = 128 → 4096-bit numbers (matches the largest practical
 * RSA modulus in deployed Secure Boot infrastructure). Doubled to 256
 * for the multiplication output buffer.
 * ========================================================================= */

#define BN_MAX_LIMBS    128
#define BN_MAX_LIMBS_2  256

typedef struct {
    uint32_t v[BN_MAX_LIMBS];
} BnNum;

typedef struct {
    uint32_t v[BN_MAX_LIMBS_2];
} BnDouble;

static void bn_zero(BnNum *a)
{
    memset(a, 0, sizeof(*a));
}

static void bn_double_zero(BnDouble *a)
{
    memset(a, 0, sizeof(*a));
}

/* Load big-endian byte string into limb representation (limb[0] = LSB).
 * Returns false if `len` exceeds the buffer; caller MUST check key size
 * before invoking — silent truncation here would mask invalid keys. */
static bool bn_load_be(BnNum *out, const uint8_t *be, uint32_t len)
{
    bn_zero(out);
    if (len > BN_MAX_LIMBS * 4) return false;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte_from_end = len - 1 - i;
        uint32_t limb_idx      = byte_from_end / 4;
        uint32_t shift         = (byte_from_end % 4) * 8;
        out->v[limb_idx] |= ((uint32_t)be[i]) << shift;
    }
    return true;
}

/* True iff all `limbs` limbs are zero. Used to reject malformed RSA
 * keys with a zero modulus before they reach modular reduction (where
 * `m == 0` would yield silently wrong results). */
static bool bn_is_zero(const uint32_t *v, int limbs)
{
    for (int i = 0; i < limbs; i++) if (v[i]) return false;
    return true;
}

/* a ?= b across `limbs` limbs. Returns -1, 0, +1. */
static int bn_cmp(const uint32_t *a, const uint32_t *b, int limbs)
{
    for (int i = limbs - 1; i >= 0; i--) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

/* Schoolbook multiply: out (2k limbs) = a (k limbs) * b (k limbs).
 * Time: O(k²). k=64 (2048-bit): ~4096 64-bit mults — fast. */
static void bn_mul(uint32_t *out, const uint32_t *a, const uint32_t *b, int k)
{
    for (int i = 0; i < 2 * k; i++) out[i] = 0;
    for (int i = 0; i < k; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < k; j++) {
            uint64_t prod = (uint64_t)a[i] * b[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)prod;
            carry = prod >> 32;
        }
        out[i + k] = (uint32_t)carry;
    }
}

/* Compute rem = num mod m, with num being a (2k)-limb dividend and m
 * a k-limb modulus. Uses bit-by-bit shift-subtract long division.
 *
 * 2*k limb num → 2*k*32 bits to shift; per bit we do one bn_cmp + one
 * conditional bn_sub on a (k+1)-limb working register. For k=64
 * (2048-bit RSA): 4096 bits × 65 limbs = 266 k cheap ops per modmul.
 * × 17 modmuls in modexp e=65537 = 4.5M ops ≈ 1.5 ms. Adequate.
 */
static void bn_mod(uint32_t *out_rem, const uint32_t *num, int k_num,
                    const uint32_t *m, int k_m)
{
    /* working register: k_m + 1 limbs (one bit headroom). */
    uint32_t w[BN_MAX_LIMBS + 1] = {0};

    int bits = k_num * 32;
    for (int b = bits - 1; b >= 0; b--) {
        /* shift w left by 1 */
        uint32_t carry = 0;
        for (int i = 0; i < k_m + 1; i++) {
            uint32_t nxt = (w[i] >> 31) & 1;
            w[i] = (w[i] << 1) | carry;
            carry = nxt;
        }
        /* OR in bit b of num */
        uint32_t bit = (num[b / 32] >> (b & 31)) & 1;
        w[0] |= bit;

        /* If w >= m, subtract m */
        int cmp;
        if (w[k_m]) {
            cmp = 1;
        } else {
            cmp = bn_cmp(w, m, k_m);
        }
        if (cmp >= 0) {
            uint64_t borrow = 0;
            for (int i = 0; i < k_m; i++) {
                uint64_t d = (uint64_t)w[i] - m[i] - borrow;
                w[i] = (uint32_t)d;
                borrow = (d >> 63) & 1;
            }
            /* propagate to top limb */
            uint64_t d = (uint64_t)w[k_m] - borrow;
            w[k_m] = (uint32_t)d;
        }
    }
    memcpy(out_rem, w, k_m * sizeof(uint32_t));
}

/* out = a * b mod m, all k limbs (mod the same width). */
static void bn_mod_mul(uint32_t *out, const uint32_t *a, const uint32_t *b,
                        const uint32_t *m, int k)
{
    BnDouble prod;
    bn_double_zero(&prod);
    bn_mul(prod.v, a, b, k);
    bn_mod(out, prod.v, 2 * k, m, k);
}

/* out = base^exp mod m. exp is a big-endian byte string (matches DER
 * INTEGER encoding). k = number of limbs in m. Caller pre-loads base
 * into a k-limb form (already reduced mod m if base ≥ m).
 *
 * Implementation: square-and-multiply (left-to-right), scanning the
 * exponent MSB-first to LSB-last. */
static void bn_mod_exp(uint32_t *out, const uint32_t *base,
                        const uint8_t *exp_be, uint32_t exp_len,
                        const uint32_t *m, int k)
{
    /* Skip leading-zero bytes (DER positive integers may have one
     * leading 0x00 to avoid sign-bit ambiguity — asn1_read_integer
     * already strips that, but be defensive). */
    while (exp_len > 0 && exp_be[0] == 0) {
        exp_be++; exp_len--;
    }

    /* result = 1 */
    uint32_t result[BN_MAX_LIMBS] = {0};
    result[0] = 1;

    /* Find the highest set bit of the exponent. */
    if (exp_len == 0) {
        /* base^0 = 1 mod m */
        memcpy(out, result, k * sizeof(uint32_t));
        return;
    }

    /* Locate MSB of exp_be[0]. */
    uint8_t top_byte = exp_be[0];
    int top_bit = 7;
    while (top_bit >= 0 && !(top_byte & (1u << top_bit))) top_bit--;
    if (top_bit < 0) {
        memcpy(out, result, k * sizeof(uint32_t));
        return;
    }

    /* Iterate exp bits MSB → LSB. The very first bit (highest set bit)
     * is processed differently: result starts as 1, so square+multiply
     * for that bit yields `base`. We can short-circuit by initialising
     * result = base after that bit. Practically, doing it inside the
     * loop is fine. */
    uint32_t tmp[BN_MAX_LIMBS];
    for (uint32_t byte_idx = 0; byte_idx < exp_len; byte_idx++) {
        int bit_lo = (byte_idx == 0) ? top_bit : 7;
        for (int b = bit_lo; b >= 0; b--) {
            /* result = result^2 mod m */
            bn_mod_mul(tmp, result, result, m, k);
            memcpy(result, tmp, k * sizeof(uint32_t));
            /* if bit set: result = result * base mod m */
            if (exp_be[byte_idx] & (1u << b)) {
                bn_mod_mul(tmp, result, base, m, k);
                memcpy(result, tmp, k * sizeof(uint32_t));
            }
        }
    }
    memcpy(out, result, k * sizeof(uint32_t));
}

/* Store k-limb bigint as big-endian into byte buffer of size byte_len.
 * Pads with leading zeros if needed. */
static void bn_store_be(const uint32_t *v, int k, uint8_t *out, uint32_t byte_len)
{
    for (uint32_t i = 0; i < byte_len; i++) {
        uint32_t byte_from_end = byte_len - 1 - i;
        uint32_t limb_idx      = byte_from_end / 4;
        uint32_t shift         = (byte_from_end % 4) * 8;
        if ((int)limb_idx < k) {
            out[i] = (uint8_t)(v[limb_idx] >> shift);
        } else {
            out[i] = 0;
        }
    }
}

/* =========================================================================
 * PKCS#1 v1.5 EMSA decoding
 *
 * Per RFC 8017 §9.2 step 4, the encoded message must look like:
 *
 *    EM = 0x00 || 0x01 || PS || 0x00 || T
 *
 * where PS is at least 8 bytes of 0xFF, and T is the DigestInfo DER:
 *
 *    DigestInfo ::= SEQUENCE {
 *        digestAlgorithm AlgorithmIdentifier,
 *        digest OCTET STRING
 *    }
 *
 * For SHA-256 the canonical T is the 51-byte sequence below.
 * ========================================================================= */

/* DigestInfo prefix for SHA-256 — 19 bytes, followed by the 32-byte hash. */
static const uint8_t SHA256_DIGEST_INFO_PREFIX[19] = {
    0x30, 0x31,                                     /* SEQUENCE, len 49 */
    0x30, 0x0D,                                     /* SEQUENCE, len 13 (AlgId) */
    0x06, 0x09,                                     /* OID, len 9 */
    0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, /* SHA-256 OID */
    0x05, 0x00,                                     /* NULL (parameters) */
    0x04, 0x20                                      /* OCTET STRING, len 32 */
};
#define SHA256_T_LEN  (sizeof(SHA256_DIGEST_INFO_PREFIX) + 32)   /* 51 */

bool efi_rsa_verify_pkcs1_sha256(const EfiRsaPublicKey *key,
                                  const uint8_t *sha256,
                                  const uint8_t *signature,
                                  uint32_t       sig_len)
{
    if (!key || !sha256 || !signature) return false;
    if (key->modulus_len == 0)         return false;
    if (sig_len != key->modulus_len)   return false;

    /* Spec: PS must be ≥ 8 bytes. PS_len = k - 3 - T_len. So
     *   k ≥ 3 + 8 + 51 = 62 bytes (= 496 bits). All real RSA keys
     * (1024+) clear this trivially; reject below. */
    if (key->modulus_len < 62) return false;
    if (key->modulus_len > BN_MAX_LIMBS * 4) return false;

    /* Decide limb count: ceil(modulus_len / 4). */
    int k = (int)((key->modulus_len + 3) / 4);

    /* Load modulus + signature. Reject malformed (oversized) inputs. */
    BnNum m_bn, s_bn;
    if (!bn_load_be(&m_bn, key->modulus, key->modulus_len)) return false;
    if (!bn_load_be(&s_bn, signature,    sig_len))          return false;

    /* Reject malformed RSA keys: modulus must be > 1 (a zero or unit
     * modulus would cause silently wrong modular arithmetic). The
     * "modulus > 1" check is approximated by "modulus != 0" since
     * legitimate keys are always thousands of bits long. */
    if (bn_is_zero(m_bn.v, k)) return false;

    /* Spec sanity: signature must be in range [0, n-1]. */
    if (bn_cmp(s_bn.v, m_bn.v, k) >= 0) return false;

    /* RSAVP1: em_int = s^e mod n */
    BnNum em_bn;
    bn_zero(&em_bn);
    bn_mod_exp(em_bn.v, s_bn.v, key->exponent, key->exponent_len,
                m_bn.v, k);

    /* Convert em_int → byte string of exact modulus length. */
    uint8_t em[BN_MAX_LIMBS * 4];
    bn_store_be(em_bn.v, k, em, key->modulus_len);

    /* RFC 8017 §9.2 step 4 decode check. */
    if (em[0] != 0x00 || em[1] != 0x01) return false;

    uint32_t i = 2;
    while (i < key->modulus_len && em[i] == 0xFF) i++;
    uint32_t ps_len = i - 2;
    if (ps_len < 8) return false;                       /* PS too short */
    if (i >= key->modulus_len)         return false;
    if (em[i] != 0x00) return false;
    i++;

    /* The remaining bytes must be exactly T = DigestInfo for SHA-256. */
    uint32_t t_off = i;
    if (key->modulus_len - t_off != SHA256_T_LEN) return false;

    if (memcmp(em + t_off, SHA256_DIGEST_INFO_PREFIX,
                sizeof(SHA256_DIGEST_INFO_PREFIX)) != 0) return false;

    if (memcmp(em + t_off + sizeof(SHA256_DIGEST_INFO_PREFIX),
                sha256, 32) != 0) return false;

    return true;
}
