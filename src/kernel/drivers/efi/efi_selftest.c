/*
 * BoxOS — EFI/crypto self-test.
 *
 * Tests:
 *   T1  SHA-256 against FIPS 180-4 test vectors (one-shot + streaming).
 *   T2  Streaming SHA-256 matches one-shot for randomized chunk splits.
 *   T3  ASN.1 DER parser round-trips a known SEQUENCE { INTEGER, OID }.
 *   T4  RSA-PKCS1-v1.5 verify REJECTS: a non-signature, a wrong length,
 *       and a zero modulus. All three return before the bigint core runs.
 *   T5  RSA-PKCS1-v1.5 SHA-256 verify ACCEPTS a real 2048-bit signature
 *       and rejects a one-bit corruption of it and of the digest.
 *   T6  SHA-384 / SHA-512 against FIPS 180-4 vectors, both padding
 *       branches, plus streaming-equals-one-shot for arbitrary splits.
 *
 * ‼ T5 is the one that covers anything. This list used to claim a T5 that
 * did not exist and a T4 that "exercises bigint modular exponentiation" —
 * and T4's three assertions all return inside efi_rsa_verify_pkcs1_sha256's
 * argument checks, before bn_mod_exp is ever reached. bn_mul, bn_mod,
 * bn_mod_mul, bn_mod_exp and bn_store_be had exactly zero coverage, so a
 * verifier that returned false unconditionally passed the whole self-test
 * and the breakage showed up only as every signed image coming back
 * "signature invalid" — indistinguishable from a genuinely untrusted image.
 * A positive vector is the only assertion that can tell those apart.
 *
 * The aim is not to substitute for real-platform Authenticode validation
 * but to ensure the primitives themselves are correct after every code
 * change — so a regression in SHA-256 or RSA modexp is detected at boot
 * instead of in the field.
 */

#include "efi_selftest.h"
#include "efi_asn1.h"
#include "efi_rsa.h"
#include "efi_authenticode.h"   /* efi_sha512* — T6 */
#include "klib.h"
#include "crypto.h"

/* =========================================================================
 * T1 — SHA-256 known answers (FIPS 180-4 Appendix B examples)
 * ========================================================================= */

/* "abc" → ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
static const uint8_t SHA256_VEC_ABC[32] = {
    0xba,0x78,0x16,0xbf, 0x8f,0x01,0xcf,0xea,
    0x41,0x41,0x40,0xde, 0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3, 0x96,0x17,0x7a,0x9c,
    0xb4,0x10,0xff,0x61, 0xf2,0x00,0x15,0xad
};

/* "" (empty) → e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
static const uint8_t SHA256_VEC_EMPTY[32] = {
    0xe3,0xb0,0xc4,0x42, 0x98,0xfc,0x1c,0x14,
    0x9a,0xfb,0xf4,0xc8, 0x99,0x6f,0xb9,0x24,
    0x27,0xae,0x41,0xe4, 0x64,0x9b,0x93,0x4c,
    0xa4,0x95,0x99,0x1b, 0x78,0x52,0xb8,0x55
};

/* "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" →
 *   248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1 */
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
        kprintf("[SELFTEST] T1 sha256(abc) mismatch\n");
        return false;
    }

    KSha256((const uint8_t *)"", 0, out);
    if (memcmp(out, SHA256_VEC_EMPTY, 32) != 0) {
        kprintf("[SELFTEST] T1 sha256(empty) mismatch\n");
        return false;
    }

    KSha256(SHA256_VEC_LONG_INPUT, sizeof(SHA256_VEC_LONG_INPUT) - 1, out);
    if (memcmp(out, SHA256_VEC_LONG, 32) != 0) {
        kprintf("[SELFTEST] T1 sha256(long) mismatch\n");
        return false;
    }
    return true;
}

