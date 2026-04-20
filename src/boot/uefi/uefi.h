#ifndef UEFI_H
#define UEFI_H

/*
 * Minimal UEFI type and protocol definitions for TagBoot.
 * No external dependencies — implements exactly what TagBoot needs.
 * Based on UEFI Specification 2.10.
 *
 * CALLING CONVENTION (critical):
 *   UEFI on x86-64 uses the Microsoft x64 ABI (UEFI spec §2.3.4):
 *   arguments in RCX, RDX, R8, R9 — NOT System V (RDI, RSI, RDX, RCX).
 *   Every EFI function pointer and every EFI callback must be declared
 *   EFIAPI so GCC emits correct call sequences.  Without this attribute
 *   ALL UEFI calls silently pass arguments in the wrong registers.
 */

/* =========================================================================
 * Basic types
 * ========================================================================= */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;
typedef signed int         int32_t;

/*
 * uintptr_t must be 64-bit on the UEFI x86_64 target regardless of the
 * compiler's platform model (LP64 on Linux, LLP64 on Windows).
 * size_t is also guaranteed 64-bit on x86_64 UEFI.
 * Guard against redefinitions that may come from built-in headers.
 */
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
typedef unsigned long long uintptr_t;
#endif

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned long long size_t;
#endif

/* =========================================================================
 * EFIAPI — Microsoft x64 calling convention for all UEFI functions.
 * Applied to every function pointer typedef and every EFI callback.
 * ========================================================================= */
#if defined(__GNUC__) || defined(__clang__)
#  define EFIAPI __attribute__((ms_abi))
#else
#  define EFIAPI
#endif

typedef uint16_t  CHAR16;
typedef uint64_t  UINTN;
typedef int64_t   INTN;
typedef uint8_t   BOOLEAN;
typedef void*     EFI_HANDLE;
typedef void*     EFI_EVENT;
typedef uint64_t  EFI_STATUS;
typedef uint64_t  EFI_PHYSICAL_ADDRESS;
typedef uint64_t  EFI_VIRTUAL_ADDRESS;
typedef uint64_t  EFI_LBA;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

/* =========================================================================
 * EFI_STATUS codes
 * ========================================================================= */

#define EFI_SUCCESS               0ULL
#define EFI_ERROR_BIT             (1ULL << 63)
#define EFI_LOAD_ERROR            (EFI_ERROR_BIT | 1ULL)
#define EFI_INVALID_PARAMETER     (EFI_ERROR_BIT | 2ULL)
#define EFI_UNSUPPORTED           (EFI_ERROR_BIT | 3ULL)
#define EFI_BAD_BUFFER_SIZE       (EFI_ERROR_BIT | 4ULL)
#define EFI_BUFFER_TOO_SMALL      (EFI_ERROR_BIT | 5ULL)
#define EFI_NOT_READY             (EFI_ERROR_BIT | 6ULL)
#define EFI_DEVICE_ERROR          (EFI_ERROR_BIT | 7ULL)
#define EFI_WRITE_PROTECTED       (EFI_ERROR_BIT | 8ULL)
#define EFI_OUT_OF_RESOURCES      (EFI_ERROR_BIT | 9ULL)
#define EFI_NOT_FOUND             (EFI_ERROR_BIT | 14ULL)
#define EFI_ABORTED               (EFI_ERROR_BIT | 21ULL)

#define EFI_ERROR(s)  ((s) & EFI_ERROR_BIT)

/* =========================================================================
 * GUID
 * ========================================================================= */

typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} EFI_GUID;

#define EFI_GUID_INIT(d1, d2, d3, b0,b1,b2,b3,b4,b5,b6,b7) \
    { (d1), (d2), (d3), { (b0),(b1),(b2),(b3),(b4),(b5),(b6),(b7) } }

/* Protocol GUIDs we use */
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    EFI_GUID_INIT(0x964e5b21,0x6459,0x11d2, 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b)

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    EFI_GUID_INIT(0x9042a9de,0x23dc,0x4a38, 0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a)

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    EFI_GUID_INIT(0x5b1b31a1,0x9562,0x11d2, 0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b)

/* ACPI table GUIDs (UEFI spec §4.6) — for RSDP discovery */
#define EFI_ACPI_20_TABLE_GUID \
    EFI_GUID_INIT(0x8868e871,0xe4f1,0x11d3, 0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81)
#define EFI_ACPI_TABLE_GUID \
    EFI_GUID_INIT(0xeb9d2d30,0x2d88,0x11d3, 0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d)

/* EFI_CONFIGURATION_TABLE — one entry in the system table vendor table array */
typedef struct {
    EFI_GUID  vendor_guid;
    void     *vendor_table;
} EFI_CONFIGURATION_TABLE;

