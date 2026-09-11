
#include "uefi.h"
#include "tagfs_boot.h"


static __attribute__((noinline)) void PhysWrite16(uint64_t addr, uint16_t val)
{
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)addr;
    *p = val;
}

#define BOOT_INFO_MAGIC       0x42583031U
#define BOOT_INFO_VERSION_V4  4U

#define EFI_CFG_TABLE_MAX_ENTRIES  256U

#define KERNEL_LOAD_ADDR      0x100000ULL

#define KERNEL_HEADER_VERSION 1U
#define KERNEL_MAX_SIZE       0x2000000ULL
#define BOOT_INFO_ADDR        0xA000ULL

#ifdef BOOT_INFO_ADDR_FROM_BUILD
_Static_assert(BOOT_INFO_ADDR == BOOT_INFO_ADDR_FROM_BUILD,
               "TagBoot writes boot_info somewhere the build does not expect");
#endif
#define E820_COUNT_ADDR       0x500ULL
#define E820_SIZE_ADDR        0x502ULL
#define E820_MAP_ADDR         0x504ULL
#define E820_MAX_ENTRIES      128U

#define PAGE_TABLE_SIZE       0x8000ULL
#define GUARD_PAGE_SIZE       0x1000ULL
#define BOOT_STACK_SIZE       0x10000ULL

#define PAGE_2MB              0x200000ULL
#define PAGE_4KB              0x1000ULL

#define PTE_PRESENT           (1ULL << 0)
#define PTE_WRITABLE          (1ULL << 1)
#define PTE_PAGE_SIZE         (1ULL << 7)

#define MSR_EFER              0xC0000080U
#define EFER_LME              (1U << 8)
#define EFER_NXE              (1U << 11)

#define CR4_PAE               (1U << 5)
#define CR4_PGE               (1U << 7)


#define E820_USABLE    1U
#define E820_RESERVED  2U
#define E820_ACPI_RECL 3U
#define E820_ACPI_NVS  4U


typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t e820_map_addr;
    uint16_t e820_count;
    uint16_t reserved1;
    uint32_t kernel_start;
    uint32_t kernel_end;
    uint8_t  boot_drive;
    uint8_t  reserved2;
    uint16_t reserved3;
    uint32_t page_table_base;
    uint32_t stack_base;
    uint32_t total_size;
    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_stride;
    uint32_t fb_format;
    uint8_t  boot_method;
    uint8_t  reserved_v2[3];
    uint64_t rsdp_addr;
    uint64_t efi_rt_services_phys;
    uint64_t efi_mmap_phys;
    uint32_t efi_mmap_size;
    uint32_t efi_mmap_desc_size;
    uint32_t efi_mmap_desc_ver;
    uint32_t efi_fw_revision;
    uint64_t efi_system_table_phys;
    uint64_t efi_cfg_table_phys;
    uint32_t efi_cfg_table_count;
    uint64_t efi_esrt_copy_phys;
    uint32_t efi_esrt_copy_size;
} BootInfoV4;

_Static_assert(sizeof(BootInfoV4) == 140, "BootInfoV4 must be 140 bytes");


typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} E820Entry;

_Static_assert(sizeof(E820Entry) == 24, "E820Entry must be 24 bytes");


static EFI_SYSTEM_TABLE  *g_st   = NULL;
static EFI_BOOT_SERVICES *g_bs   = NULL;
static EFI_HANDLE         g_image_handle = NULL;


static void MemZero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = 0;
}

static void MemCopy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static int MemEqual(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p++ != *q++) return 0;
    }
    return 1;
}

static int GuidEqual(const EFI_GUID *a, const EFI_GUID *b)
{
    return MemEqual(a, b, sizeof(EFI_GUID));
}

