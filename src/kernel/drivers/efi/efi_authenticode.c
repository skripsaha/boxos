/*
 * BoxOS — PE/COFF Authenticode signature verification.
 *
 * Implements: PE parsing → Authenticode SHA-256 hash → SignedData parse
 *             → signer cert lookup → RSA-PKCS1-v1.5 SHA-256 verify
 *             → cert chain validation to db → dbx revocation check.
 *
 * Real-HW references (cited inline):
 *   - Microsoft "Windows Authenticode Portable Executable Signature Format" v1.0
 *   - PE/COFF Specification rev 11 (Microsoft, 2022)
 *   - RFC 5652 §5 (CMS SignedData)
 *   - RFC 8017 §8.2 (RSASSA-PKCS1-v1_5)
 *   - RFC 5280 (X.509 v3)
 *   - UEFI 2.10 §32.4.2 (Image Verification)
 */

#include "efi_authenticode.h"
#include "efi_asn1.h"
#include "efi_rsa.h"
#include "efi_secureboot.h"
#include "klib.h"
#include "crypto.h"
#include "touch.h"

/* =========================================================================
 * PE/COFF constants
 * ========================================================================= */

#define DOS_MAGIC          0x5A4D     /* "MZ" */
#define DOS_E_LFANEW_OFF   0x3C
#define PE_SIGNATURE       0x00004550 /* "PE\0\0" little-endian */

#define COFF_HEADER_SIZE         20
#define OPT_HDR_PE32_MAGIC       0x010B
#define OPT_HDR_PE32PLUS_MAGIC   0x020B

/* Offset of CheckSum within OptionalHeader (PE32 and PE32+). */
#define OPT_HDR_CHECKSUM_OFF     64

/* Data Directories array begins at:
 *   PE32:    OptHeader + 96
 *   PE32+:   OptHeader + 112 */
#define OPT_HDR_PE32_DD_OFF      96
#define OPT_HDR_PE32PLUS_DD_OFF  112

/* Data Directories index for the Certificate Table. */
#define CERT_TABLE_DD_INDEX      4

/* COFF SectionHeader. */
#define SECTION_HEADER_SIZE      40

/* WIN_CERTIFICATE types per PE/COFF spec §Attribute Certificate Table. */
#define WIN_CERT_TYPE_PKCS_SIGNED_DATA  0x0002

/* =========================================================================
 * Small helpers
 * ========================================================================= */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Streaming SHA-256 wrapper. Built directly on KSha256Ctx from crypto.h
 * — no allocation, no intermediate copy of the PE. Lets us hash images
 * of arbitrary size (the previous kmalloc-the-whole-PE path was capped
 * at 64 MB and would fail under heap fragmentation). */
typedef KSha256Ctx HashCtx;

static bool hash_init(HashCtx *h)
{
    KSha256Init(h);
    return true;
}

static bool hash_update(HashCtx *h, const uint8_t *data, uint32_t n)
{
    KSha256Update(h, data, n);
    return true;
}

static void hash_final(HashCtx *h, uint8_t out[32])
{
    KSha256Final(h, out);
}

/* =========================================================================
 * PE/COFF structural parse
 *
 * Locates: pe_offset, opt_hdr_offset, opt_hdr is PE32+ y/n, section table
 * offset, section count, certtable directory entry (offset+size), and
 * actual Attribute Certificate Table location in the file.
 * ========================================================================= */

typedef struct {
    uint32_t pe_sig_offset;       /* offset of "PE\0\0" */
    uint32_t opt_hdr_offset;      /* offset of OptionalHeader */
    bool     is_pe32plus;
    uint32_t opt_hdr_size;
    uint32_t section_table_offset;
    uint32_t section_count;
    uint32_t size_of_headers;
    uint32_t checksum_offset;          /* in file */
    uint32_t certtable_dd_offset;      /* directory entry offset (8 bytes there) */
    uint32_t attr_cert_table_offset;   /* in file (Data Directories[4].VirtualAddress) */
    uint32_t attr_cert_table_size;     /* bytes */
} PeView;

static bool pe_parse(const uint8_t *buf, uint32_t len, PeView *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 64) return false;
    if (rd16(buf) != DOS_MAGIC) return false;
    uint32_t e_lfanew = rd32(buf + DOS_E_LFANEW_OFF);
    if (e_lfanew + 4 > len || e_lfanew > 0x10000000U) return false;
    if (rd32(buf + e_lfanew) != PE_SIGNATURE) return false;
    out->pe_sig_offset = e_lfanew;

    uint32_t coff_off = e_lfanew + 4;
    if (coff_off + COFF_HEADER_SIZE > len) return false;
    /* COFF header: Machine(2) NumberOfSections(2) TimeDateStamp(4)
     *              PointerToSymbolTable(4) NumberOfSymbols(4)
     *              SizeOfOptionalHeader(2) Characteristics(2) */
    out->section_count = rd16(buf + coff_off + 2);
    uint16_t opt_hdr_size = rd16(buf + coff_off + 16);
    out->opt_hdr_size    = opt_hdr_size;
    out->opt_hdr_offset  = coff_off + COFF_HEADER_SIZE;
    if (out->opt_hdr_offset + opt_hdr_size > len) return false;
    out->section_table_offset = out->opt_hdr_offset + opt_hdr_size;
    if (out->section_table_offset +
        (uint64_t)out->section_count * SECTION_HEADER_SIZE > len) return false;

    if (opt_hdr_size < 2) return false;
    uint16_t magic = rd16(buf + out->opt_hdr_offset);
    if (magic == OPT_HDR_PE32_MAGIC)         out->is_pe32plus = false;
    else if (magic == OPT_HDR_PE32PLUS_MAGIC) out->is_pe32plus = true;
    else return false;

    out->checksum_offset = out->opt_hdr_offset + OPT_HDR_CHECKSUM_OFF;
    uint32_t dd_off = out->opt_hdr_offset +
                       (out->is_pe32plus ? OPT_HDR_PE32PLUS_DD_OFF
                                          : OPT_HDR_PE32_DD_OFF);
    if (dd_off + (CERT_TABLE_DD_INDEX + 1) * 8 >
        out->opt_hdr_offset + opt_hdr_size) return false;

    /* SizeOfHeaders in OptionalHeader at:
     *   PE32:   offset 60
     *   PE32+:  offset 60 (same — only fields above 60 differ in width) */
    out->size_of_headers = rd32(buf + out->opt_hdr_offset + 60);
    if (out->size_of_headers > len) return false;

    out->certtable_dd_offset = dd_off + CERT_TABLE_DD_INDEX * 8;
    out->attr_cert_table_offset = rd32(buf + out->certtable_dd_offset);
    out->attr_cert_table_size   = rd32(buf + out->certtable_dd_offset + 4);

    if (out->attr_cert_table_size != 0) {
        uint64_t end = (uint64_t)out->attr_cert_table_offset +
                        out->attr_cert_table_size;
        if (end > len) return false;
        /* Authenticode hash math (Microsoft Authenticode v1.0 §step 14)
         * subtracts cert_size from the trailing-bytes region — that
         * subtraction only yields the spec-correct exclusion when the
         * cert table is at the very end of the file. Every production
         * signing tool (signtool, osslsigncode, sbsigntool, pesign)
         * emits cert tables at end; a cert table interleaved with
         * section data would silently mis-hash and yield a "valid"
         * signature over the wrong bytes, so refuse it explicitly. */
        if (end != len) return false;
    }
    return true;
}

