/*
 * BoxOS — UEFI Secure Boot variable consumer (UEFI 2.10 §32).
 *
 * Reads firmware-side auth variables (state + the four databases),
 * parses the EFI_SIGNATURE_LIST chain in each database into a kernel
 * inventory of cert/hash entries, and exposes lookup helpers to
 * efi_authenticode.c for image-signature verification.
 *
 * Real-HW notes (Microsoft + Lenovo + HP + Dell + Apple T2 + ARM-based
 * UEFI boards, audited against their published Secure Boot specifications):
 *
 *   * SetupMode = 1 means PK is unenrolled. SetVariable on the auth
 *     databases is unauthenticated. Production OEMs ship with PK
 *     enrolled at the factory, so we expect SetupMode == 0 in the field.
 *
 *   * AuditMode is intended for fleet-test enrolment; many OEMs never
 *     expose the UI for it but the variable is always readable.
 *
 *   * The dbx variable on Microsoft-signed Windows boards has grown
 *     past 4 KB on many systems; our GetVariable retry path accepts up
 *     to EFI_SB_MAX_DB_BYTES. That cap exists to refuse a firmware that
 *     answers with a nonsense size (a chip returning 0xFFFFFFFF), not to
 *     predict how large a shipping dbx is — this note used to name a byte
 *     count for "the August 2025 dbx revision" that nothing in this tree
 *     ever measured, and a revocation list nobody measured is not a fact.
 *     A dbx that does not fit, or that cannot be read for any other
 *     reason, is now fatal to efi_secureboot_init rather than silently
 *     indistinguishable from an empty one (see stage_read_db).
 *
 *   * EFI_VARIABLE_AUTHENTICATION_2 wrappers are NOT synthesised here.
 *     A future SetVariable-on-auth-var path (e.g. for our own deploy
 *     pipeline) must build the WIN_CERT + UEFI_AUTHENTICATED_VARIABLE
 *     payload externally; this driver only READS db variables.
 */

#include "efi.h"
#include "efi_secureboot.h"
/* For EFI_SB_HASH_KIND_BITS / EFI_SB_HASH_KIND_X509 — the one place the
 * encoding this file writes into EfiSbHashEntry.hash_kind is spelled out,
 * shared with the consumer that reads it back. */
#include "efi_authenticode.h"
#include "efi_runtime_internal.h"
#include "klib.h"
#include "crypto.h"
#include "touch.h"

/* =========================================================================
 * GUID constants (UEFI 2.10)
 * ========================================================================= */

const EfiGuid EFI_GLOBAL_VARIABLE_GUID = {
    0x8BE4DF61, 0x93CA, 0x11D2,
    {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}
};

const EfiGuid EFI_IMAGE_SECURITY_DATABASE_GUID = {
    0xD719B2CB, 0x3D3A, 0x4596,
    {0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F}
};

/* UEFI 2.10 §32.4.1 Table 32-5 signature type GUIDs. */
const EfiGuid EFI_CERT_X509_GUID = {
    0xA5C059A1, 0x94E4, 0x4AA7,
    {0x87, 0xB5, 0xAB, 0x15, 0x5C, 0x2B, 0xF0, 0x72}
};
const EfiGuid EFI_CERT_RSA2048_GUID = {
    0x3C5766E8, 0x269C, 0x4E34,
    {0xAA, 0x14, 0xED, 0x77, 0x6E, 0x85, 0xB3, 0xB6}
};
const EfiGuid EFI_CERT_RSA2048_SHA256_GUID = {
    0xE2B36190, 0x879B, 0x4A3D,
    {0xAD, 0x8D, 0xF2, 0xE7, 0xBB, 0xA3, 0x27, 0x84}
};
const EfiGuid EFI_CERT_SHA256_GUID = {
    0xC1C41626, 0x504C, 0x4092,
    {0xAC, 0xA9, 0x41, 0xF9, 0x36, 0x93, 0x43, 0x28}
};
const EfiGuid EFI_CERT_SHA384_GUID = {
    0xFF3E5307, 0x9FD0, 0x48C9,
    {0x85, 0xF1, 0x8A, 0xD5, 0x6C, 0x70, 0x1E, 0x01}
};
const EfiGuid EFI_CERT_SHA512_GUID = {
    0x093E0FAE, 0xA6C4, 0x4F50,
    {0x9F, 0x1B, 0xD4, 0x1E, 0x2B, 0x89, 0xC1, 0x9A}
};
const EfiGuid EFI_CERT_X509_SHA256_GUID = {
    0x3BD2A492, 0x96C0, 0x4079,
    {0xB4, 0x20, 0xFC, 0xF9, 0x8E, 0xF1, 0x03, 0xED}
};
const EfiGuid EFI_CERT_X509_SHA384_GUID = {
    0x7076876E, 0x80C2, 0x4EE6,
    {0xAA, 0xD2, 0x28, 0xB3, 0x49, 0xA6, 0x86, 0x5B}
};
const EfiGuid EFI_CERT_X509_SHA512_GUID = {
    0x446DBF63, 0x2502, 0x4CDA,
    {0xBC, 0xFA, 0x24, 0x65, 0xD2, 0xB0, 0xFE, 0x9D}
};