static size_t StrLen8(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
static int StrEqual8(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
#pragma GCC diagnostic pop


static void Com1Init(void)
{
    uint16_t p;
    uint8_t  v;
    p = 0x3F9; v = 0x00; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
    p = 0x3FB; v = 0x80; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
    p = 0x3F8; v = 0x01; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
    p = 0x3F9; v = 0x00; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
    p = 0x3FB; v = 0x03; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
    p = 0x3FC; v = 0x03; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}

static void Com1Byte(char c)
{
    uint8_t  lsr;
    uint16_t lsr_port = 0x3FD;
    uint16_t dat_port = 0x3F8;
    do {
        __asm__ volatile("inb %1,%0":"=a"(lsr):"Nd"(lsr_port));
    } while (!(lsr & 0x20));
    uint8_t b = (uint8_t)c;
    __asm__ volatile("outb %0,%1"::"a"(b),"Nd"(dat_port));
}

static void Com1Str(const char *s)
{
    for (; *s; s++) Com1Byte(*s);
}


static void Print(const char *msg)
{
    if (!g_st || !g_st->con_out || !msg) return;

    CHAR16 buf[129];
    size_t pos = 0;
    for (size_t i = 0; msg[i]; i++) {
        buf[pos++] = (CHAR16)(unsigned char)msg[i];
        if (pos == 128) {
            buf[128] = 0;
            g_st->con_out->output_string(g_st->con_out, buf);
            pos = 0;
        }
    }
    if (pos > 0) {
        buf[pos] = 0;
        g_st->con_out->output_string(g_st->con_out, buf);
    }
}

static void PrintHex64(uint64_t v)
{
    char buf[19];
    buf[0]  = '0'; buf[1] = 'x';
    for (int i = 15; i >= 2; i--) {
        uint8_t nibble = (uint8_t)(v & 0xF);
        buf[i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        v >>= 4;
    }
    buf[18] = '\0';
    Print(buf);
}

static void PrintDec(uint64_t v)
{
    if (v == 0) { Print("0"); return; }
    char buf[21];
    int  pos = 20;
    buf[pos]  = '\0';
    while (v && pos > 0) {
        buf[--pos] = (char)('0' + v % 10);
        v /= 10;
    }
    Print(buf + pos);
}

static void Panic(const char *msg)
{
    Print("\r\n[TAGBOOT FATAL] ");
    Print(msg);
    Print("\r\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}


static uint64_t FindAcpiRsdp(void)
{
    if (!g_st || g_st->number_of_table_entries == 0) return 0;

    EFI_CONFIGURATION_TABLE *ct = g_st->configuration_table;
    if (!ct) return 0;

    UINTN n = g_st->number_of_table_entries;
    if (n > EFI_CFG_TABLE_MAX_ENTRIES) n = EFI_CFG_TABLE_MAX_ENTRIES;

    EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;

    uint64_t rsdp_v2 = 0;
    uint64_t rsdp_v1 = 0;

    for (UINTN i = 0; i < n; i++) {
        if (GuidEqual(&ct[i].vendor_guid, &acpi20_guid))
            rsdp_v2 = (uint64_t)(uintptr_t)ct[i].vendor_table;
        else if (GuidEqual(&ct[i].vendor_guid, &acpi10_guid))
            rsdp_v1 = (uint64_t)(uintptr_t)ct[i].vendor_table;
    }

    return rsdp_v2 ? rsdp_v2 : rsdp_v1;
}


static EFI_BLOCK_IO_PROTOCOL *g_block_io = NULL;


static uint64_t g_ground_start   = 0;
static uint64_t g_ground_sectors = 0;

static VolumeLayout g_layout;
static VolumeBoot   g_boot;
static uint8_t      g_volume_uuid[16];
static int          g_have_boot = 0;

#define MBR_TABLE_OFFSET     446U
#define MBR_ENTRY_BYTES      16U
#define MBR_ENTRY_COUNT      4U
#define MBR_ENTRY_TYPE       4U
#define MBR_ENTRY_START_LBA  8U
#define MBR_ENTRY_SECTORS    12U
#define MBR_TYPE_BOXOS       0x7FU
#define MBR_TYPE_PROTECTIVE  0xEEU

#define GPT_HEADER_LBA          1U
#define GPT_HEADER_SIZE_OFFSET  0x0CU
#define GPT_HEADER_CRC_OFFSET   0x10U
#define GPT_ENTRY_LBA_OFFSET    0x48U
#define GPT_ENTRY_COUNT_OFFSET  0x50U
#define GPT_ENTRY_BYTES_OFFSET  0x54U
#define GPT_ENTRY_CRC_OFFSET    0x58U
#define GPT_ENTRY_TYPE_OFFSET   0U
#define GPT_ENTRY_FIRST_OFFSET  0x20U
#define GPT_ENTRY_LAST_OFFSET   0x28U
#define GPT_HEADER_MIN_BYTES    92U
#define GPT_ENTRY_MIN_BYTES     128U
#define GPT_ARRAY_MAX_BYTES     (64U * 1024U)

static const uint8_t g_boxos_type_guid[16] = {
    0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
    0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb
};

static const uint8_t g_deed_magic[8] = {
    VOLUME_DEED_MAGIC_0, VOLUME_DEED_MAGIC_1, VOLUME_DEED_MAGIC_2,
    VOLUME_DEED_MAGIC_3, VOLUME_DEED_MAGIC_4, VOLUME_DEED_MAGIC_5,
    VOLUME_DEED_MAGIC_6, VOLUME_DEED_MAGIC_7
};

static uint32_t Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t Le64(const uint8_t *p)
{
    return (uint64_t)Le32(p) | ((uint64_t)Le32(p + 4) << 32);
}

static EFI_STATUS ReadMediumSectors(EFI_BLOCK_IO_PROTOCOL *bio,
                                    uint64_t lba_512, uint32_t count_512,
                                    void *buffer)
{
    if (!bio || !bio->media) return EFI_NOT_READY;
    uint32_t bsz = bio->media->block_size;
    if (bsz == 0) bsz = 512;

    if (bsz == 512) {
        return bio->read_blocks(bio, bio->media->media_id, (EFI_LBA)lba_512,
                                (UINTN)count_512 * 512, buffer);
    }

    static uint8_t align_buf[8192];
    if (bsz > sizeof(align_buf)) return EFI_UNSUPPORTED;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t byte_off = lba_512 * 512ULL;
    uint32_t left = count_512 * 512;

    while (left > 0) {
        uint64_t phys = byte_off / bsz;
        uint32_t off  = (uint32_t)(byte_off % bsz);
        uint32_t can  = bsz - off;
        if (can > left) can = left;

        EFI_STATUS st = bio->read_blocks(bio, bio->media->media_id,
                                         (EFI_LBA)phys, bsz, align_buf);
        if (EFI_ERROR(st)) return st;
        MemCopy(out, align_buf + off, can);
        out += can; byte_off += can; left -= can;
    }
    return EFI_SUCCESS;
}

static int DeedAt(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint32_t want_role,
                  uint8_t *out )
{
    if (EFI_ERROR(ReadMediumSectors(bio, lba, 8, out))) return 0;

    VolumeDeed deed;
    MemCopy(&deed, out, sizeof(deed));

    for (int i = 0; i < 8; i++)
        if (deed.magic[i] != g_deed_magic[i]) return 0;

    if (deed.prologue_bytes < sizeof(VolumeDeed) || deed.prologue_bytes > 4096)
        return 0;
    if (deed.stamp_bytes > 4096 - deed.prologue_bytes)
        return 0;

    uint32_t summed = (uint32_t)deed.prologue_bytes + deed.stamp_bytes;
    static uint8_t probe[4096];
    MemCopy(probe, out, summed);
    MemZero(probe + __builtin_offsetof(VolumeDeed, crc32), 4);
    if (Crc32(probe, summed) != deed.crc32) {
        Print("TagBoot: a deed here does not match its own checksum\r\n");
        return 0;
    }

    if (deed.role != want_role) return 0;
    return 1;
}

static const void *DeedStampOf(const uint8_t *raw, uint16_t kind, uint16_t *out_bytes)
{
    VolumeDeed deed;
    MemCopy(&deed, raw, sizeof(deed));

    uint32_t left = deed.stamp_bytes;
    const uint8_t *p = raw + deed.prologue_bytes;

    while (left >= sizeof(VolumeStamp)) {
        VolumeStamp st;
        MemCopy(&st, p, sizeof(st));
        if (st.bytes > left - sizeof(VolumeStamp)) return NULL;

        if (st.kind == kind) {
            if (out_bytes) *out_bytes = st.bytes;
            return p + sizeof(VolumeStamp);
        }
        uint32_t step = (sizeof(VolumeStamp) + st.bytes + 3U) & ~3U;
        if (step > left) return NULL;
        p += step; left -= step;
    }
    return NULL;
}

static int FindGroundOn(EFI_BLOCK_IO_PROTOCOL *bio, uint8_t *deed_out,
                        uint64_t *out_start, uint64_t *out_sectors)
{
    static uint8_t sector0[512];
    if (EFI_ERROR(ReadMediumSectors(bio, 0, 1, sector0))) return 0;
    if (sector0[510] != 0x55 || sector0[511] != 0xAA)     return 0;

    int protective = 0;
    for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
        if (sector0[MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES + MBR_ENTRY_TYPE] ==
            MBR_TYPE_PROTECTIVE) { protective = 1; break; }
    }

    if (!protective) {
        for (uint32_t i = 0; i < MBR_ENTRY_COUNT; i++) {
            const uint8_t *e = sector0 + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
            if (e[MBR_ENTRY_TYPE] != MBR_TYPE_BOXOS) continue;

            uint64_t start = Le32(e + MBR_ENTRY_START_LBA);
            uint64_t count = Le32(e + MBR_ENTRY_SECTORS);
            if (count < 8) continue;

            if (DeedAt(bio, start, VOLUME_DEED_ROLE_HEAD, deed_out)) {
                *out_start = start; *out_sectors = count; return 1;
            }
            uint64_t tail = start + ((count / 8) - 1) * 8;
            if (DeedAt(bio, tail, VOLUME_DEED_ROLE_TAIL, deed_out)) {
                Print("TagBoot: read the deed from the far end of the volume\r\n");
                *out_start = start; *out_sectors = count; return 1;
            }
        }
        return 0;
    }

    static uint8_t header[512];
    if (EFI_ERROR(ReadMediumSectors(bio, GPT_HEADER_LBA, 1, header))) return 0;

    const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++)
        if (header[i] != (uint8_t)sig[i]) return 0;

    uint32_t header_bytes = Le32(header + GPT_HEADER_SIZE_OFFSET);
    if (header_bytes < GPT_HEADER_MIN_BYTES || header_bytes > 512) return 0;

    static uint8_t hprobe[512];
    MemCopy(hprobe, header, header_bytes);
    MemZero(hprobe + GPT_HEADER_CRC_OFFSET, 4);
    if (Crc32(hprobe, header_bytes) != Le32(header + GPT_HEADER_CRC_OFFSET)) {
        Print("TagBoot: this disk's GPT header fails its own checksum\r\n");
        return 0;
    }

    uint64_t entry_lba   = Le64(header + GPT_ENTRY_LBA_OFFSET);
    uint32_t entry_count = Le32(header + GPT_ENTRY_COUNT_OFFSET);
    uint32_t entry_bytes = Le32(header + GPT_ENTRY_BYTES_OFFSET);
    if (entry_bytes < GPT_ENTRY_MIN_BYTES || entry_count == 0 ||
        entry_bytes > GPT_ARRAY_MAX_BYTES ||
        entry_count > GPT_ARRAY_MAX_BYTES / entry_bytes) return 0;

    uint32_t array_bytes = entry_count * entry_bytes;
    static uint8_t array[GPT_ARRAY_MAX_BYTES];
    uint32_t array_secs = (array_bytes + 511) / 512;
    if (EFI_ERROR(ReadMediumSectors(bio, entry_lba, array_secs, array))) return 0;

    if (Crc32(array, array_bytes) != Le32(header + GPT_ENTRY_CRC_OFFSET)) {
        Print("TagBoot: this disk's GPT entries fail their own checksum\r\n");
        return 0;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = array + (uint64_t)i * entry_bytes;
        int ours = 1;
        for (int b = 0; b < 16; b++)
            if (e[GPT_ENTRY_TYPE_OFFSET + b] != g_boxos_type_guid[b]) { ours = 0; break; }
        if (!ours) continue;

        uint64_t first = Le64(e + GPT_ENTRY_FIRST_OFFSET);
        uint64_t last  = Le64(e + GPT_ENTRY_LAST_OFFSET);
        if (last < first || (last - first + 1) < 8) continue;
        uint64_t count = last - first + 1;

        if (DeedAt(bio, first, VOLUME_DEED_ROLE_HEAD, deed_out)) {
            *out_start = first; *out_sectors = count; return 1;
        }
        uint64_t tail = first + ((count / 8) - 1) * 8;
        if (DeedAt(bio, tail, VOLUME_DEED_ROLE_TAIL, deed_out)) {
            Print("TagBoot: read the deed from the far end of the volume\r\n");
            *out_start = first; *out_sectors = count; return 1;
        }
    }
    return 0;
}

static uint8_t g_deed_block[4096];

static int BlockIoProbeVolume(EFI_BLOCK_IO_PROTOCOL *bio)
{
    if (!bio || !bio->media) return 0;
    if (bio->media->block_size == 0 || !bio->media->media_present) return 0;

    uint64_t start = 0, sectors = 0;
    if (!FindGroundOn(bio, g_deed_block, &start, &sectors)) return 0;

    g_ground_start   = start;
    g_ground_sectors = sectors;
    return 1;
}

static EFI_STATUS BlockIoFindDevice(EFI_HANDLE image_handle)
{
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID block_io_guid     = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS status;

    Print("TagBoot: scanning for TagFS block device...\r\n");

    status = g_bs->handle_protocol(image_handle,
                                   &loaded_image_guid,
                                   (void **)&loaded_image);
    if (!EFI_ERROR(status) && loaded_image) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        status = g_bs->handle_protocol(loaded_image->device_handle,
                                       &block_io_guid,
                                       (void **)&bio);
        if (!EFI_ERROR(status) && BlockIoProbeVolume(bio)) {
            Print("TagBoot: the volume is on the device this loader came from\r\n");
            g_block_io = bio;
            return EFI_SUCCESS;
        }
    }

    Print("TagBoot: loaded-image device has no TagFS, scanning all handles...\r\n");

    UINTN       handle_count = 0;
    EFI_HANDLE *handles      = NULL;
    status = g_bs->locate_handle_buffer(EFI_BY_PROTOCOL,
                                        &block_io_guid,
                                        NULL,
                                        &handle_count,
                                        &handles);
    if (EFI_ERROR(status)) return status;

    EFI_STATUS found = EFI_NOT_FOUND;

    for (UINTN i = 0; i < handle_count; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        if (EFI_ERROR(g_bs->handle_protocol(handles[i], &block_io_guid, (void **)&bio)))
            continue;
        if (!bio || !bio->media)              continue;
        if (bio->media->logical_partition)    continue;

        if (!bio->media->media_present) {
            if (bio->reset) {
                (void)bio->reset(bio, FALSE);
            }
            if (!bio->media->media_present) continue;
        }

        if (BlockIoProbeVolume(bio)) {
            g_block_io = bio;
            found = EFI_SUCCESS;
            break;
        }
    }

    g_bs->free_pool(handles);

    if (EFI_ERROR(found))
        Print("TagBoot: no medium here carries a BoxOS volume with a readable deed\r\n");

    return found;
}

static EFI_STATUS ReadSectors(uint64_t vlba, uint32_t count_512, void *buffer)
{
    if (!g_block_io) return EFI_NOT_READY;
    return ReadMediumSectors(g_block_io, g_ground_start + vlba, count_512, buffer);
}

static EFI_STATUS ReadDataBlock(uint32_t block, void *buf)
{
    uint64_t vlba = ((uint64_t)g_layout.data_block + block) * TAGFS_SECTORS_PER_BLOCK;
    return ReadSectors(vlba, TAGFS_SECTORS_PER_BLOCK, buf);
}


static EFI_STATUS DeedTakeUp(void)
{
    VolumeDeed deed;
    MemCopy(&deed, g_deed_block, sizeof(deed));
    MemCopy(g_volume_uuid, deed.uuid, 16);

    uint16_t bytes = 0;
    const VolumeLayout *layout =
        (const VolumeLayout *)DeedStampOf(g_deed_block, VOLUME_STAMP_LAYOUT, &bytes);
    if (!layout || bytes < sizeof(VolumeLayout)) {
        Print("TagBoot: this deed does not say where anything is\r\n");
        return EFI_UNSUPPORTED;
    }
    MemCopy(&g_layout, layout, sizeof(g_layout));

    uint64_t blocks_in_volume = deed.sectors / TAGFS_SECTORS_PER_BLOCK;
    if (g_layout.data_block == 0 || g_layout.data_blocks == 0 ||
        (uint64_t)g_layout.data_block + g_layout.data_blocks > blocks_in_volume) {
        Print("TagBoot: this deed lays out a data run that does not fit inside it\r\n");
        return EFI_UNSUPPORTED;
    }

    const VolumeBoot *boot =
        (const VolumeBoot *)DeedStampOf(g_deed_block, VOLUME_STAMP_BOOT, &bytes);
    if (boot && bytes >= sizeof(VolumeBoot)) {
        MemCopy(&g_boot, boot, sizeof(g_boot));
        g_have_boot = (g_boot.kernel_block != 0 && g_boot.kernel_blocks != 0);
    }
    return EFI_SUCCESS;
}


static uint16_t TagRegistryFindLabel(const char *key)
{
    uint32_t block_idx = g_layout.tag_registry_block - g_layout.data_block;
    size_t   key_len    = StrLen8(key);

    static TagBootRegistryBlock reg_blk;

    for (uint32_t chain = 0; chain < 64; chain++) {
        if (EFI_ERROR(ReadDataBlock(block_idx, &reg_blk))) break;
        if (reg_blk.magic != TAGFS_REGISTRY_MAGIC)                      break;

        uint32_t pos = 0;
        for (uint16_t i = 0; i < reg_blk.entry_count && pos < reg_blk.used_bytes; i++) {
            if (pos + 6 > reg_blk.used_bytes) break;

            uint8_t  *p         = reg_blk.data + pos;
            uint16_t  tag_id;
            uint8_t   flags;
            uint8_t   this_key_len;
            uint16_t  this_val_len;

            MemCopy(&tag_id,        p + TAG_RECORD_ID_OFF,     2);
            flags        = p[TAG_RECORD_FLAGS_OFF];
            this_key_len = p[TAG_RECORD_KEYLEN_OFF];
            MemCopy(&this_val_len,  p + TAG_RECORD_VALLEN_OFF, 2);

            uint32_t record_size = 6 + this_key_len + this_val_len;
            if (pos + record_size > reg_blk.used_bytes) break;

            if ((flags & TAGFS_TAG_FLAG_HAS_VALUE) == 0 && this_val_len == 0 &&
                this_key_len == (uint8_t)key_len &&
                MemEqual(p + TAG_RECORD_KEY_OFF, key, key_len)) {
                return tag_id;
            }

            pos += record_size;
        }

        if (reg_blk.next_block == 0) break;
        block_idx = reg_blk.next_block;
    }

    return TAGFS_INVALID_TAG_ID;
}

static int TagFsFindFileByTagId(uint16_t target_tag_id,
                                uint32_t *out_start_block,
                                uint32_t *out_block_count,
                                uint64_t *out_file_size)
{
    uint32_t block_idx = g_layout.metadata_pool_block - g_layout.data_block;

    static TagBootMetaPoolBlock mpool;

    for (uint32_t chain = 0; chain < 256; chain++) {
        if (EFI_ERROR(ReadDataBlock(block_idx, &mpool))) break;
        if (mpool.magic != TAGFS_MPOOL_MAGIC)                         break;

        uint32_t pos = 0;
        for (uint16_t rec = 0; rec < mpool.record_count && pos < mpool.used_bytes; rec++) {
            if (pos + META_RECORD_VARDATA_OFF > mpool.used_bytes) break;

            uint8_t *r = mpool.payload + pos;

            uint16_t record_len, tag_count, extent_count, name_len;
            uint32_t flags;

            MemCopy(&record_len,   r + META_RECORD_LEN_OFF,     2);
            MemCopy(&flags,        r + META_RECORD_FLAGS_OFF,    4);
            MemCopy(&tag_count,    r + META_RECORD_TAGCOUNT_OFF, 2);
            MemCopy(&extent_count, r + META_RECORD_EXTCOUNT_OFF, 2);
            MemCopy(&name_len,     r + META_RECORD_NAMELEN_OFF,  2);

            if (record_len == 0 || pos + record_len > mpool.used_bytes) break;

            if (!(flags & TAGFS_FILE_ACTIVE) || (flags & TAGFS_FILE_TRASHED)) {
                pos += record_len;
                continue;
            }

            uint8_t *tag_ids_ptr = r + META_RECORD_VARDATA_OFF;

            for (uint16_t t = 0; t < tag_count; t++) {
                uint16_t tid;
                MemCopy(&tid, tag_ids_ptr + t * 2, 2);
                if (tid == target_tag_id) {
                    uint8_t *extent_ptr = tag_ids_ptr + tag_count * 2;
                    if (extent_count == 0) break;

                    TagBootFileExtent ext;
                    MemCopy(&ext, extent_ptr, sizeof(ext));
                    *out_start_block  = ext.start_block;
                    *out_block_count  = ext.block_count;

                    uint64_t file_size = 0;
                    MemCopy(&file_size, r + META_RECORD_SIZE_OFF, 8);
                    *out_file_size = file_size;
                    return 1;
                }
            }

            pos += record_len;
        }

        if (mpool.next_block == 0) break;
        block_idx = mpool.next_block;
    }

    return 0;
}

static int TagFsFindKernelByTags(uint32_t *out_start_block, uint32_t *out_block_count,
                                  uint64_t *out_file_size)
{
    static const char *const priority_tags[] = { "god", "boot", "system", NULL };

    for (int i = 0; priority_tags[i] != NULL; i++) {
        uint16_t tid = TagRegistryFindLabel(priority_tags[i]);
        if (tid == TAGFS_INVALID_TAG_ID) continue;

        Print("TagBoot: searching for tag '");
        Print(priority_tags[i]);
        Print("' (id=");
        PrintDec(tid);
        Print(")\r\n");

        if (TagFsFindFileByTagId(tid, out_start_block, out_block_count, out_file_size)) {
            Print("TagBoot: kernel found via tag '");
            Print(priority_tags[i]);
            Print("'\r\n");
            return 1;
        }
    }
    return 0;
}

static int TagFsBootStamp(uint32_t *out_volume_block, uint32_t *out_block_count,
                          uint64_t *out_file_size)
{
    if (!g_have_boot) return 0;
    *out_volume_block = g_boot.kernel_block;
    *out_block_count  = g_boot.kernel_blocks;
    *out_file_size    = (uint64_t)g_boot.kernel_bytes;
    return 1;
}


static uint64_t TagFsLoadKernel(uint32_t volume_block,
                                uint32_t block_count,
                                uint64_t file_size)
{
    EFI_PHYSICAL_ADDRESS load_addr = KERNEL_LOAD_ADDR;
    uint64_t total_bytes = (uint64_t)block_count * TAGFS_BLOCK_SIZE;

    if (total_bytes > KERNEL_MAX_SIZE) {
        Print("TagBoot: kernel too large (");
        PrintDec(total_bytes);
        Print(" bytes)\r\n");
        return 0;
    }

    if (file_size == 0 || file_size > total_bytes)
        file_size = total_bytes;

    UINTN pages = (UINTN)((total_bytes + PAGE_4KB - 1) / PAGE_4KB);
    EFI_STATUS status = g_bs->allocate_pages(AllocateAddress,
                                             EfiLoaderData,
                                             pages,
                                             &load_addr);
    if (EFI_ERROR(status)) {
        g_bs->free_pages(KERNEL_LOAD_ADDR, pages);
        status = g_bs->allocate_pages(AllocateAddress,
                                      EfiLoaderData,
                                      pages,
                                      &load_addr);
        if (EFI_ERROR(status)) {
            Print("TagBoot: cannot allocate ");
            PrintDec(pages);
            Print(" pages at 0x100000\r\n");
            return 0;
        }
    }

    uint8_t *dst = (uint8_t *)(uintptr_t)KERNEL_LOAD_ADDR;

    for (uint32_t b = 0; b < block_count; b++) {
        uint64_t vlba = (uint64_t)(volume_block + b) * TAGFS_SECTORS_PER_BLOCK;
        status = ReadSectors(vlba, TAGFS_SECTORS_PER_BLOCK,
                             dst + (uint64_t)b * TAGFS_BLOCK_SIZE);
        if (EFI_ERROR(status)) {
            Print("TagBoot: disk read error at volume block ");
            PrintDec(volume_block + b);
            Print("\r\n");
            return 0;
        }
    }

    return file_size;
}


typedef struct {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
} FbInfo;

#define MIN_FB_WIDTH   640U
#define MIN_FB_HEIGHT  400U
#define PREFERRED_FB_WIDTH  1920U
#define PREFERRED_FB_HEIGHT 1080U
#define MAX_FB_WIDTH   3840U
#define MAX_FB_HEIGHT  2160U

static int FbModeInBand(uint32_t w, uint32_t h)
{
    return (w >= MIN_FB_WIDTH  && w <= MAX_FB_WIDTH  &&
            h >= MIN_FB_HEIGHT && h <= MAX_FB_HEIGHT);
}

static void SelectGopBestMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    if (!gop || !gop->mode) return;

    if (gop->mode->info) {
        uint32_t w = gop->mode->info->horizontal_resolution;
        uint32_t h = gop->mode->info->vertical_resolution;
        if (gop->mode->info->pixel_format != PixelBltOnly &&
            FbModeInBand(w, h)) {
            return;
        }
    }

    uint32_t preferred_pixels = PREFERRED_FB_WIDTH * PREFERRED_FB_HEIGHT;
    uint32_t max_pixels       = MAX_FB_WIDTH * MAX_FB_HEIGHT;

    uint32_t best_mode_pref   = gop->mode->mode;
    uint32_t best_pixels_pref = 0;
    int      found_pref       = 0;

    uint32_t best_mode_max    = gop->mode->mode;
    uint32_t best_pixels_max  = 0;
    int      found_max        = 0;

    uint32_t small_mode       = gop->mode->mode;
    uint32_t small_pixels     = 0xFFFFFFFFU;

    for (uint32_t m = 0; m < gop->mode->max_mode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->query_mode(gop, m, &size_of_info, &info))) continue;
        if (!info) continue;
        if (info->pixel_format == PixelBltOnly) continue;

        uint32_t w = info->horizontal_resolution;
        uint32_t h = info->vertical_resolution;
        uint32_t pixels = w * h;

        if (pixels < small_pixels) {
            small_pixels = pixels;
            small_mode   = m;
        }

        if (w >= MIN_FB_WIDTH && h >= MIN_FB_HEIGHT &&
            pixels <= preferred_pixels && pixels > best_pixels_pref) {
            best_pixels_pref = pixels;
            best_mode_pref   = m;
            found_pref       = 1;
        }

        if (w >= MIN_FB_WIDTH && h >= MIN_FB_HEIGHT &&
            pixels <= max_pixels && pixels > best_pixels_max) {
            best_pixels_max = pixels;
            best_mode_max   = m;
            found_max       = 1;
        }
    }

    uint32_t best_mode;
    if (found_pref)      best_mode = best_mode_pref;
    else if (found_max)  best_mode = best_mode_max;
    else                 best_mode = small_mode;

    if (best_mode != gop->mode->mode) {
        Print("TagBoot: GOP switching to mode ");
        PrintDec(best_mode);
        Print(" (");
        if (!EFI_ERROR(gop->set_mode(gop, best_mode))) {
            Print("ok");
        } else {
            Print("failed, keeping current");
            best_mode = gop->mode->mode;
        }
        Print(")\r\n");
    }
}

