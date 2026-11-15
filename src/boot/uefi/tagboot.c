/*
 * TagBoot — UEFI bootloader for BoxOS
 *
 * Boot sequence:
 *   1. Locate Block IO on boot device via loaded image device handle
 *   2. Read the medium's partition table for the ground claimed for BoxOS,
 *      and the Deed at the head of it
 *   3. Take up what the Deed says (tagboot_deed.c): a volume whose Deed bears
 *      no FRAME, or one that does not hold, is no book this loader boots, and
 *      its medium is passed over for the next one
 *   4. The Succession (tagboot_attest.c, tagboot_succession.c) — the kernel
 *      BOOT names, then the kernels the book names by role, each placed at
 *      0x100000 and held to the digests the book keeps of its blocks; the
 *      first attested is the one jumped into, one that does not match never is
 *   5. Query GOP framebuffer; record address and parameters
 *   6. Get UEFI memory map; convert to e820_entry_t format at 0x500
 *   7. Allocate 32 KB for page tables after kernel_end + guard page
 *   8. Build identity + higher-half page tables (2 MB pages)
 *   9. Fill boot_info_t (v2) at BOOT_INFO_ADDR
 *  10. ExitBootServices, then rebuild the E820 map from the map whose key
 *      the firmware accepted — the only one that describes this loader's
 *      own late allocations
 *  11. Install CR3, switch to the boot stack, jump to 0x100000
 *
 * What this loader does NOT do, and what said it did for a long time:
 * touch EFER. UEFI hands us a machine already in long mode (LME=1, PG=1,
 * PAE=1) and the kernel establishes every further EFER bit it needs — NXE
 * in CpuTakeUpNoExecute, SCE in per_core_setup_notify_msrs. Three comments
 * in this file used to claim EFER.LME+NXE was set here while
 * tagboot_jump.asm said the opposite and did nothing; the kernel's own NXE
 * write then landed after it had already mapped firmware pages with bit 63,
 * and two real boards panicked inside SetVirtualAddressMap with err=0x9.
 */

#include "tagboot.h"

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
#define BOOT_INFO_VERSION_V4  4U   /* current TagBoot output: system_table +
                                    * cfg_table + ESRT copy + EFI RT handoff */

/* Defensive ceiling on EFI Configuration Table entry count. Real firmware
 * publishes 6-20 entries (ACPI 1.0, ACPI 2.0, SMBIOS 2.1/3.0, ESRT, MPS,
 * SAL, debug-image-info, RT-properties, etc.). A count larger than this
 * indicates a corrupted system table; treat as "no entries" rather than
 * loop over uncontrolled memory. */
#define EFI_CFG_TABLE_MAX_ENTRIES  256U

/* KERNEL_LOAD_ADDR is tagboot.h's; KERNEL_HEADER_VERSION — the header layout
 * bootloader and kernel agree on, bumped when src/kernel/entry/
 * kernel_entry.asm changes its shape, a mismatch refused — and
 * KERNEL_MAX_SIZE are the Lectern's (tagboot_lectern.h), which holds every
 * kernel to them before it is jumped into. */
#define BOOT_INFO_ADDR        0xA000ULL

/* Same block, same address, third file to say so. The build passes the value
 * it wrote into the image layout; disagreeing with it means the kernel would
 * read this structure from somewhere TagBoot never wrote. */
#ifdef BOOT_INFO_ADDR_FROM_BUILD
_Static_assert(BOOT_INFO_ADDR == BOOT_INFO_ADDR_FROM_BUILD,
               "TagBoot writes boot_info somewhere the build does not expect");
#endif
#define E820_COUNT_ADDR       0x500ULL
#define E820_SIZE_ADDR        0x502ULL
#define E820_MAP_ADDR         0x504ULL
/* Must stay equal to E820_MAX_ENTRIES in src/kernel/core/memory/e820/e820.h
 * and in stage2.asm — all three describe the same bytes at the same address,
 * and the ceiling is stage1's scratch at 0x1200 (asserted in stage2.asm).
 * EmitE820 merges adjacent runs of the same type, which is what keeps a
 * two-hundred-descriptor UEFI map an order of magnitude below this. */
#define E820_MAX_ENTRIES      128U

/* Page-table layout (offsets relative to g_pt_base):
 *   +0x0000  PML4   (4 KB)
 *   +0x1000  PDPT   (4 KB)
 *   +0x2000  PD0..PD3  (16 KB) — identity map first 4 GB
 *   +0x6000  PDPT_high (4 KB) — kernel higher-half
 *   +0x7000  PD_high   (4 KB)
 *
 * 32 KB total. 4 GB identity covers every UEFI implementation observed in
 * the wild (spec recommends < 4 GB). If we ever encounter firmware that
 * loads the EFI image above 4 GB, QueryEfiImageEnd panics with a clear
 * message rather than silently mis-mapping. */
#define PAGE_TABLE_SIZE       0x8000ULL     /* 32 KB */
#define GUARD_PAGE_SIZE       0x1000ULL     /* 4 KB — guard between PT and stack */
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
 * boot_info_t v2 layout at BOOT_INFO_ADDR
 * Matches src/include/boot_info.h extended with v2 fields.
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    /* v1 fields (40 bytes) */
    uint32_t magic;           /* +0  BOOT_INFO_MAGIC */
    uint32_t version;         /* +4  4 for v4 UEFI boot */
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
    uint32_t total_size;      /* +36 140 for v4 */
    /* v2 additions (+40) */
    uint64_t fb_addr;         /* +40 GOP framebuffer physical address */
    uint32_t fb_width;        /* +48 */
    uint32_t fb_height;       /* +52 */
    uint32_t fb_stride;       /* +56 bytes per row */
    uint32_t fb_format;       /* +60 0=RGB, 1=BGR. Other layouts are refused
                               * because the kernel backend writes uint32_t
                               * pixels in exactly these two orders. */
    uint8_t  boot_method;     /* +64 1=UEFI */
    uint8_t  reserved_v2[3];  /* +65 */
    uint64_t rsdp_addr;       /* +68 ACPI RSDP physical address (0 if not found) */
    /* v3 additions — EFI runtime services handoff.
     * Layout MUST mirror src/include/boot_info.h exactly. */
    uint64_t efi_rt_services_phys; /* +76 EFI_RUNTIME_SERVICES* phys (0 if absent) */
    uint64_t efi_mmap_phys;        /* +84 copy of EFI memory map (EfiACPIMemoryNVS) */
    uint32_t efi_mmap_size;        /* +92 total bytes */
    uint32_t efi_mmap_desc_size;   /* +96 bytes per descriptor */
    uint32_t efi_mmap_desc_ver;    /* +100 descriptor version */
    uint32_t efi_fw_revision;      /* +104 firmware revision */
    /* v4 additions — EFI System Table + Configuration Table + ESRT copy. */
    uint64_t efi_system_table_phys; /* +108 EFI_SYSTEM_TABLE* phys */
    uint64_t efi_cfg_table_phys;    /* +116 EFI_CONFIGURATION_TABLE[] phys */
    uint32_t efi_cfg_table_count;   /* +124 count */
    uint64_t efi_esrt_copy_phys;    /* +128 NVS-preserved ESRT copy (0 if absent) */
    uint32_t efi_esrt_copy_size;    /* +136 bytes (0 if absent) */
} BootInfoV4;

_Static_assert(sizeof(BootInfoV4) == 140, "BootInfoV4 must be 140 bytes");

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

void MemCopy(void *dst, const void *src, size_t n)
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

/* Print a narrow ASCII string by widening to CHAR16 in a stack buffer,
 * then one output_string call. Pre-fix did one boot-service call per
 * character, which on serial console + heavy logging meant each line
 * cost ~80 round-trips through firmware. Batching is a ~10x reduction
 * in boot-time wall clock when the firmware funnels console through a
 * 115200-baud serial port (real HP/Lenovo enterprise default).
 *
 * Buffer is on stack so re-entrant from any context; sized for the
 * longest line we emit (~96 chars after rounding up). Strings longer
 * than the buffer are chunked. */
