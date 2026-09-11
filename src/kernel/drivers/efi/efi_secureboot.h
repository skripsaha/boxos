#ifndef EFI_SECUREBOOT_H
#define EFI_SECUREBOOT_H


#include "efi.h"


extern const EfiGuid EFI_GLOBAL_VARIABLE_GUID;
extern const EfiGuid EFI_IMAGE_SECURITY_DATABASE_GUID;


extern const EfiGuid EFI_CERT_X509_GUID;
extern const EfiGuid EFI_CERT_RSA2048_GUID;
extern const EfiGuid EFI_CERT_RSA2048_SHA256_GUID;
extern const EfiGuid EFI_CERT_SHA256_GUID;
extern const EfiGuid EFI_CERT_SHA384_GUID;
extern const EfiGuid EFI_CERT_SHA512_GUID;
extern const EfiGuid EFI_CERT_X509_SHA256_GUID;
extern const EfiGuid EFI_CERT_X509_SHA384_GUID;
extern const EfiGuid EFI_CERT_X509_SHA512_GUID;


typedef struct {
    EfiGuid  signature_type;
    uint32_t signature_list_size;
    uint32_t signature_header_size;
    uint32_t signature_size;
} __attribute__((packed)) EfiSignatureList;

typedef struct {
    EfiGuid  signature_owner;
    uint8_t  signature_data[];
} __attribute__((packed)) EfiSignatureData;


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
    uint16_t db_kind;
    uint16_t _pad;
    uint32_t cert_len;
    const uint8_t *cert;
} EfiSbCertEntry;

typedef struct {
    EfiGuid  owner;
    uint16_t db_kind;
    uint16_t hash_kind;
    uint8_t  hash[64];
} EfiSbHashEntry;


typedef struct {
    bool     available;
    bool     enforced;
    bool     setup_mode;
    bool     audit_mode;
    bool     deployed_mode;
    uint32_t cert_count;
    uint32_t hash_count;
    uint32_t cert_count_by_db[EFI_SB_DB_COUNT];
    uint32_t hash_count_by_db[EFI_SB_DB_COUNT];
} EfiSecureBootState;

bool efi_secureboot_init(void);

bool efi_secureboot_available(void);

void efi_secureboot_get_state(EfiSecureBootState *out);

const EfiSbCertEntry *efi_secureboot_cert_get(uint32_t idx);
const EfiSbHashEntry *efi_secureboot_hash_get(uint32_t idx);

const EfiSbCertEntry *efi_secureboot_find_db_cert(const uint8_t *der, uint32_t len);
const EfiSbCertEntry *efi_secureboot_find_db_cert_sha256(const uint8_t hash[32]);
const EfiSbCertEntry *efi_secureboot_find_dbx_cert_sha256(const uint8_t hash[32]);
const EfiSbHashEntry *efi_secureboot_find_dbx_hash_sha256(const uint8_t hash[32]);
const EfiSbHashEntry *efi_secureboot_find_db_hash_sha256(const uint8_t hash[32]);

void efi_secureboot_print(void);

void efi_secureboot_publish_touch(void);

#endif