/* =========================================================================
 * Memory types and map
 * ========================================================================= */

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

#define EFI_MEMORY_UC   0x0000000000000001ULL
#define EFI_MEMORY_WC   0x0000000000000002ULL
#define EFI_MEMORY_WT   0x0000000000000004ULL
#define EFI_MEMORY_WB   0x0000000000000008ULL
#define EFI_MEMORY_UCE  0x0000000000000010ULL
#define EFI_MEMORY_WP   0x0000000000001000ULL
#define EFI_MEMORY_RP   0x0000000000002000ULL
#define EFI_MEMORY_XP   0x0000000000004000ULL
#define EFI_MEMORY_RO   0x0000000000020000ULL
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

typedef struct {
    uint32_t              type;
    uint32_t              _pad;
    EFI_PHYSICAL_ADDRESS  physical_start;
    EFI_VIRTUAL_ADDRESS   virtual_start;
    uint64_t              number_of_pages;
    uint64_t              attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* =========================================================================
 * Simple Text Output Protocol (minimal — for printing)
 * ========================================================================= */

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this_proto,
    CHAR16                          *string);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this_proto);

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void                 *reset;
    EFI_TEXT_STRING       output_string;
    void                 *test_string;
    void                 *query_mode;
    void                 *set_mode;
    void                 *set_attribute;
    EFI_TEXT_CLEAR_SCREEN clear_screen;
    void                 *set_cursor_position;
    void                 *enable_cursor;
    void                 *mode;
};

/* =========================================================================
 * Block IO Protocol
 * ========================================================================= */

#define EFI_BLOCK_IO_PROTOCOL_REVISION  0x00010000ULL

typedef struct {
    uint32_t  media_id;
    BOOLEAN   removable_media;
    BOOLEAN   media_present;
    BOOLEAN   logical_partition;
    BOOLEAN   read_only;
    BOOLEAN   write_caching;
    uint8_t   _pad[3];
    uint32_t  block_size;
    uint32_t  io_align;
    EFI_LBA   last_block;
    EFI_LBA   lowest_aligned_lba;
    uint32_t  logical_blocks_per_physical_block;
    uint32_t  optimal_transfer_length_granularity;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_RESET)(
    EFI_BLOCK_IO_PROTOCOL *this_proto,
    BOOLEAN                extended_verification);

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_READ)(
    EFI_BLOCK_IO_PROTOCOL *this_proto,
    uint32_t               media_id,
    EFI_LBA                lba,
    UINTN                  buffer_size,
    void                  *buffer);

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_WRITE)(
    EFI_BLOCK_IO_PROTOCOL *this_proto,
    uint32_t               media_id,
    EFI_LBA                lba,
    UINTN                  buffer_size,
    void                  *buffer);

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_FLUSH)(
    EFI_BLOCK_IO_PROTOCOL *this_proto);

struct EFI_BLOCK_IO_PROTOCOL {
    uint64_t             revision;
    EFI_BLOCK_IO_MEDIA  *media;
    EFI_BLOCK_RESET      reset;
    EFI_BLOCK_READ       read_blocks;
    EFI_BLOCK_WRITE      write_blocks;
    EFI_BLOCK_FLUSH      flush_blocks;
};

/* =========================================================================
 * Graphics Output Protocol
 * ========================================================================= */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t  red_mask;
    uint32_t  green_mask;
    uint32_t  blue_mask;
    uint32_t  reserved_mask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t                   version;
    uint32_t                   horizontal_resolution;
    uint32_t                   vertical_resolution;
    EFI_GRAPHICS_PIXEL_FORMAT  pixel_format;
    EFI_PIXEL_BITMASK          pixel_info;
    uint32_t                   pixels_per_scan_line;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                            max_mode;
    uint32_t                            mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINTN                               size_of_info;
    EFI_PHYSICAL_ADDRESS                frame_buffer_base;
    UINTN                               frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef void *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT;  /* unused */

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL            *this_proto,
    uint32_t                                 mode_number,
    UINTN                                   *size_of_info,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION   **info);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *this_proto,
    uint32_t                      mode_number);

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE  query_mode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE    set_mode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT         blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *mode;
};

/* =========================================================================
 * Loaded Image Protocol
 * ========================================================================= */