/* =========================================================================
 * Section table sort: produce indices in ascending PointerToRawData order,
 * skipping zero-sized sections (per Authenticode spec §step 9). */
static int section_compare_indices(const uint8_t *base,
                                    uint32_t section_table_offset,
                                    uint32_t a, uint32_t b)
{
    uint32_t pa = rd32(base + section_table_offset + a * SECTION_HEADER_SIZE + 20);
    uint32_t pb = rd32(base + section_table_offset + b * SECTION_HEADER_SIZE + 20);
    return pa < pb ? -1 : (pa > pb ? 1 : 0);
}

/* =========================================================================
 * Authenticode hash computation (Microsoft Authenticode v1.0).
 *
 * Excluded regions (in this order in the file):
 *    1. CheckSum field        (4 bytes at OptHdr+64)
 *    2. CertTable DD entry    (8 bytes inside DataDirectories[4])
 *    3. Attribute Cert Table  (variable, anywhere — typically at EOF)
 * ========================================================================= */

bool efi_authenticode_compute_pe_hash(const uint8_t *buf, uint32_t len,
                                       uint8_t out_sha256[32])
{
    PeView pv;
    if (!pe_parse(buf, len, &pv)) return false;

    /* Streaming SHA-256 — no allocation, hash regions one at a time
     * directly from the PE image. */
    HashCtx h;
    if (!hash_init(&h)) return false;

    /* Header parts.
     *   A: 0 .. checksum_offset
     *   B: checksum_offset+4 .. certtable_dd_offset
     *   C: certtable_dd_offset+8 .. size_of_headers (end of headers)
     */
    if (!hash_update(&h, buf, pv.checksum_offset))                    goto fail;
    if (!hash_update(&h, buf + pv.checksum_offset + 4,
                      pv.certtable_dd_offset - (pv.checksum_offset + 4))) goto fail;
    if (!hash_update(&h, buf + pv.certtable_dd_offset + 8,
                      pv.size_of_headers - (pv.certtable_dd_offset + 8))) goto fail;

    /* Section data, sorted by PointerToRawData ascending. */
    static uint16_t order[96];
    uint32_t nsec = pv.section_count;
    if (nsec > 96) goto fail;
    uint32_t nused = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t raw_size = rd32(buf + pv.section_table_offset +
                                  i * SECTION_HEADER_SIZE + 16);
        if (raw_size == 0) continue;
        order[nused++] = (uint16_t)i;
    }
    /* insertion sort (nused ≤ 96) */
    for (uint32_t i = 1; i < nused; i++) {
        uint16_t cur = order[i];
        uint32_t j = i;
        while (j > 0 &&
               section_compare_indices(buf, pv.section_table_offset,
                                        cur, order[j - 1]) < 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = cur;
    }

    uint32_t sum_hashed = pv.size_of_headers;
    for (uint32_t i = 0; i < nused; i++) {
        uint32_t s = order[i];
        uint32_t raw_off  = rd32(buf + pv.section_table_offset +
                                  s * SECTION_HEADER_SIZE + 20);
        uint32_t raw_size = rd32(buf + pv.section_table_offset +
                                  s * SECTION_HEADER_SIZE + 16);
        if ((uint64_t)raw_off + raw_size > len) goto fail;
        if (!hash_update(&h, buf + raw_off, raw_size)) goto fail;
        sum_hashed += raw_size;
    }

    /* Any extra bytes after the last hashed section but NOT in the
     * attribute certificate table.  Per spec §step 14. */
    uint32_t cert_size = pv.attr_cert_table_size;
    if (len > sum_hashed) {
        uint32_t extra = len - sum_hashed;
        if (extra < cert_size) goto fail;          /* impossible — sanity */
        extra -= cert_size;
        if (extra > 0) {
            if (!hash_update(&h, buf + sum_hashed, extra)) goto fail;
        }
    }

    hash_final(&h, out_sha256);
    return true;

fail:
    /* Streaming context has no heap state to release; just leave. */
    return false;
}

/* =========================================================================
 * WIN_CERTIFICATE walk
 *
 * The Attribute Cert Table is a concatenation of WIN_CERTIFICATE structs:
 *   uint32 length      (incl. header)
 *   uint16 revision
 *   uint16 cert_type
 *   bytes  cert_data
 * Each WIN_CERTIFICATE is 8-byte aligned (per spec).
 * ========================================================================= */

static bool find_pkcs7_signed_data(const uint8_t *buf, uint32_t len,
                                    uint32_t cert_offset, uint32_t cert_size,
                                    const uint8_t **out_pkcs7, uint32_t *out_len)
{
    uint32_t pos = 0;
    while (pos + 8 <= cert_size) {
        const uint8_t *p = buf + cert_offset + pos;
        uint32_t wlen = rd32(p);
        uint16_t wtype = rd16(p + 6);
        /* 64-bit arithmetic on the bounds check: a malformed wlen
         * (e.g. 0xFFFFFFFE) combined with a large pos would wrap
         * around in uint32 and slip past the size check. */
        if (wlen < 8 || (uint64_t)pos + wlen > cert_size) return false;
        if (wtype == WIN_CERT_TYPE_PKCS_SIGNED_DATA) {
            *out_pkcs7 = p + 8;
            *out_len   = wlen - 8;
            (void)len;
            return true;
        }
        /* 8-byte align next position. Overflow-safe because pos+wlen has
         * already been verified ≤ cert_size which fits in uint32. */
        pos = (pos + wlen + 7) & ~7u;
    }
    return false;
}

