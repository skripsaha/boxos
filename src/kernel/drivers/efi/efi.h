#ifndef EFI_H
#define EFI_H

/*
 * BoxOS EFI runtime services driver — kernel side.
 *
 * Consumes the EFI handoff fields in boot_info v3 (see
 * src/include/boot_info.h) and:
 *
 *   1. Maps every EFI_MEMORY_RUNTIME descriptor at a dedicated kernel
 *      virtual range (EFI_RT_VA_BASE..+span) with cacheability matching
 *      the descriptor type (WB for Code/Data, UC for MemoryMappedIO).
 *   2. Calls SetVirtualAddressMap (UEFI 2.10 §8.4) exactly once to switch
 *      the firmware's RT services from flat-physical to virtual mode.
 *   3. Re-bases the runtime services pointer to its new virtual address
 *      so subsequent ResetSystem / GetTime / GetVariable calls land
 *      correctly.
 *
 * After init the public wrappers (efi_reset_system, efi_get_time,
 * efi_get_variable) provide spinlock-serialised, MS-x64-ABI calls into
 * the firmware. Available on UEFI boots only; the predicate
 * efi_runtime_available() returns false on BIOS boots and SVAM-failure
 * fallbacks.
 *
 * References:
 *   UEFI 2.10 §4.4   EFI System Table
 *   UEFI 2.10 §7.4   ExitBootServices
 *   UEFI 2.10 §8.4   Virtual Memory Services (SetVirtualAddressMap)
 *   UEFI 2.10 §8.5.1 ResetSystem
 *   UEFI 2.10 §8.3   Time Services (GetTime)
 *   UEFI 2.10 §8.2   Variable Services (GetVariable)
 */

#include "ktypes.h"

/* =========================================================================
 * EFI types and status codes (kernel-side mirror of src/boot/uefi/uefi.h)
 * ========================================================================= */

typedef uint64_t  EfiStatus;
typedef uint64_t  EfiUintn;

#define EFI_STATUS_ERROR_BIT   (1ULL << 63)
#define EFI_STATUS_SUCCESS     0ULL
#define EFI_IS_ERROR(s)        ((s) & EFI_STATUS_ERROR_BIT)

typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} EfiGuid;

/* UEFI 2.10 §4.2 EFI_TABLE_HEADER */
typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} EfiTableHeader;

/* =========================================================================
 * EFI memory descriptor (UEFI 2.10 §7.2 Table 7.10)
 * ========================================================================= */

typedef enum {
    EFI_RESERVED_MEMORY_TYPE  = 0,
    EFI_LOADER_CODE           = 1,
    EFI_LOADER_DATA           = 2,
    EFI_BOOT_SERVICES_CODE    = 3,
    EFI_BOOT_SERVICES_DATA    = 4,
    EFI_RUNTIME_SERVICES_CODE = 5,
    EFI_RUNTIME_SERVICES_DATA = 6,
    EFI_CONVENTIONAL_MEMORY   = 7,
    EFI_UNUSABLE_MEMORY       = 8,
    EFI_ACPI_RECLAIM_MEMORY   = 9,
    EFI_ACPI_MEMORY_NVS       = 10,
    EFI_MEMORY_MAPPED_IO      = 11,
    EFI_MEMORY_MAPPED_IO_PORT_SPACE = 12,
    EFI_PAL_CODE              = 13,
    EFI_PERSISTENT_MEMORY     = 14,
    EFI_MAX_MEMORY_TYPE
} EfiMemoryType;

/* Attribute bits (UEFI 2.10 §7.2 Table 7.10). */
#define EFI_MEMORY_UC          0x0000000000000001ULL
#define EFI_MEMORY_WC          0x0000000000000002ULL
#define EFI_MEMORY_WT          0x0000000000000004ULL
#define EFI_MEMORY_WB          0x0000000000000008ULL
#define EFI_MEMORY_UCE         0x0000000000000010ULL
#define EFI_MEMORY_WP          0x0000000000001000ULL
#define EFI_MEMORY_RP          0x0000000000002000ULL
#define EFI_MEMORY_XP          0x0000000000004000ULL
#define EFI_MEMORY_RO          0x0000000000020000ULL
#define EFI_MEMORY_RUNTIME     0x8000000000000000ULL

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} EfiMemoryDescriptor;

/* =========================================================================
 * Runtime Services table (UEFI 2.10 §8.5, canonical layout)
 *
 * Member order is fixed by the spec — any change breaks every UEFI
 * implementation. All function pointers use MS-x64 ABI (ms_abi); GCC
 * emits System V by default, so each typedef MUST be annotated.
 * ========================================================================= */

#define EFIAPI __attribute__((ms_abi))

/* UEFI 2.10 §8.3.1 EFI_TIME structure */
typedef struct {
    uint16_t year;       /* 1900..9999 */
    uint8_t  month;      /* 1..12 */
    uint8_t  day;        /* 1..31 */
    uint8_t  hour;       /* 0..23 */
    uint8_t  minute;     /* 0..59 */
    uint8_t  second;     /* 0..59 */
    uint8_t  pad1;
    uint32_t nanosecond; /* 0..999_999_999 */
    int16_t  timezone;   /* -1440..1440 or 2047 */
    uint8_t  daylight;
    uint8_t  pad2;
} EfiTime;

typedef struct {
    uint32_t resolution;     /* clock resolution in Hz */
    uint32_t accuracy;       /* error rate in 1e-6 parts */
    uint8_t  sets_to_zero;   /* true if SetTime resets sub-second to 0 */
} EfiTimeCapabilities;

/* UEFI 2.10 §8.5.1 ResetType */
typedef enum {
    EFI_RESET_COLD              = 0,
    EFI_RESET_WARM              = 1,
    EFI_RESET_SHUTDOWN          = 2,
    EFI_RESET_PLATFORM_SPECIFIC = 3
} EfiResetType;