void Print(const char *msg)
{
    if (!g_st || !g_st->con_out || !msg) return;

    CHAR16 buf[129];   /* 128 chars + terminator */
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

void PrintHex64(uint64_t v)
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

void PrintDec(uint64_t v)
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

    /* Defensive: a sane firmware whose ConfigurationTable[] pointer is NULL
     * MUST report number_of_table_entries==0 (UEFI 2.10 §4.3). Real boards
     * are not always sane — refuse to deref NULL with non-zero count. */
    EFI_CONFIGURATION_TABLE *ct = g_st->configuration_table;
    if (!ct) return 0;

    /* Bound the iteration. A corrupt system table with a >256 entry count
     * (real firmware ships ~6-20) would otherwise have us scan arbitrary
     * memory. Capped scan returns whatever ACPI/SMBIOS/ESRT GUIDs we find
     * in the legitimate prefix and ignores the rest. */
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

/* =========================================================================
 * Block IO: read sectors from disk
 * ========================================================================= */

static EFI_BLOCK_IO_PROTOCOL *g_block_io = NULL;

/* =========================================================================
 * The ground, and the Deed on it
 *
 * This loader used to read a superblock at absolute sector 1034 of whatever
 * device it was looking at. Now it reads the medium's partition table for the
 * run claimed for BoxOS, and the Deed at the head of that run — the same two
 * steps, over the same bytes, that the kernel takes when it mounts the volume
 * (core/boardroom/ground.c and tagfs/deed/deed.c).
 * ========================================================================= */

/* Where the volume being read begins on the medium, in 512-byte sectors. The
 * one absolute address this loader holds, and it is discovered, never assumed.
 * Every ReadSectors below is counted from it. */
static uint64_t g_ground_start = 0;

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

/* cf8ae49a-d26a-4959-9a6f-71c932e0c9eb, in the order GPT stores a type GUID —
 * the same sixteen bytes ground.c compares against. */
static const uint8_t g_boxos_type_guid[16] = {
    0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
    0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb
};

static const uint8_t g_deed_magic[8] = {
    VOLUME_DEED_MAGIC_0, VOLUME_DEED_MAGIC_1, VOLUME_DEED_MAGIC_2,
    VOLUME_DEED_MAGIC_3, VOLUME_DEED_MAGIC_4, VOLUME_DEED_MAGIC_5,
    VOLUME_DEED_MAGIC_6, VOLUME_DEED_MAGIC_7
};

/* CRC-32 (ISO 3309), bitwise. The same sum GPT is specified in and the same
 * one a Deed carries, so one routine checks both. A table would be a kilobyte
 * of a loader that runs once. */
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

/* Read 512-byte logical sectors straight off a device, with no volume in the
 * picture — the partition table, and the probe that follows it. Handles a
 * device whose physical block size is not 512. */
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

    /* A whole physical block bounced through here, aligned to the 4096 every
     * medium BoxOS meets asks of a buffer at the most (EFI_BLOCK_IO_MEDIA
     * IoAlign, UEFI 2.10 §13.9: 0 or 1 any, else a power of two) — read_blocks
     * refuses a buffer off its IoAlign with EFI_INVALID_PARAMETER. */
    static uint8_t align_buf[8192] __attribute__((aligned(4096)));
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

/*
 * Is there a Deed at this sector, and is it the head of a volume?
 *
 * Same order of checks as the kernel's reader: magic, then the two lengths
 * bounded BEFORE anything is summed over them, then the checksum, then the
 * role. `out` gets the whole block so the stamps can be walked afterwards.
 */
static int DeedAt(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint32_t want_role,
                  uint8_t *out /* 4096 bytes */)
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

/*
 * Find the run of this device claimed for BoxOS that carries a readable Deed.
 *
 * MBR and GPT both, because a medium BoxOS was installed onto by somebody
 * else's partitioning tool is the ordinary case on a real machine — and
 * because a GPT that does not check out must be refused rather than half-read.
 *
 * On success, sets the ground and leaves the Deed's block in `deed_out`.
 */
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
            /* The head is gone; the copy at the far end of the ground is the
             * volume's own second opinion, and it says the same things. */
            uint64_t tail = start + ((count / 8) - 1) * 8;
            if (DeedAt(bio, tail, VOLUME_DEED_ROLE_TAIL, deed_out)) {
                Print("TagBoot: read the deed from the far end of the volume\r\n");
                *out_start = start; *out_sectors = count; return 1;
            }
        }
        return 0;
    }

    /* GPT. Both checksums, because a header that passes its own sum can still
     * point at an entry array that was interrupted mid-write. */
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

/* The block a medium's Deed is read into while the medium is probed;
 * DeedTakeUp copies out of it what the loader keeps. */
static uint8_t g_deed_block[4096];

/*
 * A medium in words a person can find it by: the device this loader came from
 * (`of` 0), or the one at `which` of the firmware's `of` Block IO handles —
 * and its size, and whether it comes out.
 */
static void SayMedium(EFI_BLOCK_IO_PROTOCOL *bio, UINTN which, UINTN of)
{
    if (of == 0) {
        Print("the device this loader came from");
    } else {
        Print("the medium at Block IO handle ");
        PrintDec(which + 1);
        Print(" of ");
        PrintDec(of);
    }
    Print(" (");
    PrintDec((((uint64_t)bio->media->last_block + 1) * bio->media->block_size) >> 20);
    Print(bio->media->removable_media ? " MiB, removable)" : " MiB, fixed)");
}

/*
 * Does this medium carry a book this loader boots? Its ground and Deed found
 * (FindGroundOn), the Deed taken up (DeedTakeUp). A volume that is no book —
 * an older volume, another system's data, a FRAME that does not hold — is said
 * in one line naming the medium and passed over: one such disk in a machine is
 * no reason not to boot the book on the next.
 */
static int BlockIoProbeVolume(EFI_BLOCK_IO_PROTOCOL *bio, UINTN which, UINTN of)
{
    if (!bio || !bio->media) return 0;
    if (bio->media->block_size == 0 || !bio->media->media_present) return 0;

    uint64_t start = 0, sectors = 0;
    if (!FindGroundOn(bio, g_deed_block, &start, &sectors)) return 0;
    if (EFI_ERROR(DeedTakeUp(g_deed_block, start))) {
        Print("; passed over: ");
        SayMedium(bio, which, of);
        Print("\r\n");
        return 0;
    }

    g_ground_start = start;
    return 1;
}

/*
 * Find the BlockIO handle whose volume is a book this loader boots.
 *
 * Strategy:
 *   1. Try the loaded-image device first (common case: booting from same disk
 *      that holds both ESP and TagFS, e.g. on real hardware with GPT layout).
 *   2. Scan every non-partition BlockIO handle, reading each one's partition
 *      table for BoxOS ground and each ground for a Deed (handles the QEMU
 *      two-drive setup and GPT/MBR alike).
 * A medium whose volume is no book is passed over, said, and the scan goes on
 * (BlockIoProbeVolume).
 *
 * The probe is safe: it reads a partition table and, at most, one 4 KiB block
 * from a disk that may have nothing of ours on it, and writes nothing.
 */
static EFI_STATUS BlockIoFindDevice(EFI_HANDLE image_handle)
{
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID block_io_guid     = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_HANDLE probed = NULL;
    EFI_STATUS status;

    Print("TagBoot: scanning for TagFS block device...\r\n");

    status = g_bs->handle_protocol(image_handle,
                                   &loaded_image_guid,
                                   (void **)&loaded_image);
    if (!EFI_ERROR(status) && loaded_image) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        probed = loaded_image->device_handle;
        status = g_bs->handle_protocol(loaded_image->device_handle,
                                       &block_io_guid,
                                       (void **)&bio);
        if (!EFI_ERROR(status) && BlockIoProbeVolume(bio, 0, 0)) {
            Print("TagBoot: the volume is on the device this loader came from\r\n");
            g_block_io = bio;
            return EFI_SUCCESS;
        }
    }

    /* The loaded-image device carries no book.
     * Enumerate all raw (non-partition) BlockIO handles and probe each. */
    Print("TagBoot: loaded-image device has no TagFS book, scanning all handles...\r\n");

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
        /* Probed already, and said if it was passed over: once is enough. */
        if (handles[i] == probed) continue;
        if (EFI_ERROR(g_bs->handle_protocol(handles[i], &block_io_guid, (void **)&bio)))
            continue;
        if (!bio || !bio->media)              continue;
        if (bio->media->logical_partition)    continue;  /* skip partitions */

        /* Hot-plugged removable media (USB sticks, SD cards) frequently
         * report media_present=0 on the first probe — the controller
         * hasn't latched the media-change yet. UEFI spec §13.9: issue a
         * Reset(verify=FALSE), then re-check. If still absent, skip. */
        if (!bio->media->media_present) {
            if (bio->reset) {
                (void)bio->reset(bio, FALSE);
            }
            if (!bio->media->media_present) continue;
        }

        if (BlockIoProbeVolume(bio, i, handle_count)) {
            g_block_io = bio;
            found = EFI_SUCCESS;
            break;
        }
    }

    g_bs->free_pool(handles);

    if (EFI_ERROR(found))
        Print("TagBoot: no medium here carries a book — a TagFS volume whose deed this loader reads and holds\r\n");

    return found;
}

/* Read 512-byte logical sectors of the VOLUME — counted from the start of its
 * ground, never from the start of the medium. The one place the base is
 * applied, which is what makes "nothing inside a volume is an absolute
 * address" hold here as well as in the kernel. */
static EFI_STATUS ReadSectors(uint64_t vlba, uint32_t count_512, void *buffer)
{
    if (!g_block_io) return EFI_NOT_READY;
    return ReadMediumSectors(g_block_io, g_ground_start + vlba, count_512, buffer);
}

/* =========================================================================
 * Kernel loading
 * ========================================================================= */

/* Where the pages the kernel was placed in END (tagboot_attest.c, AttestPlace,
 * asked for them; SuccessionTake says where they end). Page-aligned by
 * construction, and the first address past it is the first one the loader has
 * NOT already reserved — which is where the BSS reservation has to begin, for
 * two reasons at once: AllocateAddress refuses an unaligned address outright
 * (EDK2 CoreInternalAllocatePages returns EFI_NOT_FOUND from its granularity
 * check before it looks at the map at all), and asking again for a page this
 * loader already holds is a range that is allocated, which is the other thing
 * that answer means. */
static uint64_t g_kernel_alloc_end = 0;

/* =========================================================================
 * GOP framebuffer
 * ========================================================================= */

typedef struct {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;  /* 0=RGB, 1=BGR — the only two layouts we publish */
} FbInfo;

typedef struct {
    EFI_HANDLE handle;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
} GopSelection;

typedef struct {
    uint32_t width;
    uint32_t height;
    int      valid;
    int      native;
} EdidPreferredTiming;

typedef enum {
    GopChoiceCurrent,
    GopChoiceEdid,
    GopChoiceLargest,
    GopChoiceSmallest,
    GopChoiceEdidTooLarge,
    GopChoiceEdidUnavailable,
    GopChoiceUnavailable
} GopModeChoice;

typedef struct {
    uint32_t selected_mode;
    uint32_t largest_mode;
    uint32_t smallest_mode;
    uint32_t aspect_mode;
    uint64_t largest_area;
    uint64_t smallest_area;
    uint64_t aspect_area;
    uint32_t invalid_modes;
    int      found_selected;
    int      selected_matches_format;
    int      found_largest;
    int      found_smallest;
    int      found_aspect;
} GopModeCandidates;

/*
 * ── what one pixel of a GOP mode is, in the only two orders this loader can
 *    tell the kernel about
 *
 * UEFI 2.10 §12.9.2: PixelFormat says how a pixel is laid out, and only the
 * two 8-bit-per-colour formats are a fixed four bytes. PixelBitMask means
 * "read PixelInformation" — "This bit-mask is only valid if PixelFormat is
 * set to PixelPixelBitMask. A bit being set defines what bits are used for
 * what purpose such as Red, Green, Blue, or Reserved." PixelBltOnly means
 * there is no linear framebuffer at all, and anything at or past
 * PixelFormatMax is not a format.
 *
 * The kernel's framebuffer backend writes uint32_t pixels, so this contract
 * intentionally accepts only the two 32-bit layouts it can encode. Other
 * valid GOP modes remain usable by firmware, but are refused here rather than
 * published with a false stride or pixel format.
 */