/* =========================================================================
 * Driver state
 * ========================================================================= */

#define EFI_SB_MAX_DB_BYTES   (256u * 1024u)   /* dbx ceiling we accept */
#define EFI_SB_MAX_CERT_LEN   8192u             /* sanity per X.509 entry */
#define EFI_SB_MAX_HASH_LEN   64u
#define EFI_SB_MAX_CERTS      4096u             /* hard cap defensive */
#define EFI_SB_MAX_HASHES     16384u            /* hard cap defensive */

typedef struct {
    uint8_t  *blob;            /* kmalloc'd buffer holding the raw db */
    uint32_t  blob_len;
} EfiSbDbBlob;

static EfiSbDbBlob   g_db_blobs[EFI_SB_DB_COUNT];   /* indexed by EfiSbDb */

/* What the firmware actually answered for each database, kept because
 * "absent" and "unreadable" are different answers and collapsing them into
 * one NULL blob is a fail-OPEN on exactly one of the six.
 *
 * UEFI 2.10 §32.6.3.3 makes dbx a must-not-succeed condition: verification
 * must not succeed if dbx contains a matching record. A consumer that could
 * not READ dbx has not evaluated that condition, so it may not answer as
 * though dbx were empty. §8.2.1 gives GetVariable the vocabulary for the
 * difference — EFI_NOT_FOUND is "the variable was not found", while
 * EFI_DEVICE_ERROR ("The variable could not be retrieved due to a hardware
 * error") and EFI_SECURITY_VIOLATION are failures to read one that may well
 * exist, and the size cap and the allocation failure in efi_sb_read_var are
 * failures of the same kind.
 *
 * Only dbx is fatal. An unreadable db fails CLOSED on its own: with no
 * anchors, cert_terminates_chain and find_parent find nothing and the
 * verifier answers chain-untrusted. */
static EfiStatus     g_db_read_status[EFI_SB_DB_COUNT];

/* Inventory arrays are LAZY-ALLOCATED with exact sizes after the
 * two-pass scan in efi_secureboot_init. A system with no Secure Boot
 * enrolment has cert_count=0 + hash_count=0 and no kmalloc happens. */
static EfiSbCertEntry *g_certs       = NULL;
static uint32_t        g_certs_cap   = 0;   /* allocated capacity */
static uint32_t        g_cert_count  = 0;
static EfiSbHashEntry *g_hashes      = NULL;
static uint32_t        g_hashes_cap  = 0;
static uint32_t        g_hash_count  = 0;

static EfiSecureBootState g_state = {0};
static bool               g_ready = false;

/* =========================================================================
 * Variable read helper with EFI_BUFFER_TOO_SMALL retry
 *
 * UEFI 2.10 §8.2.1 — GetVariable on data=NULL returns BUFFER_TOO_SMALL
 * with *data_size set to the required size. We retry once with a
 * matching allocation. Cap to EFI_SB_MAX_DB_BYTES to refuse runaway
 * firmware bugs (e.g. a chip that returns 0xFFFFFFFF for the size).
 * ========================================================================= */