/* =========================================================================
 * T2 — Streaming SHA-256 must match one-shot for arbitrary chunk splits.
 *
 * Walks through a 1024-byte buffer of repeating bytes and hashes it both
 * via KSha256 in one shot and via KSha256Init/Update/Final fed one byte,
 * five bytes, 64 bytes, and 100 bytes at a time. If any of the streaming
 * digests diverges from the one-shot result, the streaming path is buggy
 * and the Authenticode hash (which uses streaming) cannot be trusted.
 * ========================================================================= */

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
            kprintf("[SELFTEST] T2 streaming chunk=%u diverged\n", cs);
            return false;
        }
    }
    return true;
}

/* =========================================================================
 * T3 — DER parser handles a known SEQUENCE { INTEGER, OID }.
 *
 * Hand-encoded:
 *   SEQUENCE (12 bytes) {
 *     INTEGER (1 byte)  0x2A   → 42 decimal
 *     OID     (7 bytes) 1.2.840.113549.1.1.1  partial
 *   }
 *
 *   30 0C
 *   02 01 2A
 *   06 07 2A 86 48 86 F7 0D 01
 * ========================================================================= */

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
        kprintf("[SELFTEST] T3 open SEQUENCE failed\n");
        return false;
    }
    const uint8_t *intval = NULL; uint32_t intlen = 0;
    if (!asn1_read_integer(&seq, &intval, &intlen)) {
        kprintf("[SELFTEST] T3 read INTEGER failed\n");
        return false;
    }
    if (intlen != 1 || intval[0] != 0x2A) {
        kprintf("[SELFTEST] T3 INTEGER value mismatch (len=%u val=0x%x)\n",
                intlen, intval ? intval[0] : 0);
        return false;
    }
    const uint8_t *oidval = NULL; uint32_t oidlen = 0;
    if (!asn1_read_typed(&seq, ASN1_TAG_OID, &oidval, &oidlen)) {
        kprintf("[SELFTEST] T3 read OID failed\n");
        return false;
    }
    if (oidlen != 7 || oidval[0] != 0x2A) {
        kprintf("[SELFTEST] T3 OID value mismatch\n");
        return false;
    }
    return true;
}

/* =========================================================================
 * RSA test vector — a real 2048-bit key, a real PKCS#1 v1.5 SHA-256
 * signature, and the fixed message they were made over.
 *
 * Produced once, offline, with OpenSSL 3.6.2:
 *
 *   openssl genrsa -out sk.pem 2048
 *   printf 'BoxOS EFI self-test vector v1' > msg.bin
 *   openssl dgst -sha256 -sign sk.pem -out sig.bin msg.bin
 *   openssl rsa -in sk.pem -pubout | openssl rsa -pubin -noout -modulus
 *
 * The private key was never kept and is not needed: verification uses the
 * public half only, and a public modulus plus a signature over a published
 * message are not secrets. sk.pem existed for the length of that shell
 * pipeline and nothing about this key protects anything.
 *
 * e = 65537, which is what firmware-signed certificates use in practice, so
 * bn_mod_exp walks all 24 bit positions of the three-byte exponent instead
 * of the short ladder a small e would give it. 2048 bits is 64 limbs through
 * bn_mod_mul — the same arithmetic an Authenticode chain walk performs.
 * Cost: 512 bytes of .rodata.
 * ========================================================================= */

static const uint8_t SELFTEST_RSA_MSG[] = "BoxOS EFI self-test vector v1";

