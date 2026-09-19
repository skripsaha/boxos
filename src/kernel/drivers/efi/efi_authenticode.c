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

/* Streaming image-digest wrapper — no allocation, no intermediate copy of
 * the PE. Lets us hash images of arbitrary size (the previous
 * kmalloc-the-whole-PE path was capped at 64 MB and would fail under heap
 * fragmentation).
 *
 * Three algorithms, not one. The Authenticode hash proper is SHA-256, but
 * UEFI 2.10 §32.4.1 lets db and dbx carry EFI_CERT_SHA384 and
 * EFI_CERT_SHA512 image hashes, and the digest such a record is compared
 * against is THE SAME region walk under a wider algorithm — EDK2's
 * HashPeImage() is called with HASHALG_SHA256, HASHALG_SHA384 and
 * HASHALG_SHA512 over one identical set of regions. `alg` is the digest
 * width in bits, matching EfiSbHashEntry.hash_kind so the two never need
 * translating between each other. */
typedef struct {
    uint16_t     alg;      /* 256, 384 or 512 */
    KSha256Ctx   s256;
    EfiSha512Ctx s512;
} HashCtx;

static bool hash_init(HashCtx *h, uint16_t alg)
{
    h->alg = alg;
    switch (alg) {
        case 256: KSha256Init(&h->s256);           return true;
        case 384: efi_sha512_init(&h->s512, true);  return true;
        case 512: efi_sha512_init(&h->s512, false); return true;
        default:  return false;
    }
}

static bool hash_update(HashCtx *h, const uint8_t *data, uint32_t n)
{
    if (h->alg == 256) KSha256Update(&h->s256, data, n);
    else               efi_sha512_update(&h->s512, data, n);
    return true;
}

