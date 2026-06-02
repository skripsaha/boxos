#ifndef EFI_H
#define EFI_H

/*
 * BoxOS EFI runtime services driver — kernel side.
 *
 * Consumes the EFI handoff fields in boot_info v4 (see
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
 *   4. Surfaces wrappers for every UEFI 2.10 §8 RT service we use:
 *        ResetSystem (§8.5.1)
 *        GetTime / SetTime / GetWakeupTime / SetWakeupTime (§8.3)
 *        GetVariable / SetVariable / GetNextVariableName / QueryVariableInfo (§8.2)
 *        UpdateCapsule / QueryCapsuleCapabilities (§8.5.3-4)
 *
 * After init the public wrappers provide spinlock-serialised,
 * MS-x64-ABI calls into the firmware. Available on UEFI boots only;
 * the predicate efi_runtime_available() returns false on BIOS boots
 * and SVAM-failure fallbacks.
 *
 * References:
 *   UEFI 2.10 §4.4   EFI System Table
 *   UEFI 2.10 §4.6   EFI Configuration Table
 *   UEFI 2.10 §7.4   ExitBootServices
 *   UEFI 2.10 §8.1   Runtime Services overview (non-reentrant rule)
 *   UEFI 2.10 §8.2   Variable Services
 *   UEFI 2.10 §8.3   Time Services
 *   UEFI 2.10 §8.4   Virtual Memory Services (SetVirtualAddressMap)
 *   UEFI 2.10 §8.5   Misc Services (ResetSystem, Capsule)
 *   UEFI 2.10 §23    Firmware Update / ESRT
 */

#include "ktypes.h"

/* =========================================================================
 * EFI types and status codes (kernel-side mirror of src/boot/uefi/uefi.h)
 * ========================================================================= */

typedef uint64_t  EfiStatus;
typedef uint64_t  EfiUintn;

#define EFI_STATUS_ERROR_BIT     (1ULL << 63)
#define EFI_STATUS_SUCCESS       0ULL
#define EFI_IS_ERROR(s)          ((s) & EFI_STATUS_ERROR_BIT)

/* Named codes used by callers (UEFI 2.10 Appendix D). */
#define EFI_STATUS_UNSUPPORTED       (EFI_STATUS_ERROR_BIT | 3ULL)
#define EFI_STATUS_BAD_BUFFER_SIZE   (EFI_STATUS_ERROR_BIT | 4ULL)
#define EFI_STATUS_BUFFER_TOO_SMALL  (EFI_STATUS_ERROR_BIT | 5ULL)
#define EFI_STATUS_NOT_READY         (EFI_STATUS_ERROR_BIT | 6ULL)
#define EFI_STATUS_DEVICE_ERROR      (EFI_STATUS_ERROR_BIT | 7ULL)
#define EFI_STATUS_WRITE_PROTECTED   (EFI_STATUS_ERROR_BIT | 8ULL)
#define EFI_STATUS_OUT_OF_RESOURCES  (EFI_STATUS_ERROR_BIT | 9ULL)
#define EFI_STATUS_INVALID_PARAMETER (EFI_STATUS_ERROR_BIT | 2ULL)
#define EFI_STATUS_NOT_FOUND         (EFI_STATUS_ERROR_BIT | 14ULL)
#define EFI_STATUS_SECURITY_VIOLATION (EFI_STATUS_ERROR_BIT | 26ULL)

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

/* UEFI 2.10 §4.6 EFI_CONFIGURATION_TABLE entry. */
typedef struct {
    EfiGuid  vendor_guid;
    void    *vendor_table;
} EfiConfigurationTable;

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

/* UEFI 2.10 §8.2 — Variable Attributes. */
#define EFI_VARIABLE_NON_VOLATILE                          0x00000001U
#define EFI_VARIABLE_BOOTSERVICE_ACCESS                    0x00000002U
#define EFI_VARIABLE_RUNTIME_ACCESS                        0x00000004U
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD                 0x00000008U
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS            0x00000010U   /* deprecated 2.4 */
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020U
#define EFI_VARIABLE_APPEND_WRITE                          0x00000040U
#define EFI_VARIABLE_ENHANCED_AUTHENTICATED_ACCESS         0x00000080U

/* UEFI 2.10 §8.5.3.1 EFI_CAPSULE_HEADER. */
typedef struct {
    EfiGuid  capsule_guid;
    uint32_t header_size;
    uint32_t flags;
    uint32_t capsule_image_size;
} EfiCapsuleHeader;

/* Capsule flag bits (UEFI 2.10 §8.5.3.1). */
#define CAPSULE_FLAGS_PERSIST_ACROSS_RESET    0x00010000U
#define CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE   0x00020000U
#define CAPSULE_FLAGS_INITIATE_RESET          0x00040000U

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
typedef EfiStatus (EFIAPI *EfiUpdateCapsuleFn)(EfiCapsuleHeader **capsule_header_array,
                                                EfiUintn capsule_count,
                                                uint64_t scatter_gather_list);