static EfiStatus efi_sb_read_var(const char *name,
                                  const EfiGuid *vendor,
                                  uint8_t **out_buf,
                                  uint32_t *out_len,
                                  uint32_t *out_attrs)
{
    if (out_buf) *out_buf = NULL;
    if (out_len) *out_len = 0;
    if (out_attrs) *out_attrs = 0;

    uint64_t want = 0;
    uint32_t attrs = 0;
    EfiStatus s = efi_get_variable_ascii(name, vendor, &attrs, &want, NULL);

    if (s == EFI_STATUS_SUCCESS) {
        /* Variable existed but had zero-length data — legal. */
        if (out_attrs) *out_attrs = attrs;
        return EFI_STATUS_SUCCESS;
    }
    if (s != EFI_STATUS_BUFFER_TOO_SMALL) {
        return s;   /* not present, no permission, device error */
    }
    if (want == 0 || want > EFI_SB_MAX_DB_BYTES) {
        return EFI_STATUS_OUT_OF_RESOURCES;
    }

    uint8_t *buf = kmalloc(want);
    if (!buf) return EFI_STATUS_OUT_OF_RESOURCES;

    uint64_t got = want;
    s = efi_get_variable_ascii(name, vendor, &attrs, &got, buf);
    if (EFI_IS_ERROR(s)) {
        kfree(buf);
        return s;
    }
    if (out_buf)  *out_buf  = buf;
    if (out_len)  *out_len  = (uint32_t)got;
    if (out_attrs) *out_attrs = attrs;
    return EFI_STATUS_SUCCESS;
}

/* =========================================================================
 * UINT8 state-variable reads
 * ========================================================================= */

static bool read_u8_var(const char *name, uint8_t *out)
{
    uint8_t v = 0;
    uint64_t sz = sizeof(v);
    uint32_t attrs = 0;
    EfiStatus s = efi_get_variable_ascii(name, &EFI_GLOBAL_VARIABLE_GUID,
                                          &attrs, &sz, &v);
    if (s == EFI_STATUS_SUCCESS && sz == 1) {
        *out = v;
        return true;
    }
    return false;
}

/* =========================================================================
 * EFI_SIGNATURE_LIST parser
 *
 * Walk a database blob. Each EFI_SIGNATURE_LIST block contains a header
 * + signature_header_size opaque bytes + N entries of fixed size:
 *   signature_size = sizeof(EfiGuid)+signature_payload_size
 *
 * UEFI 2.10 §32.4.1 specifies the exact format. We validate every length
 * field — a malformed block aborts parsing of that block (others continue).
 * ========================================================================= */

static int classify_signature(const EfiGuid *type, int *out_hash_bits)
{
    if (out_hash_bits) *out_hash_bits = 0;
    if (efi_guid_equal(type, &EFI_CERT_X509_GUID))           return 1;  /* cert */
    if (efi_guid_equal(type, &EFI_CERT_SHA256_GUID))         { *out_hash_bits = 256; return 2; }
    if (efi_guid_equal(type, &EFI_CERT_SHA384_GUID))         { *out_hash_bits = 384; return 2; }
    if (efi_guid_equal(type, &EFI_CERT_SHA512_GUID))         { *out_hash_bits = 512; return 2; }
    if (efi_guid_equal(type, &EFI_CERT_X509_SHA256_GUID))    { *out_hash_bits = 256; return 3; }
    if (efi_guid_equal(type, &EFI_CERT_X509_SHA384_GUID))    { *out_hash_bits = 384; return 3; }
    if (efi_guid_equal(type, &EFI_CERT_X509_SHA512_GUID))    { *out_hash_bits = 512; return 3; }
    if (efi_guid_equal(type, &EFI_CERT_RSA2048_GUID))        return 4;
    if (efi_guid_equal(type, &EFI_CERT_RSA2048_SHA256_GUID)) return 5;
    return 0;   /* unknown */
}

/* Walk a signature list once.
 *   `count_only=true`  → just bumps *out_cert_count / *out_hash_count.
 *   `count_only=false` → appends parsed entries to g_certs / g_hashes
 *                        using g_cert_count / g_hash_count as cursors
 *                        (already pre-allocated to exact size).
 * The fixed-cap defenses still apply against runaway firmware. */