static void hash_final(HashCtx *h, uint8_t *out)
{
    if (h->alg == 256) KSha256Final(&h->s256, out);
    else               efi_sha512_final(&h->s512, out);
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

    /* SizeOfHeaders is attacker-controlled and is used as the END of the
     * third hashed header region (Authenticode v1.0, "Calculating the PE
     * Image Hash" step 7: hash from the end of the Certificate Table entry
     * to the end of the image header, Section Table included). An upper
     * bound alone is not enough: a SizeOfHeaders below the start of that
     * region underflows its length into ~4 GiB. Require the region to
     * exist and the whole section table to lie inside the headers —
     * PE/COFF defines SizeOfHeaders as covering the DOS stub, the PE
     * header AND the section headers, and Linux's pefile_parse_binary
     * enforces the same two bounds (`cursor >= header_size` and
     * `n_sections > (header_size - cursor) / sizeof(*sec)`). 64-bit
     * throughout so no bound can wrap. */
    if ((uint64_t)out->size_of_headers <
        (uint64_t)out->certtable_dd_offset + 8) return false;
    if ((uint64_t)out->size_of_headers <
        (uint64_t)out->section_table_offset +
        (uint64_t)out->section_count * SECTION_HEADER_SIZE) return false;

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

static bool pe_image_digest(const uint8_t *buf, uint32_t len, uint16_t alg,
                             uint8_t *out)
{
    PeView pv;
    if (!pe_parse(buf, len, &pv)) return false;

    /* Streaming — no allocation, hash regions one at a time directly from
     * the PE image. `alg` picks the digest; the region walk below is the
     * same one for all three, which is what makes an EFI_CERT_SHA384 dbx
     * record comparable at all. */
    HashCtx h;
    if (!hash_init(&h, alg)) return false;

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

    /* Section data, sorted by PointerToRawData ascending.
     *
     * ‼ A PLAIN LOCAL, and that is the whole point. It was `static`, so the
     * per-image table Authenticode v1.0 steps 8-13 describe — built from THIS
     * image's NumberOfSections, sorted by THIS image's PointerToRawData — was
     * one array shared by every core. Two verifications at once interleaved
     * their filter loop, their insertion sort and their hashing walk over the
     * same 96 entries, and a genuinely valid signed image came back
     * `bad-pe` or `hash-mismatch` on one run and `ok` on the next. 192 bytes
     * on a 16 KiB kernel stack buys determinism. */
    uint16_t order[96];
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
        /* The index is about to become a subscript into the section table
         * that pe_parse bounded against `len`; it is checked here rather
         * than assumed from how `order` was filled ten lines up. */
        if (s >= pv.section_count) goto fail;
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

    hash_final(&h, out);
    return true;

fail:
    /* Streaming context has no heap state to release; just leave. */
    return false;
}

bool efi_authenticode_compute_pe_hash(const uint8_t *buf, uint32_t len,
                                       uint8_t out_sha256[32])
{
    return pe_image_digest(buf, len, 256, out_sha256);
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

/* =========================================================================
 * SHA-384 / SHA-512 (FIPS 180-4 §4.1.3, §5.1.2, §6.4, §6.5)
 *
 * Here rather than in src/lib/kernel/crypto.c because Secure Boot is the
 * only thing in this kernel that asks for them: UEFI 2.10 §32.4.1 defines
 * EFI_CERT_X509_SHA384/512 revocations of certificates and
 * EFI_CERT_SHA384/512 revocations of images, and a revocation record this
 * kernel cannot hash is a revocation it would silently ignore — which is
 * the defect this block exists to close.
 *
 * Declared in efi_authenticode.h, not static, for one reason: efi_selftest
 * T6 has to be able to run published FIPS vectors through the exact entry
 * points production uses. No board the project can boot in QEMU ships a
 * dbx with SHA-384 records, so the boot self-test is the only thing that
 * will exercise this arithmetic before a real machine depends on it, and a
 * wrong digest here fails OPEN — the revoked certificate compares unequal
 * and the image verifies.
 * ========================================================================= */

static const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static inline uint64_t sha512_ror(uint64_t x, unsigned n)
{
    return (x >> n) | (x << (64u - n));
}

static void sha512_block(uint64_t h[8], const uint8_t block[128])
{
    uint64_t w[80];
    for (uint32_t i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (uint32_t j = 0; j < 8; j++) v = (v << 8) | block[i * 8 + j];
        w[i] = v;
    }
    for (uint32_t i = 16; i < 80; i++) {
        uint64_t s0 = sha512_ror(w[i - 15], 1) ^ sha512_ror(w[i - 15], 8) ^
                      (w[i - 15] >> 7);
        uint64_t s1 = sha512_ror(w[i - 2], 19) ^ sha512_ror(w[i - 2], 61) ^
                      (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint64_t e = h[4], f = h[5], g = h[6], k = h[7];
    for (uint32_t i = 0; i < 80; i++) {
        uint64_t S1  = sha512_ror(e, 14) ^ sha512_ror(e, 18) ^ sha512_ror(e, 41);
        uint64_t ch  = (e & f) ^ (~e & g);
        uint64_t t1  = k + S1 + ch + kSha512K[i] + w[i];
        uint64_t S0  = sha512_ror(a, 28) ^ sha512_ror(a, 34) ^ sha512_ror(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2  = S0 + maj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void efi_sha512_init(EfiSha512Ctx *ctx, bool sha384)
{
    static const uint64_t iv512[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
        0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    static const uint64_t iv384[8] = {
        0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
        0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
        0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
    };

    memcpy(ctx->h, sha384 ? iv384 : iv512, sizeof(ctx->h));
    ctx->total   = 0;
    ctx->buf_len = 0;
    ctx->sha384  = sha384;
}

void efi_sha512_update(EfiSha512Ctx *ctx, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return;
    ctx->total += len;

    if (ctx->buf_len) {
        uint32_t want = 128u - ctx->buf_len;
        if (want > len) want = len;
        memcpy(ctx->buf + ctx->buf_len, data, want);
        ctx->buf_len += want;
        data += want;
        len  -= want;
        if (ctx->buf_len == 128u) {
            sha512_block(ctx->h, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    while (len >= 128u) {
        sha512_block(ctx->h, data);
        data += 128u;
        len  -= 128u;
    }
    if (len) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = len;
    }
}

void efi_sha512_final(EfiSha512Ctx *ctx, uint8_t *out)
{
    /* FIPS 180-4 §5.1.2: append 0x80, pad with zeros to 112 mod 128, then
     * the message length as a 128-bit BIG-ENDIAN count of BITS. Both
     * halves are written: `total` is a byte count in a uint64_t, so
     * total*8 overflows 64 bits above 2 EiB and the high half is
     * total >> 61 — computed rather than assumed zero, because assuming
     * it is the kind of shortcut that is right until it is not. */
    uint64_t bits_lo = ctx->total << 3;
    uint64_t bits_hi = ctx->total >> 61;

    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 112u) {
        memset(ctx->buf + ctx->buf_len, 0, 128u - ctx->buf_len);
        sha512_block(ctx->h, ctx->buf);
        ctx->buf_len = 0;
    }
    memset(ctx->buf + ctx->buf_len, 0, 112u - ctx->buf_len);
    for (uint32_t i = 0; i < 8; i++) {
        ctx->buf[112 + i] = (uint8_t)(bits_hi >> (56u - 8u * i));
        ctx->buf[120 + i] = (uint8_t)(bits_lo >> (56u - 8u * i));
    }
    sha512_block(ctx->h, ctx->buf);

    uint32_t words = ctx->sha384 ? 6u : 8u;
    for (uint32_t i = 0; i < words; i++)
        for (uint32_t j = 0; j < 8; j++)
            out[i * 8 + j] = (uint8_t)(ctx->h[i] >> (56u - 8u * j));
}

void efi_sha512(const uint8_t *data, uint32_t len, bool sha384, uint8_t *out)
{
    EfiSha512Ctx ctx;
    efi_sha512_init(&ctx, sha384);
    efi_sha512_update(&ctx, data, len);
    efi_sha512_final(&ctx, out);
}

/* Is this certificate revoked by an EFI_CERT_X509_SHA{256,384,512} record
 * in dbx?
 *
 * UEFI 2.10 §32.4.1: the SignatureData of those three types is
 * { EFI_SHA{256,384,512}_HASH ToBeSignedHash; EFI_TIME TimeOfRevocation; }
 * and the hash covers the certificate's TO-BE-SIGNED contents, not its
 * whole DER. That is why the answer has to be computed here: efi_secureboot.c
 * holds the records but never sees a certificate, and this file is the only
 * place a parsed TBSCertificate (cv->tbs / cv->tbs_len) exists.
 *
 * TimeOfRevocation is deliberately not consulted, and that is the spec's own
 * first clause rather than a shortcut. §32.6.1: images matching one of these
 * types "shall only be considered forbidden if the firmware either does not
 * support timestamp verification, or the signature type has a time of
 * revocation equal to zero, or the timestamp does not pass verification
 * against the authorized timestamp and forbidden signature databases".
 * BoxOS parses no RFC 3161 counter-signature — signedAttrs is read, the
 * unsignedAttrs where a timestamp would live is not — so it does not support
 * timestamp verification and the first clause holds for every record, at
 * every revocation time. EDK2 arrives at the same verdict by a different
 * road: IsCertHashFoundInDbx calls PassTimestampCheck, which returns FALSE
 * when the image carries no timestamp counter-signature, and the entry then
 * forbids the image outright.
 *
 * Each digest is computed at most once per certificate, and only for an
 * algorithm dbx actually carries — a machine whose dbx holds only SHA-256
 * certificate hashes never runs SHA-512 at all. */
static bool tbs_is_in_dbx(const CertView *c)
{
    uint8_t d256[32], d384[48], d512[64];
    bool have256 = false, have384 = false, have512 = false;

    if (!c->tbs || c->tbs_len == 0) return false;

    for (uint32_t i = 0; ; i++) {
        const EfiSbHashEntry *h = efi_secureboot_hash_get(i);
        if (!h) break;
        if (h->db_kind != EFI_SB_DB_DBX)        continue;
        if (!(h->hash_kind & EFI_SB_HASH_KIND_X509)) continue;

        switch (h->hash_kind & EFI_SB_HASH_KIND_BITS) {
            case 256:
                if (!have256) {
                    KSha256(c->tbs, c->tbs_len, d256);
                    have256 = true;
                }
                if (memcmp(h->hash, d256, 32) == 0) return true;
                break;
            case 384:
                if (!have384) {
                    efi_sha512(c->tbs, c->tbs_len, true, d384);
                    have384 = true;
                }
                if (memcmp(h->hash, d384, 48) == 0) return true;
                break;
            case 512:
                if (!have512) {
                    efi_sha512(c->tbs, c->tbs_len, false, d512);
                    have512 = true;
                }
                if (memcmp(h->hash, d512, 64) == 0) return true;
                break;
            default:
                break;
        }
    }
    return false;
}

static bool cert_is_in_dbx(const CertView *c)
{
    uint8_t d[32];
    KSha256(c->cert_full, c->cert_full_len, d);
    if (efi_secureboot_find_dbx_cert_sha256(d)) return true;
    if (tbs_is_in_dbx(c)) return true;
    return false;
}

/* Is the IMAGE revoked by a raw-hash dbx record?
 *
 * UEFI 2.10 §32.4.1 gives dbx three ways to name an image by its digest:
 * EFI_CERT_SHA256, EFI_CERT_SHA384 and EFI_CERT_SHA512, each holding the
 * Authenticode region hash under that algorithm. §32.6.3.3 makes any of
 * them a veto. Only the SHA-256 arm was ever asked — the other two were
 * parsed by efi_secureboot.c, counted into the state the operator sees,
 * and then never consulted: the same dead inventory, the same fail-open
 * direction as the certificate-hash records, and it survives on a board
 * whose vendor revokes an image by its SHA-384 digest.
 *
 * The wide digests are computed only when dbx actually holds a record of
 * that width, and at most once each, so a machine with Microsoft's dbx
 * (SHA-256 image hashes only) walks the image exactly once as before.
 *
 * A digest that cannot be computed is a question that was not answered,
 * and the answer is not "clean": pe_parse already succeeded for the
 * SHA-256 pass, so a failure here means the walk disagreed with itself,
 * and the image is refused out loud rather than passed. */
static bool image_is_in_dbx(const uint8_t *buf, uint32_t len,
                             const uint8_t sha256[32])
{
    if (efi_secureboot_find_dbx_hash_sha256(sha256)) return true;

    uint8_t d384[48], d512[64];
    bool have384 = false, have512 = false;

    for (uint32_t i = 0; ; i++) {
        const EfiSbHashEntry *h = efi_secureboot_hash_get(i);
        if (!h) break;
        if (h->db_kind != EFI_SB_DB_DBX)          continue;
        if (h->hash_kind & EFI_SB_HASH_KIND_X509) continue;

        switch (h->hash_kind & EFI_SB_HASH_KIND_BITS) {
            case 384:
                if (!have384) {
                    if (!pe_image_digest(buf, len, 384, d384)) {
                        kprintf("[AUTHENTICODE] dbx holds a SHA-384 image "
                                "hash and this image's SHA-384 digest could "
                                "not be computed — refusing the image\n");
                        return true;
                    }
                    have384 = true;
                }
                if (memcmp(h->hash, d384, 48) == 0) return true;
                break;
            case 512:
                if (!have512) {
                    if (!pe_image_digest(buf, len, 512, d512)) {
                        kprintf("[AUTHENTICODE] dbx holds a SHA-512 image "
                                "hash and this image's SHA-512 digest could "
                                "not be computed — refusing the image\n");
                        return true;
                    }
                    have512 = true;
                }
                if (memcmp(h->hash, d512, 64) == 0) return true;
                break;
            default:
                break;   /* 256 answered above by the dedicated lookup */
        }
    }
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

    /* The image hash is computed FIRST — before the attribute certificate
     * table is even looked at.
     *
     * UEFI 2.10 §32.6.3.3: "the security database db must either contain an
     * entry with a hash value of the image (with a supported hash type), or
     * it must contain an entry with a certificate against which an entry in
     * the image's certificate table can be verified", and "Only one hash or
     * signature is required to be present in db in order to pass validation,
     * so long as neither the hash of the binary nor any present signature is
     * reflected in dbx." Two arms authorise an image, and BoxOS ran only one
     * of them: efi_secureboot_find_db_hash_sha256 existed, the header
     * advertised it as part of this file's lookup surface, and nothing ever
     * called it. An image enrolled by digest — the AMI/Insyde setup item that
     * adds a chosen EFI file's hash to db, or an sbsiglist/KeyTool write —
     * came back `no-cert-table` while the platform's own db said it was
     * trusted, and the caller got an all-zero pe_sha256 to boot, because the
     * old order returned before the hash was computed.
     *
     * EDK2 runs it in this order too (DxeImageVerificationLib: the image
     * digest against dbx, then against db, then the certificate walk). */
    uint8_t pe_hash[32];
    if (!efi_authenticode_compute_pe_hash(buf, len, pe_hash)) {
        publish_result_touch(EFI_AC_RESULT_BAD_PE, NULL, NULL, len);
        return EFI_AC_RESULT_BAD_PE;
    }
    if (out_pe_hash) memcpy(out_pe_hash, pe_hash, 32);

    /* dbx PE-hash revocation vetoes both arms; it goes first. All three
     * widths dbx may use are asked, not just SHA-256. */
    if (image_is_in_dbx(buf, len, pe_hash)) {
        publish_result_touch(EFI_AC_RESULT_REVOKED_BY_DBX, pe_hash, NULL, len);
        return EFI_AC_RESULT_REVOKED_BY_DBX;
    }

    if (pv.attr_cert_table_size == 0) {
        /* Unsigned: the db image-hash arm is the only one there is. */
        if (efi_secureboot_find_db_hash_sha256(pe_hash)) {
            publish_result_touch(EFI_AC_RESULT_VALID, pe_hash, NULL, len);
            return EFI_AC_RESULT_VALID;
        }
        publish_result_touch(EFI_AC_RESULT_NO_CERT_TABLE, pe_hash, NULL, len);
        return EFI_AC_RESULT_NO_CERT_TABLE;
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
        if (revoked) {
            publish_result_touch(EFI_AC_RESULT_REVOKED_BY_DBX, pe_hash,
                                  signer_hash, len);
            return EFI_AC_RESULT_REVOKED_BY_DBX;
        }
        /* The chain reaches no db anchor — but §32.6.3.3's other arm is
         * still open: "Only one hash or signature is required to be present
         * in db in order to pass validation." A signed image whose digest is
         * in db is authorised even when its signer is not, and both things
         * this arm authorises have already faced dbx: the image digest, at
         * the dbx-hash test above, and the signer, which
         * chain_validates_to_db puts through cert_is_in_dbx at step 0 before
         * it looks for a parent. (Only those two. The walk stops at the
         * first cert whose parent it cannot find, so an intermediate above
         * the break was never tested — which is why this arm authorises on
         * the DIGEST and not on anything the walk failed to reach.) EDK2
         * reaches the same verdict by checking the image digest against db
         * in the signed path, not only the unsigned one. */
        if (efi_secureboot_find_db_hash_sha256(pe_hash)) {
            publish_result_touch(EFI_AC_RESULT_VALID, pe_hash, signer_hash, len);
            return EFI_AC_RESULT_VALID;
        }
        publish_result_touch(EFI_AC_RESULT_CHAIN_UNTRUSTED, pe_hash,
                              signer_hash, len);
        return EFI_AC_RESULT_CHAIN_UNTRUSTED;
    }

    publish_result_touch(EFI_AC_RESULT_VALID, pe_hash, signer_hash, len);
    return EFI_AC_RESULT_VALID;
}