static const uint8_t SELFTEST_RSA_MODULUS[256] = {
    0xea,0x59,0x6c,0x00,0xda,0xce,0x1f,0x04, 0xe3,0xaf,0x64,0x7f,0x27,0x81,0x61,0xcc,
    0xde,0xbf,0x81,0x91,0x59,0x71,0x93,0x20, 0xb5,0xf6,0x7e,0x53,0x89,0xca,0xc7,0xe7,
    0xf7,0xa4,0x38,0x23,0xcf,0xbd,0xd4,0x47, 0xc8,0x2e,0x33,0x26,0xfa,0x23,0x48,0xac,
    0xf0,0x2c,0x1a,0x5b,0xe0,0xea,0x57,0x47, 0x56,0x52,0xb9,0x89,0x65,0x4a,0x75,0xd0,
    0x61,0x58,0x31,0xa0,0xeb,0x20,0xde,0x3b, 0xa2,0xb3,0xf4,0x6d,0x22,0xfb,0x46,0xde,
    0x0a,0x4b,0xba,0xbc,0xa2,0x8a,0x3b,0xf9, 0x69,0xd8,0x2e,0x69,0x98,0x76,0x0a,0xfd,
    0xb3,0xf0,0x58,0xe0,0x1e,0x56,0xe4,0x85, 0xd6,0x83,0x44,0x9b,0xe7,0x67,0xf1,0xe9,
    0xb6,0x88,0x9d,0x0a,0xc0,0xdc,0xbf,0x39, 0x02,0x1d,0xb3,0x54,0xce,0xc8,0x3d,0x2c,
    0x2d,0x8d,0xef,0xd5,0x68,0xcb,0x7b,0x58, 0x37,0x01,0xac,0xb2,0x0f,0xab,0xf4,0x53,
    0xd8,0x0b,0xde,0xf5,0x83,0x0c,0x89,0xeb, 0x5f,0xa2,0x43,0x9d,0xb6,0x65,0x02,0x06,
    0x01,0xc7,0xa5,0x14,0x7f,0x58,0xc5,0x6c, 0x22,0x1f,0xdb,0xb8,0x7e,0x71,0xca,0x0f,
    0x89,0x0c,0x5e,0x94,0x16,0x40,0xde,0xf4, 0x8b,0xde,0x66,0xd2,0x97,0xde,0x22,0x0c,
    0x49,0xcc,0x63,0xd3,0xde,0xed,0x46,0xe1, 0xc3,0xba,0x07,0xf1,0x1c,0x1f,0x76,0xe0,
    0x5d,0x09,0xd4,0xd4,0x0a,0xa8,0x46,0x53, 0x25,0x17,0xba,0x15,0x95,0x5f,0xa1,0x1c,
    0x4f,0xdf,0x50,0x8b,0xb5,0xdc,0x23,0x47, 0x9e,0x42,0xb3,0xee,0xc3,0xb6,0xa7,0x3e,
    0xa5,0xe5,0x59,0xb6,0xd6,0xff,0x33,0x2c, 0xc7,0x06,0xb0,0x74,0xb8,0x80,0xb4,0x81,
};

static const uint8_t SELFTEST_RSA_EXPONENT[3] = { 0x01, 0x00, 0x01 };  /* 65537 */