static void walk_signature_list(EfiSbDb kind, const uint8_t *blob, uint32_t blob_len,
                                 bool count_only,
                                 uint32_t *out_cert_count, uint32_t *out_hash_count)
{
    uint32_t pos = 0;
    while (pos + sizeof(EfiSignatureList) <= blob_len) {
        const EfiSignatureList *sl = (const EfiSignatureList *)(blob + pos);
        uint32_t list_size = sl->signature_list_size;
        if (list_size < sizeof(EfiSignatureList)) break;
        if (list_size > blob_len - pos)            break;

        uint32_t sig_size = sl->signature_size;
        if (sig_size <= sizeof(EfiGuid))           goto next_list;
        if (sig_size > EFI_SB_MAX_CERT_LEN)        goto next_list;

        /* Remainder, not sum: signature_header_size is firmware-supplied
         * and a value near UINT32_MAX would wrap `28 + header_size` back
         * into a small uint32_t that passes a `fixed > list_size` test. */
        uint32_t header_size = sl->signature_header_size;
        if (header_size > list_size - sizeof(EfiSignatureList)) goto next_list;
        uint32_t fixed = (uint32_t)sizeof(EfiSignatureList) + header_size;

        uint32_t entries_bytes = list_size - fixed;
        if (entries_bytes % sig_size)              goto next_list;
        uint32_t n = entries_bytes / sig_size;

        int hash_bits = 0;
        EfiGuid stype;
        memcpy(&stype, &sl->signature_type, sizeof(stype));
        int klass = classify_signature(&stype, &hash_bits);
        if (klass == 0 || klass == 4 || klass == 5) goto next_list;

        const uint8_t *entries = blob + pos + fixed;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = entries + i * sig_size;
            EfiGuid owner;
            memcpy(&owner, e, sizeof(owner));
            const uint8_t *payload = e + sizeof(EfiGuid);
            uint32_t payload_len = sig_size - sizeof(EfiGuid);

            if (klass == 1) {
                /* X.509 DER cert */
                if (count_only) { (*out_cert_count)++; continue; }
                if (g_cert_count >= g_certs_cap)    continue;
                g_certs[g_cert_count].owner    = owner;
                g_certs[g_cert_count].db_kind  = (uint16_t)kind;
                g_certs[g_cert_count]._pad     = 0;
                g_certs[g_cert_count].cert_len = payload_len;
                g_certs[g_cert_count].cert     = payload;
                g_cert_count++;
            } else if (klass == 2) {
                if (count_only) { (*out_hash_count)++; continue; }
                if (g_hash_count >= g_hashes_cap)    continue;
                if (payload_len > EFI_SB_MAX_HASH_LEN) continue;
                EfiSbHashEntry *h = &g_hashes[g_hash_count++];
                h->owner = owner;
                h->db_kind   = (uint16_t)kind;
                h->hash_kind = (uint16_t)hash_bits;
                memset(h->hash, 0, sizeof(h->hash));
                memcpy(h->hash, payload, payload_len);
            } else if (klass == 3) {
                if (count_only) { (*out_hash_count)++; continue; }
                if (g_hash_count >= g_hashes_cap)    continue;
                EfiSbHashEntry *h = &g_hashes[g_hash_count++];
                h->owner = owner;
                h->db_kind   = (uint16_t)kind;
                h->hash_kind = (uint16_t)hash_bits | EFI_SB_HASH_KIND_X509;
                uint32_t copy_len = payload_len > EFI_SB_MAX_HASH_LEN
                                  ? EFI_SB_MAX_HASH_LEN : payload_len;
                memset(h->hash, 0, sizeof(h->hash));
                memcpy(h->hash, payload, copy_len);
            }
        }
next_list:
        pos += list_size;
    }
}

/* =========================================================================
 * Touch publishing
 * ========================================================================= */

static void touch_publish_state(void)
{
    TouchPublish(g_state.enforced     ? "secureboot:on"            : "secureboot:off",         &g_state, sizeof(g_state));
    TouchPublish(g_state.setup_mode   ? "secureboot:setup-mode"    : "secureboot:user-mode",   &g_state, sizeof(g_state));
    if (g_state.audit_mode)    TouchPublish("secureboot:audit-mode",    &g_state, sizeof(g_state));
    if (g_state.deployed_mode) TouchPublish("secureboot:deployed-mode", &g_state, sizeof(g_state));
}

static void hash_to_hex(const uint8_t *hash, uint32_t len, char *out, uint32_t out_cap)
{
    static const char hex[] = "0123456789abcdef";
    if (!out || out_cap == 0) return;
    uint32_t need = len * 2;
    if (need >= out_cap) need = out_cap - 1;
    uint32_t produced = 0;
    for (uint32_t i = 0; i < len && produced + 2 <= need; i++) {
        out[produced++] = hex[hash[i] >> 4];
        out[produced++] = hex[hash[i] & 0xF];
    }
    out[produced] = 0;
}

static const char *db_name(EfiSbDb kind)
{
    switch (kind) {
        case EFI_SB_DB_PK:  return "pk";
        case EFI_SB_DB_KEK: return "kek";
        case EFI_SB_DB_DB:  return "db";
        case EFI_SB_DB_DBX: return "dbx";
        case EFI_SB_DB_DBT: return "dbt";
        case EFI_SB_DB_DBR: return "dbr";
        default:            return "?";
    }
}

