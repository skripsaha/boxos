/*
 * TagBoot — UEFI bootloader for BoxOS
 *
 * Boot sequence:
 *   1. Locate Block IO on boot device via loaded image device handle
 *   2. Read TagFS superblock from sector 1034; verify magic
 *   3. Search tag registry + metadata pool for kernel by tag priority:
 *      "god" → "boot" → "system". Fall back to superblock boot hints.
 *   4. Allocate pages at 0x100000 and load kernel binary
 *   5. Query GOP framebuffer; record address and parameters
 *   6. Get UEFI memory map; convert to e820_entry_t format at 0x500
 *   7. Allocate 32 KB for page tables after kernel_end + guard page
 *   8. Build identity + higher-half page tables (2 MB pages)
 *   9. Fill boot_info_t (v2) at 0x9000
 *  10. ExitBootServices
 *  11. Enable PAE, load CR3, enable EFER.LME+NXE, enable paging
 *  12. Jump to 0x100000
 */

#include "uefi.h"
#include "tagfs_boot.h"

/* =========================================================================
 * Boot constants — must match kernel contract
 * ========================================================================= */

/*
 * Raw physical address write for 16-bit values.
 * noinline prevents GCC from tracing the pointer back to its origin and
 * incorrectly firing -Warray-bounds on known-good physical memory addresses.
 */
static __attribute__((noinline)) void PhysWrite16(uint64_t addr, uint16_t val)
{
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)addr;
    *p = val;
}

#define BOOT_INFO_MAGIC       0x42583031U   /* "BX01" */
#define BOOT_INFO_VERSION_V2  2U

#define KERNEL_LOAD_ADDR      0x100000ULL
#define KERNEL_MAX_SIZE       0x2000000ULL  /* 32 MB */
#define BOOT_INFO_ADDR        0x9000ULL
#define E820_COUNT_ADDR       0x500ULL
#define E820_SIZE_ADDR        0x502ULL
#define E820_MAP_ADDR         0x504ULL
#define E820_MAX_ENTRIES      128U

#define PAGE_TABLE_SIZE       0x8000ULL     /* 32 KB */
#define GUARD_PAGE_SIZE       0x1000ULL     /* 4 KB */
#define BOOT_STACK_SIZE       0x10000ULL    /* 64 KB */

#define PAGE_2MB              0x200000ULL
#define PAGE_4KB              0x1000ULL

/* PTE flags */
#define PTE_PRESENT           (1ULL << 0)
#define PTE_WRITABLE          (1ULL << 1)
#define PTE_PAGE_SIZE         (1ULL << 7)   /* PS bit for 2MB pages */

/* EFER MSR */
#define MSR_EFER              0xC0000080U
#define EFER_LME              (1U << 8)
#define EFER_NXE              (1U << 11)

/* CR4 bits */
#define CR4_PAE               (1U << 5)
#define CR4_PGE               (1U << 7)

/* =========================================================================
 * E820 entry type codes
 * ========================================================================= */

#define E820_USABLE    1U
#define E820_RESERVED  2U
#define E820_ACPI_RECL 3U
#define E820_ACPI_NVS  4U

/* =========================================================================
 * boot_info_t v2 layout at 0x9000
 * Matches src/include/boot_info.h extended with v2 fields.
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    /* v1 fields (40 bytes) */
    uint32_t magic;           /* +0  BOOT_INFO_MAGIC */
    uint32_t version;         /* +4  2 for UEFI boot */
    uint32_t e820_map_addr;   /* +8  physical address of e820 entries */
    uint16_t e820_count;      /* +12 */
    uint16_t reserved1;       /* +14 */
    uint32_t kernel_start;    /* +16 */
    uint32_t kernel_end;      /* +20 */
    uint8_t  boot_drive;      /* +24 0xFF for UEFI */
    uint8_t  reserved2;       /* +25 */
    uint16_t reserved3;       /* +26 */
    uint32_t page_table_base; /* +28 */
    uint32_t stack_base;      /* +32 */
    uint32_t total_size;      /* +36 76 for v2 */
    /* v2 additions (+40) */
    uint64_t fb_addr;         /* +40 GOP framebuffer physical address */
    uint32_t fb_width;        /* +48 */
    uint32_t fb_height;       /* +52 */
    uint32_t fb_stride;       /* +56 bytes per row */
    uint32_t fb_format;       /* +60 0=RGB,1=BGR,2=BGRX */
    uint8_t  boot_method;     /* +64 1=UEFI */
    uint8_t  reserved_v2[3];  /* +65 */
    uint64_t rsdp_addr;       /* +68 ACPI RSDP physical address (0 if not found) */
} BootInfoV2;

_Static_assert(sizeof(BootInfoV2) == 76, "BootInfoV2 must be 76 bytes");

/* =========================================================================
 * e820_entry_t (matches kernel's e820.h)
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} E820Entry;

_Static_assert(sizeof(E820Entry) == 24, "E820Entry must be 24 bytes");

/* =========================================================================
 * Global state (no heap allocation needed — use static storage)
 * ========================================================================= */

