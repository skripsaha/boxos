
#include "efi.h"
#include "efi_secureboot.h"
#include "efi_runtime_internal.h"
#include "klib.h"
#include "crypto.h"
#include "touch.h"


const EfiGuid EFI_GLOBAL_VARIABLE_GUID = {
    0x8BE4DF61, 0x93CA, 0x11D2,
    {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}
};

const EfiGuid EFI_IMAGE_SECURITY_DATABASE_GUID = {
    0xD719B2CB, 0x3D3A, 0x4596,
    {0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F}
};

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


#define EFI_SB_MAX_DB_BYTES   (256u * 1024u)
#define EFI_SB_MAX_CERT_LEN   8192u
#define EFI_SB_MAX_HASH_LEN   64u
#define EFI_SB_MAX_CERTS      4096u
#define EFI_SB_MAX_HASHES     16384u

typedef struct {
    uint8_t  *blob;
    uint32_t  blob_len;
} EfiSbDbBlob;

static EfiSbDbBlob   g_db_blobs[EFI_SB_DB_COUNT];

static EfiSbCertEntry *g_certs       = NULL;
static uint32_t        g_certs_cap   = 0;
static uint32_t        g_cert_count  = 0;
static EfiSbHashEntry *g_hashes      = NULL;
static uint32_t        g_hashes_cap  = 0;
static uint32_t        g_hash_count  = 0;

static EfiSecureBootState g_state = {0};
static bool               g_ready = false;


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
        if (out_attrs) *out_attrs = attrs;
        return EFI_STATUS_SUCCESS;
    }
    if (s != EFI_STATUS_BUFFER_TOO_SMALL) {
        return s;
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


static int classify_signature(const EfiGuid *type, int *out_hash_bits)
{
    if (out_hash_bits) *out_hash_bits = 0;
    if (efi_guid_equal(type, &EFI_CERT_X509_GUID))           return 1;
    if (efi_guid_equal(type, &EFI_CERT_SHA256_GUID))         { *out_hash_bits = 256; return 2; }
    if (efi_guid_equal(type, &EFI_CERT_SHA384_GUID))         { *out_hash_bits = 384; return 2; }
    if (efi_guid_equal(type, &EFI_CERT_SHA512_GUID))         { *out_hash_bits = 512; return 2; }
    if (efi_guid_equal(type, &EFI_CERT_X509_SHA256_GUID))    { *out_hash_bits = 256; return 3; }
    if (efi_guid_equal(type, &EFI_CERT_X509_SHA384_GUID))    { *out_hash_bits = 384; return 3; }
    if (efi_guid_equal(type, &EFI_CERT_X509_SHA512_GUID))    { *out_hash_bits = 512; return 3; }
    if (efi_guid_equal(type, &EFI_CERT_RSA2048_GUID))        return 4;
    if (efi_guid_equal(type, &EFI_CERT_RSA2048_SHA256_GUID)) return 5;
    return 0;
}

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

        uint32_t header_size = sl->signature_header_size;
        uint32_t fixed = sizeof(EfiSignatureList) + header_size;
        if (fixed > list_size)                     goto next_list;

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
                h->hash_kind = (uint16_t)hash_bits | 0x8000;
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

    for (uint32_t i = 0; i < g_hash_count; i++) {
        const EfiSbHashEntry *h = &g_hashes[i];
        if ((h->hash_kind & 0x7FFF) != 256) continue;
        if (h->hash_kind & 0x8000)          continue;
        char hex[65];
        hash_to_hex(h->hash, 32, hex, sizeof(hex));
        char tag[96];
        ksnprintf(tag, sizeof(tag), "secureboot:%s-hash:%s", db_name(h->db_kind), hex);
        TouchPublish(tag, h->hash, 32);
    }
}


static void stage_read_db(EfiSbDb kind, const char *name, const EfiGuid *vendor)
{
    uint8_t *buf = NULL;
    uint32_t len = 0;
    uint32_t attrs = 0;
    EfiStatus s = efi_sb_read_var(name, vendor, &buf, &len, &attrs);
    if (EFI_IS_ERROR(s)) {
        debug_printf("[SB] %s: not present (status 0x%lx)\n", name, (unsigned long)s);
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

bool efi_secureboot_init(void)
{
    if (g_ready) return true;
    if (!efi_runtime_available()) {
        debug_printf("[SB] efi runtime unavailable\n");
        return false;
    }

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

    stage_read_db(EFI_SB_DB_PK,  "PK",  &EFI_GLOBAL_VARIABLE_GUID);
    stage_read_db(EFI_SB_DB_KEK, "KEK", &EFI_GLOBAL_VARIABLE_GUID);
    stage_read_db(EFI_SB_DB_DB,  "db",  &EFI_IMAGE_SECURITY_DATABASE_GUID);
    stage_read_db(EFI_SB_DB_DBX, "dbx", &EFI_IMAGE_SECURITY_DATABASE_GUID);
    stage_read_db(EFI_SB_DB_DBT, "dbt", &EFI_IMAGE_SECURITY_DATABASE_GUID);
    stage_read_db(EFI_SB_DB_DBR, "dbr", &EFI_IMAGE_SECURITY_DATABASE_GUID);

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

    if (total_certs > 0) {
        g_certs = kmalloc(sizeof(EfiSbCertEntry) * total_certs);
        if (!g_certs) {
            debug_printf("[SB] cert inventory alloc failed (%u entries)\n", total_certs);
            return false;
        }
        g_certs_cap = total_certs;
    }
    if (total_hashes > 0) {
        g_hashes = kmalloc(sizeof(EfiSbHashEntry) * total_hashes);
        if (!g_hashes) {
            debug_printf("[SB] hash inventory alloc failed (%u entries)\n", total_hashes);
            if (g_certs) { kfree(g_certs); g_certs = NULL; g_certs_cap = 0; }
            return false;
        }
        g_hashes_cap = total_hashes;
    }
    g_cert_count = 0;
    g_hash_count = 0;

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
        if ((h->hash_kind & 0x7FFF) != 256) continue;
        if (h->hash_kind & 0x8000)          continue;
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


void efi_secureboot_print(void)
{
    if (!g_ready) {
        debug_printf("[SB] not initialised\n");
        return;
    }
    debug_printf("[SB] enforced=%d setup=%d audit=%d deployed=%d "
                 "certs=%u hashes=%u\n",
                 g_state.enforced, g_state.setup_mode,
                 g_state.audit_mode, g_state.deployed_mode,
                 g_cert_count, g_hash_count);
    for (uint32_t k = 0; k < EFI_SB_DB_COUNT; k++) {
        if (g_state.cert_count_by_db[k] || g_state.hash_count_by_db[k]) {
            debug_printf("  %s: %u cert, %u hash\n",
                         db_name((EfiSbDb)k),
                         g_state.cert_count_by_db[k],
                         g_state.hash_count_by_db[k]);
        }
    }
}