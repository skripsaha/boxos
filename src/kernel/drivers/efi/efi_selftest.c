
#include "efi_selftest.h"
#include "efi_asn1.h"
#include "efi_rsa.h"
#include "klib.h"
#include "crypto.h"


static const uint8_t SHA256_VEC_ABC[32] = {
    0xba,0x78,0x16,0xbf, 0x8f,0x01,0xcf,0xea,
    0x41,0x41,0x40,0xde, 0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3, 0x96,0x17,0x7a,0x9c,
    0xb4,0x10,0xff,0x61, 0xf2,0x00,0x15,0xad
};

static const uint8_t SHA256_VEC_EMPTY[32] = {
    0xe3,0xb0,0xc4,0x42, 0x98,0xfc,0x1c,0x14,
    0x9a,0xfb,0xf4,0xc8, 0x99,0x6f,0xb9,0x24,
    0x27,0xae,0x41,0xe4, 0x64,0x9b,0x93,0x4c,
    0xa4,0x95,0x99,0x1b, 0x78,0x52,0xb8,0x55
};

static const uint8_t SHA256_VEC_LONG_INPUT[] =
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const uint8_t SHA256_VEC_LONG[32] = {
    0x24,0x8d,0x6a,0x61, 0xd2,0x06,0x38,0xb8,
    0xe5,0xc0,0x26,0x93, 0x0c,0x3e,0x60,0x39,
    0xa3,0x3c,0xe4,0x59, 0x64,0xff,0x21,0x67,
    0xf6,0xec,0xed,0xd4, 0x19,0xdb,0x06,0xc1
};

static bool t1_sha256_vectors(void)
{
    uint8_t out[32];

    KSha256((const uint8_t *)"abc", 3, out);
    if (memcmp(out, SHA256_VEC_ABC, 32) != 0) {
        debug_printf("[SELFTEST] T1 sha256(abc) mismatch\n");
        return false;
    }

    KSha256((const uint8_t *)"", 0, out);
    if (memcmp(out, SHA256_VEC_EMPTY, 32) != 0) {
        debug_printf("[SELFTEST] T1 sha256(empty) mismatch\n");
        return false;
    }

    KSha256(SHA256_VEC_LONG_INPUT, sizeof(SHA256_VEC_LONG_INPUT) - 1, out);
    if (memcmp(out, SHA256_VEC_LONG, 32) != 0) {
        debug_printf("[SELFTEST] T1 sha256(long) mismatch\n");
        return false;
    }
    return true;
}


static bool t2_streaming_consistency(void)
{
    static uint8_t buf[1024];
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 31 + 7);

    uint8_t ref[32];
    KSha256(buf, sizeof(buf), ref);

    const uint32_t chunks[] = {1, 5, 63, 64, 100, 256};
    for (uint32_t ci = 0; ci < sizeof(chunks)/sizeof(chunks[0]); ci++) {
        uint32_t cs = chunks[ci];
        KSha256Ctx ctx;
        KSha256Init(&ctx);
        uint32_t off = 0;
        while (off < sizeof(buf)) {
            uint32_t want = cs;
            if (off + want > sizeof(buf)) want = sizeof(buf) - off;
            KSha256Update(&ctx, buf + off, want);
            off += want;
        }
        uint8_t got[32];
        KSha256Final(&ctx, got);
        if (memcmp(got, ref, 32) != 0) {
            debug_printf("[SELFTEST] T2 streaming chunk=%u diverged\n", cs);
            return false;
        }
    }
    return true;
}


static bool t3_asn1_basic(void)
{
    static const uint8_t der[] = {
        0x30, 0x0C,
        0x02, 0x01, 0x2A,
        0x06, 0x07, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01
    };
    Asn1Reader root = asn1_reader(der, sizeof(der));
    Asn1Reader seq;
    if (!asn1_open(&root, ASN1_TAG_SEQUENCE, &seq)) {
        debug_printf("[SELFTEST] T3 open SEQUENCE failed\n");
        return false;
    }
    const uint8_t *intval = NULL; uint32_t intlen = 0;
    if (!asn1_read_integer(&seq, &intval, &intlen)) {
        debug_printf("[SELFTEST] T3 read INTEGER failed\n");
        return false;
    }
    if (intlen != 1 || intval[0] != 0x2A) {
        debug_printf("[SELFTEST] T3 INTEGER value mismatch (len=%u val=0x%x)\n",
                     intlen, intval ? intval[0] : 0);
        return false;
    }
    const uint8_t *oidval = NULL; uint32_t oidlen = 0;
    if (!asn1_read_typed(&seq, ASN1_TAG_OID, &oidval, &oidlen)) {
        debug_printf("[SELFTEST] T3 read OID failed\n");
        return false;
    }
    if (oidlen != 7 || oidval[0] != 0x2A) {
        debug_printf("[SELFTEST] T3 OID value mismatch\n");
        return false;
    }
    return true;
}


