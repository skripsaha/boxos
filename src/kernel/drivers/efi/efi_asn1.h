#ifndef EFI_ASN1_H
#define EFI_ASN1_H

/*
 * Minimal DER (Distinguished Encoding Rules) parser — only the subset
 * required by EFI Secure Boot / Authenticode:
 *
 *   * X.509 v3 certificate decomposition into ToBeSigned + signature
 *     algorithm + signature value.
 *   * RSA public-key extraction (SubjectPublicKeyInfo → RSAPublicKey).
 *   * PKCS#7 / CMS SignedData walk for embedded Authenticode signatures.
 *
 * The parser intentionally does NOT support BER (indefinite-length form,
 * multi-byte tags beyond the short form). Real-world DER output from
 * X.509-aware tooling is fully covered by the short-form subset.
 *
 * Conventions:
 *
 *   - All offsets/lengths are bounded against the parent slice. Every
 *     reader returns 0 (false) on any malformed input — never reads
 *     past the slice end, never trusts a length field bigger than the
 *     remaining bytes.
 *
 *   - "Slice" = pointer + remaining-length pair, mirroring the common
 *     ASN.1 walk pattern. The reader advances the slice in-place when
 *     it succeeds.
 */

#include "ktypes.h"

/* DER universal tags (ITU-T X.690 §8). Only ones we touch. */
#define ASN1_TAG_BOOLEAN          0x01
#define ASN1_TAG_INTEGER          0x02
#define ASN1_TAG_BIT_STRING       0x03
#define ASN1_TAG_OCTET_STRING     0x04
#define ASN1_TAG_NULL             0x05
#define ASN1_TAG_OID              0x06
#define ASN1_TAG_UTF8_STRING      0x0C
#define ASN1_TAG_PRINTABLE_STRING 0x13
#define ASN1_TAG_TELETEX_STRING   0x14
#define ASN1_TAG_UTC_TIME         0x17
#define ASN1_TAG_GENERALIZED_TIME 0x18
#define ASN1_TAG_SEQUENCE         0x30
#define ASN1_TAG_SET              0x31

/* Context-specific class. Tag form is 0xA0 | n for constructed, 0x80|n for
 * primitive. Used heavily by X.509 v3 extensions and PKCS#7. */
#define ASN1_TAG_CONTEXT(n)             (0xA0u | (n))
#define ASN1_TAG_CONTEXT_PRIMITIVE(n)   (0x80u | (n))

typedef struct {
    const uint8_t *p;     /* current read pointer */
    uint32_t       len;   /* bytes remaining at p */
} Asn1Reader;

/* Construct a reader over a buffer. */
static inline Asn1Reader asn1_reader(const uint8_t *buf, uint32_t len)
{
    Asn1Reader r = { .p = buf, .len = len };
    return r;
}

/* Peek the next tag without advancing. Returns 0 on empty slice. */
uint8_t asn1_peek_tag(const Asn1Reader *r);

/* Read the next TLV header — fills `out_tag`, `out_value`, `out_value_len`,
 * advances the reader past the TLV. Returns false on malformed input.
 *
 *   r          [in/out]  the parent slice; advanced past the TLV on success.
 *   expect_tag if non-zero, asserts the tag matches and rejects otherwise.
 *   out_tag    [out]     the parsed tag byte.
 *   out_value  [out]     pointer to the value (inside the original buffer).
 *   out_value_len [out]  length of the value in bytes.
 */
bool asn1_read_tlv(Asn1Reader *r, uint8_t expect_tag,
                    uint8_t *out_tag, const uint8_t **out_value,
                    uint32_t *out_value_len);

/* Convenience: read TLV that MUST have a specific tag; returns the value
 * slice on success or {NULL,0} on failure. */
bool asn1_read_typed(Asn1Reader *r, uint8_t expect_tag,
                      const uint8_t **out_value, uint32_t *out_value_len);

/* Open a SEQUENCE/SET — reads the TLV and returns its value as a sub-reader. */
bool asn1_open(Asn1Reader *r, uint8_t expect_tag, Asn1Reader *out_inner);

/* Read an OCTET STRING / BIT STRING value into a slice.
 * For BIT STRING the leading byte (unused-bit count) is stripped and
 * required to be 0 — otherwise the bit string isn't byte-aligned and we
 * refuse to consume it. */
bool asn1_read_octet_string(Asn1Reader *r, const uint8_t **out, uint32_t *out_len);
bool asn1_read_bit_string(Asn1Reader *r, const uint8_t **out, uint32_t *out_len);

/* Read an INTEGER — returns the raw big-endian byte representation.
 * Strips the optional sign byte 0x00 (DER positive integer whose MSB
 * would otherwise be set). Always positive. */
bool asn1_read_integer(Asn1Reader *r, const uint8_t **out, uint32_t *out_len);

/* Skip the next TLV. */
bool asn1_skip(Asn1Reader *r);

/* Compare a DER OID (the value portion of an OID TLV, without tag/length)
 * to a constant DER-encoded OID. */
bool asn1_oid_equal(const uint8_t *enc1, uint32_t len1,
                     const uint8_t *enc2, uint32_t len2);

/* Pre-encoded DER OIDs commonly needed. The encoded form (without tag+len)
 * is the natural comparand because asn1_read_typed returns the value slice. */

/* 1.2.840.113549.1.1.1 — rsaEncryption */
extern const uint8_t OID_RSA_ENCRYPTION[];
extern const uint32_t OID_RSA_ENCRYPTION_LEN;

/* 1.2.840.113549.1.1.11 — sha256WithRSAEncryption */
extern const uint8_t OID_SHA256_WITH_RSA[];
extern const uint32_t OID_SHA256_WITH_RSA_LEN;

/* 1.2.840.113549.1.1.12 — sha384WithRSAEncryption */
extern const uint8_t OID_SHA384_WITH_RSA[];
extern const uint32_t OID_SHA384_WITH_RSA_LEN;

/* 2.16.840.1.101.3.4.2.1 — sha256 */
extern const uint8_t OID_SHA256[];
extern const uint32_t OID_SHA256_LEN;

/* 1.2.840.113549.1.7.2 — pkcs7-signedData */
extern const uint8_t OID_PKCS7_SIGNED_DATA[];
extern const uint32_t OID_PKCS7_SIGNED_DATA_LEN;

/* 1.3.6.1.4.1.311.2.1.4 — Authenticode SpcIndirectDataContent */
extern const uint8_t OID_SPC_INDIRECT_DATA[];
extern const uint32_t OID_SPC_INDIRECT_DATA_LEN;

/* 1.2.840.113549.1.9.4 — pkcs9-messageDigest */
extern const uint8_t OID_PKCS9_MESSAGE_DIGEST[];
extern const uint32_t OID_PKCS9_MESSAGE_DIGEST_LEN;

/* 1.2.840.113549.1.9.3 — pkcs9-contentType */
extern const uint8_t OID_PKCS9_CONTENT_TYPE[];
extern const uint32_t OID_PKCS9_CONTENT_TYPE_LEN;

#endif /* EFI_ASN1_H */
