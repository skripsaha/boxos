#ifndef EFI_SECUREBOOT_H
#define EFI_SECUREBOOT_H

/*
 * UEFI Secure Boot variable consumer (UEFI 2.10 §32).
 *
 * Reads the firmware's auth variables and parses every EFI_SIGNATURE_LIST
 * into a kernel-side inventory of trust anchors (db) and revocations (dbx),
 * plus the chain anchors PK and KEK.
 *
 * Real-HW state we expose:
 *
 *   SecureBoot   (UINT8) — UEFI 2.10 §32.3.4. 1 when the firmware is
 *                          actively enforcing image authentication on
 *                          every boot.
 *   SetupMode    (UINT8) — UEFI 2.10 §32.3.1. 1 means no PK is enrolled
 *                          and SetVariable on PK is unauthenticated.
 *   AuditMode    (UINT8) — UEFI 2.10 §32.3.2. Log-only mode.
 *   DeployedMode (UINT8) — UEFI 2.10 §32.3.3. Locked-down mode, no
 *                          mode transitions allowed.
 *
 * Databases parsed — all six that UEFI 2.10 §32 defines:
 *
 *   PK   (EFI_GLOBAL_VARIABLE)              Platform key (single X.509).
 *   KEK  (EFI_GLOBAL_VARIABLE)              Key-exchange keys (chain root).
 *   db   (EFI_IMAGE_SECURITY_DATABASE_GUID) Authorised image database.
 *   dbx  (...)                              Forbidden image / cert database
 *                                            (revocations).
 *   dbt  (...)                              Timestamp signing database.
 *   dbr  (...)                              Recovery key database.
 *
 * Each database is a concatenation of EFI_SIGNATURE_LIST blocks. Each
 * block carries one signature_type GUID + N signature payloads of
 * uniform size. Our parser flattens all six into two collections, each
 * entry tagged with which database it came from (EfiSbCertEntry.db_kind /
 * EfiSbHashEntry.db_kind):
 *   - certificate entries     (X.509 DER blobs)
 *   - raw hash entries        (SHA-256/384/512, up to 64 bytes). Every
 *                              width efi_authenticode.c is handed — raw
 *                              hash and X.509-then-hash alike — is
 *                              consulted during verification; SHA-256 is
 *                              not a verification limit, only the width
 *                              this file's Touch tags carry.
 *
 * Touch surface (one pair of tags per database that holds anything, named
 * by db_kind: pk/kek/db/dbx/dbt/dbr):
 *   secureboot:on / secureboot:off          state
 *   secureboot:setup-mode / user-mode       PK enrollment state
 *   secureboot:deployed-mode                locked
 *   secureboot:audit-mode                   log-only
 *   secureboot:<db>-cert:<sha256-of-cert>   per X.509 in that database
 *   secureboot:<db>-hash:<sha256>           per raw SHA-256 in that
 *                                            database (SHA-384/512 and
 *                                            X.509-hash records are kept
 *                                            in the inventory but not
 *                                            published under this tag)
 *
 * Storage: parsed certs/hashes are kmalloc'd at init; cleared by
 * efi_secureboot_reset() (only used in tests). On production boots
 * the inventory lives for the kernel session.
 */

#include "efi.h"

/* =========================================================================
 * Global UEFI variable namespace GUIDs
 *
 * EFI_GLOBAL_VARIABLE: {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}
 * EFI_IMAGE_SECURITY_DATABASE_GUID: {D719B2CB-3D3A-4596-A3BC-DAD00E67656F}
 * ========================================================================= */

extern const EfiGuid EFI_GLOBAL_VARIABLE_GUID;
extern const EfiGuid EFI_IMAGE_SECURITY_DATABASE_GUID;

/* =========================================================================
 * Signature type GUIDs (UEFI 2.10 §32.4.1 Table 32-5)
 * ========================================================================= */

extern const EfiGuid EFI_CERT_X509_GUID;          /* X.509 certificate */
extern const EfiGuid EFI_CERT_RSA2048_GUID;       /* raw RSA-2048 modulus */
extern const EfiGuid EFI_CERT_RSA2048_SHA256_GUID;/* RSA-2048 signed SHA-256 */
extern const EfiGuid EFI_CERT_SHA256_GUID;        /* raw SHA-256 hash */
extern const EfiGuid EFI_CERT_SHA384_GUID;
extern const EfiGuid EFI_CERT_SHA512_GUID;
extern const EfiGuid EFI_CERT_X509_SHA256_GUID;   /* hash of cert + ToBeSigned */
extern const EfiGuid EFI_CERT_X509_SHA384_GUID;
extern const EfiGuid EFI_CERT_X509_SHA512_GUID;

/* =========================================================================
 * EFI_SIGNATURE_LIST / EFI_SIGNATURE_DATA (UEFI 2.10 §32.4.1)
 * ========================================================================= */

typedef struct {
    EfiGuid  signature_type;
    uint32_t signature_list_size;
    uint32_t signature_header_size;
    uint32_t signature_size;
    /* followed by signature_header_size bytes + N * (16 + signature_payload_size) */
} __attribute__((packed)) EfiSignatureList;