typedef struct {
    uint32_t       revision;
    EFI_HANDLE     parent_handle;
    void          *system_table;
    EFI_HANDLE     device_handle;
    void          *file_path;
    void          *reserved;
    uint32_t       load_options_size;
    void          *load_options;
    void          *image_base;
    uint64_t       image_size;
    EFI_MEMORY_TYPE image_code_type;
    EFI_MEMORY_TYPE image_data_type;
    void          *unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* =========================================================================
 * Boot Services
 * ========================================================================= */

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE     type,
    EFI_MEMORY_TYPE       memory_type,
    UINTN                 pages,
    EFI_PHYSICAL_ADDRESS *memory);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS memory,
    UINTN                pages);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN                 *memory_map_size,
    EFI_MEMORY_DESCRIPTOR *memory_map,
    UINTN                 *map_key,
    UINTN                 *descriptor_size,
    uint32_t              *descriptor_version);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE  pool_type,
    UINTN            size,
    void           **buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    void *buffer);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE image_handle,
    UINTN      map_key);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *protocol,
    void     *registration,
    void    **interface);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE  handle,
    EFI_GUID   *protocol,
    void      **interface);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    uint32_t      search_type,
    EFI_GUID     *protocol,
    void         *search_key,
    UINTN        *no_handles,
    EFI_HANDLE  **buffer);

typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL)(
    EFI_HANDLE  handle,
    EFI_GUID   *protocol,
    void      **interface,
    EFI_HANDLE  agent_handle,
    EFI_HANDLE  controller_handle,
    uint32_t    attributes);

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL  0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL        0x00000002
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL       0x00000004

#define EFI_BY_PROTOCOL  2

typedef struct {
    uint64_t          signature;
    uint32_t          revision;
    uint32_t          header_size;
    uint32_t          crc32;
    uint32_t          reserved;
} EFI_TABLE_HEADER;

typedef struct {
    EFI_TABLE_HEADER          hdr;
    /* raise/restore TPL */
    void                     *raise_tpl;
    void                     *restore_tpl;
    /* memory */
    EFI_ALLOCATE_PAGES        allocate_pages;
    EFI_FREE_PAGES            free_pages;
    EFI_GET_MEMORY_MAP        get_memory_map;
    EFI_ALLOCATE_POOL         allocate_pool;
    EFI_FREE_POOL             free_pool;
    /* events */
    void                     *create_event;
    void                     *set_timer;
    void                     *wait_for_event;
    void                     *signal_event;
    void                     *close_event;
    void                     *check_event;
    /* protocols */
    void                     *install_protocol_interface;
    void                     *reinstall_protocol_interface;
    void                     *uninstall_protocol_interface;
    EFI_HANDLE_PROTOCOL       handle_protocol;
    void                     *reserved;
    void                     *register_protocol_notify;
    void                     *locate_handle;
    void                     *locate_device_path;
    void                     *install_configuration_table;
    /* image */
    void                     *load_image;
    void                     *start_image;
    void                     *exit;
    void                     *unload_image;
    EFI_EXIT_BOOT_SERVICES    exit_boot_services;
    /* misc */
    void                     *get_next_monotonic_count;
    void                     *stall;
    void                     *set_watchdog_timer;
    /* driver */
    void                     *connect_controller;
    void                     *disconnect_controller;
    /* protocols (extended) */
    EFI_OPEN_PROTOCOL         open_protocol;
    void                     *close_protocol;
    void                     *open_protocol_information;
    void                     *protocols_per_handle;
    EFI_LOCATE_HANDLE_BUFFER  locate_handle_buffer;
    EFI_LOCATE_PROTOCOL       locate_protocol;
    void                     *install_multiple_protocol_interfaces;
    void                     *uninstall_multiple_protocol_interfaces;
    /* crc */
    void                     *calculate_crc32;
    /* misc */
    void                     *copy_mem;
    void                     *set_mem;
    void                     *create_event_ex;
} EFI_BOOT_SERVICES;

/* =========================================================================
 * Runtime Services (minimal — only what we need)
 * ========================================================================= */

typedef struct {
    EFI_TABLE_HEADER hdr;
    /* lots of fields we don't use */
    uint8_t          _pad[256];
} EFI_RUNTIME_SERVICES;

/* =========================================================================
 * System Table
 * ========================================================================= */

typedef struct {
    EFI_TABLE_HEADER                  hdr;
    CHAR16                           *firmware_vendor;
    uint32_t                          firmware_revision;
    uint32_t                          _pad0;
    EFI_HANDLE                        console_in_handle;
    void                             *con_in;
    EFI_HANDLE                        console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *con_out;
    EFI_HANDLE                        std_err_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *std_err;
    EFI_RUNTIME_SERVICES             *runtime_services;
    EFI_BOOT_SERVICES                *boot_services;
    UINTN                             number_of_table_entries;
    EFI_CONFIGURATION_TABLE          *configuration_table;
} EFI_SYSTEM_TABLE;

#endif /* UEFI_H */