/* =========================================================================
 * X.509 helpers
 *
 * Each helper takes a DER-encoded cert and writes pointers into the original
 * buffer.  No allocations.
 * ========================================================================= */

typedef struct {
    const uint8_t *cert_full;
    uint32_t       cert_full_len;
    const uint8_t *tbs;             /* DER of TBSCertificate including its own SEQUENCE header */
    uint32_t       tbs_len;
    const uint8_t *issuer_raw;      /* DER of the issuer Name (SEQUENCE) */
    uint32_t       issuer_raw_len;
    const uint8_t *subject_raw;
    uint32_t       subject_raw_len;
    const uint8_t *serial;          /* INTEGER value bytes, big-endian, unsigned */
    uint32_t       serial_len;
    const uint8_t *spki;            /* DER of SubjectPublicKeyInfo (SEQUENCE) */
    uint32_t       spki_len;
    /* Signature algorithm + value (over tbs). */
    const uint8_t *sig_alg_oid;
    uint32_t       sig_alg_oid_len;
    const uint8_t *signature;
    uint32_t       signature_len;
    /* RSA public-key components (parsed from spki). */
    const uint8_t *rsa_modulus;
    uint32_t       rsa_modulus_len;
    const uint8_t *rsa_exponent;
    uint32_t       rsa_exponent_len;
} CertView;

/* Parse the outermost X.509 Certificate SEQUENCE into a CertView.
 *
 * Certificate ::= SEQUENCE {
 *     tbsCertificate          TBSCertificate,
 *     signatureAlgorithm      AlgorithmIdentifier,
 *     signatureValue          BIT STRING
 * }
 *
 * TBSCertificate ::= SEQUENCE {
 *     version                 [0] EXPLICIT Version DEFAULT v1,
 *     serialNumber            CertificateSerialNumber,
 *     signature               AlgorithmIdentifier,
 *     issuer                  Name,
 *     validity                Validity,
 *     subject                 Name,
 *     subjectPublicKeyInfo    SubjectPublicKeyInfo,
 *     ...
 * }
 */
static bool x509_parse(const uint8_t *cert, uint32_t cert_len, CertView *cv)
{
    memset(cv, 0, sizeof(*cv));
    cv->cert_full     = cert;
    cv->cert_full_len = cert_len;

    Asn1Reader root = asn1_reader(cert, cert_len);
    Asn1Reader top;
    if (!asn1_open(&root, ASN1_TAG_SEQUENCE, &top)) return false;

    /* Capture TBSCertificate full DER (tag+len+value). The first child of
     * `top` is the TBS SEQUENCE — its tag/length bytes precede the value
     * pointer returned by asn1_read_typed.  We compute the full TLV by
     * looking at top.p before/after the read. */
    const uint8_t *tbs_start = top.p;
    Asn1Reader tbs;
    if (!asn1_open(&top, ASN1_TAG_SEQUENCE, &tbs)) return false;
    const uint8_t *tbs_after_value = tbs.p + tbs.len;
    cv->tbs     = tbs_start;
    cv->tbs_len = (uint32_t)(tbs_after_value - tbs_start);

    /* signatureAlgorithm ::= AlgorithmIdentifier (SEQUENCE { algorithm OID, parameters ANY }) */
    Asn1Reader sig_alg;
    if (!asn1_open(&top, ASN1_TAG_SEQUENCE, &sig_alg)) return false;
    if (!asn1_read_typed(&sig_alg, ASN1_TAG_OID,
                          &cv->sig_alg_oid, &cv->sig_alg_oid_len)) return false;
    /* parameters: ignored (often NULL for sha256WithRSAEncryption) */

    /* signatureValue ::= BIT STRING */
    if (!asn1_read_bit_string(&top, &cv->signature, &cv->signature_len)) return false;

    /* Walk TBSCertificate. */
    /* version [0] EXPLICIT Version DEFAULT v1 — optional. */
    if (asn1_peek_tag(&tbs) == ASN1_TAG_CONTEXT(0)) {
        if (!asn1_skip(&tbs)) return false;
    }
    /* serialNumber ::= CertificateSerialNumber (INTEGER) */
    if (!asn1_read_integer(&tbs, &cv->serial, &cv->serial_len)) return false;
    /* signature AlgorithmIdentifier — skip; redundant with outer one. */
    if (!asn1_skip(&tbs)) return false;
    /* issuer Name. */
    {
        const uint8_t *issuer_start = tbs.p;
        Asn1Reader issuer_inner;
        if (!asn1_open(&tbs, ASN1_TAG_SEQUENCE, &issuer_inner)) return false;
        const uint8_t *issuer_end = issuer_inner.p + issuer_inner.len;
        cv->issuer_raw     = issuer_start;
        cv->issuer_raw_len = (uint32_t)(issuer_end - issuer_start);
    }
    /* validity — skip. */
    if (!asn1_skip(&tbs)) return false;
    /* subject Name. */
    {
        const uint8_t *subj_start = tbs.p;
        Asn1Reader subj_inner;
        if (!asn1_open(&tbs, ASN1_TAG_SEQUENCE, &subj_inner)) return false;
        const uint8_t *subj_end = subj_inner.p + subj_inner.len;
        cv->subject_raw     = subj_start;
        cv->subject_raw_len = (uint32_t)(subj_end - subj_start);
    }
    /* subjectPublicKeyInfo. */
    {
        const uint8_t *spki_start = tbs.p;
        Asn1Reader spki_inner;
        if (!asn1_open(&tbs, ASN1_TAG_SEQUENCE, &spki_inner)) return false;
        const uint8_t *spki_end = spki_inner.p + spki_inner.len;
        cv->spki     = spki_start;
        cv->spki_len = (uint32_t)(spki_end - spki_start);

        /* AlgorithmIdentifier — must be rsaEncryption. */
        Asn1Reader alg;
        if (!asn1_open(&spki_inner, ASN1_TAG_SEQUENCE, &alg)) return false;
        const uint8_t *alg_oid = NULL;
        uint32_t alg_oid_len = 0;
        if (!asn1_read_typed(&alg, ASN1_TAG_OID, &alg_oid, &alg_oid_len)) return false;
        if (!asn1_oid_equal(alg_oid, alg_oid_len,
                             OID_RSA_ENCRYPTION, OID_RSA_ENCRYPTION_LEN)) {
            /* Non-RSA pubkey — ECDSA, etc.  We don't implement those yet. */
            return false;
        }

        /* subjectPublicKey BIT STRING — contains DER of RSAPublicKey. */
        const uint8_t *bs = NULL;
        uint32_t bs_len = 0;
        if (!asn1_read_bit_string(&spki_inner, &bs, &bs_len)) return false;

        /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
        Asn1Reader pk = asn1_reader(bs, bs_len);
        Asn1Reader pk_seq;
        if (!asn1_open(&pk, ASN1_TAG_SEQUENCE, &pk_seq)) return false;
        if (!asn1_read_integer(&pk_seq, &cv->rsa_modulus, &cv->rsa_modulus_len)) return false;
        if (!asn1_read_integer(&pk_seq, &cv->rsa_exponent, &cv->rsa_exponent_len)) return false;
    }
    /* Other TBS fields ignored. */
    return true;
}