static FbInfo QueryGopFramebuffer(void)
{
    FbInfo fb;
    MemZero(&fb, sizeof(fb));

    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

    EFI_STATUS status = g_bs->locate_protocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->mode) return fb;

    SelectGopBestMode(gop);

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->mode;
    if (!mode->info) return fb;

    fb.addr   = mode->frame_buffer_base;
    fb.width  = mode->info->horizontal_resolution;
    fb.height = mode->info->vertical_resolution;
    fb.stride = mode->info->pixels_per_scan_line * 4;

    switch (mode->info->pixel_format) {
        case PixelRedGreenBlueReserved8BitPerColor: fb.format = 0; break;
        case PixelBlueGreenRedReserved8BitPerColor: fb.format = 1; break;
        default:                                    fb.format = 2; break;
    }

    Print("TagBoot: GOP ");
    PrintDec(fb.width);
    Print("x");
    PrintDec(fb.height);
    Print(" fb=");
    PrintHex64(fb.addr);
    Print("\r\n");

    return fb;
}


static uint32_t UefiTypeToE820(uint32_t uefi_type)
{
    switch (uefi_type) {
        case EfiConventionalMemory:    return E820_USABLE;
        case EfiLoaderCode:            return E820_USABLE;
        case EfiLoaderData:            return E820_USABLE;
        case EfiBootServicesCode:      return E820_USABLE;
        case EfiBootServicesData:      return E820_USABLE;
        case EfiACPIReclaimMemory:     return E820_ACPI_RECL;
        case EfiACPIMemoryNVS:         return E820_ACPI_NVS;
        case EfiReservedMemoryType:    return E820_RESERVED;
        case EfiRuntimeServicesCode:   return E820_RESERVED;
        case EfiRuntimeServicesData:   return E820_RESERVED;
        case EfiUnusableMemory:        return E820_RESERVED;
        case EfiMemoryMappedIO:        return E820_RESERVED;
        case EfiMemoryMappedIOPortSpace: return E820_RESERVED;
        default:                       return E820_RESERVED;
    }
}

