#ifndef EFI_RSA_H
#define EFI_RSA_H

/*
 * BoxOS — RSA-PKCS1-v1.5 verify (RFC 8017 §8.2).
 *
 * Public-key-only RSA: we recover the message representative from the
 * signature, EMSA-PKCS1-v1.5-decode it, and compare the embedded DigestInfo
 * to the expected SHA-256 hash. No private-key operations — verification
 * is the only flow Secure Boot needs.
 *
 * Supported key sizes: 1024, 2048, 3072, 4096 bits.
 * Supported public exponents: any positive integer encoded as a big-endian
 *   byte string; in practice firmware-signed certs all use e=65537.
 * Supported hash: SHA-256 (the only digest currently used by Microsoft's
 *   Authenticode + Linux shim signing pipelines).
 */

#include "ktypes.h"

/* Public-key handle. The caller fills modulus[] + modulus_len and
 * exponent[] + exponent_len from an X.509 SubjectPublicKeyInfo extraction;
 * the verifier reads them and treats them as opaque. */
typedef struct {
    const uint8_t *modulus;
    uint32_t       modulus_len;     /* bytes; ≤ 512 (= 4096 bits) */
    const uint8_t *exponent;
    uint32_t       exponent_len;
} EfiRsaPublicKey;

/* Verify an RSA-PKCS1-v1.5 SHA-256 signature.
 *
 *   key        public-key components as parsed from the cert.
 *   sha256     32-byte SHA-256 digest of the signed content.
 *   signature  signature octet string (length == modulus_len).
 *   sig_len    bytes in signature; must match modulus_len.
 *
 * Returns true on a valid signature, false otherwise. False positives are
 * not possible if SHA-256 is collision-resistant: the function performs
 * constant-time decoded-EM comparison against the expected DigestInfo
 * envelope (RFC 8017 §9.2 step 4).
 */
bool efi_rsa_verify_pkcs1_sha256(const EfiRsaPublicKey *key,
                                  const uint8_t *sha256,
                                  const uint8_t *signature,
                                  uint32_t       sig_len);

#endif /* EFI_RSA_H */
