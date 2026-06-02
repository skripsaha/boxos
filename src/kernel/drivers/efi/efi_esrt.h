#ifndef EFI_ESRT_H
#define EFI_ESRT_H

/*
 * EFI System Resource Table driver (UEFI 2.10 §23.4).
 *
 * ESRT is a Configuration Table published by firmware that enumerates
 * every updatable firmware resource on the platform (system firmware,
 * device firmware, UEFI drivers). Each entry carries a class GUID, a
 * type, the current/lowest-supported version, the capsule flags it
 * accepts, and the status of the most recent update attempt.
 *
 * The kernel consumes ESRT for two purposes:
 *   1. Pre-flight UpdateCapsule — match a candidate capsule's GUID
 *      against ESRT entries; only resources advertised in ESRT can
 *      be updated.
 *   2. Telemetry — publish per-entry Touch tags so userspace tools
 *      (firmware updater, fleet inventory) can list firmware components
 *      without parsing the raw table.
 */

#include "efi.h"

/* UEFI 2.10 §23.4.1 fixed values. */
#define ESRT_FW_RESOURCE_VERSION_V1   1ULL

/* UEFI 2.10 §23.4.2 fw_type field. */
#define ESRT_FW_TYPE_UNKNOWN          0U
#define ESRT_FW_TYPE_SYSTEM_FIRMWARE  1U
#define ESRT_FW_TYPE_DEVICE_FIRMWARE  2U
#define ESRT_FW_TYPE_UEFI_DRIVER      3U

/* ESRT_FW_LAST_ATTEMPT_STATUS_* (UEFI 2.10 §23.4.2 Table 23-5). */
#define ESRT_LAST_ATTEMPT_SUCCESS                  0U
#define ESRT_LAST_ATTEMPT_ERROR_UNSUCCESSFUL       1U
#define ESRT_LAST_ATTEMPT_ERROR_INSUFFICIENT_RES   2U
#define ESRT_LAST_ATTEMPT_ERROR_INCORRECT_VERSION  3U
#define ESRT_LAST_ATTEMPT_ERROR_INVALID_FORMAT     4U
#define ESRT_LAST_ATTEMPT_ERROR_AUTH_ERROR         5U
#define ESRT_LAST_ATTEMPT_ERROR_AC_NOT_CONNECTED   6U
#define ESRT_LAST_ATTEMPT_ERROR_INSUFFICIENT_BATT  7U

/* UEFI 2.10 §23.4.1 EFI_SYSTEM_RESOURCE_TABLE. */
typedef struct {
    uint32_t fw_resource_count;
    uint32_t fw_resource_count_max;
    uint64_t fw_resource_version;
    /* followed by N * EFI_SYSTEM_RESOURCE_ENTRY */
} __attribute__((packed)) EfiSystemResourceTable;

/* UEFI 2.10 §23.4.2 EFI_SYSTEM_RESOURCE_ENTRY (40 bytes). */
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

/* Initialise ESRT from the boot-info v4 capture.  Idempotent.  Returns
 * false on BIOS boots or when the firmware did not publish an ESRT. */
bool efi_esrt_init(void);

/* True iff ESRT was successfully parsed. */
bool efi_esrt_available(void);

/* Number of EFI_SYSTEM_RESOURCE_ENTRY records.  0 if unavailable. */
uint32_t efi_esrt_count(void);

/* Read-only view of an entry.  Returns NULL when idx >= efi_esrt_count(). */
const EfiSystemResourceEntry *efi_esrt_get(uint32_t idx);

/* Locate an entry whose fw_class GUID matches `target`.  Returns NULL
 * if not present. */
const EfiSystemResourceEntry *efi_esrt_find(const EfiGuid *target);

/* Diagnostic: dump each entry to debug_printf. */
void efi_esrt_print(void);

/* Touch tag publisher — call once after init to broadcast esrt:ready +
 * one esrt:fwclass per entry.  Idempotent (re-publishing on demand is
 * safe but spammy). */
void efi_esrt_publish_touch(void);

#endif /* EFI_ESRT_H */