void efi_secureboot_publish_touch(void)
{
    if (!g_ready) return;
    touch_publish_state();

    /* Per-cert tags. */
    for (uint32_t i = 0; i < g_cert_count; i++) {
        const EfiSbCertEntry *c = &g_certs[i];
        uint8_t digest[32];
        KSha256(c->cert, c->cert_len, digest);
        char hex[65];
        hash_to_hex(digest, 32, hex, sizeof(hex));
        char tag[96];
        ksnprintf(tag, sizeof(tag), "secureboot:%s-cert:%s", db_name(c->db_kind), hex);
        TouchPublish(tag, digest, sizeof(digest));
    }

    /* Per-hash tags, raw SHA-256 image hashes only.
     *
     * This is a limit of the Touch SURFACE, not of verification, and the
     * distinction matters because the comment here used to claim the
     * opposite ("authenticode currently uses SHA-256 only"). It does not:
     * efi_authenticode.c matches EFI_CERT_X509_SHA256/384/512 records
     * against a certificate's TBSCertificate and EFI_CERT_SHA256/384/512
     * records against the image digest, reading this inventory directly
     * through efi_secureboot_hash_get(). Every parsed record is consulted.
     *
     * What is not published is the wide ones: secureboot:<db>-hash:<hex>
     * carries 64 hex characters today, and a SHA-512 value needs 128 —
     * a longer tag AND a subscriber that can tell the widths apart by
     * length. An EFI_CERT_X509_SHA* record is a third thing again: a hash
     * of a CERTIFICATE, which under a tag that has always meant "image
     * hash" would say something false. Widening the surface is a decision
     * about the tag namespace, so it is asked rather than assumed. */
    for (uint32_t i = 0; i < g_hash_count; i++) {
        const EfiSbHashEntry *h = &g_hashes[i];
        if ((h->hash_kind & EFI_SB_HASH_KIND_BITS) != 256) continue;
        if (h->hash_kind & EFI_SB_HASH_KIND_X509)          continue;
        char hex[65];
        hash_to_hex(h->hash, 32, hex, sizeof(hex));
        char tag[96];
        ksnprintf(tag, sizeof(tag), "secureboot:%s-hash:%s", db_name(h->db_kind), hex);
        TouchPublish(tag, h->hash, 32);
    }
}

/* =========================================================================
 * Init / Inventory loading
 * ========================================================================= */

/* Stage 1: read the raw variable blob into g_db_blobs[kind]. No parsing,
 * no inventory commits — just preserve the bytes so the second pass can
 * walk them once we know exact alloc sizes. */
static void stage_read_db(EfiSbDb kind, const char *name, const EfiGuid *vendor)
{
    uint8_t *buf = NULL;
    uint32_t len = 0;
    uint32_t attrs = 0;
    EfiStatus s = efi_sb_read_var(name, vendor, &buf, &len, &attrs);
    g_db_read_status[kind] = s;
    if (EFI_IS_ERROR(s)) {
        if (s == EFI_STATUS_NOT_FOUND) {
            debug_printf("[SB] %s: not present\n", name);
        } else {
            /* kprintf, not debug_printf: debug_printf compiles to nothing in
             * a production build, and this is the one line that separates
             * "this platform enrolled nothing" from "this kernel never got
             * to look". The old code said "not present" for both. */
            kprintf("[SB] %s could not be read (status 0x%lx) — this is not "
                    "the same as an empty %s\n",
                    name, (unsigned long)s, name);
        }
        return;
    }
    if (!buf || len == 0) {
        debug_printf("[SB] %s: zero-length\n", name);
        return;
    }
    g_db_blobs[kind].blob     = buf;
    g_db_blobs[kind].blob_len = len;
    debug_printf("[SB] %s: %u bytes attrs=0x%x staged\n", name, len, attrs);
}

/* Give the staged blobs back. The inventory points INTO them, so this may
 * only run on a path that abandons the inventory entirely. */
static void release_staged_blobs(void)
{
    for (uint32_t k = 0; k < EFI_SB_DB_COUNT; k++) {
        if (g_db_blobs[k].blob) kfree(g_db_blobs[k].blob);
        g_db_blobs[k].blob     = NULL;
        g_db_blobs[k].blob_len = 0;
    }
}