static const uint8_t SELFTEST_RSA_SIGNATURE[256] = {
    0x89,0x77,0xdb,0x4c,0x49,0x97,0x91,0x18, 0x7e,0xcb,0x40,0x45,0x68,0x8e,0xd7,0x46,
    0xf3,0xfe,0x32,0x60,0xcf,0xb4,0xc7,0x69, 0x92,0x86,0xad,0x1a,0xd8,0x70,0xc6,0x42,
    0x0c,0x6c,0xcd,0x29,0xd3,0xed,0xbc,0xdd, 0xcc,0x32,0xfd,0x41,0xbd,0x00,0xb5,0x77,
    0xb0,0xe8,0x5e,0x2f,0x2b,0xb7,0x43,0x6b, 0x89,0x6c,0xf2,0xbe,0xaf,0x9f,0x08,0xc4,
    0xbb,0x4f,0xb7,0x00,0xf0,0x44,0x2c,0x38, 0xf9,0x1c,0x54,0xe0,0x37,0x1f,0x7b,0x8c,
    0xf4,0xaf,0x57,0xcd,0xd0,0x6f,0x71,0x6c, 0x51,0x5c,0x3d,0x97,0x7c,0xba,0x63,0x5b,
    0x74,0x4c,0x9d,0x5d,0xac,0xcc,0x78,0x22, 0xc7,0xe4,0x4e,0x71,0x57,0x8b,0xa8,0x7e,
    0x00,0x75,0x79,0xa5,0xcd,0xd2,0x91,0xb8, 0xa1,0x25,0x0f,0x2a,0x2f,0x28,0xa9,0xf0,
    0x94,0x58,0x33,0xcd,0xb0,0x46,0xc1,0x9b, 0x7c,0x70,0xdb,0x48,0x9c,0x62,0xe0,0x31,
    0xe6,0x61,0x68,0xbe,0xb8,0xab,0xd6,0x44, 0x04,0x51,0x26,0xcb,0xa1,0x27,0x75,0x6f,
    0x0f,0x2d,0x6b,0xec,0xd5,0x0c,0x25,0xaa, 0x72,0x45,0x85,0xd3,0x30,0x49,0x87,0xb6,
    0xb4,0xc9,0x29,0xfa,0x84,0x9a,0xd7,0xbc, 0xb7,0x99,0xac,0x92,0x6d,0xcb,0x26,0x16,
    0x06,0xaf,0xe8,0x06,0x53,0x02,0x30,0xd9, 0x95,0xc0,0xb9,0x9c,0xd4,0x28,0xd2,0x15,
    0x94,0x67,0x8e,0x42,0xf9,0xd0,0x16,0x96, 0xc1,0x68,0x52,0x4d,0x6c,0x7d,0xf6,0x81,
    0xd9,0xa8,0xd7,0x3c,0x32,0x9e,0x02,0x99, 0x30,0xc9,0x75,0x4b,0x3f,0x01,0x92,0x36,
    0xce,0xc3,0x37,0x76,0x66,0x0e,0x31,0xe9, 0x0a,0x09,0xbb,0x28,0xc4,0xa3,0x52,0x03,
};

/* =========================================================================
 * T4 — the verifier's refusals.
 *
 * Three inputs it must reject, and all three are rejected by the argument
 * checks at the top of efi_rsa_verify_pkcs1_sha256 — before bn_mod_exp.
 * That is worth saying out loud, because this test used to be described as
 * exercising modular exponentiation and it does not touch it. T5 is what
 * covers the bigint core.
 * ========================================================================= */

static bool t4_rsa_refusals(void)
{
    uint8_t hash[32];
    KSha256(SELFTEST_RSA_MSG, sizeof(SELFTEST_RSA_MSG) - 1, hash);

    /* sig_len == modulus_len, but the value is >= n (top byte 0xFF beats the
     * modulus's 0xEA), which RFC 8017 forbids as a signature representative. */
    uint8_t bad_sig[sizeof(SELFTEST_RSA_MODULUS)];
    memset(bad_sig, 0xFF, sizeof(bad_sig));

    EfiRsaPublicKey k = {
        .modulus      = SELFTEST_RSA_MODULUS,
        .modulus_len  = sizeof(SELFTEST_RSA_MODULUS),
        .exponent     = SELFTEST_RSA_EXPONENT,
        .exponent_len = sizeof(SELFTEST_RSA_EXPONENT),
    };
    if (efi_rsa_verify_pkcs1_sha256(&k, hash, bad_sig, sizeof(bad_sig))) {
        kprintf("[SELFTEST] T4 RSA accepted a signature >= modulus\n");
        return false;
    }

    /* Wrong signature length. */
    if (efi_rsa_verify_pkcs1_sha256(&k, hash, bad_sig, sizeof(bad_sig) / 2)) {
        kprintf("[SELFTEST] T4 RSA accepted truncated signature\n");
        return false;
    }

    /* Zero modulus. */
    uint8_t zero_mod[sizeof(SELFTEST_RSA_MODULUS)] = {0};
    EfiRsaPublicKey zk = {
        .modulus      = zero_mod,
        .modulus_len  = sizeof(zero_mod),
        .exponent     = SELFTEST_RSA_EXPONENT,
        .exponent_len = sizeof(SELFTEST_RSA_EXPONENT),
    };
    if (efi_rsa_verify_pkcs1_sha256(&zk, hash, bad_sig, sizeof(bad_sig))) {
        kprintf("[SELFTEST] T4 RSA accepted zero-modulus key\n");
        return false;
    }
    return true;
}