static const uint8_t SELFTEST_RSA_MODULUS[128] = {
    0xb8,0x47,0xfa,0x82,0x40,0xa2,0x2b,0x1d, 0xc8,0xc7,0xa9,0x46,0x71,0xf1,0xa6,0xd5,
    0x07,0x29,0x35,0x49,0xdc,0x4e,0xfd,0xc2, 0x52,0xfa,0xc4,0xc1,0x83,0xed,0x05,0xf8,
    0x47,0x39,0x21,0x9e,0xd5,0xa8,0x9b,0xae, 0xe5,0x35,0xee,0x53,0xcd,0xd9,0x4c,0xfc,
    0xfe,0x49,0x10,0xc9,0xfa,0xc0,0xc6,0x71, 0xeb,0x14,0xbe,0xa1,0x7e,0xe5,0x4b,0x1a,
    0x4d,0x29,0x5c,0xa5,0x69,0xee,0x76,0xc1, 0x1f,0xed,0xf8,0xa1,0x82,0x7a,0xfb,0x7b,
    0x4e,0xf8,0x66,0x40,0x29,0x40,0x35,0x10, 0x44,0xa7,0xcc,0x37,0x05,0x09,0xb9,0xd1,
    0xed,0xf3,0xe5,0x06,0x33,0x40,0x35,0x9a, 0x4b,0x69,0xc3,0x82,0xea,0x3a,0xa0,0x6c,
    0xa5,0xb9,0x88,0xe8,0x42,0x10,0x4f,0xd3, 0xae,0x7f,0x44,0x49,0xfa,0xb6,0xb2,0x67
};

static const uint8_t SELFTEST_RSA_EXPONENT[3] = { 0x01, 0x00, 0x01 };


static bool t4_rsa_smoke(void)
{
    uint8_t hash[32] = {0};
    KSha256((const uint8_t *)"BoxOS-selftest", 14, hash);

    uint8_t bad_sig[128];
    memset(bad_sig, 0xFF, sizeof(bad_sig));

    EfiRsaPublicKey k = {
        .modulus      = SELFTEST_RSA_MODULUS,
        .modulus_len  = sizeof(SELFTEST_RSA_MODULUS),
        .exponent     = SELFTEST_RSA_EXPONENT,
        .exponent_len = sizeof(SELFTEST_RSA_EXPONENT),
    };
    if (efi_rsa_verify_pkcs1_sha256(&k, hash, bad_sig, sizeof(bad_sig))) {
        debug_printf("[SELFTEST] T4 RSA accepted obviously-bad signature\n");
        return false;
    }

    if (efi_rsa_verify_pkcs1_sha256(&k, hash, bad_sig, 64)) {
        debug_printf("[SELFTEST] T4 RSA accepted truncated signature\n");
        return false;
    }

    uint8_t zero_mod[128] = {0};
    EfiRsaPublicKey zk = {
        .modulus      = zero_mod,
        .modulus_len  = sizeof(zero_mod),
        .exponent     = SELFTEST_RSA_EXPONENT,
        .exponent_len = sizeof(SELFTEST_RSA_EXPONENT),
    };
    if (efi_rsa_verify_pkcs1_sha256(&zk, hash, bad_sig, sizeof(bad_sig))) {
        debug_printf("[SELFTEST] T4 RSA accepted zero-modulus key\n");
        return false;
    }
    return true;
}


bool efi_selftest_run(void)
{
    bool ok = true;
    if (!t1_sha256_vectors())       { ok = false; debug_printf("[SELFTEST] T1 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T1 sha256 vectors PASS\n");

    if (!t2_streaming_consistency()){ ok = false; debug_printf("[SELFTEST] T2 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T2 streaming consistency PASS\n");

    if (!t3_asn1_basic())           { ok = false; debug_printf("[SELFTEST] T3 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T3 asn1 parse PASS\n");

    if (!t4_rsa_smoke())            { ok = false; debug_printf("[SELFTEST] T4 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T4 rsa smoke PASS\n");

    debug_printf("[SELFTEST] Overall: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}