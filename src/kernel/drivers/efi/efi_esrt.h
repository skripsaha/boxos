#ifndef EFI_ESRT_H
#define EFI_ESRT_H


#include "efi.h"

#define ESRT_FW_RESOURCE_VERSION_V1   1ULL

#define ESRT_FW_TYPE_UNKNOWN          0U
#define ESRT_FW_TYPE_SYSTEM_FIRMWARE  1U
#define ESRT_FW_TYPE_DEVICE_FIRMWARE  2U
#define ESRT_FW_TYPE_UEFI_DRIVER      3U

#define ESRT_LAST_ATTEMPT_SUCCESS                  0U
#define ESRT_LAST_ATTEMPT_ERROR_UNSUCCESSFUL       1U
#define ESRT_LAST_ATTEMPT_ERROR_INSUFFICIENT_RES   2U
#define ESRT_LAST_ATTEMPT_ERROR_INCORRECT_VERSION  3U
#define ESRT_LAST_ATTEMPT_ERROR_INVALID_FORMAT     4U
#define ESRT_LAST_ATTEMPT_ERROR_AUTH_ERROR         5U
#define ESRT_LAST_ATTEMPT_ERROR_AC_NOT_CONNECTED   6U
#define ESRT_LAST_ATTEMPT_ERROR_INSUFFICIENT_BATT  7U

typedef struct {
    uint32_t fw_resource_count;
    uint32_t fw_resource_count_max;
    uint64_t fw_resource_version;
} __attribute__((packed)) EfiSystemResourceTable;

typedef struct {
    EfiGuid  fw_class;
    uint32_t fw_type;
    uint32_t fw_version;
    uint32_t lowest_supported_fw_version;
    uint32_t capsule_flags;
    uint32_t last_attempt_version;
    uint32_t last_attempt_status;
} __attribute__((packed)) EfiSystemResourceEntry;

_Static_assert(sizeof(EfiSystemResourceEntry) == 40,
               "EfiSystemResourceEntry must be 40 bytes (UEFI 2.10 §23.4.2)");

bool efi_esrt_init(void);

bool efi_esrt_available(void);

uint32_t efi_esrt_count(void);

const EfiSystemResourceEntry *efi_esrt_get(uint32_t idx);

const EfiSystemResourceEntry *efi_esrt_find(const EfiGuid *target);

void efi_esrt_print(void);

void efi_esrt_publish_touch(void);

#endif