typedef struct {
    UINTN     map_size;
    UINTN     map_key;
    UINTN     desc_size;
    uint32_t  desc_version;
    uint8_t  *map_buf;
    uint32_t  e820_count;
} MemMapResult;

static int g_e820_truncated = 0;

static uint32_t EmitE820(const uint8_t *map, uint64_t map_size, uint64_t desc_size)
{
    volatile uint8_t *e820_raw = (volatile uint8_t *)(uintptr_t)E820_MAP_ADDR;
    uint32_t count = 0;

    int      have_run   = 0;
    uint64_t run_base   = 0;
    uint64_t run_length = 0;
    uint32_t run_type   = 0;

    const uint8_t *p   = map;
    const uint8_t *end = map + map_size;
    int truncated = 0;

    while (p + desc_size <= end) {
        const EFI_MEMORY_DESCRIPTOR *desc = (const EFI_MEMORY_DESCRIPTOR *)p;
        p += desc_size;

        uint64_t base   = desc->physical_start;
        uint64_t length = desc->number_of_pages * PAGE_4KB;
        uint32_t type   = UefiTypeToE820(desc->type);

        if (length == 0) continue;

        if (have_run && run_type == type && run_base + run_length == base) {
            run_length += length;
            continue;
        }

        if (have_run) {
            if (count >= E820_MAX_ENTRIES) { truncated = 1; break; }
            volatile uint8_t *slot = e820_raw + count * sizeof(E820Entry);
            uint32_t acpi = 1;
            MemCopy((void *)slot,       &run_base,   8);
            MemCopy((void *)(slot + 8), &run_length, 8);
            MemCopy((void *)(slot + 16), &run_type,  4);
            MemCopy((void *)(slot + 20), &acpi,      4);
            count++;
        }

        have_run   = 1;
        run_base   = base;
        run_length = length;
        run_type   = type;
    }

    if (have_run && !truncated && count < E820_MAX_ENTRIES) {
        volatile uint8_t *slot = e820_raw + count * sizeof(E820Entry);
        uint32_t acpi = 1;
        MemCopy((void *)slot,       &run_base,   8);
        MemCopy((void *)(slot + 8), &run_length, 8);
        MemCopy((void *)(slot + 16), &run_type,  4);
        MemCopy((void *)(slot + 20), &acpi,      4);
        count++;
    } else if (have_run) {
        truncated = 1;
    }

    PhysWrite16(E820_COUNT_ADDR, (uint16_t)count);
    PhysWrite16(E820_SIZE_ADDR,  (uint16_t)(count * sizeof(E820Entry)));

    g_e820_truncated = truncated;
    return count;
}