static int GopDescribePixels(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                             uint32_t *out_format, uint32_t *out_pixel_bytes)
{
    if (!info) return 0;

    switch (info->pixel_format) {
        case PixelRedGreenBlueReserved8BitPerColor:
            *out_format = 0; *out_pixel_bytes = 4; return 1;
        case PixelBlueGreenRedReserved8BitPerColor:
            *out_format = 1; *out_pixel_bytes = 4; return 1;
        case PixelBitMask:
            break;
        default:
            return 0;   /* PixelBltOnly, and anything >= PixelFormatMax */
    }

    /* Masks are over the 32-bit pixel value; written little-endian, bits
     * 0..7 are byte 0. The reserved byte is required because the kernel
     * stores one complete uint32_t for every pixel. */
    const EFI_PIXEL_BITMASK *m = &info->pixel_info;
    if (m->green_mask != 0x0000FF00U || m->reserved_mask != 0xFF000000U)
        return 0;
    if (m->red_mask == 0x00FF0000U && m->blue_mask == 0x000000FFU) {
        *out_format = 1; *out_pixel_bytes = 4; return 1;   /* B,G,R,X in memory */
    }
    if (m->red_mask == 0x000000FFU && m->blue_mask == 0x00FF0000U) {
        *out_format = 0; *out_pixel_bytes = 4; return 1;   /* R,G,B,X in memory */
    }
    return 0;
}

static int GopModeInfoUsable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                             uint32_t *out_format, uint32_t *out_pixel_bytes);

/* EFI_CONSOLE_OUT_DEVICE_GUID — UEFI 2.10 §12.3, the tag the firmware puts on
 * the handle its own console output goes to. Defined here rather than in
 * uefi.h because this is the only question in this loader that needs it. */
#define EFI_CONSOLE_OUT_DEVICE_GUID \
    EFI_GUID_INIT(0xd3b36f2c,0xd551,0x11d4, 0x9a,0x46,0x00,0x90,0x27,0x3f,0xc1,0x4d)

/* Has this adapter ANY mode whose pixels this loader can hand over? The
 * current one first, because that is the one it will usually keep. */
static int GopHasUsableMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    if (!gop || !gop->mode) return 0;

    uint32_t fmt = 0, pixel_bytes = 0;
    if (gop->mode->info && GopModeInfoUsable(gop->mode->info, &fmt, &pixel_bytes))
        return 1;

    for (uint32_t m = 0; m < gop->mode->max_mode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->query_mode(gop, m, &size_of_info, &info))) continue;
        if (!info) continue;
        /* QueryMode's buffer is the caller's to release (UEFI 2.10 §12.9.2.1).
         * Before ExitBootServices every pool block left behind is one more
         * descriptor in the map this loader is about to convert. */
        int usable = GopModeInfoUsable(info, &fmt, &pixel_bytes);
        g_bs->free_pool(info);
        if (usable) return 1;
    }
    return 0;
}

/*
 * ── which GOP, on a machine that has more than one
 *
 * locate_protocol returns ONE handle and the firmware picks which. On
 * EDK2-derived firmware the ConSplitter installs a virtual GOP on the console
 * handle by default (PcdConOutGopSupport), and every mode of that GOP is
 * PixelBltOnly with FrameBufferBase == 0 in two states that exist on shipping
 * hardware: no physical GOP connected, and two or more of them — a server with
 * BMC video beside an add-in card, a desktop with iGPU and dGPU both attached.
 * Taking that handle means refusing a framebuffer the machine really has and
 * bringing the kernel up on VGA text, which on a UEFI class-3 board is a black
 * screen with nothing said.
 *
 * So every handle is asked, one with no describable mode at all is passed
 * over, and one that also carries EFI_CONSOLE_OUT_DEVICE — the device the
 * firmware's own console writes to, and therefore the display in front of the
 * operator — wins outright. This keeps the output identity explicit instead
 * of trusting whichever GOP a firmware happens to return first.
 */
static int GopFindPrimary(GopSelection *out)
{
    if (!out) return 0;
    out->handle = NULL;
    out->gop = NULL;

    EFI_GUID gop_guid    = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID conout_guid = EFI_CONSOLE_OUT_DEVICE_GUID;

    UINTN       handle_count = 0;
    EFI_HANDLE *handles      = NULL;
    EFI_STATUS  status = g_bs->locate_handle_buffer(EFI_BY_PROTOCOL, &gop_guid,
                                                    NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles || handle_count == 0) {
        /* The firmware will not enumerate its GOPs. One question is left. */
        EFI_GRAPHICS_OUTPUT_PROTOCOL *only = NULL;
        if (EFI_ERROR(g_bs->locate_protocol(&gop_guid, NULL, (void **)&only)))
            return 0;
        out->gop = only;
        return 1;
    }

    EFI_HANDLE first_handle = NULL;
    EFI_HANDLE conout_handle = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *first  = NULL;   /* first usable adapter */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *conout = NULL;   /* ...that is the console */

    for (UINTN i = 0; i < handle_count && !conout; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        if (EFI_ERROR(g_bs->handle_protocol(handles[i], &gop_guid, (void **)&gop)))
            continue;
        if (!GopHasUsableMode(gop)) continue;

        void *console = NULL;
        if (!EFI_ERROR(g_bs->handle_protocol(handles[i], &conout_guid, &console))) {
            conout = gop;
            conout_handle = handles[i];
        } else if (!first) {
            first = gop;
            first_handle = handles[i];
        }
    }

    g_bs->free_pool(handles);

    if (conout) {
        out->handle = conout_handle;
        out->gop = conout;
        return 1;
    }
    if (first) {
        out->handle = first_handle;
        out->gop = first;
        return 1;
    }

    /* Not one adapter here has a mode this loader can hand over. Answer with
     * whatever locate_protocol names anyway, so QueryGopFramebuffer prints
     * WHICH format it is refusing — that line is the whole diagnosis on a
     * board that comes up black, and it is worth more than a bare "none". */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *any = NULL;
    if (EFI_ERROR(g_bs->locate_protocol(&gop_guid, NULL, (void **)&any)))
        return 0;
    out->gop = any;
    return 1;
}

#define MIN_FB_WIDTH   640U
#define MIN_FB_HEIGHT  400U
#define MAX_FB_WIDTH   3840U
#define MAX_FB_HEIGHT  2160U

static int FbModeInBand(uint32_t w, uint32_t h)
{
    return (w >= MIN_FB_WIDTH  && w <= MAX_FB_WIDTH  &&
            h >= MIN_FB_HEIGHT && h <= MAX_FB_HEIGHT);
}

static int GopModeInfoUsable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                             uint32_t *out_format, uint32_t *out_pixel_bytes)
{
    if (!info || !GopDescribePixels(info, out_format, out_pixel_bytes)) return 0;
    if (info->horizontal_resolution == 0 || info->vertical_resolution == 0 ||
        info->pixels_per_scan_line < info->horizontal_resolution)
        return 0;
    return 1;
}

static uint64_t GopModeArea(uint32_t width, uint32_t height)
{
    return (uint64_t)width * (uint64_t)height;
}

static int GopModeMatchesAspect(uint32_t width, uint32_t height,
                                const EdidPreferredTiming *preferred)
{
    if (!preferred || !preferred->valid || width == 0 || height == 0) return 0;
    return (uint64_t)width * preferred->height ==
           (uint64_t)height * preferred->width;
}

static int ParseEdidPreferredTiming(const uint8_t *edid, uint32_t size,
                                    EdidPreferredTiming *out)
{
    if (!out) return 0;
    MemZero(out, sizeof(*out));
    if (!edid || size < 128) return 0;

    static const uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0x00 };
    if (!MemEqual(edid, header, sizeof(header))) return 0;

    uint32_t checksum = 0;
    for (uint32_t i = 0; i < 128; i++) checksum += edid[i];
    if ((checksum & 0xFFU) != 0) return 0;
    if (edid[18] != 1 || edid[19] < 3) return 0;

    /* The first detailed timing descriptor is at byte 54. A zero pixel clock
     * denotes a monitor descriptor rather than a video timing. */
    uint16_t pixel_clock = (uint16_t)edid[54] | ((uint16_t)edid[55] << 8);
    if (pixel_clock == 0) return 0;

    uint32_t width = (uint32_t)edid[56] |
                     ((uint32_t)(edid[58] & 0xF0U) << 4);
    uint32_t height = (uint32_t)edid[59] |
                      ((uint32_t)(edid[61] & 0xF0U) << 4);
    if (!width || !height) return 0;

    out->width = width;
    out->height = height;
    out->valid = 1;
    /* EDID 1.4 bit 1 says that the preferred timing includes native format
     * and refresh. Earlier revisions still define the first timing as the
     * preferred timing, but do not carry that native-confidence bit. */
    out->native = (edid[19] >= 4) ? ((edid[24] & 0x02U) != 0) : 1;
    return 1;
}

static int GopReadPreferredTiming(EFI_HANDLE handle, EdidPreferredTiming *out)
{
    if (!out) return 0;
    MemZero(out, sizeof(*out));
    if (!handle) return 0;

    EFI_GUID active_guid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
    EFI_GUID discovered_guid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;
    EFI_EDID_PROTOCOL *protocol = NULL;

    if (!EFI_ERROR(g_bs->handle_protocol(handle, &active_guid,
                                         (void **)&protocol)) && protocol &&
        ParseEdidPreferredTiming(protocol->edid, protocol->size_of_edid, out))
        return 1;

    protocol = NULL;
    if (!EFI_ERROR(g_bs->handle_protocol(handle, &discovered_guid,
                                         (void **)&protocol)) && protocol &&
        ParseEdidPreferredTiming(protocol->edid, protocol->size_of_edid, out))
        return 1;

    MemZero(out, sizeof(*out));
    return 0;
}