/* =========================================================================
 * T5 — the verifier ACCEPTS a genuine signature, and only that one.
 *
 * This is the assertion the whole self-test was missing. It runs the entire
 * chain end to end: KSha256 over the message, bn_load_be of modulus and
 * signature, bn_mod_exp with e=65537 over 64 limbs, bn_store_be of the
 * result, and the EMSA-PKCS1-v1.5 decode of EM = 0x00 0x01 PS 0x00
 * DigestInfo(SHA-256, hash) against the expected DigestInfo envelope
 * (RFC 8017 §9.2 step 4). A regression anywhere on that path — including a
 * verifier that simply returns false — fails here and only here.
 *
 * Then the two negatives that make the positive mean something: one flipped
 * bit in the signature, and one flipped bit in the digest.
 * ========================================================================= */

static bool t5_rsa_known_good(void)
{
    uint8_t hash[32];
    KSha256(SELFTEST_RSA_MSG, sizeof(SELFTEST_RSA_MSG) - 1, hash);

    EfiRsaPublicKey k = {
        .modulus      = SELFTEST_RSA_MODULUS,
        .modulus_len  = sizeof(SELFTEST_RSA_MODULUS),
        .exponent     = SELFTEST_RSA_EXPONENT,
        .exponent_len = sizeof(SELFTEST_RSA_EXPONENT),
    };

    if (!efi_rsa_verify_pkcs1_sha256(&k, hash, SELFTEST_RSA_SIGNATURE,
                                      sizeof(SELFTEST_RSA_SIGNATURE))) {
        kprintf("[SELFTEST] T5 RSA rejected a known-good signature\n");
        return false;
    }

    /* One bit of the signature, in the middle where the modexp result
     * differs wholesale rather than in a padding byte. */
    uint8_t tampered[sizeof(SELFTEST_RSA_SIGNATURE)];
    memcpy(tampered, SELFTEST_RSA_SIGNATURE, sizeof(tampered));
    tampered[sizeof(tampered) / 2] ^= 0x01;
    if (efi_rsa_verify_pkcs1_sha256(&k, hash, tampered, sizeof(tampered))) {
        kprintf("[SELFTEST] T5 RSA accepted a one-bit-corrupted "
                "signature\n");
        return false;
    }

    /* One bit of the digest — the signature is intact, the message is not. */
    uint8_t wrong_hash[32];
    memcpy(wrong_hash, hash, sizeof(wrong_hash));
    wrong_hash[31] ^= 0x01;
    if (efi_rsa_verify_pkcs1_sha256(&k, wrong_hash, SELFTEST_RSA_SIGNATURE,
                                     sizeof(SELFTEST_RSA_SIGNATURE))) {
        kprintf("[SELFTEST] T5 RSA accepted a signature over a "
                "different digest\n");
        return false;
    }
    return true;
}