static uint64_t g_efi_rt_services_phys = 0;
static uint64_t g_efi_mmap_copy_phys   = 0;
static uint32_t g_efi_mmap_copy_size   = 0;
static uint32_t g_efi_mmap_desc_size   = 0;
static uint32_t g_efi_mmap_desc_ver    = 0;
static uint32_t g_efi_fw_revision      = 0;
static uint64_t g_efi_system_table_phys = 0;
static uint64_t g_efi_cfg_table_phys    = 0;
static uint32_t g_efi_cfg_table_count   = 0;
static uint64_t g_efi_esrt_copy_phys    = 0;
static uint32_t g_efi_esrt_copy_size    = 0;


#define ESRT_FW_RESOURCE_VERSION  1ULL
#define ESRT_ENTRY_SIZE           40U

#define EFI_SYSTEM_RESOURCE_TABLE_GUID \
    EFI_GUID_INIT(0xb122a263,0x3661,0x4f68, 0x99,0x29,0x78,0xf8,0xb0,0xd6,0x21,0x80)

typedef struct __attribute__((packed)) {
    uint32_t fw_resource_count;
    uint32_t fw_resource_count_max;
    uint64_t fw_resource_version;
} EsrtHeader;

_Static_assert(sizeof(EsrtHeader) == 16, "ESRT header must be 16 bytes");

static uint64_t FindEsrt(uint32_t *out_size)
{
    *out_size = 0;
    if (!g_st || g_st->number_of_table_entries == 0) return 0;

    EFI_CONFIGURATION_TABLE *ct = g_st->configuration_table;
    if (!ct) return 0;

    UINTN n = g_st->number_of_table_entries;
    if (n > EFI_CFG_TABLE_MAX_ENTRIES) n = EFI_CFG_TABLE_MAX_ENTRIES;

    EFI_GUID esrt_guid = EFI_SYSTEM_RESOURCE_TABLE_GUID;

    for (UINTN i = 0; i < n; i++) {
        if (!GuidEqual(&ct[i].vendor_guid, &esrt_guid)) continue;

        EsrtHeader *hdr = (EsrtHeader *)ct[i].vendor_table;
        if (!hdr) return 0;
        if (hdr->fw_resource_version != ESRT_FW_RESOURCE_VERSION) {
            Print("TagBoot: ESRT: unknown FwResourceVersion=");
            PrintDec(hdr->fw_resource_version);
            Print(" — ignoring\r\n");
            return 0;
        }
        if (hdr->fw_resource_count > 1024U) {
            Print("TagBoot: ESRT: unreasonable FwResourceCount=");
            PrintDec(hdr->fw_resource_count);
            Print(" — ignoring\r\n");
            return 0;
        }
        *out_size = sizeof(EsrtHeader) +
                    hdr->fw_resource_count * ESRT_ENTRY_SIZE;
        return (uint64_t)(uintptr_t)hdr;
    }
    return 0;
}

static uint64_t CopyEsrtToNvs(uint64_t esrt_phys, uint32_t esrt_size)
{
    if (esrt_phys == 0 || esrt_size == 0) return 0;
    if (esrt_size > 65536U) return 0;

    UINTN pages = (esrt_size + PAGE_4KB - 1) / PAGE_4KB;
    EFI_PHYSICAL_ADDRESS buf = 0xFFFFF000ULL;
    EFI_STATUS s = g_bs->allocate_pages(AllocateMaxAddress,
                                         EfiACPIMemoryNVS,
                                         pages,
                                         &buf);
    if (EFI_ERROR(s)) {
        Print("TagBoot: ESRT NVS alloc failed\r\n");
        return 0;
    }
    MemCopy((void *)(uintptr_t)buf,
             (const void *)(uintptr_t)esrt_phys,
             esrt_size);
    return (uint64_t)buf;
}