typedef EfiStatus (EFIAPI *EfiQueryCapsuleCapsFn)(EfiCapsuleHeader **capsule_header_array,
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

/* Initialise EFI runtime services from boot_info v3 or v4. Maps every
 * EFI_MEMORY_RUNTIME descriptor at EFI_RT_VA_BASE + phys, then calls
 * SetVirtualAddressMap. After success the public wrappers below are
 * usable. Idempotent on the second call (returns OK without re-mapping).
 * Returns true on success, false on any failure path — fallbacks must
 * continue to work even when SVAM fails. */
bool efi_runtime_init(void);

/* Predicate: true iff the kernel can call RT services (i.e. SVAM has
 * succeeded and rt pointer is rebased). */
bool efi_runtime_available(void);

/* ------------------------------------------------------------------------
 * Misc / Reset (UEFI 2.10 §8.5.1)
 * ------------------------------------------------------------------------ */
void      efi_reset_system(EfiResetType type, EfiStatus status,
                           uint64_t data_size, void *data);

/* ------------------------------------------------------------------------
 * Time (UEFI 2.10 §8.3)
 * ------------------------------------------------------------------------ */
EfiStatus efi_get_time(EfiTime *time, EfiTimeCapabilities *cap);
EfiStatus efi_set_time(EfiTime *time);
EfiStatus efi_get_wakeup_time(uint8_t *enabled, uint8_t *pending, EfiTime *time);
EfiStatus efi_set_wakeup_time(uint8_t enabled, EfiTime *time);

/* ------------------------------------------------------------------------
 * Variable Services (UEFI 2.10 §8.2)
 *
 * variable_name is a UCS-2 (UTF-16LE) string. ASCII helpers below convert
 * "BootCurrent" → u"BootCurrent\0" on the fly so call sites stay
 * C-string ergonomic.
 * ------------------------------------------------------------------------ */
EfiStatus efi_get_variable(uint16_t *variable_name,
                           const EfiGuid *vendor,
                           uint32_t *attributes,
                           uint64_t *data_size,
                           void *data);

EfiStatus efi_set_variable(uint16_t *variable_name,
                           const EfiGuid *vendor,
                           uint32_t attributes,
                           uint64_t data_size,
                           const void *data);

EfiStatus efi_get_next_variable_name(uint64_t *variable_name_size,
                                     uint16_t *variable_name,
                                     EfiGuid  *vendor);

EfiStatus efi_query_variable_info(uint32_t attributes,
                                  uint64_t *max_var_storage,
                                  uint64_t *remaining_var_storage,
                                  uint64_t *max_var_size);

/* ASCII helpers (call site supplies a narrow string; we widen to UCS-2 in
 * a kernel scratch buffer with no allocation). */
EfiStatus efi_get_variable_ascii(const char *name_ascii,
                                 const EfiGuid *vendor,
                                 uint32_t *attributes,
                                 uint64_t *data_size,
                                 void *data);

EfiStatus efi_set_variable_ascii(const char *name_ascii,
                                 const EfiGuid *vendor,
                                 uint32_t attributes,
                                 uint64_t data_size,
                                 const void *data);

/* ------------------------------------------------------------------------
 * Capsule (UEFI 2.10 §8.5.3-4)
 *
 * scatter_gather_list points to a chain of EFI_CAPSULE_BLOCK_DESCRIPTOR
 * records — null when the capsule is contiguous and the caller is OK
 * passing the capsule_header_array directly.
 * ------------------------------------------------------------------------ */
EfiStatus efi_update_capsule(EfiCapsuleHeader **capsule_header_array,
                             uint64_t          capsule_count,
                             uint64_t          scatter_gather_list);

EfiStatus efi_query_capsule_capabilities(EfiCapsuleHeader **capsule_header_array,
                                         uint64_t           capsule_count,
                                         uint64_t          *maximum_capsule_size,
                                         uint32_t          *reset_type);

/* ------------------------------------------------------------------------
 * Configuration Table access (UEFI 2.10 §4.6)
 *
 * Returns a kernel-VA pointer (via Pull Map) to the firmware-published
 * Configuration Table array and writes the count to *out_count. Returns
 * NULL with *out_count=0 on BIOS boots or when the v4 handoff is absent.
 *
 * The vendor_table fields in each entry are physical addresses — apply
 * vmm_phys_to_virt() before dereferencing them.
 * ------------------------------------------------------------------------ */
EfiConfigurationTable *efi_get_configuration_table(uint32_t *out_count);

/* Look up a specific vendor table by GUID. Returns the entry's
 * vendor_table field (a physical address; caller applies vmm_phys_to_virt)
 * or NULL if not present. */
void *efi_find_configuration_table(const EfiGuid *target);

/* Compare two EfiGuid records. Uses byte-wise compare so it's safe to
 * call on a GUID embedded inside a __packed struct (e.g. ESRT entries),
 * where the EfiGuid may not satisfy its natural 4-byte alignment.
 * memcmp is intrinsified by GCC for fixed sizeof(EfiGuid)=16. */
bool efi_guid_equal(const EfiGuid *a, const EfiGuid *b);

/* ------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------ */
void efi_runtime_print_info(void);

#endif /* EFI_H */