/* Verify that `child` was signed by `parent`'s public key. */
static bool cert_verify_chain_step(const CertView *parent, const CertView *child)
{
    /* Must use SHA-256 for now — pre-2018 SHA-1 certs are not supported
     * (Microsoft revoked SHA-1 signing in db).  Future extension: add
     * SHA-384 alongside. */
    if (!asn1_oid_equal(child->sig_alg_oid, child->sig_alg_oid_len,
                         OID_SHA256_WITH_RSA, OID_SHA256_WITH_RSA_LEN)) {
        return false;
    }

    uint8_t tbs_hash[32];
    KSha256(child->tbs, child->tbs_len, tbs_hash);

    EfiRsaPublicKey pk = {
        .modulus      = parent->rsa_modulus,
        .modulus_len  = parent->rsa_modulus_len,
        .exponent     = parent->rsa_exponent,
        .exponent_len = parent->rsa_exponent_len,
    };
    return efi_rsa_verify_pkcs1_sha256(&pk, tbs_hash,
                                        child->signature, child->signature_len);
}

/* =========================================================================
 * PKCS#7 / CMS SignedData parser (Authenticode-specific subset).
 * ========================================================================= */

typedef struct {
    const uint8_t *cert_der;
    uint32_t       cert_der_len;
} EmbeddedCert;

#define MAX_EMBEDDED_CERTS  16

typedef struct {
    /* The Authenticode hash we must match. */
    uint8_t  spc_message_digest[32];
    bool     spc_md_present;

    /* SHA-256 of SpcIndirectDataContent.eContent (used to match
     * signedAttrs.messageDigest per CMS rules). */
    uint8_t  encap_content_hash[32];
    bool     encap_content_hashed;

    /* Embedded certs (X.509 DER, raw slices into the WIN_CERTIFICATE buffer). */
    EmbeddedCert certs[MAX_EMBEDDED_CERTS];
    uint32_t     cert_count;

    /* SignerInfo fields. */
    const uint8_t *signer_issuer_raw;
    uint32_t       signer_issuer_raw_len;
    const uint8_t *signer_serial;
    uint32_t       signer_serial_len;

    const uint8_t *signed_attrs_full;    /* full TLV (tag 0xA0 ...) for hashing */
    uint32_t       signed_attrs_full_len;
    bool           has_signed_attrs;

    const uint8_t *signature;
    uint32_t       signature_len;
} CmsSigned;

/* Parse the AlgorithmIdentifier SEQUENCE inside signerInfo, return true
 * if it is SHA-256 (we only implement SHA-256 currently). */
static bool digest_alg_is_sha256(Asn1Reader *r)
{
    Asn1Reader alg;
    if (!asn1_open(r, ASN1_TAG_SEQUENCE, &alg)) return false;
    const uint8_t *oid = NULL;
    uint32_t oid_len = 0;
    if (!asn1_read_typed(&alg, ASN1_TAG_OID, &oid, &oid_len)) return false;
    return asn1_oid_equal(oid, oid_len, OID_SHA256, OID_SHA256_LEN);
}

/* Parse SpcIndirectDataContent → extract messageDigest hash bytes.
 *
 *   SpcIndirectDataContent ::= SEQUENCE {
 *       data                SpcAttributeTypeAndOptionalValue,
 *       messageDigest       DigestInfo
 *   }
 *   DigestInfo ::= SEQUENCE { digestAlgorithm AlgId, digest OCTET STRING }
 */
static bool parse_spc_indirect(const uint8_t *p, uint32_t len, CmsSigned *cs)
{
    Asn1Reader r = asn1_reader(p, len);
    Asn1Reader outer;
    if (!asn1_open(&r, ASN1_TAG_SEQUENCE, &outer)) return false;
    /* data — skip */
    if (!asn1_skip(&outer)) return false;
    /* messageDigest = DigestInfo */
    Asn1Reader di;
    if (!asn1_open(&outer, ASN1_TAG_SEQUENCE, &di)) return false;
    if (!digest_alg_is_sha256(&di)) return false;
    const uint8_t *dg = NULL;
    uint32_t dg_len = 0;
    if (!asn1_read_octet_string(&di, &dg, &dg_len)) return false;
    if (dg_len != 32) return false;
    memcpy(cs->spc_message_digest, dg, 32);
    cs->spc_md_present = true;
    return true;
}

/* Parse the SignedData inside a ContentInfo blob (RFC 5652 §5.1).
 *
 *   ContentInfo ::= SEQUENCE { contentType OID, content [0] EXPLICIT ANY }
 *   SignedData  ::= SEQUENCE {
 *       version                    INTEGER,
 *       digestAlgorithms           SET OF AlgorithmIdentifier,
 *       encapContentInfo           EncapsulatedContentInfo,
 *       certificates           [0] IMPLICIT CertificateSet OPTIONAL,
 *       crls                   [1] IMPLICIT RevocationInfoChoices OPTIONAL,
 *       signerInfos                SET OF SignerInfo
 *   }
 *   EncapsulatedContentInfo ::= SEQUENCE {
 *       eContentType  OID,
 *       eContent      [0] EXPLICIT OCTET STRING OPTIONAL
 *   }
 *   SignerInfo ::= SEQUENCE {
 *       version             INTEGER,
 *       sid                 SignerIdentifier (IssuerAndSerialNumber here),
 *       digestAlgorithm     AlgorithmIdentifier,
 *       signedAttrs     [0] IMPLICIT SET OF Attribute OPTIONAL,
 *       signatureAlg        AlgorithmIdentifier,
 *       signature           OCTET STRING,
 *       unsignedAttrs   [1] IMPLICIT SET OF Attribute OPTIONAL
 *   }
 */