static EFI_SYSTEM_TABLE  *g_st   = NULL;
static EFI_BOOT_SERVICES *g_bs   = NULL;
static EFI_HANDLE         g_image_handle = NULL;

/* =========================================================================
 * Utility: memory operations (no libc available)
 * ========================================================================= */

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

/* StrEqual8: currently unused — kept for future diagnostic use */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
static int StrEqual8(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
#pragma GCC diagnostic pop

/* =========================================================================
 * COM1 serial debug output (raw port I/O — works before UEFI console)
 *
 * Use these before g_st is valid.  UEFI runs at ring 0, so outb is legal.
 * QEMU -serial stdio will show this output on stdout.
 * ========================================================================= */

static void Com1Init(void)
{
    uint16_t p;
    uint8_t  v;
    p = 0x3F9; v = 0x00; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* IER=0     */
    p = 0x3FB; v = 0x80; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* DLAB=1    */
    p = 0x3F8; v = 0x01; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* div lo=1  */
    p = 0x3F9; v = 0x00; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* div hi=0  */
    p = 0x3FB; v = 0x03; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* 8N1       */
    p = 0x3FC; v = 0x03; __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));  /* RTS+DTR   */
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

/* =========================================================================
 * Utility: print to UEFI console
 * ========================================================================= */

/* Print a narrow ASCII string by converting each char to CHAR16 on the fly. */
static void Print(const char *msg)
{
    if (!g_st || !g_st->con_out) return;
    CHAR16 buf[2];
    buf[1] = 0;
    for (size_t i = 0; msg[i]; i++) {
        buf[0] = (CHAR16)(unsigned char)msg[i];
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

/* Fatal halt: print error, spin forever. */
static void Panic(const char *msg)
{
    Print("\r\n[TAGBOOT FATAL] ");
    Print(msg);
    Print("\r\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* =========================================================================
 * ACPI RSDP discovery from UEFI Configuration Table
 * ========================================================================= */

/*
 * FindAcpiRsdp — scan the UEFI Configuration Table for the ACPI RSDP.
 * Prefers ACPI 2.0 (XSDP) over ACPI 1.0 (RSDP).
 * Returns the physical address of the RSDP, or 0 if not found.
 */
static uint64_t FindAcpiRsdp(void)
{
    if (!g_st || g_st->number_of_table_entries == 0) return 0;

    EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;
    EFI_CONFIGURATION_TABLE *ct = g_st->configuration_table;

    uint64_t rsdp_v2 = 0;
    uint64_t rsdp_v1 = 0;

    for (UINTN i = 0; i < g_st->number_of_table_entries; i++) {
        if (GuidEqual(&ct[i].vendor_guid, &acpi20_guid))
            rsdp_v2 = (uint64_t)(uintptr_t)ct[i].vendor_table;
        else if (GuidEqual(&ct[i].vendor_guid, &acpi10_guid))
            rsdp_v1 = (uint64_t)(uintptr_t)ct[i].vendor_table;
    }

    return rsdp_v2 ? rsdp_v2 : rsdp_v1;
}

/* =========================================================================
 * Block IO: read sectors from disk
 * ========================================================================= */

static EFI_BLOCK_IO_PROTOCOL *g_block_io = NULL;

/*
 * Probe a single BlockIO handle for the TagFS superblock magic.
 * Returns TRUE if the magic is found at the expected sector.
 */
static int BlockIoProbeTagFs(EFI_BLOCK_IO_PROTOCOL *bio)
{
    if (!bio || !bio->media) return 0;
    uint32_t bsz = bio->media->block_size;
    if (bsz == 0 || !bio->media->media_present) return 0;

    /* Map 512-byte logical sector 1034 to the device's physical block. */
    uint64_t sb_lba  = (TAGFS_SUPERBLOCK_SECTOR * (uint64_t)TAGFS_SECTOR_SIZE) / bsz;
    uint32_t read_sz = bsz < TAGFS_SECTOR_SIZE ? TAGFS_SECTOR_SIZE : bsz;

    static uint8_t probe_buf[4096];   /* covers 4 KB physical sectors */
    if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id,
                                   (EFI_LBA)sb_lba, read_sz, probe_buf)))
        return 0;

    uint32_t magic_val;
    MemCopy(&magic_val, probe_buf, 4);
    return magic_val == TAGFS_MAGIC;
}

/*
 * Find the BlockIO handle that contains the TagFS volume.
 *
 * Strategy:
 *   1. Try the loaded-image device first (common case: booting from same disk
 *      that holds both ESP and TagFS, e.g. on real hardware with GPT layout).
 *   2. Scan every non-partition BlockIO handle and probe sector 1034 for the
 *      TagFS magic (handles the QEMU two-drive setup and GPT/MBR edge cases).
 *
 * The probe is safe: reading one sector from a disk that lacks TagFS does no
 * harm and the magic check is a 4-byte compare.
 */
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
        if (!EFI_ERROR(status) && BlockIoProbeTagFs(bio)) {
            Print("TagBoot: TagFS found on loaded-image device\r\n");
            g_block_io = bio;
            return EFI_SUCCESS;
        }
    }

    /* Loaded-image device does not carry TagFS.
     * Enumerate all raw (non-partition) BlockIO handles and probe each. */
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
        if (bio->media->logical_partition)    continue;  /* skip partitions */
        if (!bio->media->media_present)       continue;

        if (BlockIoProbeTagFs(bio)) {
            g_block_io = bio;
            found = EFI_SUCCESS;
            break;
        }
    }

    g_bs->free_pool(handles);

    if (EFI_ERROR(found))
        Print("TagBoot: no BlockIO handle has valid TagFS superblock\r\n");

    return found;
}