bool efi_secureboot_init(void)
{
    if (g_ready) return true;
    if (!efi_runtime_available()) {
        debug_printf("[SB] efi runtime unavailable\n");
        return false;
    }

    /* Read state variables first — fast and gives early signal. */
    uint8_t sb = 0, setup = 0, audit = 0, deployed = 0;
    bool have_sb       = read_u8_var("SecureBoot",   &sb);
    bool have_setup    = read_u8_var("SetupMode",    &setup);
    bool have_audit    = read_u8_var("AuditMode",    &audit);
    bool have_deployed = read_u8_var("DeployedMode", &deployed);

    g_state.available     = true;
    g_state.enforced      = have_sb       && sb       == 1;
    g_state.setup_mode    = have_setup    && setup    == 1;
    g_state.audit_mode    = have_audit    && audit    == 1;
    g_state.deployed_mode = have_deployed && deployed == 1;

    debug_printf("[SB] state: SB=%d Setup=%d Audit=%d Deployed=%d\n",
                 g_state.enforced, g_state.setup_mode,
                 g_state.audit_mode, g_state.deployed_mode);

    /* Stage 1: read all six databases verbatim into g_db_blobs.
     * Variables not present (SB-disabled boards) leave their slot
     * with blob=NULL. No inventory allocation yet. */
    stage_read_db(EFI_SB_DB_PK,  "PK",  &EFI_GLOBAL_VARIABLE_GUID);
    stage_read_db(EFI_SB_DB_KEK, "KEK", &EFI_GLOBAL_VARIABLE_GUID);
    stage_read_db(EFI_SB_DB_DB,  "db",  &EFI_IMAGE_SECURITY_DATABASE_GUID);
    stage_read_db(EFI_SB_DB_DBX, "dbx", &EFI_IMAGE_SECURITY_DATABASE_GUID);
    stage_read_db(EFI_SB_DB_DBT, "dbt", &EFI_IMAGE_SECURITY_DATABASE_GUID);
    stage_read_db(EFI_SB_DB_DBR, "dbr", &EFI_IMAGE_SECURITY_DATABASE_GUID);

    /* An unreadable dbx is where this driver stops.
     *
     * Every revocation check in efi_authenticode.c — find_dbx_hash_sha256,
     * find_dbx_cert_sha256 — answers from this inventory, so a dbx that was
     * never read makes each of them return "not revoked" for every input.
     * A Microsoft-revoked bootloader would then walk the chain to a db
     * anchor and come back `ok`, with nothing in the state blob or the boot
     * log to say the revocation list had never been opened. UEFI 2.10
     * §32.6.3.3 does not allow that answer: the dbx test is a condition
     * verification MUST NOT succeed against, and a consumer that could not
     * evaluate it has to refuse rather than guess.
     *
     * So g_ready stays false. efi_secureboot_available() then reports the
     * service as unavailable, efi_authenticode_verify_pe returns
     * EFI_AC_RESULT_SB_UNAVAILABLE at its first line, SYSTEM_OP_EFI_INFO
     * hands back a zeroed state with available=false, and boxcxx's
     * box::efi::verified() — which is already written to fail closed when
     * Secure Boot is unavailable — gets the answer its own design expects.
     * Linux takes the same shape of decision, clearing the capability rather
     * than serving a half-read one.
     *
     * ‼ And it costs more than the verdict, which has to be said rather
     * than discovered: efi_secureboot_get_state() memsets the whole blob
     * when g_ready is false, so on a board that is ENFORCING Secure Boot
     * but cannot hand over dbx, the operator loses the PK/KEK/db inventory
     * and the SecureBoot / SetupMode / AuditMode / DeployedMode flags too —
     * the state is erased, not qualified. That is a consequence of the
     * shape, not of the finding: EfiSecureBootState has no field in which a
     * per-database read status could be carried instead, and giving it one
     * is a change to efi_secureboot.h. Until it has one, refusing is the
     * only answer here that is not a guess.
     *
     * Two statuses are NOT this case, and both have to be let through by
     * name rather than by being errors that happen not to matter:
     *
     *   - EFI_NOT_FOUND. §8.2.1: "the variable was not found." The
     *     platform enrolled no revocation list. That is an answer, and an
     *     empty dbx is what it means.
     *
     *   - EFI_UNSUPPORTED. §8.2.1: after ExitBootServices() GetVariable is
     *     unsupported "if no variable storage is supported". A board with
     *     no runtime variable store cannot hand over db either, so the
     *     verifier already fails closed on its own — no anchor reaches
     *     cert_terminates_chain and every image comes back
     *     chain-untrusted. Refusing here would take Secure Boot state,
     *     efi_secureboot_print and the Touch publication away from a
     *     platform that was never fail-open to begin with. */
    if (EFI_IS_ERROR(g_db_read_status[EFI_SB_DB_DBX]) &&
        g_db_read_status[EFI_SB_DB_DBX] != EFI_STATUS_NOT_FOUND &&
        g_db_read_status[EFI_SB_DB_DBX] != EFI_STATUS_UNSUPPORTED) {
        kprintf("[SB] dbx could not be read (status 0x%lx) — refusing to "
                "serve Secure Boot state: an unreadable revocation list is "
                "not an empty one, and every image would verify against a "
                "dbx this kernel never saw\n",
                (unsigned long)g_db_read_status[EFI_SB_DB_DBX]);
        release_staged_blobs();
        return false;
    }

    /* Stage 2: counting pass — walk each preserved blob to compute the
     * exact cert+hash totals. Per-DB counters drive both the lazy alloc
     * and the diagnostic state struct. */
    uint32_t per_db_cert[EFI_SB_DB_COUNT] = {0};
    uint32_t per_db_hash[EFI_SB_DB_COUNT] = {0};
    uint32_t total_certs = 0, total_hashes = 0;
    for (uint32_t k = 0; k < EFI_SB_DB_COUNT; k++) {
        if (!g_db_blobs[k].blob) continue;
        walk_signature_list((EfiSbDb)k, g_db_blobs[k].blob,
                             g_db_blobs[k].blob_len,
                             true, &per_db_cert[k], &per_db_hash[k]);
        total_certs  += per_db_cert[k];
        total_hashes += per_db_hash[k];
        g_state.cert_count_by_db[k] = per_db_cert[k];
        g_state.hash_count_by_db[k] = per_db_hash[k];
    }
    if (total_certs  > EFI_SB_MAX_CERTS)   total_certs  = EFI_SB_MAX_CERTS;
    if (total_hashes > EFI_SB_MAX_HASHES)  total_hashes = EFI_SB_MAX_HASHES;

    debug_printf("[SB] inventory totals: certs=%u hashes=%u (skipping alloc if zero)\n",
                 total_certs, total_hashes);

    /* Stage 3: lazy allocation. Zero entries → zero alloc → ~690 KB saved
     * on every BIOS-only / SB-disabled machine. */
    if (total_certs > 0) {
        g_certs = kmalloc(sizeof(EfiSbCertEntry) * total_certs);
        if (!g_certs) {
            debug_printf("[SB] cert inventory alloc failed (%u entries)\n", total_certs);
            release_staged_blobs();
            return false;
        }
        g_certs_cap = total_certs;
    }
    if (total_hashes > 0) {
        g_hashes = kmalloc(sizeof(EfiSbHashEntry) * total_hashes);
        if (!g_hashes) {
            debug_printf("[SB] hash inventory alloc failed (%u entries)\n", total_hashes);
            if (g_certs) { kfree(g_certs); g_certs = NULL; g_certs_cap = 0; }
            release_staged_blobs();
            return false;
        }
        g_hashes_cap = total_hashes;
    }
    g_cert_count = 0;
    g_hash_count = 0;

    /* Stage 4: populate pass — re-walk each blob and commit entries. */
    for (uint32_t k = 0; k < EFI_SB_DB_COUNT; k++) {
        if (!g_db_blobs[k].blob) continue;
        walk_signature_list((EfiSbDb)k, g_db_blobs[k].blob,
                             g_db_blobs[k].blob_len,
                             false, NULL, NULL);
    }

    g_state.cert_count = g_cert_count;
    g_state.hash_count = g_hash_count;
    g_ready = true;
    return true;
}

