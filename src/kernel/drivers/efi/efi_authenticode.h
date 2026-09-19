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
 *  11. Final check, three dbx questions asked of every chain step
 *      (UEFI 2.10 §32.4.1 signature types, §32.6.1 forbidden rules):
 *        - SHA-256 of the whole certificate DER in dbx (EFI_CERT_X509
 *          entries, matched by digest) → revoked.
 *        - SHA-256/384/512 of the certificate's TBSCertificate in dbx
 *          (EFI_CERT_X509_SHA256/384/512 entries) → revoked. The
 *          TimeOfRevocation carried by those records is not consulted:
 *          §32.6.1 forbids the image outright when the firmware "does not
 *          support timestamp verification", and this kernel reads no
 *          RFC 3161 counter-signature.
 *        - The Authenticode image digest in dbx (EFI_CERT_SHA256/384/512
 *          entries) → revoked. The 384/512 digests are the same region
 *          walk under a wider algorithm and are computed only when dbx
 *          actually holds a record of that width, so an ordinary board
 *          pays for SHA-256 alone.
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

/* =========================================================================
 * EfiSbHashEntry.hash_kind encoding — ONE definition site.
 *
 * efi_secureboot.c packs two facts into that uint16_t: the digest width in
 * bits, and whether the record is an EFI_CERT_X509_SHA{256,384,512} entry
 * (a hash of a CERTIFICATE's TBS contents) or an EFI_CERT_SHA{256,384,512}
 * entry (a hash of an IMAGE) — UEFI 2.10 §32.4.1 Table 32-5. The two are
 * not interchangeable: searching one set for the other finds nothing and
 * fails OPEN, which is exactly how both kinds came to be parsed, counted
 * and never consulted.
 *
 * The names belong next to the struct in efi_secureboot.h, which describes
 * the field as "256, 384, 512 — alg in bits" and does not mention the flag
 * at all. They are here because both the producer (efi_secureboot.c) and
 * the consumer (efi_authenticode.c) include this header, so the bit has one
 * spelling that the compiler checks instead of four bare literals that
 * agree by coincidence.
 * ========================================================================= */
#define EFI_SB_HASH_KIND_BITS  0x7FFFu
#define EFI_SB_HASH_KIND_X509  0x8000u

/* =========================================================================
 * SHA-384 / SHA-512 (FIPS 180-4 §4.1.3, §5.1.2, §6.4, §6.5)
 *
 * Exported from this module rather than from src/lib/kernel/crypto.h
 * because Secure Boot is the only thing in this kernel that asks for them:
 * dbx carries EFI_CERT_X509_SHA384/512 revocations of certificates and
 * EFI_CERT_SHA384/512 revocations of images, and a revocation this kernel
 * cannot hash is a revocation it silently ignores.
 *
 * Exported at all — rather than kept static beside their only caller —
 * because a digest that decides whether a revoked certificate is honoured
 * must be provable at boot: efi_selftest.c's T6 runs published FIPS
 * vectors through these entry points. A dbx with SHA-384 records exists on
 * no machine the project can boot in QEMU, so the self-test is the only
 * thing that will ever execute this code before a real board does.
 *
 * Streaming as well as one-shot: a TBSCertificate is one contiguous slice,
 * but an Authenticode image digest is a walk over disjoint regions of a PE
 * that is never copied into one buffer.
 *
 *   efi_sha512_init   — reset for SHA-384 (sha384 = true) or SHA-512.
 *   efi_sha512_update — absorb len bytes; any number of calls.
 *   efi_sha512_final  — write 48 bytes (SHA-384) or 64 (SHA-512). The
 *                       context is spent afterwards: re-init to reuse it.
 *   efi_sha512        — one shot, same output sizes.
 *
 * No allocation, no locks, IRQ-safe; klib memcpy/memset is the only
 * dependency.
 * ========================================================================= */
typedef struct {
    uint64_t h[8];        /* running chaining value */
    uint64_t total;       /* message bytes absorbed so far */
    uint32_t buf_len;     /* bytes held in buf (< 128) */
    bool     sha384;      /* truncate to 48 bytes and use the SHA-384 IV */
    uint8_t  buf[128];    /* partial block */
} EfiSha512Ctx;

void efi_sha512_init(EfiSha512Ctx *ctx, bool sha384);
void efi_sha512_update(EfiSha512Ctx *ctx, const uint8_t *data, uint32_t len);
void efi_sha512_final(EfiSha512Ctx *ctx, uint8_t *out);
void efi_sha512(const uint8_t *data, uint32_t len, bool sha384, uint8_t *out);

#endif /* EFI_AUTHENTICODE_H */
