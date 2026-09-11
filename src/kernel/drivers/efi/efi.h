#ifndef EFI_H
#define EFI_H


#include "ktypes.h"


typedef uint64_t  EfiStatus;
typedef uint64_t  EfiUintn;

#define EFI_STATUS_ERROR_BIT     (1ULL << 63)
#define EFI_STATUS_SUCCESS       0ULL
#define EFI_IS_ERROR(s)          ((s) & EFI_STATUS_ERROR_BIT)

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

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} EfiTableHeader;

typedef struct {
    EfiGuid  vendor_guid;
    void    *vendor_table;
} EfiConfigurationTable;


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


#define EFIAPI __attribute__((ms_abi))

typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  pad1;
    uint32_t nanosecond;
    int16_t  timezone;
    uint8_t  daylight;
    uint8_t  pad2;
} EfiTime;

typedef struct {
    uint32_t resolution;
    uint32_t accuracy;
    uint8_t  sets_to_zero;
} EfiTimeCapabilities;

typedef enum {
    EFI_RESET_COLD              = 0,
    EFI_RESET_WARM              = 1,
    EFI_RESET_SHUTDOWN          = 2,
    EFI_RESET_PLATFORM_SPECIFIC = 3
} EfiResetType;

#define EFI_VARIABLE_NON_VOLATILE                          0x00000001U
#define EFI_VARIABLE_BOOTSERVICE_ACCESS                    0x00000002U
#define EFI_VARIABLE_RUNTIME_ACCESS                        0x00000004U
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD                 0x00000008U
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS            0x00000010U
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020U
#define EFI_VARIABLE_APPEND_WRITE                          0x00000040U
#define EFI_VARIABLE_ENHANCED_AUTHENTICATED_ACCESS         0x00000080U

typedef struct {
    EfiGuid  capsule_guid;
    uint32_t header_size;
    uint32_t flags;
    uint32_t capsule_image_size;
} EfiCapsuleHeader;

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


#define EFI_RT_VA_BASE  0xFFFFFF8000000000ULL

bool efi_runtime_init(void);

bool efi_runtime_available(void);

void      efi_reset_system(EfiResetType type, EfiStatus status,
                           uint64_t data_size, void *data);

EfiStatus efi_get_time(EfiTime *time, EfiTimeCapabilities *cap);
EfiStatus efi_set_time(EfiTime *time);
EfiStatus efi_get_wakeup_time(uint8_t *enabled, uint8_t *pending, EfiTime *time);
EfiStatus efi_set_wakeup_time(uint8_t enabled, EfiTime *time);

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

EfiStatus efi_update_capsule(EfiCapsuleHeader **capsule_header_array,
                             uint64_t          capsule_count,
                             uint64_t          scatter_gather_list);

EfiStatus efi_query_capsule_capabilities(EfiCapsuleHeader **capsule_header_array,
                                         uint64_t           capsule_count,
                                         uint64_t          *maximum_capsule_size,
                                         uint32_t          *reset_type);

EfiConfigurationTable *efi_get_configuration_table(uint32_t *out_count);

extern const EfiGuid EFI_MEMORY_ATTRIBUTES_TABLE_GUID;

void *efi_find_configuration_table(const EfiGuid *target);

bool efi_guid_equal(const EfiGuid *a, const EfiGuid *b);

void efi_runtime_print_info(void);

#endif