static bool parse_signer_info(Asn1Reader *si_r, CmsSigned *cs)
{
    Asn1Reader si;
    if (!asn1_open(si_r, ASN1_TAG_SEQUENCE, &si)) return false;

    /* version */
    const uint8_t *v = NULL; uint32_t vl = 0;
    if (!asn1_read_integer(&si, &v, &vl)) return false;

    /* sid = IssuerAndSerialNumber ::= SEQUENCE { issuer Name, serial INTEGER } */
    Asn1Reader sid;
    if (!asn1_open(&si, ASN1_TAG_SEQUENCE, &sid)) return false;
    {
        const uint8_t *issuer_start = sid.p;
        Asn1Reader issuer;
        if (!asn1_open(&sid, ASN1_TAG_SEQUENCE, &issuer)) return false;
        cs->signer_issuer_raw     = issuer_start;
        cs->signer_issuer_raw_len = (uint32_t)((issuer.p + issuer.len) - issuer_start);
        if (!asn1_read_integer(&sid, &cs->signer_serial, &cs->signer_serial_len))
            return false;
    }

    /* digestAlgorithm */
    if (!digest_alg_is_sha256(&si)) return false;

    /* signedAttrs [0] IMPLICIT SET OF Attribute OPTIONAL — must hash it
     * if present. */
    if (asn1_peek_tag(&si) == ASN1_TAG_CONTEXT(0)) {
        const uint8_t *attrs_start = si.p;
        uint8_t tag = 0;
        const uint8_t *attrs_val = NULL;
        uint32_t attrs_val_len = 0;
        if (!asn1_read_tlv(&si, ASN1_TAG_CONTEXT(0), &tag,
                            &attrs_val, &attrs_val_len)) return false;
        cs->has_signed_attrs      = true;
        cs->signed_attrs_full     = attrs_start;
        cs->signed_attrs_full_len = (uint32_t)((attrs_val + attrs_val_len) - attrs_start);
    }

    /* signatureAlgorithm — should be rsaEncryption (or sha256WithRSA;
     * accept either as they both denote RSA-PKCS1-v1.5). */
    Asn1Reader sa;
    if (!asn1_open(&si, ASN1_TAG_SEQUENCE, &sa)) return false;
    const uint8_t *sa_oid = NULL;
    uint32_t sa_oid_len = 0;
    if (!asn1_read_typed(&sa, ASN1_TAG_OID, &sa_oid, &sa_oid_len)) return false;
    bool ok_alg = asn1_oid_equal(sa_oid, sa_oid_len,
                                  OID_RSA_ENCRYPTION, OID_RSA_ENCRYPTION_LEN) ||
                   asn1_oid_equal(sa_oid, sa_oid_len,
                                   OID_SHA256_WITH_RSA, OID_SHA256_WITH_RSA_LEN);
    if (!ok_alg) return false;

    /* signature OCTET STRING */
    if (!asn1_read_octet_string(&si, &cs->signature, &cs->signature_len))
        return false;
    return true;
}

static bool parse_signed_data(const uint8_t *pkcs7, uint32_t len, CmsSigned *cs)
{
    memset(cs, 0, sizeof(*cs));

    Asn1Reader r = asn1_reader(pkcs7, len);

    /* ContentInfo SEQUENCE */
    Asn1Reader ci;
    if (!asn1_open(&r, ASN1_TAG_SEQUENCE, &ci)) return false;

    /* contentType OID — must be pkcs7-signedData. */
    const uint8_t *ct_oid = NULL; uint32_t ct_oid_len = 0;
    if (!asn1_read_typed(&ci, ASN1_TAG_OID, &ct_oid, &ct_oid_len)) return false;
    if (!asn1_oid_equal(ct_oid, ct_oid_len,
                         OID_PKCS7_SIGNED_DATA, OID_PKCS7_SIGNED_DATA_LEN))
        return false;

    /* content [0] EXPLICIT SignedData */
    Asn1Reader content;
    if (!asn1_open(&ci, ASN1_TAG_CONTEXT(0), &content)) return false;

    Asn1Reader sd;
    if (!asn1_open(&content, ASN1_TAG_SEQUENCE, &sd)) return false;

    /* version */
    const uint8_t *v = NULL; uint32_t vl = 0;
    if (!asn1_read_integer(&sd, &v, &vl)) return false;

    /* digestAlgorithms — skip; we only care about SHA-256 inner. */
    Asn1Reader dalgs;
    if (!asn1_open(&sd, ASN1_TAG_SET, &dalgs)) return false;
    (void)dalgs;

    /* encapContentInfo */
    {
        Asn1Reader eci;
        if (!asn1_open(&sd, ASN1_TAG_SEQUENCE, &eci)) return false;
        const uint8_t *ect_oid = NULL; uint32_t ect_oid_len = 0;
        if (!asn1_read_typed(&eci, ASN1_TAG_OID, &ect_oid, &ect_oid_len))
            return false;
        /* For Authenticode: eContentType = spcIndirectDataContent */
        if (!asn1_oid_equal(ect_oid, ect_oid_len,
                             OID_SPC_INDIRECT_DATA, OID_SPC_INDIRECT_DATA_LEN))
            return false;
        /* eContent [0] EXPLICIT OCTET STRING. Microsoft encodes the SPC
         * blob as inline OCTET STRING wrapper around the SEQUENCE. */
        Asn1Reader ec;
        if (!asn1_open(&eci, ASN1_TAG_CONTEXT(0), &ec)) return false;
        /* The inner blob's encoding varies — some signers wrap the
         * SpcIndirectDataContent in OCTET STRING, others place the
         * SEQUENCE directly inside [0].  Handle both forms. */
        const uint8_t *spc_p = NULL;
        uint32_t       spc_l = 0;
        const uint8_t *peek_p = ec.p;
        uint32_t       peek_l = ec.len;
        uint8_t        peek_tag = asn1_peek_tag(&ec);
        if (peek_tag == ASN1_TAG_OCTET_STRING) {
            if (!asn1_read_octet_string(&ec, &spc_p, &spc_l)) return false;
        } else {
            spc_p = peek_p;
            spc_l = peek_l;
        }
        if (!parse_spc_indirect(spc_p, spc_l, cs)) return false;

        /* The signedAttrs.messageDigest equals SHA-256 of the
         * encapsulated content (the bytes of SpcIndirectDataContent
         * itself per CMS §5.4). */
        KSha256(spc_p, spc_l, cs->encap_content_hash);
        cs->encap_content_hashed = true;
    }

    /* certificates [0] IMPLICIT CertificateSet OPTIONAL */
    if (asn1_peek_tag(&sd) == ASN1_TAG_CONTEXT(0)) {
        Asn1Reader certs;
        if (!asn1_read_tlv(&sd, ASN1_TAG_CONTEXT(0), NULL,
                            &certs.p, &certs.len)) return false;
        /* Each cert is a SEQUENCE (X.509). */
        while (certs.len > 0 && cs->cert_count < MAX_EMBEDDED_CERTS) {
            const uint8_t *cs_start = certs.p;
            Asn1Reader inner;
            if (!asn1_open(&certs, ASN1_TAG_SEQUENCE, &inner)) break;
            const uint8_t *cs_end = inner.p + inner.len;
            cs->certs[cs->cert_count].cert_der     = cs_start;
            cs->certs[cs->cert_count].cert_der_len = (uint32_t)(cs_end - cs_start);
            cs->cert_count++;
        }
    }

    /* crls [1] IMPLICIT — skip if present. */
    if (asn1_peek_tag(&sd) == ASN1_TAG_CONTEXT(1)) {
        if (!asn1_skip(&sd)) return false;
    }

    /* signerInfos SET — we use the first SignerInfo. */
    Asn1Reader sis;
    if (!asn1_open(&sd, ASN1_TAG_SET, &sis)) return false;
    if (!parse_signer_info(&sis, cs)) return false;

    return true;
}

