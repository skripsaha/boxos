#ifndef EFI_AUTHENTICODE_H
#define EFI_AUTHENTICODE_H

/*
 * BoxOS — PE/COFF Authenticode signature verification.
 *
 * Implements the verifier side of "Windows Authenticode Portable
 * Executable Signature Format" v1.0 (Microsoft, Mar 2008) layered on
 * RFC 5652 CMS SignedData and RFC 8017 RSA-PKCS1-v1.5.
 *
 * Verification flow:
 *
 *   1. Parse PE/COFF headers (PE32 or PE32+).
 *   2. Locate the Attribute Certificate Table from the Optional Header
 *      Data Directories[4].
 *   3. Compute the deterministic Authenticode SHA-256 hash of the image
 *      bytes minus three excluded regions:
 *        - the Optional Header CheckSum field (4 bytes),
 *        - the CertTable data directory entry (8 bytes),
 *        - the Attribute Certificate Table itself (variable, at end).
 *   4. Parse WIN_CERTIFICATE from the Attribute Certificate Table — must
 *      be PKCS_7 / PKCS-#7-SignedData (cert_type 0x0002).
 *   5. Decode the PKCS#7 ContentInfo → SignedData.
 *   6. From SignedData.encapContentInfo extract the SpcIndirectDataContent
 *      and assert messageDigest equals the computed Authenticode hash.
 *   7. From SignedData.signerInfos[0] extract:
 *        - the issuer + serial of the signer cert,
 *        - the SHA-256 digest algorithm,
 *        - the signedAttrs SET (must contain pkcs9-messageDigest matching
 *          the hash of the encapContentInfo),
 *        - the encryptedDigest (the RSA signature over signedAttrs).
 *   8. Locate the signer's cert in SignedData.certificates by matching
 *      issuer + serial.
 *   9. RSA-verify the encryptedDigest using the signer's cert public key
 *      over SHA-256(DER-reencoded signedAttrs).  (Per CMS, when
 *      signedAttrs is present the signature covers SHA-256(IMPLICIT-
 *      tagged-SET DER-encoded with explicit tag 0x31).)
 *  10. Validate the cert chain:
 *        - For each chain step (signer → intermediate → ... → root),
 *          verify the child cert's tbsCertificate is RSA-signed by the
 *          parent cert's public key.
 *        - The parent cert may be in the embedded SignedData.certificates
 *          list OR in the platform db.  A chain that terminates inside
 *          db == trusted.
 *  11. Final check: SHA-256 of any chain cert appearing in dbx → image
 *      is revoked.  SHA-256 of the computed Authenticode hash appearing
 *      in dbx → image is revoked.
 *
 * Touch surface:
 *
 *   authenticode:pass:<sha256-of-pe>   payload = file size + signer hash
 *   authenticode:fail:<reason>         payload = reason code
 *
 * The verifier needs efi_runtime_init + efi_secureboot_init to have run
 * (it walks the db/dbx inventory).
 */

#include "ktypes.h"

typedef enum {
    EFI_AC_RESULT_VALID                       = 0,
    EFI_AC_RESULT_BAD_PE                      = 1,
    EFI_AC_RESULT_NO_CERT_TABLE               = 2,
    EFI_AC_RESULT_BAD_CERT_TABLE              = 3,
    EFI_AC_RESULT_BAD_SIGNED_DATA             = 4,
    EFI_AC_RESULT_AUTH_HASH_MISMATCH          = 5,
    EFI_AC_RESULT_UNSUPPORTED_DIGEST          = 6,
    EFI_AC_RESULT_SIGNER_NOT_FOUND            = 7,
    EFI_AC_RESULT_BAD_SIGNER_CERT             = 8,
    EFI_AC_RESULT_BAD_RSA_KEY                 = 9,
    EFI_AC_RESULT_SIGNATURE_INVALID           = 10,
    EFI_AC_RESULT_CHAIN_UNTRUSTED             = 11,
    EFI_AC_RESULT_REVOKED_BY_DBX              = 12,
    EFI_AC_RESULT_SB_UNAVAILABLE              = 13,
} EfiAuthenticodeResult;

/* Verify a PE binary against the platform's db / dbx.
 *
 *   buf, len        the entire PE image as it would be loaded from disk.
 *   out_pe_hash     optional; receives the 32-byte Authenticode hash on
 *                   success (zero-filled on failure).
 *   out_signer_hash optional; receives SHA-256 of the signer cert DER.
 *
 * Returns EFI_AC_RESULT_VALID on a complete success. Any non-zero return
 * means the image MUST NOT be trusted for boot.
 */
EfiAuthenticodeResult efi_authenticode_verify_pe(const uint8_t *buf,
                                                  uint32_t        len,
                                                  uint8_t        *out_pe_hash,
                                                  uint8_t        *out_signer_hash);

/* Compute the Authenticode SHA-256 hash of a PE image (no signature
 * verification).  Useful for tooling.  Returns true on success. */
bool efi_authenticode_compute_pe_hash(const uint8_t *buf, uint32_t len,
                                       uint8_t out_sha256[32]);

/* Convert an EfiAuthenticodeResult to a stable string slug used in
 * authenticode:fail:<slug> Touch tag. */
const char *efi_authenticode_result_slug(EfiAuthenticodeResult r);

#endif /* EFI_AUTHENTICODE_H */