/* =========================================================================
 * T6 — SHA-384 / SHA-512 known answers, both padding branches, and
 *      streaming-equals-one-shot.
 *
 * These two digests decide whether a dbx EFI_CERT_X509_SHA384/512
 * certificate revocation is honoured and whether an EFI_CERT_SHA384/512
 * image hash matches. Nothing else in BoxOS calls them, and no firmware
 * the project can boot under QEMU ships a dbx carrying such a record — so
 * without this test the arithmetic would first execute on a customer's
 * board, and a wrong digest fails OPEN: the revoked certificate compares
 * unequal and the image is accepted as valid. That is exactly the reason
 * T5 exists, applied to the newer primitive.
 *
 * Vectors: the FIPS 180-4 Appendix D examples ("abc" and the 112-byte
 * two-block message), the published empty-message digests, and a
 * generated 1024-byte pattern used for the exact-block-multiple case and
 * for the streaming comparison. Between them they cover both padding
 * branches (remainder < 112 and remainder >= 112 — the latter is what the
 * 112-byte message forces), the exact 128-byte multiple, the multi-block
 * loop, and update() calls that split a block.
 *
 * Failures here print with kprintf. So do T1-T5's now: the argument that
 * was written for this test alone — a production boot (DEBUG=off) must be
 * able to say WHICH vector broke, not only that something did — never
 * distinguished between them. debug_printf compiles to nothing at DEBUG=off,
 * so on the boot that matters every T1-T5 failure was silent and all the
 * operator saw was main.c's "EFI self-test FAILED", with the name of the
 * broken primitive left in a build they were not running. The PASS lines
 * and the per-test FAIL echo in efi_selftest_run stay on debug_printf: a
 * healthy boot owes the console nothing, and the echo says no more than the
 * named line above it already did.
 * ========================================================================= */

static const uint8_t SHA_VEC_MSG_112[] =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
    "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

static const uint8_t SHA512_VEC_ABC[64] = {
    0xdd,0xaf,0x35,0xa1,0x93,0x61,0x7a,0xba,
    0xcc,0x41,0x73,0x49,0xae,0x20,0x41,0x31,
    0x12,0xe6,0xfa,0x4e,0x89,0xa9,0x7e,0xa2,
    0x0a,0x9e,0xee,0xe6,0x4b,0x55,0xd3,0x9a,
    0x21,0x92,0x99,0x2a,0x27,0x4f,0xc1,0xa8,
    0x36,0xba,0x3c,0x23,0xa3,0xfe,0xeb,0xbd,
    0x45,0x4d,0x44,0x23,0x64,0x3c,0xe8,0x0e,
    0x2a,0x9a,0xc9,0x4f,0xa5,0x4c,0xa4,0x9f,
};

static const uint8_t SHA384_VEC_ABC[48] = {
    0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,
    0xb5,0xa0,0x3d,0x69,0x9a,0xc6,0x50,0x07,
    0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,
    0x1a,0x8b,0x60,0x5a,0x43,0xff,0x5b,0xed,
    0x80,0x86,0x07,0x2b,0xa1,0xe7,0xcc,0x23,
    0x58,0xba,0xec,0xa1,0x34,0xc8,0x25,0xa7,
};

static const uint8_t SHA512_VEC_EMPTY[64] = {
    0xcf,0x83,0xe1,0x35,0x7e,0xef,0xb8,0xbd,
    0xf1,0x54,0x28,0x50,0xd6,0x6d,0x80,0x07,
    0xd6,0x20,0xe4,0x05,0x0b,0x57,0x15,0xdc,
    0x83,0xf4,0xa9,0x21,0xd3,0x6c,0xe9,0xce,
    0x47,0xd0,0xd1,0x3c,0x5d,0x85,0xf2,0xb0,
    0xff,0x83,0x18,0xd2,0x87,0x7e,0xec,0x2f,
    0x63,0xb9,0x31,0xbd,0x47,0x41,0x7a,0x81,
    0xa5,0x38,0x32,0x7a,0xf9,0x27,0xda,0x3e,
};

static const uint8_t SHA384_VEC_EMPTY[48] = {
    0x38,0xb0,0x60,0xa7,0x51,0xac,0x96,0x38,
    0x4c,0xd9,0x32,0x7e,0xb1,0xb1,0xe3,0x6a,
    0x21,0xfd,0xb7,0x11,0x14,0xbe,0x07,0x43,
    0x4c,0x0c,0xc7,0xbf,0x63,0xf6,0xe1,0xda,
    0x27,0x4e,0xde,0xbf,0xe7,0x6f,0x65,0xfb,
    0xd5,0x1a,0xd2,0xf1,0x48,0x98,0xb9,0x5b,
};