/* =========================================================================
 * SignerInfo signature verification.
 *
 * When signedAttrs is present (it always is in Authenticode), the RSA
 * signature is computed over the DER of the signedAttrs SET re-encoded
 * with explicit tag 0x31 (instead of the [0] IMPLICIT 0xA0 transmitted
 * tag).  RFC 5652 §5.4.
 *
 * Step:
 *   1. Locate signedAttrs full TLV (already captured in cs).
 *   2. SHA-256 over [0x31 || length-bytes || value-bytes] — same length
 *      as the 0xA0-tagged form, only the tag byte changes.
 *   3. RSA-verify (encryptedDigest, computed hash, signer-cert public key).
 *
 * The Authenticode-specific cross-check: signedAttrs MUST contain a
 * pkcs9-messageDigest attribute equal to SHA-256(encapsulated SPC content),
 * which guarantees the SignerInfo signs the correct content.  Once that
 * is verified, the RSA check over the signedAttrs DER closes the loop.
 * ========================================================================= */

static bool check_signed_attrs_md(const uint8_t *attrs_full, uint32_t full_len,
                                   const uint8_t expected_md[32])
{
    /* attrs_full[0] = 0xA0; convert to 0x31 to walk the SET payload. */
    /* Parse out the value slice (skip tag + length header). */
    if (full_len < 2) return false;
    /* Re-read header: same encoding rules. */
    uint32_t consumed = 0;
    uint32_t vlen     = 0;
    if (attrs_full[1] < 0x80) {
        vlen = attrs_full[1];
        consumed = 2;
    } else {
        uint32_t n = attrs_full[1] & 0x7F;
        if (n == 0 || n > 4)             return false;
        if ((uint32_t)2 + n > full_len)  return false;
        for (uint32_t i = 0; i < n; i++) vlen = (vlen << 8) | attrs_full[2 + i];
        consumed = 2 + n;
    }
    if (consumed + vlen > full_len) return false;

    Asn1Reader r;
    r.p   = attrs_full + consumed;
    r.len = vlen;

    /* SET OF Attribute. Each Attribute ::= SEQUENCE { type OID, values SET OF ANY } */
    while (r.len > 0) {
        Asn1Reader attr;
        if (!asn1_open(&r, ASN1_TAG_SEQUENCE, &attr)) return false;
        const uint8_t *oid = NULL; uint32_t oid_len = 0;
        if (!asn1_read_typed(&attr, ASN1_TAG_OID, &oid, &oid_len)) return false;
        Asn1Reader val_set;
        if (!asn1_open(&attr, ASN1_TAG_SET, &val_set)) return false;

        if (asn1_oid_equal(oid, oid_len,
                            OID_PKCS9_MESSAGE_DIGEST,
                            OID_PKCS9_MESSAGE_DIGEST_LEN)) {
            const uint8_t *md = NULL; uint32_t md_len = 0;
            if (!asn1_read_octet_string(&val_set, &md, &md_len)) return false;
            if (md_len != 32) return false;
            return memcmp(md, expected_md, 32) == 0;
        }
    }
    return false;
}

static bool verify_signer_info_signature(const CmsSigned *cs, const CertView *signer)
{
    if (!cs->has_signed_attrs) return false;   /* unsupported — rare in Authenticode */

    /* Build SHA-256 input: [0x31 || rest-of-attrs_full[1..]] */
    uint32_t full_len = cs->signed_attrs_full_len;
    if (full_len < 2) return false;

    uint8_t *scratch = kmalloc(full_len);
    if (!scratch) return false;
    memcpy(scratch, cs->signed_attrs_full, full_len);
    scratch[0] = ASN1_TAG_SET;       /* 0xA0 → 0x31 */

    uint8_t attrs_hash[32];
    KSha256(scratch, full_len, attrs_hash);
    kfree(scratch);

    EfiRsaPublicKey pk = {
        .modulus      = signer->rsa_modulus,
        .modulus_len  = signer->rsa_modulus_len,
        .exponent     = signer->rsa_exponent,
        .exponent_len = signer->rsa_exponent_len,
    };
    return efi_rsa_verify_pkcs1_sha256(&pk, attrs_hash,
                                        cs->signature, cs->signature_len);
}