/* Read raw sectors (always 512-byte logical sectors from the caller's view).
 * Internally handles block devices whose physical block size != 512. */
static EFI_STATUS ReadSectors(uint64_t lba_512, uint32_t count_512, void *buffer)
{
    if (!g_block_io) return EFI_NOT_READY;

    uint32_t bsz = g_block_io->media->block_size;
    if (bsz == 0) bsz = 512;

    if (bsz == 512) {
        return g_block_io->read_blocks(g_block_io,
                                       g_block_io->media->media_id,
                                       (EFI_LBA)lba_512,
                                       (UINTN)count_512 * 512,
                                       buffer);
    }

    /* Block size != 512: must align reads to physical block boundary. */
    uint8_t *out     = (uint8_t *)buffer;
    uint64_t byte_off = lba_512 * 512ULL;
    uint32_t bytes_remaining = count_512 * 512;

    static uint8_t align_buf[4096];   /* covers up to 4 KB physical blocks */
    if (bsz > sizeof(align_buf)) return EFI_UNSUPPORTED;

    while (bytes_remaining > 0) {
        uint64_t phys_block = byte_off / bsz;
        uint32_t offset_in  = (uint32_t)(byte_off % bsz);
        uint32_t can_copy   = bsz - offset_in;
        if (can_copy > bytes_remaining) can_copy = bytes_remaining;

        EFI_STATUS st = g_block_io->read_blocks(g_block_io,
                                                 g_block_io->media->media_id,
                                                 (EFI_LBA)phys_block,
                                                 bsz,
                                                 align_buf);
        if (EFI_ERROR(st)) return st;

        MemCopy(out, align_buf + offset_in, can_copy);
        out             += can_copy;
        byte_off        += can_copy;
        bytes_remaining -= can_copy;
    }
    return EFI_SUCCESS;
}

/* Read one 4 KB TagFS block (8 sectors) into buffer. */
static EFI_STATUS ReadTagFsBlock(uint32_t data_start_sector, uint32_t block, void *buf)
{
    uint64_t lba = (uint64_t)data_start_sector + (uint64_t)block * TAGFS_SECTORS_PER_BLOCK;
    return ReadSectors(lba, TAGFS_SECTORS_PER_BLOCK, buf);
}

/* =========================================================================
 * TagFS: superblock
 * ========================================================================= */

static TagBootSuperblock g_superblock;

static EFI_STATUS TagFsReadSuperblock(void)
{
    EFI_STATUS status = ReadSectors(TAGFS_SUPERBLOCK_SECTOR, 1, &g_superblock);
    if (EFI_ERROR(status)) return status;

    if (g_superblock.magic != TAGFS_MAGIC) {
        Print("TagFS: bad magic on primary superblock, trying backup\r\n");
        status = ReadSectors(TAGFS_BACKUP_SB_SECTOR, 1, &g_superblock);
        if (EFI_ERROR(status)) return status;
        if (g_superblock.magic != TAGFS_MAGIC) return EFI_NOT_FOUND;
    }
    if (g_superblock.version != TAGFS_VERSION) return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}

/* Returns data_start_sector (first sector of block 0). */
static uint32_t TagFsDataStartSector(void)
{
    return g_superblock.block_bitmap_sector +
           g_superblock.block_bitmap_sector_count;
}

/* =========================================================================
 * TagFS: tag-based kernel lookup
 *
 * Strategy:
 *   1. Load tag registry (block 0) and scan it for the desired tag key.
 *   2. Load file table (block 1) to get meta_block/meta_offset per file_id.
 *   3. Scan all metadata pool records for a file whose tag_ids[] include the
 *      target tag. Return the first file's start_block and block_count.
 *
 * Priority: "god" → "boot" → "system".
 * Fall back to boot hints if tag lookup returns nothing.
 * ========================================================================= */

/* Read tag registry block chain; return tag_id for (key, NULL) label tag.
 * Returns TAGFS_INVALID_TAG_ID if not found. */