bool efi_secureboot_available(void) { return g_ready; }

void efi_secureboot_get_state(EfiSecureBootState *out)
{
    if (!out) return;
    if (g_ready) *out = g_state;
    else         memset(out, 0, sizeof(*out));
}

const EfiSbCertEntry *efi_secureboot_cert_get(uint32_t idx)
{
    if (!g_ready || idx >= g_cert_count) return NULL;
    return &g_certs[idx];
}

const EfiSbHashEntry *efi_secureboot_hash_get(uint32_t idx)
{
    if (!g_ready || idx >= g_hash_count) return NULL;
    return &g_hashes[idx];
}

/* =========================================================================
 * Lookup helpers (linear scan — db/dbx are tiny vs the search frequency,
 * and a precomputed hash table would add complexity without measurable
 * benefit when verification is rare).
 * ========================================================================= */

const EfiSbCertEntry *efi_secureboot_find_db_cert(const uint8_t *der, uint32_t len)
{
    if (!g_ready || !der || !len) return NULL;
    for (uint32_t i = 0; i < g_cert_count; i++) {
        const EfiSbCertEntry *c = &g_certs[i];
        if (c->db_kind != EFI_SB_DB_DB) continue;
        if (c->cert_len != len)         continue;
        if (memcmp(c->cert, der, len) == 0) return c;
    }
    return NULL;
}