static const uint8_t SHA512_VEC_112[64] = {
    0x8e,0x95,0x9b,0x75,0xda,0xe3,0x13,0xda,
    0x8c,0xf4,0xf7,0x28,0x14,0xfc,0x14,0x3f,
    0x8f,0x77,0x79,0xc6,0xeb,0x9f,0x7f,0xa1,
    0x72,0x99,0xae,0xad,0xb6,0x88,0x90,0x18,
    0x50,0x1d,0x28,0x9e,0x49,0x00,0xf7,0xe4,
    0x33,0x1b,0x99,0xde,0xc4,0xb5,0x43,0x3a,
    0xc7,0xd3,0x29,0xee,0xb6,0xdd,0x26,0x54,
    0x5e,0x96,0xe5,0x5b,0x87,0x4b,0xe9,0x09,
};

static const uint8_t SHA384_VEC_112[48] = {
    0x09,0x33,0x0c,0x33,0xf7,0x11,0x47,0xe8,
    0x3d,0x19,0x2f,0xc7,0x82,0xcd,0x1b,0x47,
    0x53,0x11,0x1b,0x17,0x3b,0x3b,0x05,0xd2,
    0x2f,0xa0,0x80,0x86,0xe3,0xb0,0xf7,0x12,
    0xfc,0xc7,0xc7,0x1a,0x55,0x7e,0x2d,0xb9,
    0x66,0xc3,0xe9,0xfa,0x91,0x74,0x60,0x39,
};

static const uint8_t SHA512_VEC_PAT128[64] = {
    0xdf,0x00,0x7a,0x08,0xf3,0xaa,0xae,0x47,
    0xe0,0xc9,0x2e,0xf8,0x40,0xec,0xd4,0x36,
    0x45,0xae,0x60,0x98,0xc8,0x19,0xf2,0xa4,
    0xa6,0x61,0x74,0xef,0x1c,0xd4,0x9e,0x5c,
    0x6d,0xfc,0xcf,0x06,0x16,0x89,0x5e,0x57,
    0x0b,0x75,0x64,0xaf,0x64,0x1d,0xe5,0x86,
    0x3d,0xff,0x9f,0x89,0xc7,0x52,0x91,0x3d,
    0x30,0xcf,0x0e,0xcf,0x67,0x8e,0x16,0x35,
};

static const uint8_t SHA384_VEC_PAT128[48] = {
    0x95,0xab,0x88,0x10,0x9c,0xbf,0x11,0x75,
    0x48,0xa8,0xad,0x35,0x90,0x9f,0xfe,0xfa,
    0x17,0x5d,0x06,0xa9,0xc4,0x26,0x14,0xb9,
    0xd3,0x59,0x54,0x8c,0xff,0x5d,0x55,0x0b,
    0x30,0xaf,0xfb,0x36,0xff,0x63,0x8c,0xd3,
    0x2a,0xa5,0xf5,0x40,0x90,0x66,0xc6,0x8a,
};

static const uint8_t SHA512_VEC_PAT1000[64] = {
    0xb4,0x1d,0x42,0xe1,0x06,0xec,0xa6,0xbf,
    0x57,0x12,0x35,0x66,0xb7,0xed,0x15,0x50,
    0xc3,0x7d,0x33,0xaf,0x23,0xaf,0xbf,0xa8,
    0xe3,0x02,0xdf,0xd4,0x4c,0x98,0x8b,0x30,
    0x03,0xa2,0x6b,0x41,0x40,0xef,0x42,0xf5,
    0x35,0xe6,0x6e,0xa4,0x24,0xe7,0xc4,0xa3,
    0x16,0x16,0x30,0x72,0x69,0xe6,0xd5,0x20,
    0xa6,0x50,0x8f,0x72,0x6d,0xa2,0x3d,0x0a,
};

static bool t6_check(const char *what, const uint8_t *data, uint32_t len,
                     bool sha384, const uint8_t *want)
{
    uint8_t got[64];
    uint32_t n = sha384 ? 48u : 64u;
    efi_sha512(data, len, sha384, got);
    if (memcmp(got, want, n) != 0) {
        kprintf("[SELFTEST] T6 sha-%u(%s) mismatch\n", sha384 ? 384u : 512u,
                what);
        return false;
    }
    return true;
}