/* =========================================================================
 * Cert chain validation
 *
 * Walk: signer cert → parent cert in embedded list whose pubkey verifies
 * the signer's tbsCertificate.signature → ... → cert that matches an
 * entry in db (by exact DER or by SHA-256-of-cert).
 *
 * MAX_CHAIN_DEPTH 5 covers every Authenticode chain seen in the wild
 * (typical: signer → MS Production PCA → MS Code Signing PCA → db,
 * depth 3-4; OEM kernel updates: signer → OEM root in db, depth 1).
 * ========================================================================= */

#define MAX_CHAIN_DEPTH  6

static bool cert_is_in_dbx(const CertView *c)
{
    uint8_t d[32];
    KSha256(c->cert_full, c->cert_full_len, d);
    if (efi_secureboot_find_dbx_cert_sha256(d)) return true;
    return false;
}

static bool cert_terminates_chain(const CertView *c)
{
    /* Direct DER match in db (covers OEM kernel updates). */
    if (efi_secureboot_find_db_cert(c->cert_full, c->cert_full_len)) return true;
    /* SHA-256-of-cert match in db. */
    uint8_t d[32];
    KSha256(c->cert_full, c->cert_full_len, d);
    if (efi_secureboot_find_db_cert_sha256(d)) return true;
    return false;
}

static bool find_parent(const CertView *child,
                         const EmbeddedCert *embedded, uint32_t n_embedded,
                         CertView *out_parent)
{
    /* Try every embedded cert (skip the child itself). */
    for (uint32_t i = 0; i < n_embedded; i++) {
        if (embedded[i].cert_der == child->cert_full) continue;
        CertView pv;
        if (!x509_parse(embedded[i].cert_der, embedded[i].cert_der_len, &pv))
            continue;
        if (cert_verify_chain_step(&pv, child)) {
            *out_parent = pv;
            return true;
        }
    }
    /* Then try every cert in db.  PER UEFI 2.10 §32.4.2 image verification
     * uses db as the trust anchor — NOT KEK.  KEK is the authority for
     * updating PK/KEK/db variables themselves, not for signing executable
     * images, so a chain that terminates at a KEK cert without reaching
     * db must be rejected as untrusted. */
    for (uint32_t i = 0; ; i++) {
        const EfiSbCertEntry *e = efi_secureboot_cert_get(i);
        if (!e) break;
        if (e->db_kind != EFI_SB_DB_DB) continue;
        CertView pv;
        if (!x509_parse(e->cert, e->cert_len, &pv)) continue;
        if (cert_verify_chain_step(&pv, child)) {
            *out_parent = pv;
            return true;
        }
    }
    return false;
}

/* Tristate chain result. `*out_revoked` is set to true iff we rejected
 * because a chain cert appears in dbx — letting the caller emit
 * REVOKED instead of CHAIN_UNTRUSTED for accurate diagnostics. */
static bool chain_validates_to_db(const CertView *signer,
                                   const EmbeddedCert *embedded,
                                   uint32_t n_embedded,
                                   bool *out_revoked)
{
    if (out_revoked) *out_revoked = false;
    CertView current = *signer;
    for (int step = 0; step < MAX_CHAIN_DEPTH; step++) {
        if (cert_is_in_dbx(&current)) {
            if (out_revoked) *out_revoked = true;
            return false;
        }
        if (cert_terminates_chain(&current)) return true;
        CertView parent;
        if (!find_parent(&current, embedded, n_embedded, &parent)) return false;
        current = parent;
    }
    return false;
}

/* Find the signer cert in the embedded list by matching IssuerAndSerialNumber. */
static const EmbeddedCert *find_signer_cert(const CmsSigned *cs)
{
    for (uint32_t i = 0; i < cs->cert_count; i++) {
        CertView cv;
        if (!x509_parse(cs->certs[i].cert_der, cs->certs[i].cert_der_len, &cv))
            continue;
        if (cv.issuer_raw_len != cs->signer_issuer_raw_len) continue;
        if (memcmp(cv.issuer_raw, cs->signer_issuer_raw,
                    cv.issuer_raw_len) != 0) continue;
        if (cv.serial_len != cs->signer_serial_len) continue;
        if (memcmp(cv.serial, cs->signer_serial, cv.serial_len) != 0) continue;
        return &cs->certs[i];
    }
    return NULL;
}

/* =========================================================================
 * Public verifier entry
 * ========================================================================= */

const char *efi_authenticode_result_slug(EfiAuthenticodeResult r)
{
    switch (r) {
        case EFI_AC_RESULT_VALID:               return "ok";
        case EFI_AC_RESULT_BAD_PE:              return "bad-pe";
        case EFI_AC_RESULT_NO_CERT_TABLE:       return "no-cert-table";
        case EFI_AC_RESULT_BAD_CERT_TABLE:      return "bad-cert-table";
        case EFI_AC_RESULT_BAD_SIGNED_DATA:     return "bad-signed-data";
        case EFI_AC_RESULT_AUTH_HASH_MISMATCH:  return "hash-mismatch";
        case EFI_AC_RESULT_UNSUPPORTED_DIGEST:  return "unsupported-digest";
        case EFI_AC_RESULT_SIGNER_NOT_FOUND:    return "signer-not-found";
        case EFI_AC_RESULT_BAD_SIGNER_CERT:     return "bad-signer-cert";
        case EFI_AC_RESULT_BAD_RSA_KEY:         return "bad-rsa-key";
        case EFI_AC_RESULT_SIGNATURE_INVALID:   return "sig-invalid";
        case EFI_AC_RESULT_CHAIN_UNTRUSTED:     return "chain-untrusted";
        case EFI_AC_RESULT_REVOKED_BY_DBX:      return "revoked";
        case EFI_AC_RESULT_SB_UNAVAILABLE:      return "sb-unavailable";
        default:                                 return "unknown";
    }
}