typedef EfiStatus (EFIAPI *EfiGetTimeFn)(EfiTime *time, EfiTimeCapabilities *cap);
typedef EfiStatus (EFIAPI *EfiSetTimeFn)(EfiTime *time);
typedef EfiStatus (EFIAPI *EfiGetWakeupTimeFn)(uint8_t *enabled, uint8_t *pending, EfiTime *time);
typedef EfiStatus (EFIAPI *EfiSetWakeupTimeFn)(uint8_t enabled, EfiTime *time);
typedef EfiStatus (EFIAPI *EfiSetVirtualAddressMapFn)(EfiUintn memory_map_size,
                                                     EfiUintn descriptor_size,
                                                     uint32_t descriptor_version,
                                                     EfiMemoryDescriptor *virtual_map);
typedef EfiStatus (EFIAPI *EfiConvertPointerFn)(EfiUintn debug_disposition, void **address);
typedef EfiStatus (EFIAPI *EfiGetVariableFn)(uint16_t *variable_name,
                                             const EfiGuid *vendor,
                                             uint32_t *attributes,
                                             EfiUintn *data_size,
                                             void *data);
typedef EfiStatus (EFIAPI *EfiGetNextVariableNameFn)(EfiUintn *variable_name_size,
                                                    uint16_t *variable_name,
                                                    EfiGuid *vendor);
typedef EfiStatus (EFIAPI *EfiSetVariableFn)(uint16_t *variable_name,
                                              const EfiGuid *vendor,
                                              uint32_t attributes,
                                              EfiUintn data_size,
                                              const void *data);
typedef EfiStatus (EFIAPI *EfiGetNextHighMonoCountFn)(uint32_t *high_count);
typedef void      (EFIAPI *EfiResetSystemFn)(EfiResetType reset_type,
                                              EfiStatus    reset_status,
                                              EfiUintn     data_size,
                                              void        *reset_data);
typedef EfiStatus (EFIAPI *EfiUpdateCapsuleFn)(void *capsule_header_array,
                                                EfiUintn capsule_count,
                                                uint64_t scatter_gather_list);
typedef EfiStatus (EFIAPI *EfiQueryCapsuleCapsFn)(void *capsule_header_array,
                                                   EfiUintn capsule_count,
                                                   uint64_t *maximum_capsule_size,
                                                   uint32_t *reset_type);
typedef EfiStatus (EFIAPI *EfiQueryVariableInfoFn)(uint32_t attributes,
                                                    uint64_t *max_var_storage,
                                                    uint64_t *remaining_var_storage,
                                                    uint64_t *max_var_size);

typedef struct {
    EfiTableHeader            hdr;
    EfiGetTimeFn              get_time;
    EfiSetTimeFn              set_time;
    EfiGetWakeupTimeFn        get_wakeup_time;
    EfiSetWakeupTimeFn        set_wakeup_time;
    EfiSetVirtualAddressMapFn set_virtual_address_map;
    EfiConvertPointerFn       convert_pointer;
    EfiGetVariableFn          get_variable;
    EfiGetNextVariableNameFn  get_next_variable_name;
    EfiSetVariableFn          set_variable;
    EfiGetNextHighMonoCountFn get_next_high_mono_count;
    EfiResetSystemFn          reset_system;
    EfiUpdateCapsuleFn        update_capsule;
    EfiQueryCapsuleCapsFn     query_capsule_caps;
    EfiQueryVariableInfoFn    query_variable_info;
} EfiRuntimeServices;

/* =========================================================================
 * Driver public API
 * ========================================================================= */

/* Kernel virtual base for EFI runtime regions. Picked above the Pull-Map
 * region (0xFFFF_8800_xx) and away from the MMIO bump (0xFFFF_8000_4000)
 * and the higher-half kernel image (0xFFFFFFFF_8xxxxxxx). Linux x86_64
 * uses a similar high range descending from -4G; we use a fixed
 * ascending offset for simplicity and determinism.
 *
 * Each RT descriptor's virtual_start is computed as:
 *     EFI_RT_VA_BASE + physical_start
 * which means RT regions retain their relative layout, simplifying
 * firmware's internal pointer fixup and matching the "offset map"
 * approach used by Linux EFI on architectures where firmware has been
 * observed to mishandle reordered virtual maps. */
#define EFI_RT_VA_BASE  0xFFFFFF8000000000ULL

/* Initialise EFI runtime services from boot_info v3. Maps every
 * EFI_MEMORY_RUNTIME descriptor at EFI_RT_VA_BASE + phys, then calls
 * SetVirtualAddressMap. After success the public wrappers below are
 * usable. Idempotent on the second call (returns OK without re-mapping).
 * Returns true on success, false on any failure path — fallbacks must
 * continue to work even when SVAM fails. */
bool efi_runtime_init(void);

/* Predicate: true iff the kernel can call RT services (i.e. SVAM has
 * succeeded and rt pointer is rebased). */
bool efi_runtime_available(void);

/* Wrappers — each takes the spinlock, disables IRQs, and calls into
 * firmware with MS-x64 ABI. */
void      efi_reset_system(EfiResetType type, EfiStatus status,
                           uint64_t data_size, void *data);
EfiStatus efi_get_time(EfiTime *time, EfiTimeCapabilities *cap);
EfiStatus efi_get_variable(uint16_t *variable_name,
                           const EfiGuid *vendor,
                           uint32_t *attributes,
                           uint64_t *data_size,
                           void *data);

/* Diagnostics — print a one-line summary of the RT state. */
void efi_runtime_print_info(void);

#endif /* EFI_H */