static uint16_t TagRegistryFindLabel(const char *key)
{
    uint32_t data_start = TagFsDataStartSector();
    uint32_t block_idx  = g_superblock.tag_registry_block;
    size_t   key_len    = StrLen8(key);

    static TagBootRegistryBlock reg_blk;

    for (uint32_t chain = 0; chain < 64; chain++) {
        if (EFI_ERROR(ReadTagFsBlock(data_start, block_idx, &reg_blk))) break;
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

            /* Label tag: flags == 0, no value */
            if (flags == 0 && this_val_len == 0 &&
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

/* Scan metadata pool for any record whose tag_ids[] contain target_tag_id.
 * Returns the start_block, block_count, and actual file_size of the first active match. */
static int TagFsFindFileByTagId(uint16_t target_tag_id,
                                uint32_t *out_start_block,
                                uint32_t *out_block_count,
                                uint64_t *out_file_size)
{
    uint32_t data_start = TagFsDataStartSector();
    uint32_t block_idx  = g_superblock.metadata_pool_block;

    static TagBootMetaPoolBlock mpool;

    for (uint32_t chain = 0; chain < 256; chain++) {
        if (EFI_ERROR(ReadTagFsBlock(data_start, block_idx, &mpool))) break;
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

            /* Skip trashed/inactive files. */
            if (!(flags & TAGFS_FILE_ACTIVE) || (flags & TAGFS_FILE_TRASHED)) {
                pos += record_len;
                continue;
            }

            /* tag_ids[] start at META_RECORD_VARDATA_OFF */
            uint8_t *tag_ids_ptr = r + META_RECORD_VARDATA_OFF;

            for (uint16_t t = 0; t < tag_count; t++) {
                uint16_t tid;
                MemCopy(&tid, tag_ids_ptr + t * 2, 2);
                if (tid == target_tag_id) {
                    /* Found match. Extract first extent. */
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

/* Try each tag in priority order; return 1 on success. */
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

/* Extract kernel location from superblock boot hints. */
static int TagFsBootHints(uint32_t *out_start_block, uint32_t *out_block_count,
                           uint32_t *out_data_start_sector, uint64_t *out_file_size)
{
    uint32_t kblock, kblocks, ksize, dstart;
    MemCopy(&kblock,  g_superblock.reserved + BOOT_HINT_KERNEL_BLOCK,  4);
    MemCopy(&kblocks, g_superblock.reserved + BOOT_HINT_KERNEL_BLOCKS, 4);
    MemCopy(&ksize,   g_superblock.reserved + BOOT_HINT_KERNEL_SIZE,   4);
    MemCopy(&dstart,  g_superblock.reserved + BOOT_HINT_DATA_START,    4);

    if (kblock == 0 || kblocks == 0) return 0;
    *out_start_block        = kblock;
    *out_block_count        = kblocks;
    *out_data_start_sector  = dstart;
    *out_file_size          = (uint64_t)ksize;
    return 1;
}

/* =========================================================================
 * Kernel loading
 * ========================================================================= */

/* Loads kernel file blocks to KERNEL_LOAD_ADDR.
 * Reads all block_count blocks from disk; returns file_size as the actual byte count,
 * or 0 on error. */
static uint64_t TagFsLoadKernel(uint32_t data_start_sector,
                                uint32_t start_block,
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

    /* Use actual file size if provided and fits within allocated blocks. */
    if (file_size == 0 || file_size > total_bytes)
        file_size = total_bytes;

    UINTN pages = (UINTN)((total_bytes + PAGE_4KB - 1) / PAGE_4KB);
    EFI_STATUS status = g_bs->allocate_pages(AllocateAddress,
                                             EfiLoaderData,
                                             pages,
                                             &load_addr);
    if (EFI_ERROR(status)) {
        /* Address not free — try to free and reallocate */
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
        uint64_t lba = (uint64_t)data_start_sector +
                       (uint64_t)(start_block + b) * TAGFS_SECTORS_PER_BLOCK;
        status = ReadSectors(lba, TAGFS_SECTORS_PER_BLOCK,
                             dst + (uint64_t)b * TAGFS_BLOCK_SIZE);
        if (EFI_ERROR(status)) {
            Print("TagBoot: disk read error at block ");
            PrintDec(start_block + b);
            Print("\r\n");
            return 0;
        }
    }

    return file_size;
}

/* =========================================================================
 * GOP framebuffer
 * ========================================================================= */

typedef struct {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;  /* 0=RGB, 1=BGR, 2=BGRX */
} FbInfo;

/*
 * SelectGopBestMode — iterate all GOP modes and set the one with the highest
 * pixel count that has a real framebuffer (not PixelBltOnly).
 * Called once before reading the framebuffer address.
 */
static void SelectGopBestMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    if (!gop || !gop->mode) return;

    uint32_t best_mode   = gop->mode->mode;
    uint32_t best_pixels = 0;

    /* Seed with current mode so we never regress. */
    if (gop->mode->info) {
        best_pixels = gop->mode->info->horizontal_resolution *
                      gop->mode->info->vertical_resolution;
    }

    for (uint32_t m = 0; m < gop->mode->max_mode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->query_mode(gop, m, &size_of_info, &info))) continue;
        if (!info) continue;
        if (info->pixel_format == PixelBltOnly) continue;   /* no real framebuffer */

        uint32_t pixels = info->horizontal_resolution * info->vertical_resolution;
        if (pixels > best_pixels) {
            best_pixels = pixels;
            best_mode   = m;
        }
    }

    if (best_mode != gop->mode->mode) {
        Print("TagBoot: GOP switching to mode ");
        PrintDec(best_mode);
        Print(" (");
        /* set_mode succeeds or we stay at current mode — either is acceptable */
        if (!EFI_ERROR(gop->set_mode(gop, best_mode))) {
            Print("ok");
        } else {
            Print("failed, keeping current");
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
    fb.stride = mode->info->pixels_per_scan_line * 4;  /* 4 bytes/pixel */

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

/* =========================================================================
 * Memory map: UEFI → E820
 * ========================================================================= */

/* Convert UEFI memory type to E820 type. */
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

/* Allocate and retrieve UEFI memory map, convert to E820 at 0x500.
 * The map_key is needed for ExitBootServices. */
static EFI_STATUS BuildMemoryMap(MemMapResult *out)
{
    UINTN     map_size     = 0;
    UINTN     map_key      = 0;
    UINTN     desc_size    = 0;
    uint32_t  desc_version = 0;
    uint8_t  *map_buf      = NULL;

    /* First call to get required size. */
    EFI_STATUS status = g_bs->get_memory_map(&map_size, NULL,
                                              &map_key, &desc_size,
                                              &desc_version);
    /* Expected: EFI_BUFFER_TOO_SMALL */
    map_size += desc_size * 8;   /* allocating extra for safety */

    status = g_bs->allocate_pool(EfiLoaderData, map_size, (void **)&map_buf);
    if (EFI_ERROR(status)) return status;

    status = g_bs->get_memory_map(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf,
                                   &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        g_bs->free_pool(map_buf);
        return status;
    }

    /* Convert to E820 entries at fixed address 0x504.
     * Use a volatile byte pointer and manual offset to avoid GCC array-bounds
     * false positives when casting raw physical addresses to typed arrays. */
    volatile uint8_t *e820_raw = (volatile uint8_t *)(uintptr_t)E820_MAP_ADDR;
    uint32_t  count = 0;

    uint8_t *p   = map_buf;
    uint8_t *end = map_buf + map_size;

    while (p < end && count < E820_MAX_ENTRIES) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)p;
        volatile uint8_t *slot = e820_raw + count * sizeof(E820Entry);

        uint64_t base   = desc->physical_start;
        uint64_t length = desc->number_of_pages * PAGE_4KB;
        uint32_t type   = UefiTypeToE820(desc->type);
        uint32_t acpi   = 0;

        MemCopy((void *)slot,      &base,   8);
        MemCopy((void *)(slot+8),  &length, 8);
        MemCopy((void *)(slot+16), &type,   4);
        MemCopy((void *)(slot+20), &acpi,   4);
        count++;

        p += desc_size;
    }

    /* Write count and byte size at 0x500/0x502. */
    PhysWrite16(E820_COUNT_ADDR, (uint16_t)count);
    PhysWrite16(E820_SIZE_ADDR,  (uint16_t)(count * sizeof(E820Entry)));

    out->map_size     = map_size;
    out->map_key      = map_key;
    out->desc_size    = desc_size;
    out->desc_version = desc_version;
    out->map_buf      = map_buf;
    out->e820_count   = count;

    Print("TagBoot: memory map ");
    PrintDec(count);
    Print(" entries\r\n");

    return EFI_SUCCESS;
}

/*
 * DoExitBootServices — proper retry loop per UEFI spec.
 *
 * Strategy: allocate a pool buffer → get_memory_map (gets fresh key after
 * the allocation) → exit_boot_services. If it fails, free the buffer and
 * retry.  The buffer is intentionally NOT freed on success because freeing
 * it would change the map key again before ExitBootServices can use it.
 * After successful ExitBootServices the pool allocator is gone anyway.
 *
 * Three attempts are sufficient in practice; real failures are firmware bugs.
 */
static EFI_STATUS DoExitBootServices(EFI_HANDLE image_handle)
{
    for (UINTN attempt = 0; attempt < 3; attempt++) {
        UINTN    map_size     = 0;
        UINTN    key          = 0;
        UINTN    desc_sz      = 0;
        uint32_t desc_ver     = 0;
        void    *buf          = NULL;

        /* First call: get required buffer size (EFI_BUFFER_TOO_SMALL expected). */
        g_bs->get_memory_map(&map_size, NULL, &key, &desc_sz, &desc_ver);
        map_size += desc_sz * 16;   /* slack for one more allocation */

        /* Allocate — this changes the map key, so we MUST call get_memory_map
         * again afterwards to obtain a key that matches the post-allocation map. */
        EFI_STATUS s = g_bs->allocate_pool(EfiLoaderData, map_size, &buf);
        if (EFI_ERROR(s)) continue;

        /* Second call: get the fresh key that reflects our allocation. */
        s = g_bs->get_memory_map(&map_size,
                                  (EFI_MEMORY_DESCRIPTOR *)buf,
                                  &key, &desc_sz, &desc_ver);
        if (EFI_ERROR(s)) { g_bs->free_pool(buf); continue; }

        /* Exit — do NOT free buf; freeing would change the key. */
        s = g_bs->exit_boot_services(image_handle, key);
        if (!EFI_ERROR(s)) return EFI_SUCCESS;

        /* Failed — free buf and retry with a new key. */
        g_bs->free_pool(buf);
    }
    return EFI_ABORTED;
}

/* =========================================================================
 * Page tables: identity + higher-half
 *
 * Layout (identical to stage2.asm's setup_paging):
 *   pt_base + 0x0000  PML4  (4 KB)
 *   pt_base + 0x1000  PDPT  (4 KB)   — identity, PML4[0]
 *   pt_base + 0x2000  PD0   (4 KB)   — 0 GB.. 1 GB
 *   pt_base + 0x3000  PD1   (4 KB)   — 1 GB.. 2 GB
 *   pt_base + 0x4000  PD2   (4 KB)   — 2 GB.. 3 GB
 *   pt_base + 0x5000  PD3   (4 KB)   — 3 GB.. 4 GB
 *   pt_base + 0x6000  PDPT_high      — higher-half, PML4[511]
 *   pt_base + 0x7000  PD_high        — first 32 MB at 0xFFFFFFFF80000000
 *
 * pt_base is placed at kernel_end (page-aligned) + 4 KB guard.
 * Stack base is pt_base + PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE.
 * ========================================================================= */

static uint64_t g_pt_base   = 0;
static uint64_t g_stack_base = 0;

/* How many 2 MB identity pages to map.  Derived from E820 usable top. */
static uint32_t CalculateIdentityMapPages(void)
{
    /* Read directly from fixed physical address using byte offsets to avoid
     * GCC's -Warray-bounds when casting a raw address to a typed array. */
    volatile uint8_t *e820_raw = (volatile uint8_t *)(uintptr_t)E820_MAP_ADDR;
    uint16_t count;
    MemCopy(&count, (const void *)(uintptr_t)E820_COUNT_ADDR, 2);
    uint64_t max_end = 0;

    for (uint16_t i = 0; i < count; i++) {
        volatile uint8_t *slot = e820_raw + (uint32_t)i * sizeof(E820Entry);
        uint64_t base, length;
        uint32_t type;
        MemCopy(&base,   (const void *)slot,      8);
        MemCopy(&length, (const void *)(slot+8),  8);
        MemCopy(&type,   (const void *)(slot+16), 4);
        if (type != E820_USABLE) continue;
        uint64_t end = base + length;
        if (end > max_end) max_end = end;
    }

    /* Convert to 2 MB page count; cap at 2048 (= 4 GB). */
    uint32_t pages = (uint32_t)((max_end + PAGE_2MB - 1) / PAGE_2MB);
    if (pages < 64)   pages = 64;
    if (pages > 2048) pages = 2048;
    return pages;
}

static EFI_STATUS SetupPageTables(uint64_t kernel_phys_end)
{
    /* pt_base after kernel, page-aligned, plus guard. */
    uint64_t aligned_end = (kernel_phys_end + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
    g_pt_base   = aligned_end + GUARD_PAGE_SIZE;
    g_stack_base = g_pt_base + PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE;

    /* Allocate the 32 KB for page tables. */
    EFI_PHYSICAL_ADDRESS pt_alloc = (EFI_PHYSICAL_ADDRESS)g_pt_base;
    UINTN pt_pages = PAGE_TABLE_SIZE / PAGE_4KB;

    EFI_STATUS status = g_bs->allocate_pages(AllocateAddress,
                                             EfiLoaderData,
                                             pt_pages,
                                             &pt_alloc);
    if (EFI_ERROR(status)) {
        g_bs->free_pages(pt_alloc, pt_pages);
        status = g_bs->allocate_pages(AllocateAddress,
                                      EfiLoaderData,
                                      pt_pages,
                                      &pt_alloc);
        if (EFI_ERROR(status)) {
            Print("TagBoot: cannot allocate page tables at ");
            PrintHex64(g_pt_base);
            Print("\r\n");
            return status;
        }
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)g_pt_base;
    MemZero(pt, PAGE_TABLE_SIZE);

    uint64_t *pml4     = pt;              /* +0x0000 */
    uint64_t *pdpt     = pt + 512;        /* +0x1000 */
    uint64_t *pd_base  = pt + 512 * 2;   /* +0x2000 = PD0 */
    uint64_t *pdpt_hi  = pt + 512 * 6;   /* +0x6000 */
    uint64_t *pd_hi    = pt + 512 * 7;   /* +0x7000 */

    uint32_t total_2mb_pages = CalculateIdentityMapPages();
    uint32_t num_pds = (total_2mb_pages + 511) / 512;
    if (num_pds > 4) num_pds = 4;

    /* PML4[0] → PDPT */
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_PRESENT | PTE_WRITABLE;

    /* PDPT[0..num_pds-1] → PD0..PD3 */
    for (uint32_t i = 0; i < num_pds; i++) {
        uint64_t *pd = pd_base + i * 512;
        pdpt[i] = (uint64_t)(uintptr_t)pd | PTE_PRESENT | PTE_WRITABLE;
    }

    /* Fill PD entries: 2 MB identity pages. */
    for (uint32_t page = 0; page < total_2mb_pages; page++) {
        uint32_t pd_idx    = page / 512;
        uint32_t entry_idx = page % 512;
        if (pd_idx >= num_pds) break;
        uint64_t *pd = pd_base + pd_idx * 512;
        pd[entry_idx] = ((uint64_t)page << 21) |
                         PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE;
    }

    /* Higher-half: PML4[511] → PDPT_high → PD_high
     * Map first 32 MB (16 entries × 2 MB) at 0xFFFFFFFF80000000. */
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

/* =========================================================================
 * boot_info_t v2 population
 * ========================================================================= */

static void FillBootInfo(const FbInfo *fb, uint32_t e820_count,
                         uint64_t kernel_end_phys)
{
    BootInfoV2 *bi = (BootInfoV2 *)(uintptr_t)BOOT_INFO_ADDR;
    MemZero(bi, sizeof(BootInfoV2));

    bi->magic          = BOOT_INFO_MAGIC;
    bi->version        = BOOT_INFO_VERSION_V2;
    bi->e820_map_addr  = (uint32_t)E820_MAP_ADDR;
    bi->e820_count     = (uint16_t)e820_count;
    bi->reserved1      = 0;
    bi->kernel_start   = (uint32_t)KERNEL_LOAD_ADDR;

    uint64_t kend_aligned = (kernel_end_phys + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
    bi->kernel_end     = (uint32_t)kend_aligned;
    bi->boot_drive     = 0xFF;   /* UEFI has no BIOS drive number */
    bi->reserved2      = 0;
    bi->reserved3      = 0;
    bi->page_table_base = (uint32_t)g_pt_base;
    bi->stack_base     = (uint32_t)g_stack_base;
    bi->total_size     = sizeof(BootInfoV2);

    bi->fb_addr        = fb->addr;
    bi->fb_width       = fb->width;
    bi->fb_height      = fb->height;
    bi->fb_stride      = fb->stride;
    bi->fb_format      = fb->format;
    bi->boot_method    = 1;   /* UEFI */
    bi->reserved_v2[0] = 0;
    bi->reserved_v2[1] = 0;
    bi->reserved_v2[2] = 0;
    bi->rsdp_addr      = FindAcpiRsdp();
}

/* =========================================================================
 * Kernel handoff: disable UEFI, enable long mode, jump to kernel
 *
 * After ExitBootServices we have no UEFI services. We must:
 *   1. Enable PAE in CR4
 *   2. Load page table base into CR3
 *   3. Set EFER.LME | EFER.NXE via WRMSR
 *   4. Set CR0.PG
 *
 * At this point we are already in 64-bit mode (UEFI boots in long mode),
 * so we just reload CR3 with our new tables and jump to the kernel.
 * ========================================================================= */

/*
 * Implemented in tagboot_jump.asm — pure NASM, no inline asm ambiguity.
 * Installs our CR3, sets EFER.LME+NXE, CR0.PG, switches RSP, jumps to entry.
 * Never returns.
 */
extern void TagBootJump(uint64_t pt_base, uint64_t stack, uint64_t entry);

static void __attribute__((noreturn)) JumpToKernel(uint64_t pt_base)
{
    TagBootJump(pt_base, g_stack_base, (uint64_t)KERNEL_LOAD_ADDR);
    __builtin_unreachable();
}

/*
 * CheckEfiLoadAddress — verify the EFI application is within our identity map.
 * On all practical OVMF systems (QEMU + real x86_64 hardware) the EFI app is
 * loaded below 4 GB.  If it is above 4 GB we cannot safely switch CR3 without
 * extending the identity map; panic early with a clear message.
 */
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
        Print("), identity map insufficient\r\n");
        Panic("EFI application above 4 GB — cannot switch CR3 safely");
    }
}

/* =========================================================================
 * UEFI entry point
 * ========================================================================= */

/*
 * TagBootMain — UEFI entry point.
 *
 * MUST be declared EFIAPI: UEFI firmware calls this with Microsoft x64 ABI
 * (image_handle in RCX, st in RDX).  Without EFIAPI the gcc-compiled function
 * reads System V registers RDI/RSI instead → garbage pointers → instant crash.
 */
EFI_STATUS EFIAPI TagBootMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    /* Raw COM1 byte BEFORE touching any UEFI pointer — confirms we were called. */
    Com1Init();
    Com1Str("\r\n[TAGBOOT] Entry reached\r\n");

    g_st           = st;
    g_bs           = st->boot_services;
    g_image_handle = image_handle;

    st->con_out->clear_screen(st->con_out);
    Print("TagBoot v2 — BoxOS UEFI Bootloader\r\n");
    Print("------------------------------------\r\n");

    /* ----- 1. Find Block IO ----- */
    Print("TagBoot: locating disk...\r\n");
    EFI_STATUS status = BlockIoFindDevice(image_handle);
    if (EFI_ERROR(status)) Panic("no Block IO device found");
    Print("TagBoot: disk found, block_size=");
    PrintDec(g_block_io->media->block_size);
    Print("\r\n");

    /* ----- 2. Read TagFS superblock ----- */
    Print("TagBoot: reading TagFS superblock...\r\n");
    status = TagFsReadSuperblock();
    if (EFI_ERROR(status)) Panic("TagFS superblock not found or invalid");
    Print("TagBoot: TagFS v");
    PrintDec(g_superblock.version);
    Print(", total_blocks=");
    PrintDec(g_superblock.total_blocks);
    Print("\r\n");

    uint32_t data_start_sector = TagFsDataStartSector();

    /* ----- 3. Find kernel file ----- */
    uint32_t kernel_start_block = 0;
    uint32_t kernel_block_count = 0;
    uint64_t kernel_file_size   = 0;

    int found_by_tag = TagFsFindKernelByTags(&kernel_start_block, &kernel_block_count,
                                              &kernel_file_size);
    if (!found_by_tag) {
        Print("TagBoot: tag search failed, trying boot hints...\r\n");
        uint32_t hint_data_start = 0;
        if (!TagFsBootHints(&kernel_start_block, &kernel_block_count,
                             &hint_data_start, &kernel_file_size)) {
            Panic("kernel not found — no matching tag and no boot hints in superblock");
        }
        if (hint_data_start != 0) data_start_sector = hint_data_start;
        Print("TagBoot: kernel from boot hints: block=");
        PrintDec(kernel_start_block);
        Print(", blocks=");
        PrintDec(kernel_block_count);
        Print("\r\n");
    }

    Print("TagBoot: kernel at data block ");
    PrintDec(kernel_start_block);
    Print(" (");
    PrintDec(kernel_block_count);
    Print(" blocks, data_start=");
    PrintDec(data_start_sector);
    Print(")\r\n");

    /* ----- 4. Load kernel ----- */
    Print("TagBoot: loading kernel to 0x100000...\r\n");
    uint64_t loaded_bytes = TagFsLoadKernel(data_start_sector,
                                             kernel_start_block,
                                             kernel_block_count,
                                             kernel_file_size);
    if (loaded_bytes == 0) Panic("kernel load failed");

    Print("TagBoot: kernel loaded (");
    PrintDec(loaded_bytes);
    Print(" bytes)\r\n");

    /* Validate kernel header magic — 'KERNEL' at byte offset 2 */
    if (!MemEqual((const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 2), "KERNEL", 6)) {
        Panic("kernel header magic invalid — wrong binary at load address");
    }
    Print("TagBoot: kernel header OK\r\n");

    uint64_t kernel_end_phys = KERNEL_LOAD_ADDR + loaded_bytes;

    /* ----- 5. Get GOP framebuffer ----- */
    FbInfo fb = QueryGopFramebuffer();

    /* ----- 6. Build E820 memory map ----- */
    Print("TagBoot: building memory map...\r\n");
    MemMapResult mmap;
    status = BuildMemoryMap(&mmap);
    if (EFI_ERROR(status)) Panic("failed to get UEFI memory map");

    /* ----- 7. Check EFI load address before switching CR3 ----- */
    CheckEfiLoadAddress();

    /* ----- 8. Set up page tables ----- */
    Print("TagBoot: setting up page tables...\r\n");
    status = SetupPageTables(kernel_end_phys);
    if (EFI_ERROR(status)) Panic("page table setup failed");

    /* ----- 9. Fill boot_info_t v2 (includes RSDP — must be before EBS) ----- */
    FillBootInfo(&fb, mmap.e820_count, kernel_end_phys);

    Print("TagBoot: boot_info at 0x9000 (v2, method=UEFI)\r\n");

    /* ----- 10. ExitBootServices ----- */
    Print("TagBoot: exiting boot services...\r\n");

    status = DoExitBootServices(image_handle);
    if (EFI_ERROR(status)) Panic("ExitBootServices failed after 3 attempts");

    /* ----- 11. Jump to kernel ----- */
    /* From here: no UEFI services, no Print(). Pure bare metal. */
    JumpToKernel(g_pt_base);
}