static bool t6_sha512_vectors(void)
{
    static uint8_t pat[1024];
    for (uint32_t i = 0; i < sizeof(pat); i++) pat[i] = (uint8_t)(i * 31 + 7);

    if (!t6_check("abc", (const uint8_t *)"abc", 3, false, SHA512_VEC_ABC))
        return false;
    if (!t6_check("abc", (const uint8_t *)"abc", 3, true, SHA384_VEC_ABC))
        return false;
    if (!t6_check("empty", (const uint8_t *)"", 0, false, SHA512_VEC_EMPTY))
        return false;
    if (!t6_check("empty", (const uint8_t *)"", 0, true, SHA384_VEC_EMPTY))
        return false;
    /* 112 bytes: the padding cannot fit in the same block, so this is the
     * only vector that drives the second sha512_block from final(). */
    if (!t6_check("112-byte", SHA_VEC_MSG_112, 112, false, SHA512_VEC_112))
        return false;
    if (!t6_check("112-byte", SHA_VEC_MSG_112, 112, true, SHA384_VEC_112))
        return false;
    /* Exactly one block: update() consumes it whole and final() pads an
     * entire extra block from an empty buffer. */
    if (!t6_check("pattern-128", pat, 128, false, SHA512_VEC_PAT128))
        return false;
    if (!t6_check("pattern-128", pat, 128, true, SHA384_VEC_PAT128))
        return false;
    /* Multi-block with a remainder. */
    if (!t6_check("pattern-1000", pat, 1000, false, SHA512_VEC_PAT1000))
        return false;

    /* Streaming must agree with one-shot for arbitrary splits — the PE
     * Authenticode walk feeds these contexts one disjoint region at a
     * time, so a bug in the carry-over buffer would show up only there. */
    uint8_t ref[64];
    efi_sha512(pat, sizeof(pat), false, ref);

    const uint32_t chunks[] = {1, 5, 127, 128, 129, 200};
    for (uint32_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
        uint32_t cs = chunks[ci];
        EfiSha512Ctx ctx;
        efi_sha512_init(&ctx, false);
        uint32_t off = 0;
        while (off < sizeof(pat)) {
            uint32_t want = cs;
            if (off + want > sizeof(pat)) want = sizeof(pat) - off;
            efi_sha512_update(&ctx, pat + off, want);
            off += want;
        }
        uint8_t got[64];
        efi_sha512_final(&ctx, got);
        if (memcmp(got, ref, 64) != 0) {
            kprintf("[SELFTEST] T6 sha-512 streaming chunk=%u diverged\n", cs);
            return false;
        }
    }
    return true;
}

/* =========================================================================
 * Public entry
 * ========================================================================= */

bool efi_selftest_run(void)
{
    bool ok = true;
    if (!t1_sha256_vectors())       { ok = false; debug_printf("[SELFTEST] T1 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T1 sha256 vectors PASS\n");

    if (!t2_streaming_consistency()){ ok = false; debug_printf("[SELFTEST] T2 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T2 streaming consistency PASS\n");

    if (!t3_asn1_basic())           { ok = false; debug_printf("[SELFTEST] T3 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T3 asn1 parse PASS\n");

    if (!t4_rsa_refusals())         { ok = false; debug_printf("[SELFTEST] T4 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T4 rsa refusals PASS\n");

    if (!t5_rsa_known_good())       { ok = false; debug_printf("[SELFTEST] T5 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T5 rsa known-good vector PASS\n");

    if (!t6_sha512_vectors())       { ok = false; debug_printf("[SELFTEST] T6 FAIL\n"); }
    else                              debug_printf("[SELFTEST] T6 sha384/sha512 vectors PASS\n");

    debug_printf("[SELFTEST] Overall: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