static void ClaimHandoffPages(void)
{
    static const struct { uint64_t at; const char *what; } wanted[] = {
        { 0x0000ULL, "the E820 map"          },
        { 0x1000ULL, "the rest of the E820 map" },
        { 0xA000ULL, "boot_info and the boarding pass" },
    };

    unsigned held = 0;
    for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        EFI_PHYSICAL_ADDRESS pa = (EFI_PHYSICAL_ADDRESS)wanted[i].at;
        EFI_STATUS s = g_bs->allocate_pages(AllocateAddress, EfiLoaderData,
                                            1, &pa);
        if (EFI_ERROR(s)) {
            Print("TagBoot: firmware would not set aside ");
            PrintHex64(wanted[i].at);
            Print(" for ");
            Print(wanted[i].what);
            Print(" — writing there anyway (status=");
            PrintHex64((uint64_t)s);
            Print(")\r\n");
            continue;
        }
        held++;
    }

    Print("TagBoot: set aside ");
    PrintDec(held);
    Print(" of 3 handoff page(s)\r\n");
}

static EFI_STATUS BuildMemoryMap(MemMapResult *out)
{
    UINTN     map_size     = 0;
    UINTN     map_key      = 0;
    UINTN     desc_size    = 0;
    uint32_t  desc_version = 0;
    uint8_t  *map_buf      = NULL;

    EFI_STATUS status = g_bs->get_memory_map(&map_size, NULL,
                                              &map_key, &desc_size,
                                              &desc_version);
    map_size += desc_size * 8;

    status = g_bs->allocate_pool(EfiLoaderData, map_size, (void **)&map_buf);
    if (EFI_ERROR(status)) return status;

    status = g_bs->get_memory_map(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf,
                                   &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        g_bs->free_pool(map_buf);
        return status;
    }

    uint32_t count = EmitE820(map_buf, map_size, desc_size);

    g_bs->free_pool(map_buf);
    map_buf = NULL;

    out->map_size     = map_size;
    out->map_key      = map_key;
    out->desc_size    = desc_size;
    out->desc_version = desc_version;
    out->map_buf      = NULL;
    out->e820_count   = count;

    Print("TagBoot: memory map ");
    PrintDec(count);
    Print(" entries\r\n");

    return EFI_SUCCESS;
}