static const char *GopChoiceName(GopModeChoice choice)
{
    switch (choice) {
        case GopChoiceCurrent:    return "current-fallback";
        case GopChoiceEdid:       return "edid";
        case GopChoiceLargest:    return "largest-fallback";
        case GopChoiceSmallest:   return "smallest-fallback";
        case GopChoiceEdidTooLarge:return "edid-preferred-too-large";
        case GopChoiceEdidUnavailable:return "edid-preferred-unavailable";
        case GopChoiceUnavailable:return "unavailable";
        default:                  return "unknown";
    }
}

static void GopScanModes(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                         const EdidPreferredTiming *preferred,
                         uint32_t current_format,
                         GopModeCandidates *out)
{
    MemZero(out, sizeof(*out));
    out->selected_mode = gop->mode->mode;
    out->smallest_area = ~0ULL;

    for (uint32_t m = 0; m < gop->mode->max_mode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->query_mode(gop, m, &size_of_info, &info)) || !info) {
            out->invalid_modes++;
            continue;
        }

        uint32_t format = 0, bytes = 0;
        if (!GopModeInfoUsable(info, &format, &bytes)) {
            out->invalid_modes++;
            g_bs->free_pool(info);
            continue;
        }

        uint32_t width = info->horizontal_resolution;
        uint32_t height = info->vertical_resolution;
        uint64_t area = GopModeArea(width, height);

        if (preferred && preferred->valid &&
            width == preferred->width && height == preferred->height &&
            FbModeInBand(width, height)) {
            int matches_format = format == current_format;
            if (!out->found_selected ||
                (matches_format && !out->selected_matches_format)) {
                out->selected_mode = m;
                out->found_selected = 1;
                out->selected_matches_format = matches_format;
            }
        }

        if (area < out->smallest_area) {
            out->smallest_area = area;
            out->smallest_mode = m;
            out->found_smallest = 1;
        }
        if (FbModeInBand(width, height)) {
            if (!out->found_largest || area > out->largest_area) {
                out->largest_area = area;
                out->largest_mode = m;
                out->found_largest = 1;
            }
            if (GopModeMatchesAspect(width, height, preferred) &&
                (!out->found_aspect || area > out->aspect_area)) {
                out->aspect_area = area;
                out->aspect_mode = m;
                out->found_aspect = 1;
            }
        }
        g_bs->free_pool(info);
    }
}

static GopModeChoice SelectGopBestMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                       const EdidPreferredTiming *preferred,
                                       uint32_t *out_invalid_modes)
{
    if (out_invalid_modes) *out_invalid_modes = 0;
    if (!gop || !gop->mode) return GopChoiceUnavailable;

    uint32_t current_format = 0, current_bytes = 0;
    int current_usable = GopModeInfoUsable(gop->mode->info,
                                           &current_format, &current_bytes);
    GopModeCandidates candidates;
    GopScanModes(gop, preferred, current_format, &candidates);
    if (out_invalid_modes) *out_invalid_modes = candidates.invalid_modes;

    if (candidates.found_selected) {
        if (candidates.selected_mode == gop->mode->mode) return GopChoiceEdid;
        EFI_STATUS status = gop->set_mode(gop, candidates.selected_mode);
        int verified = !EFI_ERROR(status) && gop->mode &&
                       gop->mode->mode == candidates.selected_mode &&
                       GopModeInfoUsable(gop->mode->info, &current_format,
                                         &current_bytes);
        if (verified)
            return GopChoiceEdid;

        Print("TagBoot: GOP EDID mode change ");
        Print(EFI_ERROR(status) ? "failed status=" : "verification failed status=");
        PrintHex64((uint64_t)status);
        Print(" — keeping the firmware mode\r\n");
        return current_usable ? GopChoiceCurrent : GopChoiceUnavailable;
    }

    if (!preferred || !preferred->valid) {
        if (current_usable && FbModeInBand(gop->mode->info->horizontal_resolution,
                                           gop->mode->info->vertical_resolution))
            return GopChoiceCurrent;
    }

    uint32_t fallback_mode;
    GopModeChoice choice;
    if (candidates.found_aspect) {
        fallback_mode = candidates.aspect_mode;
        choice = GopChoiceLargest;
    } else if (candidates.found_largest) {
        fallback_mode = candidates.largest_mode;
        choice = GopChoiceLargest;
    } else if (candidates.found_smallest) {
        fallback_mode = candidates.smallest_mode;
        choice = (preferred && preferred->valid)
                   ? GopChoiceEdidUnavailable : GopChoiceSmallest;
    } else {
        return GopChoiceUnavailable;
    }

    if (preferred && preferred->valid) {
        if (!FbModeInBand(preferred->width, preferred->height))
            choice = GopChoiceEdidTooLarge;
        else if (choice == GopChoiceLargest || choice == GopChoiceSmallest)
            choice = GopChoiceEdidUnavailable;
    }

    if (fallback_mode == gop->mode->mode) return current_usable ? choice : GopChoiceUnavailable;
    EFI_STATUS status = gop->set_mode(gop, fallback_mode);
    int verified = !EFI_ERROR(status) && gop->mode &&
                  gop->mode->mode == fallback_mode &&
                  GopModeInfoUsable(gop->mode->info, &current_format,
                                    &current_bytes);
    if (!verified) {
        Print("TagBoot: GOP fallback mode change ");
        Print(EFI_ERROR(status) ? "failed status=" : "verification failed status=");
        PrintHex64((uint64_t)status);
        Print(" — keeping the firmware mode\r\n");
        return current_usable ? GopChoiceCurrent : GopChoiceUnavailable;
    }
    return choice;
}

