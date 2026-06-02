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
 * Databases parsed:
 *
 *   PK   (EFI_GLOBAL_VARIABLE)             Platform key (single X.509).
 *   KEK  (EFI_GLOBAL_VARIABLE)             Key-exchange keys (chain root).
 *   db   (EFI_IMAGE_SECURITY_DATABASE_GUID) Authorised image database.
 *   dbx  (...)                              Forbidden image / cert database
 *                                            (revocations).
 *
 * Each database is a concatenation of EFI_SIGNATURE_LIST blocks. Each
 * block carries one signature_type GUID + N signature payloads of
 * uniform size. Our parser flattens db / dbx into two collections:
 *   - certificate entries     (X.509 DER blobs)
 *   - raw hash entries        (SHA-256, 32 bytes; SHA-384/SHA-512 also
 *                              recorded but only SHA-256 is searched in
 *                              Authenticode verify per Microsoft's
 *                              currently-deployed dbx).
 *
 * Touch surface:
 *   secureboot:on / secureboot:off          state
 *   secureboot:setup-mode / user-mode       PK enrollment state
 *   secureboot:deployed-mode                locked
 *   secureboot:audit-mode                   log-only
 *   secureboot:db-cert:<sha256-of-cert>     per X.509 in db
 *   secureboot:db-hash:<sha256>             per raw SHA-256 in db
 *   secureboot:dbx-cert:<sha256-of-cert>    per X.509 in dbx
 *   secureboot:dbx-hash:<sha256>            per raw SHA-256 in dbx
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

/* Initialise — reads SecureBoot/SetupMode/AuditMode/DeployedMode +
 * PK / KEK / db / dbx / dbt / dbr.  Idempotent.  Returns true on
 * any successful state read (even when none of the databases parse,
 * which happens on machines with Secure Boot disabled at firmware). */
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

/* Diagnostic dump. */
void efi_secureboot_print(void);

/* Re-publish all secureboot:* Touch tags. */
void efi_secureboot_publish_touch(void);

#endif /* EFI_SECUREBOOT_H */