static EFI_STATUS DoExitBootServices(EFI_HANDLE image_handle)
{
    EFI_STATUS last_status = EFI_ABORTED;

    for (UINTN attempt = 0; attempt < 10; attempt++) {
        UINTN                map_size = 0;
        UINTN                key      = 0;
        UINTN                desc_sz  = 0;
        uint32_t             desc_ver = 0;
        EFI_PHYSICAL_ADDRESS buf_pa   = 0;

        g_bs->get_memory_map(&map_size, NULL, &key, &desc_sz, &desc_ver);
        #define EFI_MMAP_SLACK_DESCS  256U
        map_size += desc_sz * EFI_MMAP_SLACK_DESCS;

        UINTN pages = (map_size + PAGE_4KB - 1) / PAGE_4KB;

        buf_pa = 0xFFFFF000ULL;
        EFI_STATUS s = g_bs->allocate_pages(AllocateMaxAddress,
                                            EfiACPIMemoryNVS,
                                            pages,
                                            &buf_pa);
        if (EFI_ERROR(s)) {
            last_status = s;
            Print("TagBoot: EBS attempt ");
            PrintDec(attempt);
            Print(" — allocate_pages(NVS) failed\r\n");
            continue;
        }

        s = g_bs->get_memory_map(&map_size,
                                  (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf_pa,
                                  &key, &desc_sz, &desc_ver);
        if (EFI_ERROR(s)) {
            last_status = s;
            Print("TagBoot: EBS attempt ");
            PrintDec(attempt);
            Print(" — get_memory_map failed\r\n");
            g_bs->free_pages(buf_pa, pages);
            continue;
        }

        g_efi_mmap_copy_phys = (uint64_t)buf_pa;
        g_efi_mmap_copy_size = (uint32_t)map_size;
        g_efi_mmap_desc_size = (uint32_t)desc_sz;
        g_efi_mmap_desc_ver  = desc_ver;

        s = g_bs->exit_boot_services(image_handle, key);
        if (!EFI_ERROR(s)) return EFI_SUCCESS;

        last_status = s;
        Print("TagBoot: EBS attempt ");
        PrintDec(attempt);
        Print(" — exit_boot_services failed status=");
        PrintHex64((uint64_t)s);
        Print(", retrying\r\n");
        g_bs->free_pages(buf_pa, pages);
        g_efi_mmap_copy_phys = 0;
        g_efi_mmap_copy_size = 0;
    }
    return last_status;
}


static uint64_t g_pt_base   = 0;
static uint64_t g_stack_base = 0;

static uint32_t CalculateIdentityMapPages(void)
{
    return 2048;
}

static EFI_STATUS SetupPageTables(uint64_t kernel_phys_end)
{
    uint64_t aligned_end = (kernel_phys_end + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
    g_pt_base   = aligned_end + GUARD_PAGE_SIZE;
    g_stack_base = g_pt_base + PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE;

    uint64_t total_alloc = PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE;
    EFI_PHYSICAL_ADDRESS pt_alloc = (EFI_PHYSICAL_ADDRESS)g_pt_base;
    UINTN alloc_pages = (UINTN)(total_alloc / PAGE_4KB);

    EFI_STATUS status = g_bs->allocate_pages(AllocateAddress,
                                             EfiLoaderData,
                                             alloc_pages,
                                             &pt_alloc);
    if (EFI_ERROR(status)) {
        if (status == EFI_NOT_FOUND) {
            g_bs->free_pages(pt_alloc, alloc_pages);
            pt_alloc = (EFI_PHYSICAL_ADDRESS)g_pt_base;
            status = g_bs->allocate_pages(AllocateAddress,
                                          EfiLoaderData,
                                          alloc_pages,
                                          &pt_alloc);
        }
        if (EFI_ERROR(status)) {
            Print("TagBoot: cannot allocate page tables + stack at ");
            PrintHex64(g_pt_base);
            Print(" (status=");
            PrintHex64((uint64_t)status);
            Print(")\r\n");
            return status;
        }
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)g_pt_base;
    MemZero(pt, PAGE_TABLE_SIZE);

    uint64_t *pml4     = pt;
    uint64_t *pdpt     = pt + 512;
    uint64_t *pd_base  = pt + 512 * 2;
    uint64_t *pdpt_hi  = pt + 512 * 6;
    uint64_t *pd_hi    = pt + 512 * 7;

    uint32_t total_2mb_pages = CalculateIdentityMapPages();
    uint32_t num_pds = (total_2mb_pages + 511) / 512;
    if (num_pds > 4) num_pds = 4;

    pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_PRESENT | PTE_WRITABLE;

    for (uint32_t i = 0; i < num_pds; i++) {
        uint64_t *pd = pd_base + i * 512;
        pdpt[i] = (uint64_t)(uintptr_t)pd | PTE_PRESENT | PTE_WRITABLE;
    }

    for (uint32_t page = 0; page < total_2mb_pages; page++) {
        uint32_t pd_idx    = page / 512;
        uint32_t entry_idx = page % 512;
        if (pd_idx >= num_pds) break;
        uint64_t *pd = pd_base + pd_idx * 512;
        pd[entry_idx] = ((uint64_t)page << 21) |
                         PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE;
    }

    pml4[511] = (uint64_t)(uintptr_t)pdpt_hi | PTE_PRESENT | PTE_WRITABLE;
    pdpt_hi[510] = (uint64_t)(uintptr_t)pd_hi | PTE_PRESENT | PTE_WRITABLE;

    for (uint32_t i = 0; i < 16; i++) {
        pd_hi[i] = ((uint64_t)i << 21) |
                   PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE;
    }

    Print("TagBoot: page tables at ");
    PrintHex64(g_pt_base);
    Print(", stack at ");
    PrintHex64(g_stack_base);
    Print("\r\n");

    return EFI_SUCCESS;
}

#define BOARDING_PASS_ADDR      0xA600ULL
#define BOARDING_PASS_BYTES     512U
#define BOARDING_PASS_MAGIC     0x53415042U
#define BOARDING_PASS_VERSION   1U
#define BOARDING_HDR_BYTES      16U
#define BOARDING_STAMP_VOLUME   1U
#define BOARDING_STAMP_MEDIUM   2U
#define BOARDING_STAMP_LOADER   3U
#define BOARDING_STAMP_SEAL     4U
#define BOARDING_FIRMWARE_UEFI  1U

#ifdef BOARDING_PASS_ADDR_FROM_BUILD
_Static_assert(BOARDING_PASS_ADDR == BOARDING_PASS_ADDR_FROM_BUILD,
               "TagBoot writes the boarding pass somewhere the build does not "
               "expect");
#endif

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint16_t used_bytes;
    uint16_t capacity;
    uint16_t count;
    uint16_t reserved;
} BoardingPassHeaderV1;

_Static_assert(sizeof(BoardingPassHeaderV1) == 16, "boarding pass header is 16 bytes");

static uint16_t BoardingStamp(uint8_t *base, uint16_t at, uint16_t kind,
                              const void *payload, uint16_t bytes)
{
    uint16_t k = kind, b = bytes;
    MemCopy(base + at,     &k, 2);
    MemCopy(base + at + 2, &b, 2);
    if (bytes) {
        MemCopy(base + at + 4, payload, bytes);
    }
    uint16_t stride = (uint16_t)((4U + bytes + 3U) & ~3U);
    return (uint16_t)(at + stride);
}

static void WriteBoardingPass(void)
{
    uint8_t *base = (uint8_t *)(uintptr_t)BOARDING_PASS_ADDR;
    MemZero(base, BOARDING_PASS_BYTES);

    uint16_t at = BOARDING_HDR_BYTES;
    uint16_t count = 0;

    at = BoardingStamp(base, at, BOARDING_STAMP_VOLUME,
                       g_volume_uuid, 16);
    count++;

    struct __attribute__((packed)) {
        uint8_t firmware; uint8_t bios_drive; uint16_t reserved;
    } medium = { BOARDING_FIRMWARE_UEFI, 0xFF, 0 };
    at = BoardingStamp(base, at, BOARDING_STAMP_MEDIUM, &medium, sizeof(medium));
    count++;

    struct __attribute__((packed)) {
        char name[12]; uint16_t major; uint16_t minor;
    } loader = { { 'T','a','g','B','o','o','t',0,0,0,0,0 }, 1, 0 };
    at = BoardingStamp(base, at, BOARDING_STAMP_LOADER, &loader, sizeof(loader));
    count++;

    uint32_t sum = 0;
    uint16_t seal_at = at;
    at = BoardingStamp(base, at, BOARDING_STAMP_SEAL, &sum, sizeof(sum));
    count++;

    BoardingPassHeaderV1 hdr;
    hdr.magic        = BOARDING_PASS_MAGIC;
    hdr.version      = BOARDING_PASS_VERSION;
    hdr.header_bytes = BOARDING_HDR_BYTES;
    hdr.used_bytes   = at;
    hdr.capacity     = BOARDING_PASS_BYTES;
    hdr.count        = count;
    hdr.reserved     = 0;
    MemCopy(base, &hdr, sizeof(hdr));

    uint32_t covered = (uint32_t)seal_at + 4U;
    sum = Crc32(base, covered);
    MemCopy(base + covered, &sum, sizeof(sum));
}


static void FillBootInfo(const FbInfo *fb, uint32_t e820_count,
                         uint64_t kernel_end_phys)
{
    BootInfoV4 *bi = (BootInfoV4 *)(uintptr_t)BOOT_INFO_ADDR;
    MemZero(bi, sizeof(BootInfoV4));

    bi->magic          = BOOT_INFO_MAGIC;
    bi->version        = BOOT_INFO_VERSION_V4;
    bi->e820_map_addr  = (uint32_t)E820_MAP_ADDR;
    bi->e820_count     = (uint16_t)e820_count;
    bi->reserved1      = 0;
    bi->kernel_start   = (uint32_t)KERNEL_LOAD_ADDR;

    uint64_t kend_aligned = (kernel_end_phys + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
    bi->kernel_end     = (uint32_t)kend_aligned;
    bi->boot_drive     = 0xFF;
    bi->reserved2      = 0;
    bi->reserved3      = 0;
    bi->page_table_base = (uint32_t)g_pt_base;
    bi->stack_base     = (uint32_t)g_stack_base;
    bi->total_size     = sizeof(BootInfoV4);

    bi->fb_addr        = fb->addr;
    bi->fb_width       = fb->width;
    bi->fb_height      = fb->height;
    bi->fb_stride      = fb->stride;
    bi->fb_format      = fb->format;
    bi->boot_method    = 1;
    bi->reserved_v2[0] = 0;
    bi->reserved_v2[1] = 0;
    bi->reserved_v2[2] = 0;
    bi->rsdp_addr      = FindAcpiRsdp();

    bi->efi_rt_services_phys = g_efi_rt_services_phys;
    bi->efi_mmap_phys        = 0;
    bi->efi_mmap_size        = 0;
    bi->efi_mmap_desc_size   = 0;
    bi->efi_mmap_desc_ver    = 0;
    bi->efi_fw_revision      = g_efi_fw_revision;

    bi->efi_system_table_phys = g_efi_system_table_phys;
    bi->efi_cfg_table_phys    = g_efi_cfg_table_phys;
    bi->efi_cfg_table_count   = g_efi_cfg_table_count;
    bi->efi_esrt_copy_phys    = g_efi_esrt_copy_phys;
    bi->efi_esrt_copy_size    = g_efi_esrt_copy_size;
}

static void PostEbsPatchBootInfo(void)
{
    BootInfoV4 *bi = (BootInfoV4 *)(uintptr_t)BOOT_INFO_ADDR;
    bi->efi_mmap_phys      = g_efi_mmap_copy_phys;
    bi->efi_mmap_size      = g_efi_mmap_copy_size;
    bi->efi_mmap_desc_size = g_efi_mmap_desc_size;
    bi->efi_mmap_desc_ver  = g_efi_mmap_desc_ver;

    if (g_efi_mmap_copy_phys && g_efi_mmap_copy_size && g_efi_mmap_desc_size) {
        uint32_t n = EmitE820((const uint8_t *)(uintptr_t)g_efi_mmap_copy_phys,
                              g_efi_mmap_copy_size,
                              g_efi_mmap_desc_size);
        bi->e820_count = (uint16_t)n;
    }
}


extern void __attribute__((sysv_abi)) TagBootJump(uint64_t pt_base,
                                                   uint64_t stack,
                                                   uint64_t entry);

static void __attribute__((noreturn)) JumpToKernel(uint64_t pt_base)
{
    TagBootJump(pt_base, g_stack_base, (uint64_t)KERNEL_LOAD_ADDR);
    __builtin_unreachable();
}

static void CheckEfiLoadAddress(void)
{
    EFI_GUID img_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (EFI_ERROR(g_bs->handle_protocol(g_image_handle, &img_guid, (void **)&li)))
        return;
    if (!li || !li->image_base) return;

    uint64_t app_base = (uint64_t)(uintptr_t)li->image_base;
    uint64_t app_end  = app_base + li->image_size;

    Print("TagBoot: EFI image at ");
    PrintHex64(app_base);
    Print(" size=");
    PrintDec(li->image_size);
    Print("\r\n");

    if (app_end > 0x100000000ULL) {
        Print("TagBoot: FATAL — EFI app loaded above 4 GB (");
        PrintHex64(app_end);
        Print(")\r\n");
        Panic("EFI app above 4 GB — extend identity map or update firmware");
    }
}


EFI_STATUS EFIAPI TagBootMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    Com1Init();
    Com1Str("\r\n[TAGBOOT] Entry reached\r\n");

    g_st           = st;
    g_bs           = st->boot_services;
    g_image_handle = image_handle;

    g_efi_rt_services_phys   = (uint64_t)(uintptr_t)st->runtime_services;
    g_efi_fw_revision        = st->firmware_revision;
    g_efi_system_table_phys  = (uint64_t)(uintptr_t)st;
    g_efi_cfg_table_phys     = (uint64_t)(uintptr_t)st->configuration_table;

    uint64_t cfg_count_raw = st->number_of_table_entries;
    if (cfg_count_raw > EFI_CFG_TABLE_MAX_ENTRIES)
        cfg_count_raw = EFI_CFG_TABLE_MAX_ENTRIES;
    g_efi_cfg_table_count    = (uint32_t)cfg_count_raw;

    st->con_out->clear_screen(st->con_out);
    Print("TagBoot v4 — BoxOS UEFI Bootloader\r\n");
    Print("------------------------------------\r\n");

    if (st->firmware_vendor) {
        Print("TagBoot: firmware vendor=");
        for (CHAR16 *v = st->firmware_vendor; *v; v++) {
            CHAR16 ch[2];
            ch[0] = (*v < 0x80) ? *v : (CHAR16)'?';
            ch[1] = 0;
            st->con_out->output_string(st->con_out, ch);
        }
        Print(" rev=");
        PrintHex64(g_efi_fw_revision);
        Print("\r\n");
    }

    Print("TagBoot: EFI ST=");
    PrintHex64(g_efi_system_table_phys);
    Print(" RT=");
    PrintHex64(g_efi_rt_services_phys);
    Print(" CT=");
    PrintHex64(g_efi_cfg_table_phys);
    Print(" entries=");
    PrintDec(g_efi_cfg_table_count);
    Print("\r\n");

    ClaimHandoffPages();

    Print("TagBoot: locating disk...\r\n");
    EFI_STATUS status = BlockIoFindDevice(image_handle);
    if (EFI_ERROR(status)) Panic("no Block IO device found");
    Print("TagBoot: disk found, block_size=");
    PrintDec(g_block_io->media->block_size);
    Print("\r\n");

    status = DeedTakeUp();
    if (EFI_ERROR(status)) Panic("the volume's deed cannot be read by this loader");
    Print("TagBoot: volume on ground at sector ");
    PrintDec(g_ground_start);
    Print(", data run of ");
    PrintDec(g_layout.data_blocks);
    Print(" blocks\r\n");

    uint32_t kernel_volume_block = 0;
    uint32_t kernel_block_count  = 0;
    uint64_t kernel_file_size    = 0;

    uint32_t kernel_data_block = 0;
    if (TagFsBootStamp(&kernel_volume_block, &kernel_block_count,
                       &kernel_file_size)) {
        Print("TagBoot: the deed names the kernel\r\n");
    } else if (TagFsFindKernelByTags(&kernel_data_block, &kernel_block_count,
                                     &kernel_file_size)) {
        kernel_volume_block = g_layout.data_block + kernel_data_block;
    } else {
        Panic("kernel not found — this deed names none and no file carries a boot tag");
    }

    Print("TagBoot: kernel at volume block ");
    PrintDec(kernel_volume_block);
    Print(" (");
    PrintDec(kernel_block_count);
    Print(" blocks)\r\n");

    const uint32_t kernel_max_blocks = (uint32_t)(KERNEL_MAX_SIZE / TAGFS_BLOCK_SIZE);
    if (kernel_block_count == 0 || kernel_block_count > kernel_max_blocks) {
        Print("TagBoot: FATAL — kernel_block_count out of range: ");
        PrintDec(kernel_block_count);
        Print("\r\n");
        Panic("TagFS metadata reports invalid kernel block count");
    }

    Print("TagBoot: loading kernel to 0x100000...\r\n");
    uint64_t loaded_bytes = TagFsLoadKernel(kernel_volume_block,
                                             kernel_block_count,
                                             kernel_file_size);
    if (loaded_bytes == 0) Panic("kernel load failed");

    Print("TagBoot: kernel loaded (");
    PrintDec(loaded_bytes);
    Print(" bytes)\r\n");

    if (!MemEqual((const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 2), "KERNEL", 6)) {
        Panic("kernel header magic invalid — wrong binary at load address");
    }
    uint32_t hdr_version = 0;
    MemCopy(&hdr_version, (const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 8), 4);
    if (hdr_version != KERNEL_HEADER_VERSION) {
        Print("TagBoot: kernel header version mismatch — got ");
        PrintDec(hdr_version);
        Print(", expected ");
        PrintDec(KERNEL_HEADER_VERSION);
        Print("\r\n");
        Panic("incompatible kernel header version");
    }
    Print("TagBoot: kernel header OK\r\n");

    uint32_t header_phys_end = 0;
    MemCopy(&header_phys_end,
            (const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 12), 4);

    uint64_t kernel_end_phys = KERNEL_LOAD_ADDR + loaded_bytes;

    if (header_phys_end > (uint32_t)kernel_end_phys && header_phys_end < 0x10000000U) {
        EFI_PHYSICAL_ADDRESS bss_pa  = (EFI_PHYSICAL_ADDRESS)kernel_end_phys;
        UINTN bss_pages = ((uint64_t)header_phys_end - kernel_end_phys + PAGE_4KB - 1)
                          / PAGE_4KB;
        EFI_STATUS bss_status = g_bs->allocate_pages(AllocateAddress, EfiLoaderData,
                                                      bss_pages, &bss_pa);
        if (bss_status == EFI_NOT_FOUND) {
            g_bs->free_pages(bss_pa, bss_pages);
            bss_pa = (EFI_PHYSICAL_ADDRESS)kernel_end_phys;
            bss_status = g_bs->allocate_pages(AllocateAddress, EfiLoaderData,
                                              bss_pages, &bss_pa);
        }
        kernel_end_phys = (uint64_t)header_phys_end;
        Print("TagBoot: kernel phys end (BSS) ");
        PrintHex64(kernel_end_phys);
        if (EFI_ERROR(bss_status)) Print(" (BSS alloc warn)");
        Print("\r\n");
    }

    FbInfo fb = QueryGopFramebuffer();

    Print("TagBoot: building memory map...\r\n");
    MemMapResult mmap;
    status = BuildMemoryMap(&mmap);
    if (EFI_ERROR(status)) Panic("failed to get UEFI memory map");

    if (g_e820_truncated) {
        Print("TagBoot: WARNING — the memory map does not fit ");
        PrintDec(E820_MAX_ENTRIES);
        Print(" entries and was cut short\r\n");
    }

    CheckEfiLoadAddress();

    Print("TagBoot: setting up page tables...\r\n");
    status = SetupPageTables(kernel_end_phys);
    if (EFI_ERROR(status)) Panic("page table setup failed");

    {
        uint32_t esrt_size = 0;
        uint64_t esrt_src  = FindEsrt(&esrt_size);
        if (esrt_src != 0 && esrt_size != 0) {
            uint64_t nvs = CopyEsrtToNvs(esrt_src, esrt_size);
            if (nvs != 0) {
                g_efi_esrt_copy_phys = nvs;
                g_efi_esrt_copy_size = esrt_size;
                Print("TagBoot: ESRT copied to ACPI_NVS at ");
                PrintHex64(nvs);
                Print(" (");
                PrintDec(esrt_size);
                Print(" bytes)\r\n");
            }
        } else {
            Print("TagBoot: ESRT not published by firmware\r\n");
        }
    }

    FillBootInfo(&fb, mmap.e820_count, kernel_end_phys);
    WriteBoardingPass();

    Print("TagBoot: boot_info at 0xA000 (v4, method=UEFI)\r\n");

    {
        EFI_STATUS wd = g_bs->set_watchdog_timer(0, 0, 0, NULL);
        if (EFI_ERROR(wd)) {
            Print("TagBoot: watchdog disable returned status=");
            PrintHex64((uint64_t)wd);
            Print(" (proceeding)\r\n");
        }
    }

    Print("TagBoot: exiting boot services...\r\n");

    status = DoExitBootServices(image_handle);
    if (EFI_ERROR(status)) Panic("ExitBootServices failed after 10 attempts");

    PostEbsPatchBootInfo();

    JumpToKernel(g_pt_base);
}