static void publish_result_touch(EfiAuthenticodeResult r,
                                  const uint8_t *pe_hash,
                                  const uint8_t *signer_hash,
                                  uint32_t pe_size)
{
    static const char hex[] = "0123456789abcdef";
    char tag[80];

    if (r == EFI_AC_RESULT_VALID) {
        char h[65];
        for (int i = 0; i < 32; i++) {
            h[2 * i]     = hex[pe_hash[i] >> 4];
            h[2 * i + 1] = hex[pe_hash[i] & 0xF];
        }
        h[64] = 0;
        ksnprintf(tag, sizeof(tag), "authenticode:pass:%s", h);
        struct { uint32_t size; uint8_t signer[32]; } pl = { .size = pe_size };
        if (signer_hash) memcpy(pl.signer, signer_hash, 32);
        TouchPublish(tag, &pl, sizeof(pl));
    } else {
        ksnprintf(tag, sizeof(tag), "authenticode:fail:%s",
                   efi_authenticode_result_slug(r));
        struct { uint32_t reason; uint32_t size; } pl = {
            .reason = (uint32_t)r, .size = pe_size,
        };
        TouchPublish(tag, &pl, sizeof(pl));
    }
}

EfiAuthenticodeResult efi_authenticode_verify_pe(const uint8_t *buf,
                                                  uint32_t        len,
                                                  uint8_t        *out_pe_hash,
                                                  uint8_t        *out_signer_hash)
{
    if (out_pe_hash)     memset(out_pe_hash,     0, 32);
    if (out_signer_hash) memset(out_signer_hash, 0, 32);

    if (!efi_secureboot_available()) {
        publish_result_touch(EFI_AC_RESULT_SB_UNAVAILABLE, NULL, NULL, len);
        return EFI_AC_RESULT_SB_UNAVAILABLE;
    }
    if (!buf || len == 0) return EFI_AC_RESULT_BAD_PE;

    PeView pv;
    if (!pe_parse(buf, len, &pv)) {
        publish_result_touch(EFI_AC_RESULT_BAD_PE, NULL, NULL, len);
        return EFI_AC_RESULT_BAD_PE;
    }

    if (pv.attr_cert_table_size == 0) {
        publish_result_touch(EFI_AC_RESULT_NO_CERT_TABLE, NULL, NULL, len);
        return EFI_AC_RESULT_NO_CERT_TABLE;
    }

    uint8_t pe_hash[32];
    if (!efi_authenticode_compute_pe_hash(buf, len, pe_hash)) {
        publish_result_touch(EFI_AC_RESULT_BAD_PE, NULL, NULL, len);
        return EFI_AC_RESULT_BAD_PE;
    }
    if (out_pe_hash) memcpy(out_pe_hash, pe_hash, 32);

    /* dbx PE-hash revocation: short-circuit. */
    if (efi_secureboot_find_dbx_hash_sha256(pe_hash)) {
        publish_result_touch(EFI_AC_RESULT_REVOKED_BY_DBX, pe_hash, NULL, len);
        return EFI_AC_RESULT_REVOKED_BY_DBX;
    }

    const uint8_t *pkcs7 = NULL;
    uint32_t pkcs7_len = 0;
    if (!find_pkcs7_signed_data(buf, len, pv.attr_cert_table_offset,
                                  pv.attr_cert_table_size,
                                  &pkcs7, &pkcs7_len)) {
        publish_result_touch(EFI_AC_RESULT_BAD_CERT_TABLE, pe_hash, NULL, len);
        return EFI_AC_RESULT_BAD_CERT_TABLE;
    }

    CmsSigned cs;
    if (!parse_signed_data(pkcs7, pkcs7_len, &cs)) {
        publish_result_touch(EFI_AC_RESULT_BAD_SIGNED_DATA, pe_hash, NULL, len);
        return EFI_AC_RESULT_BAD_SIGNED_DATA;
    }

    /* The SPC indirect content's messageDigest MUST match our computed
     * Authenticode hash; this is what binds the signature to the PE. */
    if (!cs.spc_md_present || memcmp(cs.spc_message_digest, pe_hash, 32) != 0) {
        publish_result_touch(EFI_AC_RESULT_AUTH_HASH_MISMATCH, pe_hash, NULL, len);
        return EFI_AC_RESULT_AUTH_HASH_MISMATCH;
    }

    /* And the signedAttrs.messageDigest MUST match SHA-256(encapContent)
     * which we already pre-computed. */
    if (!cs.has_signed_attrs ||
        !check_signed_attrs_md(cs.signed_attrs_full,
                                cs.signed_attrs_full_len,
                                cs.encap_content_hash)) {
        publish_result_touch(EFI_AC_RESULT_BAD_SIGNED_DATA, pe_hash, NULL, len);
        return EFI_AC_RESULT_BAD_SIGNED_DATA;
    }

    const EmbeddedCert *signer_cert = find_signer_cert(&cs);
    if (!signer_cert) {
        publish_result_touch(EFI_AC_RESULT_SIGNER_NOT_FOUND, pe_hash, NULL, len);
        return EFI_AC_RESULT_SIGNER_NOT_FOUND;
    }
    CertView signer_cv;
    if (!x509_parse(signer_cert->cert_der, signer_cert->cert_der_len, &signer_cv)) {
        publish_result_touch(EFI_AC_RESULT_BAD_SIGNER_CERT, pe_hash, NULL, len);
        return EFI_AC_RESULT_BAD_SIGNER_CERT;
    }
    if (signer_cv.rsa_modulus_len == 0) {
        publish_result_touch(EFI_AC_RESULT_BAD_RSA_KEY, pe_hash, NULL, len);
        return EFI_AC_RESULT_BAD_RSA_KEY;
    }

    uint8_t signer_hash[32];
    KSha256(signer_cv.cert_full, signer_cv.cert_full_len, signer_hash);
    if (out_signer_hash) memcpy(out_signer_hash, signer_hash, 32);

    if (!verify_signer_info_signature(&cs, &signer_cv)) {
        publish_result_touch(EFI_AC_RESULT_SIGNATURE_INVALID, pe_hash,
                              signer_hash, len);
        return EFI_AC_RESULT_SIGNATURE_INVALID;
    }

    /* Cert chain. */
    bool revoked = false;
    if (!chain_validates_to_db(&signer_cv, cs.certs, cs.cert_count, &revoked)) {
        EfiAuthenticodeResult r = revoked
            ? EFI_AC_RESULT_REVOKED_BY_DBX
            : EFI_AC_RESULT_CHAIN_UNTRUSTED;
        publish_result_touch(r, pe_hash, signer_hash, len);
        return r;
    }

    publish_result_touch(EFI_AC_RESULT_VALID, pe_hash, signer_hash, len);
    return EFI_AC_RESULT_VALID;
}