static FbInfo QueryGopFramebuffer(void)
{
    FbInfo fb;
    MemZero(&fb, sizeof(fb));

    GopSelection selection;
    if (!GopFindPrimary(&selection) || !selection.gop || !selection.gop->mode) {
        Print("TagBoot: no display adapter here offers a linear framebuffer — "
              "the kernel comes up on text\r\n");
        return fb;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = selection.gop;
    uint32_t current_mode = gop->mode->mode;
    EdidPreferredTiming preferred;
    int have_edid = GopReadPreferredTiming(selection.handle, &preferred);
    uint32_t invalid_modes = 0;
    GopModeChoice choice = SelectGopBestMode(gop, have_edid ? &preferred : NULL,
                                             &invalid_modes);

    Print("TagBoot: GOP modes=");
    PrintDec(gop->mode->max_mode);
    Print(" current=");
    PrintDec(current_mode);
    Print(" selected=");
    PrintDec(gop->mode->mode);
    Print(" edid=");
    if (have_edid) {
        PrintDec(preferred.width);
        Print("x");
        PrintDec(preferred.height);
        Print(preferred.native ? " native" : " preferred");
    } else {
        Print("none");
    }
    Print(" choice=");
    Print(GopChoiceName(choice));
    Print(" invalid=");
    PrintDec(invalid_modes);
    Print("\r\n");

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->mode;
    if (!mode->info) return fb;

    uint32_t format      = 0;
    uint32_t pixel_bytes = 0;
    if (!GopModeInfoUsable(mode->info, &format, &pixel_bytes)) {
        Print("TagBoot: GOP pixel format ");
        PrintDec((uint64_t)mode->info->pixel_format);
        Print(" (masks r=");
        PrintHex64(mode->info->pixel_info.red_mask);
        Print(" g=");
        PrintHex64(mode->info->pixel_info.green_mask);
        Print(" b=");
        PrintHex64(mode->info->pixel_info.blue_mask);
        Print(" x=");
        PrintHex64(mode->info->pixel_info.reserved_mask);
        Print(") is not a layout this loader can hand over — no framebuffer, "
              "the kernel comes up on text\r\n");
        return fb;
    }

    uint64_t stride = (uint64_t)mode->info->pixels_per_scan_line * pixel_bytes;
    if (stride > 0xFFFFFFFFULL ||
        stride > (~0ULL / (uint64_t)mode->info->vertical_resolution)) {
        Print("TagBoot: GOP scan-line geometry overflows the framebuffer contract — "
              "the kernel comes up on text\r\n");
        return fb;
    }
    uint64_t span = stride * (uint64_t)mode->info->vertical_resolution;

    if (mode->frame_buffer_base == 0 || span == 0) {
        Print("TagBoot: GOP reports no framebuffer for its own mode — "
              "the kernel comes up on text\r\n");
        return fb;
    }

    /*
     * UEFI 2.10 §12.9.2 defines FrameBufferSize as PixelsPerScanLine ×
     * VerticalResolution × PixelElementSize — the one number the firmware
     * publishes that can catch a pixel size this loader derived wrongly.
     *
     * A non-zero FrameBufferSize is authoritative for every pixel format.
     * Zero is permitted by older firmware, so it cannot be used as a reason
     * to reject an otherwise valid mode.
     */
    if (mode->frame_buffer_size != 0 &&
        span > (uint64_t)mode->frame_buffer_size) {
        Print("TagBoot: GOP's aperture is smaller than the scan lines it "
              "describes — refusing the framebuffer\r\n");
        return fb;
    }
    if (mode->frame_buffer_base > (~0ULL - span)) {
        Print("TagBoot: GOP framebuffer address overflows its visible span — "
              "the kernel comes up on text\r\n");
        return fb;
    }

    fb.addr   = mode->frame_buffer_base;
    fb.width  = mode->info->horizontal_resolution;
    fb.height = mode->info->vertical_resolution;
    fb.stride = (uint32_t)stride;
    fb.format = format;

    /* The order and the scan line go on the line as well. They are what this
     * loader decided rather than read, and a wrong one shows as inverted
     * colours or diagonal text with nothing else on screen to explain it. */
    Print("TagBoot: GOP ");
    PrintDec(fb.width);
    Print("x");
    PrintDec(fb.height);
    Print(" fb=");
    PrintHex64(fb.addr);
    Print(" stride=");
    PrintDec(fb.stride);
    Print(fb.format == 0 ? " order=RGBX\r\n" : " order=BGRX\r\n");

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

/* Set by EmitE820 when a map did not fit the table at 0x500. Read after BOTH
 * calls, and each call answers only for its own map: the pre-EBS one over the
 * firmware console, the post-EBS one over COM1, whose ports this loader drives
 * itself and which is the one voice that outlives ExitBootServices. */
static int g_e820_truncated = 0;

/*
 * EmitE820 — write one UEFI memory map out as the E820 table at 0x500.
 *
 * Called twice, and the second call is the point of the whole thing.
 *
 * ‼ The first call happens while boot services are still up, and the map it
 * describes is ALREADY STALE by the time the kernel reads it: between that
 * call and ExitBootServices this loader asks the firmware's allocator for
 * the ESRT copy and for the buffer the final memory map is staged in, and
 * both go through AllocateAnyPages — which lets the firmware pick, and EDK2
 * picks top-down out of high conventional memory. In the E820 the kernel
 * received, those pages were still CONVENTIONAL, therefore USABLE, therefore
 * handed to the buddy allocator (pmm.c) and available to be given away and
 * written over. The kernel then walks that same EFI map at runtime to call
 * SetVirtualAddressMap. Nothing would say a word: a count is still a count.
 *
 * So it is called again from PostEbsPatchBootInfo, on the map whose key
 * ExitBootServices ACCEPTED. That map is the only description of memory that
 * includes this loader's own last allocations, and it is by definition
 * current — the firmware would have refused the key otherwise.
 *
 * Adjacent runs of the same E820 type are merged. A UEFI map is far more
 * granular than E820 needs (Linux merges for the same reason —
 * efi_merge_regions), and merging is what keeps a 200-descriptor server map
 * from meeting the ceiling at all. Nothing is lost: the kernel's EFI runtime
 * driver reads the FULL descriptor list from boot_info->efi_mmap_phys, not
 * from here, so per-descriptor attributes still reach the code that needs
 * them.
 *
 * Returns the number of entries written.
 */
static uint32_t EmitE820(const uint8_t *map, uint64_t map_size, uint64_t desc_size)
{
    /* Volatile byte pointer with manual offsets: casting a raw physical
     * address to a typed array fires -Warray-bounds on known-good memory. */
    volatile uint8_t *e820_raw = (volatile uint8_t *)(uintptr_t)E820_MAP_ADDR;
    uint32_t count = 0;

    /* The run currently open, held back one entry so the next descriptor can
     * still extend it. count is the number already WRITTEN. */
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

        if (length == 0) continue;   /* a zero-page descriptor describes nothing */

        if (have_run && run_type == type && run_base + run_length == base) {
            run_length += length;
            continue;
        }

        if (have_run) {
            if (count >= E820_MAX_ENTRIES) { truncated = 1; break; }
            volatile uint8_t *slot = e820_raw + count * sizeof(E820Entry);
            /* ACPI 3.0 extended attributes (E820 entry +20):
             *   bit 0 = "valid" (1 = use this entry, 0 = ignore)
             *   bit 1 = "non-volatile"
             * Every UEFI descriptor we copy is valid by definition — the
             * firmware just told us about it — so bit 0 is set. Kernels that
             * ignore ACPI 3.0 ignore the field; kernels that use it to filter
             * stale BIOS-style entries need to see it. */
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

    /* A truncated memory map is not a smaller memory map — it is a map that
     * stops describing memory partway through, and whatever it stopped before
     * is unclaimed as far as the kernel is concerned. If it ever happens it
     * has to be said, and it can only be said before ExitBootServices. */
    g_e820_truncated = truncated;
    return count;
}

/* =========================================================================
 * EFI runtime handoff state
 *
 * Captured pre-ExitBootServices and forwarded to the kernel via
 * boot_info v4. The kernel uses this to invoke SetVirtualAddressMap
 * (UEFI §8.4) and call runtime services like ResetSystem, plus walk
 * the surviving Configuration Table for SMBIOS / ESRT / RT properties.
 * ========================================================================= */
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

/* =========================================================================
 * EFI System Resource Table capture (UEFI 2.10 §23.4)
 *
 * ESRT lives in EfiBootServicesData per spec; after EBS the OS PMM treats
 * those pages as USABLE and may overwrite them at any point. To preserve
 * the firmware-published resource list we copy the ESRT (header + every
 * EFI_SYSTEM_RESOURCE_ENTRY) into a fresh EfiACPIMemoryNVS allocation
 * before EBS. PMM never reclaims ACPI_NVS so the kernel-side ESRT driver
 * sees a stable image.
 * ========================================================================= */

#define ESRT_FW_RESOURCE_VERSION  1ULL    /* UEFI 2.10 §23.4 fixed */
#define ESRT_ENTRY_SIZE           40U     /* sizeof(EFI_SYSTEM_RESOURCE_ENTRY) */

#define EFI_SYSTEM_RESOURCE_TABLE_GUID \
    EFI_GUID_INIT(0xb122a263,0x3661,0x4f68, 0x99,0x29,0x78,0xf8,0xb0,0xd6,0x21,0x80)

typedef struct __attribute__((packed)) {
    uint32_t fw_resource_count;
    uint32_t fw_resource_count_max;
    uint64_t fw_resource_version;
} EsrtHeader;

_Static_assert(sizeof(EsrtHeader) == 16, "ESRT header must be 16 bytes");

/* Locate ESRT pointer + total size from the Configuration Table.
 * Returns 0 if not found. The pointer is a physical address; the table
 * is still in EfiBootServicesData here, valid only until EBS. */
static uint64_t FindEsrt(uint32_t *out_size)
{
    *out_size = 0;
    if (!g_st || g_st->number_of_table_entries == 0) return 0;

    EFI_CONFIGURATION_TABLE *ct = g_st->configuration_table;
    if (!ct) return 0;   /* same defense as FindAcpiRsdp */

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
        /* Defensive cap: 1024 entries (=40 KB) is wildly more than any
         * board publishes; rejects a corrupted table that would otherwise
         * overflow our NVS copy. */
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

/* Copy ESRT to fresh EfiACPIMemoryNVS pages. Returns physical address of
 * the copy, or 0 on failure. Idempotent in the sense that a subsequent
 * call would just consume another arena — but TagBootMain calls this once.
 *
 * The ESRT here still resides in EfiBootServicesData and therefore must be
 * fully read out before EBS. We do this immediately after FindEsrt. */
static uint64_t CopyEsrtToNvs(uint64_t esrt_phys, uint32_t esrt_size)
{
    if (esrt_phys == 0 || esrt_size == 0) return 0;
    if (esrt_size > 65536U) return 0;   /* 64 KB cap — same reason as above */

    UINTN pages = (esrt_size + PAGE_4KB - 1) / PAGE_4KB;
    /* Below 4 GB, deliberately. AllocateAnyPages lets the firmware answer
     * from anywhere it likes, and EDK2 answers top-down — on a machine with
     * memory above 4 GB that is where these pages land. The kernel reads
     * this copy before it has built page tables of its own, through the
     * loader's identity map, and that map covers the low 4 GB. Asking for a
     * ceiling costs nothing and removes the one address range where the
     * answer would be unreachable. */
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

/*
 * ── the pages the kernel will be told to read, asked for before they are written
 *
 * Three fixed addresses below 64 KB carry everything this loader tells the
 * kernel: the E820 map at 0x500, boot_info at 0xA000, the boarding pass at
 * 0xA600. All three were written into memory this loader had never asked the
 * firmware for.
 *
 * ‼ AND THE WINDOW IS NOT THEORETICAL. Between BuildMemoryMap writing E820 at
 * 0x504 and ExitBootServices, this loader itself calls the firmware's
 * allocator at least three more times — page tables, the ESRT copy, the memory
 * map buffer — and two of those are AllocateAnyPages, which lets the firmware
 * choose. A firmware that satisfies them from low conventional memory hands
 * out the very pages the E820 map is sitting in, and the kernel then reads a
 * memory map with somebody else's data in it. Nothing anywhere would say so:
 * the count at 0x500 would still look like a count.
 *
 * So they are asked for, once, before anything is written into them — which is
 * what AllocateAddress is for (UEFI 2.10 §7.2). Page granularity means three
 * pages cover all three structures:
 *
 *   0x0000  E820 count (0x500), entry size (0x502), start of the map (0x504)
 *   0x1000  the tail of that map — 128 entries of 24 bytes end at 0x1104
 *   0xA000  boot_info v4 (140 bytes) and the boarding pass (512 at 0xA600)
 *
 * ‼ WHAT THIS DOES NOT DO IS free_pages FIRST — and neither does any other
 * AllocateAddress site in this file. There are three of them (the kernel image,
 * the page tables + stack, the BSS tail) and all three used to free the range
 * the firmware had just refused them, two of them behind a guard on
 * EFI_NOT_FOUND. That guard read FreePages' EFI_NOT_FOUND ("The requested
 * memory pages were not allocated with AllocatePages()", UEFI 2.10 §7.2.2)
 * into a status that came back from AllocatePages, where §7.2.1 says only
 * "The requested pages could not be found" — and where EDK2 returns it
 * precisely BECAUSE somebody holds the range ("the pages to allocate have
 * been allocated", CoreConvertPagesEx). The guard admitted exactly the
 * dangerous case. Handing firmware memory back is not a retry; it is a
 * corruption with a spec section against it, and no status code tells this
 * loader's own pages apart from the firmware's.
 *
 * A refusal is not fatal either. The addresses are a contract with the kernel
 * and with stage2 — they cannot move — so the loader writes there regardless
 * and the machine goes on booting exactly as well as it did before this
 * function existed. What changes is that it SAYS which page it could not get,
 * on a board where that is the difference between a mystery and a sentence.
 */
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

    uint32_t count = EmitE820(map_buf, map_size, desc_size);

    /* The pool is ours and its only purpose was this conversion. Freeing it
     * here rather than leaking it to ExitBootServices removes one descriptor
     * from every map the firmware builds after this point — and one more
     * reason for the map key to move under the EBS retry loop. */
    g_bs->free_pool(map_buf);
    map_buf = NULL;

    out->map_size     = map_size;
    out->map_key      = map_key;
    out->desc_size    = desc_size;
    out->desc_version = desc_version;
    out->map_buf      = NULL;
    out->e820_count   = count;

    /* The ceiling goes on the line too. It is what turns "126" into a fact an
     * operator can act on, and it is the only headroom figure anyone gets:
     * the map rebuilt after ExitBootServices is a different map, and this one
     * does not bound it. */
    Print("TagBoot: memory map ");
    PrintDec(count);
    Print(" of ");
    PrintDec(E820_MAX_ENTRIES);
    Print(" entries\r\n");

    return EFI_SUCCESS;
}

/*
 * DoExitBootServices — proper retry loop per UEFI 2.10 §7.4.6.
 *
 * Strategy per attempt:
 *   1. Allocate a sized scratch buffer for the upcoming GetMemoryMap.
 *      We use EfiACPIMemoryNVS so the buffer is treated as ACPI NVS by
 *      the kernel's PMM (never reclaimed), letting it survive EBS and
 *      still be referenced via boot_info → kernel-side EFI RT driver.
 *   2. GetMemoryMap with that buffer to obtain a *fresh* key that
 *      matches the post-allocation memory layout.
 *   3. ExitBootServices(image_handle, key).
 *
 * If step 3 returns EFI_INVALID_PARAMETER the key is stale (some firmware
 * event allocated memory after step 2). Free the scratch and retry.
 *
 * On success we forward the staged map pointer + sizes to the kernel via
 * boot_info v3 so the kernel can walk RUNTIME descriptors for
 * SetVirtualAddressMap (UEFI 2.10 §8.4).
 *
 * 10 retries — spec only requires 1, but slow enterprise boards (HP Z,
 * Lenovo ThinkPad certain BIOS revs) fire 4-5 memory-allocating events
 * between GetMemoryMap and ExitBootServices while RT virtualisation
 * tables get rearranged; 3 has been observed insufficient there.
 */
static EFI_STATUS DoExitBootServices(EFI_HANDLE image_handle)
{
    EFI_STATUS last_status = EFI_ABORTED;

    for (UINTN attempt = 0; attempt < 10; attempt++) {
        UINTN                map_size = 0;
        UINTN                key      = 0;
        UINTN                desc_sz  = 0;
        uint32_t             desc_ver = 0;
        EFI_PHYSICAL_ADDRESS buf_pa   = 0;

        /* First call: get required buffer size (EFI_BUFFER_TOO_SMALL expected). */
        g_bs->get_memory_map(&map_size, NULL, &key, &desc_sz, &desc_ver);
        /* Slack for allocations we (and firmware) may do between this sizing
         * call and ExitBootServices:
         *   - the AllocatePages below splits the conventional-memory region
         *     containing our buffer into two descriptors;
         *   - many enterprise firmwares (HP Z, Lenovo, Insyde-based servers)
         *     fire 4-8 memory-allocating events per attempt while TPL
         *     timers and RT-virtualisation tables get rearranged;
         *   - QEMU OVMF can fire CPU-add / hot-plug events on multi-AP boot
         *     before ExitBootServices completes.
         * Linux uses EFI_MMAP_SLACK_DESCS for the same reason. 32 was
         * observed insufficient on real HP Z440 firmware (audit 2026-05-31). */
        #define EFI_MMAP_SLACK_DESCS  256U
        map_size += desc_sz * EFI_MMAP_SLACK_DESCS;

        /* Round up to whole pages — AllocatePages is page-granular. */
        UINTN pages = (map_size + PAGE_4KB - 1) / PAGE_4KB;

        /* Allocate as EfiACPIMemoryNVS: the kernel sees this region as
         * E820 ACPI_NVS and PMM never reclaims it (real-HW carve in PMM
         * follows the E820 type strictly). This is critical: the memory
         * must outlive ExitBootServices because the kernel-side EFI RT
         * driver walks the staged map at runtime. EfiLoaderData would
         * survive EBS too, but PMM treats it as USABLE and frees it. */
        /* Below 4 GB — same reason as the ESRT copy above: the kernel walks
         * this map through the loader's identity window, which ends at 4 GB,
         * and PmmReserveFirmwareMemory reads it before vmm_init has run. */
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

        /* Second call: get the fresh key + map into our NVS buffer. */
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

        /* Publish the map pointer + parameters BEFORE the call so even
         * a misbehaving firmware that "succeeds" but rewrites parts of
         * memory in flight still leaves the kernel a usable description.
         * On retry these get overwritten by the next attempt. */
        g_efi_mmap_copy_phys = (uint64_t)buf_pa;
        g_efi_mmap_copy_size = (uint32_t)map_size;
        g_efi_mmap_desc_size = (uint32_t)desc_sz;
        g_efi_mmap_desc_ver  = desc_ver;

        /* Exit — do NOT free buf; freeing would change the key. */
        s = g_bs->exit_boot_services(image_handle, key);
        if (!EFI_ERROR(s)) return EFI_SUCCESS;

        /* ExitBootServices failed — typically EFI_INVALID_PARAMETER (0x80000002)
         * because something (TPL change, async event) bumped the map key
         * between GetMemoryMap and our call. Other codes worth seeing on
         * console for triage on real boards: EFI_OUT_OF_RESOURCES (0x80000009)
         * indicates firmware is starved and may need a power cycle. Logging
         * the status hex lets us tell them apart without a debugger. */
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

/*
 * How many 2 MB identity pages to map: all of them, the whole low 4 GB.
 *
 * This used to be derived from the top of USABLE memory, which is the wrong
 * question by one word. What has to be reachable through these tables is not
 * where RAM ends — it is where anything the early kernel touches lives, and
 * on a UEFI machine the two are different: the GOP framebuffer, the local
 * APIC, ECAM and every runtime-services region sit ABOVE the last byte of
 * RAM, in the hole between it and 4 GB. A board with 2 GB of RAM and a
 * framebuffer at 0xE0000000 got an identity map that stopped at 2 GB, and
 * the kernel's first pixel was a page fault with no page tables of its own
 * yet to report it.
 *
 * The cost of asking the wide question is zero: PD0..PD3 are allocated
 * unconditionally either way (16 KB of the 32 KB block), and 2048 entries is
 * exactly what fills them.
 *
 * Mapping MMIO write-back here is harmless, but for a narrower reason than
 * "UC wins", which is what this note used to say under a section number that
 * names a table column rather than a section. The MTRR type does not simply
 * beat the page's: Intel SDM Vol 3A §11.5.2.2, "Selecting Memory Types for
 * Pentium III and More Recent Processor Families", Table 11-7, gives MTRR UC
 * an effective type of UC against PAT entries UC, UC-, WT, WB and WP — and
 * WC against a PAT entry of WC. A page that asked for write-combining would
 * override the firmware's UC.
 *
 * These entries ask for nothing. They carry neither PAT nor PCD nor PWT,
 * which selects PAT entry 0 ("Selecting a Memory Type from the PAT",
 * Table 11-11), whose type after power-up or reset is WB ("Programming the
 * PAT", Table 11-12) and which this loader never rewrites — it issues no
 * WRMSR at all, here or anywhere else. WB against a UC MTRR is UC, so the
 * firmware's MTRRs do decide these ranges.
 *
 * The one thing not established in this tree is the PAT the firmware hands
 * over. Every entry value but WC lands on UC anyway, and PAT0 is the type
 * ordinary RAM gets on a page with no attribute bits, so a firmware setting
 * it to WC would have mis-typed its own memory long before this line ran.
 * Nothing here may start setting PAT/PCD/PWT without revisiting that.
 *
 * These tables live only until vmm_init builds the kernel's own.
 */
static uint32_t CalculateIdentityMapPages(void)
{
    return 2048;   /* 2048 x 2 MB = 4 GB */
}

static EFI_STATUS SetupPageTables(uint64_t kernel_phys_end)
{
    /* pt_base after kernel, page-aligned, plus guard. */
    uint64_t aligned_end = (kernel_phys_end + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
    g_pt_base   = aligned_end + GUARD_PAGE_SIZE;
    g_stack_base = g_pt_base + PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE;

    /* Allocate contiguous region: page tables (32 KB) + guard (4 KB) + stack (64 KB).
     * All three must be reserved so UEFI firmware does not use them before
     * ExitBootServices.  The guard page is included in the allocation but left
     * unmapped — it exists only as a buffer between page tables and stack. */
    uint64_t total_alloc = PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE;
    EFI_PHYSICAL_ADDRESS pt_alloc = (EFI_PHYSICAL_ADDRESS)g_pt_base;
    UINTN alloc_pages = (UINTN)(total_alloc / PAGE_4KB);

    EFI_STATUS status = g_bs->allocate_pages(AllocateAddress,
                                             EfiLoaderData,
                                             alloc_pages,
                                             &pt_alloc);
    if (EFI_ERROR(status)) {
        /*
         * ‼ NOTHING IS FREED HERE, and there is no retry.
         *
         * This range begins one guard page past the kernel's physical end,
         * which is past everything this loader has ever asked for — the BSS
         * reservation stops at that end. So a refusal is somebody else's
         * range, and EDK2's CoreInternalFreePages would release it without
         * one ownership check, putting firmware memory into the pool this
         * loader is about to hand the kernel as free RAM.
         *
         * The guard this replaces tested for EFI_NOT_FOUND, reading
         * FreePages' meaning of that code (UEFI 2.10 §7.2.2) into a status
         * that came back from AllocatePages. §7.2.1 gives it the opposite
         * sense, and EDK2 returns it exactly when the pages ARE allocated to
         * someone. The guard fired only in the case it was written to stop.
         */
        Print("TagBoot: cannot allocate page tables + stack at ");
        PrintHex64(g_pt_base);
        Print(" (status=");
        PrintHex64((uint64_t)status);
        Print(") — that range is held by something else\r\n");
        return status;
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
 * The Boarding Pass — what this loader tells the kernel about its own arrival
 *
 * Mirrors src/include/boarding_pass.h. Duplicated rather than included for the
 * same reason BootInfoV4 above is: this is a freestanding UEFI application
 * built against its own headers, and the agreement is enforced by the build
 * passing the address in and by the kernel's own static assertions on the
 * layout — not by hoping two copies of a comment stay in step.
 *
 * The stamp that matters is the volume. A kernel in long mode cannot ask the
 * firmware which device it was booted from, so without this the Boardroom
 * picks by a rule — "the removable medium wins" — which is wrong on any
 * machine carrying BoxOS on an internal disk with a flash drive in a socket.
 * This loader knows the answer: it has the volume's Deed in hand.
 * ========================================================================= */
#define BOARDING_PASS_ADDR      0xA600ULL
#define BOARDING_PASS_BYTES     512U
#define BOARDING_PASS_MAGIC     0x53415042U   /* "BPAS" */
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

/* Append one stamp. The kind and the length go down first, then the payload,
 * then the write cursor moves to the next four-byte boundary — which is the
 * whole of what lets a kernel walk a pass carrying stamps it has never been
 * taught about. */
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

    /* The volume this kernel was read out of, straight out of the Deed
     * this loader verified on the way in. */
    at = BoardingStamp(base, at, BOARDING_STAMP_VOLUME,
                       g_volume_uuid, 16);
    count++;

    /* How it was reached. UEFI has no BIOS drive number, and saying 0xFF is
     * saying so rather than leaving a zero that reads as drive zero. */
    struct __attribute__((packed)) {
        uint8_t firmware; uint8_t bios_drive; uint16_t reserved;
    } medium = { BOARDING_FIRMWARE_UEFI, 0xFF, 0 };
    at = BoardingStamp(base, at, BOARDING_STAMP_MEDIUM, &medium, sizeof(medium));
    count++;

    /* Who wrote it. On a board that boots through CSM one week and UEFI the
     * next, that is a real question with nothing else on the screen to answer
     * it. */
    struct __attribute__((packed)) {
        char name[12]; uint16_t major; uint16_t minor;
    } loader = { { 'T','a','g','B','o','o','t',0,0,0,0,0 }, 1, 0 };
    at = BoardingStamp(base, at, BOARDING_STAMP_LOADER, &loader, sizeof(loader));
    count++;

    /*
     * The seal, and it goes on last because it covers everything ahead of it.
     *
     * Its four-byte kind/length pair is written here; the sum that fills its
     * payload cannot be taken until the HEADER is final, because the header is
     * part of what it covers. So: reserve the stamp, finish the header with
     * the seal counted in, then sum.
     */
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

    /* Every byte ahead of the seal's payload: the header, the stamps, and the
     * seal's own kind/length pair. Nothing sums itself. */
    uint32_t covered = (uint32_t)seal_at + 4U;   /* past its kind/length */
    sum = Crc32(base, covered);
    MemCopy(base + covered, &sum, sizeof(sum));
}

/* =========================================================================
 * boot_info_t v2 population
 * ========================================================================= */

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
    bi->boot_drive     = 0xFF;   /* UEFI has no BIOS drive number */
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
    bi->boot_method    = 1;   /* UEFI */
    bi->reserved_v2[0] = 0;
    bi->reserved_v2[1] = 0;
    bi->reserved_v2[2] = 0;
    bi->rsdp_addr      = FindAcpiRsdp();

    /* v3 fields. g_efi_mmap_* are populated inside DoExitBootServices —
     * FillBootInfo runs BEFORE EBS so we publish placeholders here and
     * the EBS retry-loop updates them in-place via the same fixed
     * BOOT_INFO_ADDR. After EBS returns we patch the final values in
     * TagBootMain (see PostEbsPatchBootInfo). */
    bi->efi_rt_services_phys = g_efi_rt_services_phys;
    bi->efi_mmap_phys        = 0;   /* set by post-EBS patch */
    bi->efi_mmap_size        = 0;
    bi->efi_mmap_desc_size   = 0;
    bi->efi_mmap_desc_ver    = 0;
    bi->efi_fw_revision      = g_efi_fw_revision;

    /* v4 fields. system_table + config_table are captured at TagBootMain
     * entry — they survive EBS because both live in EfiRuntimeServicesData.
     * efi_esrt_copy_* are populated by the pre-EBS ESRT capture path; zero
     * if the firmware did not publish an ESRT. */
    bi->efi_system_table_phys = g_efi_system_table_phys;
    bi->efi_cfg_table_phys    = g_efi_cfg_table_phys;
    bi->efi_cfg_table_count   = g_efi_cfg_table_count;
    bi->efi_esrt_copy_phys    = g_efi_esrt_copy_phys;
    bi->efi_esrt_copy_size    = g_efi_esrt_copy_size;
}

/* After ExitBootServices succeeds we know the FINAL memory map staged in
 * EfiACPIMemoryNVS; patch the still-mapped boot_info at BOOT_INFO_ADDR.
 * No UEFI services are valid here — pure memory writes. */
static void PostEbsPatchBootInfo(void)
{
    BootInfoV4 *bi = (BootInfoV4 *)(uintptr_t)BOOT_INFO_ADDR;
    bi->efi_mmap_phys      = g_efi_mmap_copy_phys;
    bi->efi_mmap_size      = g_efi_mmap_copy_size;
    bi->efi_mmap_desc_size = g_efi_mmap_desc_size;
    bi->efi_mmap_desc_ver  = g_efi_mmap_desc_ver;

    /* And the E820 the kernel will actually divide memory by is rewritten
     * from the accepted map — see the note on EmitE820. The map built before
     * ExitBootServices did not know about this loader's own last two
     * allocations (the ESRT copy and the buffer this very map is staged in),
     * both of which the firmware placed wherever it liked; the kernel's
     * allocator would have found them free and given them away. */
    if (g_efi_mmap_copy_phys && g_efi_mmap_copy_size && g_efi_mmap_desc_size) {
        uint32_t n = EmitE820((const uint8_t *)(uintptr_t)g_efi_mmap_copy_phys,
                              g_efi_mmap_copy_size,
                              g_efi_mmap_desc_size);
        bi->e820_count = (uint16_t)n;

        /*
         * ‼ And if THIS map did not fit, it has to be said here — not left to
         * the warning printed before ExitBootServices, which answered for a
         * different map and could not have known about this one.
         *
         * Boot services are gone and Print() is gone with them. COM1 is not:
         * those are port writes this loader does itself, the same ones that
         * announced its entry before g_st was even read. The UART is
         * re-programmed first because the firmware's own console driver owned
         * it in between and may have left it at another divisor.
         *
         * The count is not printed because there is only one value it can
         * have: EmitE820 stops at E820_MAX_ENTRIES, so a cut map is a full
         * table by construction. What is not known — and is the whole of the
         * damage — is how much memory lay past the cut.
         */
        if (g_e820_truncated) {
            Com1Init();
            Com1Str("\r\n[TAGBOOT] the final memory map did not fit the E820 "
                    "table: the entries past the cut are gone, and memory they "
                    "described is neither usable to the kernel nor carved out "
                    "of what it will hand away\r\n");
        }
    }
}

/* =========================================================================
 * Kernel handoff: install our tables and jump
 *
 * After ExitBootServices we have no UEFI services. We are ALREADY in
 * 64-bit long mode — UEFI spec §2.3.4 mandates LME=1, PG=1, PAE=1 for an
 * x64 firmware — so there is no mode switch to perform. All that is left:
 *   1. Enforce CR4.PAE (already set; cheap insurance)
 *   2. Load our page table base into CR3
 *   3. Switch RSP to the boot stack and jump
 *
 * EFER is deliberately NOT touched here. Every bit the kernel needs beyond
 * what firmware left is the kernel's own business, and it takes them up
 * itself before it uses them — NXE first of all, in CpuTakeUpNoExecute,
 * before vmm_init builds anything.
 * ========================================================================= */

/*
 * Implemented in tagboot_jump.asm — pure NASM, no inline asm ambiguity.
 * Installs our CR3, switches RSP, jumps to entry. Does not write EFER.
 * Never returns.
 *
 * IMPORTANT: the NASM code uses System V AMD64 ABI registers (rdi, rsi, rdx).
 * When compiled with clang -target x86_64-unknown-windows, the default calling
 * convention is MS ABI (rcx, rdx, r8), which would put arguments in the wrong
 * registers and cause a triple fault.  The sysv_abi attribute forces System V
 * calling convention regardless of the compilation target.
 */
extern void __attribute__((sysv_abi)) TagBootJump(uint64_t pt_base,
                                                   uint64_t stack,
                                                   uint64_t entry);

static void __attribute__((noreturn)) JumpToKernel(uint64_t pt_base)
{
    TagBootJump(pt_base, g_stack_base, (uint64_t)KERNEL_LOAD_ADDR);
    __builtin_unreachable();
}

/*
 * CheckEfiLoadAddress — verify the EFI application is within our 4 GB
 * identity map.
 *
 * The JMP after `mov cr3` in tagboot_jump.asm runs from the EFI .text
 * section, so the image must remain mapped through the CR3 switch. Our
 * identity map covers 4 GB. UEFI spec recommends loading EFI applications
 * below 4 GB and every implementation observed in the wild (OVMF, AMI,
 * Insyde, Phoenix UEFI, Mac, MS Surface) honours that. If a future
 * firmware violates this we panic with a clear message rather than
 * triple-faulting on the JMP.
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
        Print(")\r\n");
        Panic("EFI app above 4 GB — extend identity map or update firmware");
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

    /* Capture EFI runtime services + system table + config table pointers
     * before any boot-services call that could reshape them. Per UEFI 2.10
     * §4.4.1 and §4.6 all three structures live in EfiRuntimeServicesData
     * and survive ExitBootServices, so a pre-EBS physical address remains
     * a valid handle for the kernel. The kernel will dereference these to
     * invoke RT services and walk the Configuration Table for ESRT /
     * SMBIOS / RT-properties / debug-image-info lookups. */
    g_efi_rt_services_phys   = (uint64_t)(uintptr_t)st->runtime_services;
    g_efi_fw_revision        = st->firmware_revision;
    g_efi_system_table_phys  = (uint64_t)(uintptr_t)st;
    g_efi_cfg_table_phys     = (uint64_t)(uintptr_t)st->configuration_table;

    /* Bound the count we forward to the kernel. Real firmware publishes
     * ~6-20 entries; anything bigger here points at memory corruption
     * (or, in adversarial scenarios, a tampered system table). The kernel
     * walker uses this count as its loop bound, so capping at the source
     * prevents a kernel-side OOB read even if downstream code forgets to
     * re-check. */
    uint64_t cfg_count_raw = st->number_of_table_entries;
    if (cfg_count_raw > EFI_CFG_TABLE_MAX_ENTRIES)
        cfg_count_raw = EFI_CFG_TABLE_MAX_ENTRIES;
    g_efi_cfg_table_count    = (uint32_t)cfg_count_raw;

    st->con_out->clear_screen(st->con_out);
    Print("TagBoot v4 — BoxOS UEFI Bootloader\r\n");
    Print("------------------------------------\r\n");

    /* Firmware vendor string is a CHAR16* in EfiRuntimeServicesData and
     * survives EBS. Print as ASCII (best-effort transliteration; high
     * bytes are dropped). Lets operator see "American Megatrends",
     * "Insyde", "HP", "Apple", etc. without a debugger. */
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

    /* ----- 0. Ask for the pages this loader hands the kernel -----
     *
     * Before anything is written into them, and so before the firmware can be
     * asked for memory by any of the steps below and answer with these. */
    ClaimHandoffPages();

    /* ----- 1. Find Block IO, 2. its ground and Deed, 3. a book ----- *
     *
     * The deed of every medium probed is taken up there and then, so a medium
     * whose volume is no book is passed over for the next (BlockIoProbeVolume);
     * the one found is a book, its Deed taken up. */
    Print("TagBoot: locating disk...\r\n");
    EFI_STATUS status = BlockIoFindDevice(image_handle);
    if (EFI_ERROR(status)) Panic("no medium carries a book this loader can boot");
    Print("TagBoot: disk found, block_size=");
    PrintDec(g_block_io->media->block_size);
    Print("\r\n");

    /* ----- 4. Find the kernel and load it ----- *
     *
     * The volume's kernel is chosen by the Succession: the kernel BOOT names
     * held to the book first, then the kernels the book names by role, each
     * placed at KERNEL_LOAD_ADDR and held to the digests the book keeps of its
     * blocks — one that does not match is never jumped into, and when none may
     * be, the loader stops with every kernel it tried and why
     * (tagboot_attest.c, tagboot_succession.c). */
    uint64_t loaded_bytes = 0;
    const char *stop = SuccessionTake(g_bs, ReadSectors, &loaded_bytes, &g_kernel_alloc_end);
    if (stop) Panic(stop);

    /* Validate kernel header magic — 'KERNEL' at byte offset 2.
     * Kernel header layout:
     *   +0  2 bytes  jmp short .past_header
     *   +2  6 bytes  "KERNEL"
     *   +8  4 bytes  header version
     *   +12 4 bytes  _kernel_phys_end  (physical end INCLUDING BSS)
     */
    if (!MemEqual((const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 2), "KERNEL", 6)) {
        Panic("kernel header magic invalid — wrong binary at load address");
    }
    /* Header version: bootloader and kernel must agree on the layout below.
     * If the kernel header version drifts past what we know how to parse,
     * the offsets/fields might mean something different — refuse to boot
     * an unrecognised kernel rather than parse it as the wrong shape. */
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

    /* Read the true physical end (including BSS) from the kernel header.
     * This MUST be used for page table placement — if we place page tables
     * before BSS end, kernel_entry.asm's "rep stosb" will overwrite them. */
    uint32_t header_phys_end = 0;
    MemCopy(&header_phys_end,
            (const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 12), 4);

    uint64_t kernel_end_phys = KERNEL_LOAD_ADDR + loaded_bytes;

    if (header_phys_end > (uint32_t)kernel_end_phys && header_phys_end < 0x10000000U) {
        /* The BSS extends past the loaded file data. Reserve those pages so
         * UEFI does not allocate them for its own use before ExitBootServices.
         *
         * ‼ THE RESERVATION BEGINS WHERE THE IMAGE ALLOCATION ENDS, not at the
         * kernel's last loaded byte. It used to begin at KERNEL_LOAD_ADDR +
         * loaded_bytes, which is the kernel file's true size out of the Deed's
         * boot stamp and is therefore almost never a multiple of 4096 — and an
         * unaligned AllocateAddress is refused by EDK2 before the memory map is
         * consulted at all (CoreInternalAllocatePages: `if (Type ==
         * AllocateAddress) { if ((*Memory & (Alignment - 1)) != 0) { return
         * EFI_NOT_FOUND; } }`). Rounding down would not have helped either: the
         * page holding that byte is inside the range AttestPlace already
         * allocated, and asking for pages one already holds is the OTHER thing
         * EFI_NOT_FOUND means here. So the whole 2 MB window the kernel treats
         * as its BSS went unreserved on every UEFI boot, and the only trace was
         * a status in parentheses at the end of one line.
         *
         * Nothing here relies on what the pages contain: §7.2.1 says nothing
         * about the contents of a page AllocatePages hands back (EDK2 clears
         * pages only under DEBUG_CLEAR_MEMORY, a debug-build macro), and the
         * kernel's own entry zeroes __bss_start..__bss_end with rep stosb
         * before it runs a line of C. A comment here used to claim the spec
         * promised zeroed pages. It does not.
         *
         * A refusal is NOT followed by free_pages. Past the image allocation
         * this loader holds nothing, so a refusal is somebody else's range, and
         * EDK2 frees without one ownership check. The guard this replaces
         * tested AllocatePages' EFI_NOT_FOUND against FreePages' meaning of
         * the code (§7.2.2 vs §7.2.1) and so fired only when the pages WERE
         * held by someone. */
        kernel_end_phys = (uint64_t)header_phys_end;
        Print("TagBoot: kernel phys end (BSS) ");
        PrintHex64(kernel_end_phys);

        if (kernel_end_phys > g_kernel_alloc_end) {
            EFI_PHYSICAL_ADDRESS bss_pa = (EFI_PHYSICAL_ADDRESS)g_kernel_alloc_end;
            UINTN bss_pages = (UINTN)((kernel_end_phys - g_kernel_alloc_end +
                                       PAGE_4KB - 1) / PAGE_4KB);
            EFI_STATUS bss_status = g_bs->allocate_pages(AllocateAddress,
                                                          EfiLoaderData,
                                                          bss_pages, &bss_pa);
            if (EFI_ERROR(bss_status)) {
                Print(" (BSS pages not set aside, status=");
                PrintHex64((uint64_t)bss_status);
                Print(")");
            } else {
                Print(" (");
                PrintDec((uint64_t)bss_pages);
                Print(" BSS pages set aside from ");
                PrintHex64(g_kernel_alloc_end);
                Print(")");
            }
        } else {
            /* The BSS ends inside the pages the image was loaded into, which
             * this loader already holds. Nothing left to ask for. */
            Print(" (BSS inside the kernel's own pages)");
        }
        Print("\r\n");
    }

    /* ----- 5. Get GOP framebuffer ----- */
    FbInfo fb = QueryGopFramebuffer();

    /* ----- 6. Build E820 memory map ----- */
    Print("TagBoot: building memory map...\r\n");
    MemMapResult mmap;
    status = BuildMemoryMap(&mmap);
    if (EFI_ERROR(status)) Panic("failed to get UEFI memory map");

    /* A memory map that did not fit is a map that stops describing memory
     * partway through — and everything past the cut is unclaimed as far as
     * the kernel is concerned, including regions that were RESERVED. Worse
     * than unclaimed: pmm_buddy_free_carved carves the USABLE runs against
     * the non-USABLE entries in this same table, so a RESERVED or ACPI_NVS
     * entry dropped past the cut stops being carved out of a USABLE run
     * below it, and the buddy hands out firmware-owned pages.
     *
     * ‼ This speaks for THIS map and no other. It is not a prediction about
     * the map rebuilt after ExitBootServices: between here and there this
     * loader asks the firmware for the ESRT copy and for the buffer the final
     * map is staged in, and an NVS island carved out of a conventional run
     * splits one merged USABLE run into as many as three — while the pool
     * freed in BuildMemoryMap can just as well make the second map shorter.
     * Two different maps, neither bounding the other, which is why Linux's
     * stub budgets EFI_MMAP_NR_SLACK_SLOTS between its own two. The second
     * pass says its own piece, over COM1, in PostEbsPatchBootInfo. */
    if (g_e820_truncated) {
        Print("TagBoot: WARNING — the memory map does not fit ");
        PrintDec(E820_MAX_ENTRIES);
        Print(" entries and was cut short\r\n");
    }

    /* ----- 7. Check EFI load address before switching CR3 ----- */
    CheckEfiLoadAddress();

    /* ----- 8. Set up page tables ----- */
    Print("TagBoot: setting up page tables...\r\n");
    status = SetupPageTables(kernel_end_phys);
    if (EFI_ERROR(status)) Panic("page table setup failed");

    /* ----- 8b. Capture ESRT before EBS — it lives in EfiBootServicesData
     *           per UEFI 2.10 §23.4 and would be reclaimed by PMM otherwise. */
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

    /* ----- 9. Fill boot_info_t v4 (includes RSDP + ESRT — must be before EBS) ----- */
    FillBootInfo(&fb, mmap.e820_count, kernel_end_phys);
    WriteBoardingPass();

    Print("TagBoot: boot_info at 0xA000 (v4, method=UEFI)\r\n");

    /* ----- 10. ExitBootServices -----
     *
     * UEFI starts a 5-minute boot watchdog at LoadImage time. If the kernel
     * is slow to come up (large E820, AP wakeup, disk init), the firmware
     * will reset the box mid-boot. Disable the watchdog before handing off:
     * timeout=0 cancels it. Spec §7.5 — kernel never returns, so we never
     * want to be reset by it. Some firmware returns EFI_UNSUPPORTED (no
     * watchdog implementation) — harmless, we proceed regardless. Log the
     * status so an unexpected EFI_DEVICE_ERROR surfaces in the boot log.
     */
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

    /* From here: no UEFI services, no Print(). Pure bare metal.
     * Patch boot_info v3 with the FINAL EFI memory map (the one whose
     * key was accepted by ExitBootServices). */
    PostEbsPatchBootInfo();

    /* ----- 11. Jump to kernel ----- */
    JumpToKernel(g_pt_base);
}
