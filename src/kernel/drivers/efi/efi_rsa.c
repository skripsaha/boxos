
#include "efi_rsa.h"
#include "klib.h"


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

static bool bn_is_zero(const uint32_t *v, int limbs)
{
    for (int i = 0; i < limbs; i++) if (v[i]) return false;
    return true;
}

static int bn_cmp(const uint32_t *a, const uint32_t *b, int limbs)
{
    for (int i = limbs - 1; i >= 0; i--) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

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

static void bn_mod(uint32_t *out_rem, const uint32_t *num, int k_num,
                    const uint32_t *m, int k_m)
{
    uint32_t w[BN_MAX_LIMBS + 1] = {0};

    int bits = k_num * 32;
    for (int b = bits - 1; b >= 0; b--) {
        uint32_t carry = 0;
        for (int i = 0; i < k_m + 1; i++) {
            uint32_t nxt = (w[i] >> 31) & 1;
            w[i] = (w[i] << 1) | carry;
            carry = nxt;
        }
        uint32_t bit = (num[b / 32] >> (b & 31)) & 1;
        w[0] |= bit;

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
            uint64_t d = (uint64_t)w[k_m] - borrow;
            w[k_m] = (uint32_t)d;
        }
    }
    memcpy(out_rem, w, k_m * sizeof(uint32_t));
}

static void bn_mod_mul(uint32_t *out, const uint32_t *a, const uint32_t *b,
                        const uint32_t *m, int k)
{
    BnDouble prod;
    bn_double_zero(&prod);
    bn_mul(prod.v, a, b, k);
    bn_mod(out, prod.v, 2 * k, m, k);
}

static void bn_mod_exp(uint32_t *out, const uint32_t *base,
                        const uint8_t *exp_be, uint32_t exp_len,
                        const uint32_t *m, int k)
{
    while (exp_len > 0 && exp_be[0] == 0) {
        exp_be++; exp_len--;
    }

    uint32_t result[BN_MAX_LIMBS] = {0};
    result[0] = 1;

    if (exp_len == 0) {
        memcpy(out, result, k * sizeof(uint32_t));
        return;
    }

    uint8_t top_byte = exp_be[0];
    int top_bit = 7;
    while (top_bit >= 0 && !(top_byte & (1u << top_bit))) top_bit--;
    if (top_bit < 0) {
        memcpy(out, result, k * sizeof(uint32_t));
        return;
    }

    uint32_t tmp[BN_MAX_LIMBS];
    for (uint32_t byte_idx = 0; byte_idx < exp_len; byte_idx++) {
        int bit_lo = (byte_idx == 0) ? top_bit : 7;
        for (int b = bit_lo; b >= 0; b--) {
            bn_mod_mul(tmp, result, result, m, k);
            memcpy(result, tmp, k * sizeof(uint32_t));
            if (exp_be[byte_idx] & (1u << b)) {
                bn_mod_mul(tmp, result, base, m, k);
                memcpy(result, tmp, k * sizeof(uint32_t));
            }
        }
    }
    memcpy(out, result, k * sizeof(uint32_t));
}

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


static const uint8_t SHA256_DIGEST_INFO_PREFIX[19] = {
    0x30, 0x31,
    0x30, 0x0D,
    0x06, 0x09,
    0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
    0x05, 0x00,
    0x04, 0x20
};
#define SHA256_T_LEN  (sizeof(SHA256_DIGEST_INFO_PREFIX) + 32)

bool efi_rsa_verify_pkcs1_sha256(const EfiRsaPublicKey *key,
                                  const uint8_t *sha256,
                                  const uint8_t *signature,
                                  uint32_t       sig_len)
{
    if (!key || !sha256 || !signature) return false;
    if (key->modulus_len == 0)         return false;
    if (sig_len != key->modulus_len)   return false;

    if (key->modulus_len < 62) return false;
    if (key->modulus_len > BN_MAX_LIMBS * 4) return false;

    int k = (int)((key->modulus_len + 3) / 4);

    BnNum m_bn, s_bn;
    if (!bn_load_be(&m_bn, key->modulus, key->modulus_len)) return false;
    if (!bn_load_be(&s_bn, signature,    sig_len))          return false;

    if (bn_is_zero(m_bn.v, k)) return false;

    if (bn_cmp(s_bn.v, m_bn.v, k) >= 0) return false;

    BnNum em_bn;
    bn_zero(&em_bn);
    bn_mod_exp(em_bn.v, s_bn.v, key->exponent, key->exponent_len,
                m_bn.v, k);

    uint8_t em[BN_MAX_LIMBS * 4];
    bn_store_be(em_bn.v, k, em, key->modulus_len);

    if (em[0] != 0x00 || em[1] != 0x01) return false;

    uint32_t i = 2;
    while (i < key->modulus_len && em[i] == 0xFF) i++;
    uint32_t ps_len = i - 2;
    if (ps_len < 8) return false;
    if (i >= key->modulus_len)         return false;
    if (em[i] != 0x00) return false;
    i++;

    uint32_t t_off = i;
    if (key->modulus_len - t_off != SHA256_T_LEN) return false;

    if (memcmp(em + t_off, SHA256_DIGEST_INFO_PREFIX,
                sizeof(SHA256_DIGEST_INFO_PREFIX)) != 0) return false;

    if (memcmp(em + t_off + sizeof(SHA256_DIGEST_INFO_PREFIX),
                sha256, 32) != 0) return false;

    return true;
}