static const EfiSbCertEntry *find_cert_by_sha256(EfiSbDb kind, const uint8_t hash[32])
{
    if (!g_ready || !hash) return NULL;
    for (uint32_t i = 0; i < g_cert_count; i++) {
        const EfiSbCertEntry *c = &g_certs[i];
        if (c->db_kind != kind) continue;
        uint8_t d[32];
        KSha256(c->cert, c->cert_len, d);
        if (memcmp(d, hash, 32) == 0) return c;
    }
    return NULL;
}

const EfiSbCertEntry *efi_secureboot_find_db_cert_sha256(const uint8_t hash[32])
{
    return find_cert_by_sha256(EFI_SB_DB_DB, hash);
}

const EfiSbCertEntry *efi_secureboot_find_dbx_cert_sha256(const uint8_t hash[32])
{
    return find_cert_by_sha256(EFI_SB_DB_DBX, hash);
}

static const EfiSbHashEntry *find_hash_sha256(EfiSbDb kind, const uint8_t hash[32])
{
    if (!g_ready || !hash) return NULL;
    for (uint32_t i = 0; i < g_hash_count; i++) {
        const EfiSbHashEntry *h = &g_hashes[i];
        if (h->db_kind != kind) continue;
        if ((h->hash_kind & EFI_SB_HASH_KIND_BITS) != 256) continue;
        if (h->hash_kind & EFI_SB_HASH_KIND_X509)          continue;
        if (memcmp(h->hash, hash, 32) == 0) return h;
    }
    return NULL;
}

const EfiSbHashEntry *efi_secureboot_find_dbx_hash_sha256(const uint8_t hash[32])
{
    return find_hash_sha256(EFI_SB_DB_DBX, hash);
}

const EfiSbHashEntry *efi_secureboot_find_db_hash_sha256(const uint8_t hash[32])
{
    return find_hash_sha256(EFI_SB_DB_DB, hash);
}

/* =========================================================================
 * Diagnostic
 * ========================================================================= */

/* Said out loud, on every UEFI boot, because it is the first thing anyone
 * asks about a UEFI machine and because silence here is ambiguous: a log
 * with no [SB] line cannot be told apart from a kernel whose Secure Boot
 * reader never ran. Every line below is kprintf for that reason — a
 * debug_printf compiles to nothing in a production build, and a fact nobody
 * can read is a fact the machine did not state. */
void efi_secureboot_print(void)
{
    if (!g_ready) {
        kprintf("[SB] this kernel has no Secure Boot state to report: the "
                "firmware's databases were not taken in, so nothing here can "
                "say whether this machine authenticates what it boots — and "
                "every image it is asked to verify will be refused rather "
                "than guessed at\n");
        return;
    }

    const char *extra = "";
    if (g_state.audit_mode && g_state.deployed_mode) {
        extra = ", with audit mode on (a failing image is recorded, not "
                "refused) and deployed mode on (the firmware allows no "
                "further mode change)";
    } else if (g_state.audit_mode) {
        extra = ", with audit mode on: a failing image is recorded rather "
                "than refused";
    } else if (g_state.deployed_mode) {
        extra = ", with deployed mode on: the firmware allows no further "
                "mode change";
    }

    kprintf("[SB] Secure Boot is %s; the firmware is in %s mode%s%s. This "
            "kernel took in %u certificate(s) and %u signature hash(es) from "
            "PK/KEK/db/dbx and judges every image it loads against them\n",
            g_state.enforced
                ? "ON — the firmware authenticates every image it boots"
                : "OFF — the firmware boots images without checking a signature",
            g_state.setup_mode ? "setup" : "user",
            g_state.setup_mode
                ? " (no platform key is enrolled, so PK can still be written "
                  "unauthenticated)"
                : " (a platform key is enrolled)",
            extra, g_cert_count, g_hash_count);

    for (uint32_t k = 0; k < EFI_SB_DB_COUNT; k++) {
        if (g_state.cert_count_by_db[k] || g_state.hash_count_by_db[k]) {
            debug_printf("  %s: %u cert, %u hash\n",
                         db_name((EfiSbDb)k),
                         g_state.cert_count_by_db[k],
                         g_state.hash_count_by_db[k]);
        }
    }
}