typedef struct {
    EfiGuid  signature_owner;
    uint8_t  signature_data[];   /* signature_size - sizeof(EfiGuid) bytes */
} __attribute__((packed)) EfiSignatureData;

/* =========================================================================
 * Parsed inventory entries (kernel-internal)
 * ========================================================================= */

typedef enum {
    EFI_SB_DB_PK   = 0,
    EFI_SB_DB_KEK  = 1,
    EFI_SB_DB_DB   = 2,
    EFI_SB_DB_DBX  = 3,
    EFI_SB_DB_DBT  = 4,
    EFI_SB_DB_DBR  = 5,
    EFI_SB_DB_COUNT
} EfiSbDb;

typedef struct {
    EfiGuid  owner;
    uint16_t db_kind;    /* EfiSbDb */
    uint16_t _pad;
    uint32_t cert_len;
    const uint8_t *cert; /* DER X.509, lives inside kmalloc'd db blob */
} EfiSbCertEntry;

typedef struct {
    EfiGuid  owner;
    uint16_t db_kind;
    uint16_t hash_kind;  /* 256, 384, 512 — alg in bits */
    uint8_t  hash[64];   /* 32 bytes used for SHA-256, padded to 64 */
} EfiSbHashEntry;

/* =========================================================================
 * Driver API
 * ========================================================================= */

typedef struct {
    bool     available;          /* boot is UEFI + RT services usable */
    bool     enforced;           /* SecureBoot == 1 */
    bool     setup_mode;         /* SetupMode == 1 (no PK enrolled) */
    bool     audit_mode;         /* AuditMode == 1 */
    bool     deployed_mode;      /* DeployedMode == 1 */
    uint32_t cert_count;
    uint32_t hash_count;
    uint32_t cert_count_by_db[EFI_SB_DB_COUNT];
    uint32_t hash_count_by_db[EFI_SB_DB_COUNT];
} EfiSecureBootState;

/* Initialise — reads SecureBoot/SetupMode/AuditMode/DeployedMode, then
 * PK / KEK / db / dbx / dbt / dbr.  Idempotent.  In the order the code
 * takes them, there are four outcomes:
 *
 *   1. EFI runtime not available          -> false, nothing read at all.
 *   2. dbx present but unreadable (any status but NOT_FOUND or
 *      UNSUPPORTED)                       -> false; an unreadable
 *      revocation list must not be treated as an empty one, so the state
 *      and whatever else was staged are discarded.
 *   3. the cert or hash inventory fails to allocate once its exact size
 *      is known                           -> false; everything staged is
 *      released.
 *   4. otherwise                          -> true, including when every
 *      database is absent or empty (Secure Boot disabled at firmware) —
 *      that is a successful read of zero entries, not a failure. */
bool efi_secureboot_init(void);

/* True iff the boot is UEFI + RT services available + init ran. */
bool efi_secureboot_available(void);

/* Snapshot of the parsed state.  Always safe to call; returns a zeroed
 * struct with available=false on BIOS / pre-init paths. */
void efi_secureboot_get_state(EfiSecureBootState *out);

/* Enumerate certs / hashes (read-only views into the kmalloc'd db
 * buffers; valid until efi_secureboot_reset()).  Returns NULL when idx
 * is out of range. */
const EfiSbCertEntry *efi_secureboot_cert_get(uint32_t idx);
const EfiSbHashEntry *efi_secureboot_hash_get(uint32_t idx);

/* Lookup helpers used by efi_authenticode:
 *
 *   efi_secureboot_find_db_cert(c, l)  — exact DER match in db.
 *   efi_secureboot_find_db_cert_sha256(hash) — SHA-256-of-DER match in db.
 *   efi_secureboot_find_dbx_cert_sha256(hash) — SHA-256-of-DER match in dbx.
 *   efi_secureboot_find_dbx_hash_sha256(hash) — raw SHA-256 PE hash in dbx.
 *   efi_secureboot_find_db_hash_sha256(hash) — raw SHA-256 PE hash in db.
 *
 * Returns the matching entry pointer or NULL.  Hash inputs are 32-byte
 * SHA-256 outputs.
 */
const EfiSbCertEntry *efi_secureboot_find_db_cert(const uint8_t *der, uint32_t len);
const EfiSbCertEntry *efi_secureboot_find_db_cert_sha256(const uint8_t hash[32]);
const EfiSbCertEntry *efi_secureboot_find_dbx_cert_sha256(const uint8_t hash[32]);
const EfiSbHashEntry *efi_secureboot_find_dbx_hash_sha256(const uint8_t hash[32]);
const EfiSbHashEntry *efi_secureboot_find_db_hash_sha256(const uint8_t hash[32]);

/* Say the Secure Boot state on the console, not to a debug build: whether
 * the firmware is enforcing, whether it is in setup or user mode, and how
 * many trust anchors and revocations were taken in. Safe before init and
 * after a failed one — it then says that no state was taken in, which a
 * silent log could not be told apart from a reader that never ran. */
void efi_secureboot_print(void);

/* Re-publish all secureboot:* Touch tags. */
void efi_secureboot_publish_touch(void);

#endif /* EFI_SECUREBOOT_H */
