#include "iommu.h"
#include "acpi.h"
/* acpi_find_table — the IVRS's IVMD blocks are read HERE rather than in the
 * ACPI parser, because acpi_ivrs_info_t stores IVHDs and nothing else, and a
 * memory block firmware says a peripheral needs is a device that stops the
 * instant IommuEn goes on. */
#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "touch.h"
#include "hourglass.h"
#include "atomics.h"

/* =====================================================================
 * AMD-Vi (AMD IOMMU) driver — production implementation.
 *
 * Reference: "AMD I/O Virtualization Technology (IOMMU) Specification",
 *            revision 3.06+ (the AMD-Vi spec).
 *
 * What this driver does
 * ---------------------
 *   * Map each IVHD register block (4 KB MMIO)
 *   * Allocate Device Table (DTE) — 32-byte entries × 64 K Bus×Dev×Fn
 *     IDs; here we right-size to one full segment (≈ 2 MiB) and fill
 *     it lazily as devices are attached
 *   * Allocate Command Buffer + Event Log (4 KB each, ring buffers)
 *   * Build a single second-level Long-Mode page table identity-mapping
 *     every physical address a device on this machine can name: the top of
 *     the memory map (pmm_get_mem_end()), never below the whole of what a
 *     Single Address Cycle can address, and every memory block the IVRS's own
 *     IVMD entries publish — read out of the table, not assumed. Shared
 *     across all DTEs, "passthrough" model
 *   * Stop the unit, then program its base registers (DEV_TAB_BAR,
 *     CMD_BUF_BAR, EVT_LOG_BAR) and their head/tail pointers while it is
 *     stopped -- §2.4 and §2.5.1 permit those registers to move only while
 *     the matching Run bit in MMIO 2020h reads 0
 *   * Issue COMPLETION_WAIT to drain command buffer
 *   * `amdvi_take_the_wheel()` flips IOMMU_CTRL.IommuEn on every online unit,
 *     and only after every PCI function on the machine has a Device Table
 *     Entry. It is ALL-OR-NOTHING. init() CLEARS IommuEn, together with
 *     CmdBufEn, EventLogEn and the PPR/GA log enables: an AMD board whose
 *     firmware hands the IOMMU over already running is the ordinary case, and
 *     its tables cannot be swapped underneath it
 *
 * What IommuEn gates, and what it does not
 * ----------------------------------------
 * IommuEn is not the translation switch; it is the unit's power switch, and
 * everything else in this driver hangs off it. §3.4.1 bit 2 EventLogEn: "all
 * events detected are written to the event log when IommuEn has also been
 * set"; bit 12 CmdBufEn: "When CmdBufEn = 1b and IommuEn = 1b, the IOMMU
 * starts fetching commands". So with IommuEn clear this unit logs nothing and
 * executes nothing -- the event log below is a channel the hardware never
 * writes to, and every amdvi_completion_wait would run its full span out.
 *
 * That is exactly why seating devices and switching the unit on are two
 * separate steps here, and why the seating step pushes no commands: a
 * COMPLETION_WAIT per device, with IommuEn still clear, would spend this
 * kernel's whole patience per device and report a semaphore that could not
 * possibly have landed. Every DTE is written first, in silence; IommuEn goes
 * on; and the caches are emptied once, afterwards, when there is an engine
 * to execute the command.
 *
 * Translation is per DEVICE, in the DTE. Table 8, V = 0: "All addresses are
 * forwarded without translation; individual control fields are ignored." So
 * a device with no entry keeps passing DMA through untranslated even with
 * IommuEn set -- the AMD failure mode is a device silently OUTSIDE the
 * protection the rest of the machine has, not a dead device, which is the
 * reason amdvi_seat_device refuses rather than skips.
 *
 * Table 9, IV = 0: "All interrupts are forwarded without remapping." Every
 * DTE this driver writes leaves bits 127:64 and 191:128 zero, so IV is 0 and
 * MSI, MSI-X and the LAPIC lines all reach their processor exactly as they
 * did before translation was switched on. Table 10 says the same of LINT0,
 * LINT1, SMI, NMI, INIT and ExtInt.
 *
 * The DTE format used is "V=1, host-page-table set to identity-map"
 * (DTE bits 1:0 = 11, DomainID=0, host page table pointer = SLPT root,
 * mode=4 for 4-level paging). This is the same model used by Linux's
 * "iommu=pt" mode.
 * ===================================================================== */

/* IOMMU MMIO registers (AMD-Vi §3.4 "MMIO Registers": the 0000h-0030h block
 * is §3.4.1 "Control and Status Registers", and the buffer head/tail pointers
 * and the status register at 2000h-2020h are later subsections of the same
 * §3.4). The "§3.1.6" that stood here names nothing in this specification:
 * §3.1 has a single subsection, "Accessing MSI Capability Block Registers". */
#define MMIO_DEV_TAB_BAR     0x0000   /* 64-bit */
#define MMIO_CMD_BUF_BAR     0x0008
#define MMIO_EVT_LOG_BAR     0x0010
#define MMIO_CTRL            0x0018
/* 0020h/0028h are the Exclusion Base and Exclusion Range Limit registers —
 * except on a part that supports SNP, where §2.12.2.2 "Completion Wait Store
 * Restrictions" re-purposes the same pair as the Completion Store Base and
 * Limit and the exclusion range does not exist at all. */
#define MMIO_EXCL_BASE       0x0020
#define MMIO_EXCL_LIMIT      0x0028
#define MMIO_EXT_FEAT        0x0030
#define MMIO_CMD_HEAD        0x2000
#define MMIO_CMD_TAIL        0x2008
#define MMIO_EVT_HEAD        0x2010
#define MMIO_EVT_TAIL        0x2018
#define MMIO_STATUS          0x2020

/* Extended Feature Register (0030h), SNPSup. Linux reads the same bit as
 * FEATURE_SNP = BIT_ULL(63). */
#define EFR_SNP_SUP          (1ULL << 63)

/* Extended Feature Register (0030h) bit 6, IASup: "INVALIDATE_IOMMU_ALL
 * supported. RO. 1 = The INVALIDATE_IOMMU_ALL command is supported. 0 = The
 * INVALIDATE_IOMMU_ALL command is not supported and will generate an error
 * when used."
 *
 * §2.4.8 says the same from the other side: "When not supported, the
 * INVALIDATE_IOMMU_ALL command is reserved and causes an
 * ILLEGAL_COMMAND_ERROR." The bit therefore decides whether this driver has
 * ANY single bounded way to empty a unit's caches, which is what stands
 * between "the DTEs this kernel wrote are in force" and "the unit is still
 * translating through something firmware cached". Linux reads the same bit as
 * FEATURE_IA = BIT_ULL(6) and falls back to a walk of all 65536 device IDs;
 * this driver refuses the unit instead — see amdvi_take_the_wheel. */
#define EFR_IA_SUP           (1ULL << 6)

/* Exclusion/Completion-Store Base and Limit carry an address in bits 51:12;
 * the low bits are the exclusion range's own Enable and Allow, which stay 0
 * here. Linux masks with PM_ADDR_MASK, the same 0x000ffffffffff000. */
#define AMDVI_STORE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* CTRL bits (§3.4.1 "MMIO Offset 0018h IOMMU Control Register"). */
#define CTRL_IOMMU_EN        (1ULL << 0)
#define CTRL_HT_TUN_EN       (1ULL << 1)
#define CTRL_EVT_LOG_EN      (1ULL << 2)
#define CTRL_EVT_INT_EN      (1ULL << 3)
#define CTRL_CMD_BUF_EN      (1ULL << 12)
#define CTRL_PPR_LOG_EN      (1ULL << 13)
#define CTRL_PPR_INT_EN      (1ULL << 14)
#define CTRL_GA_LOG_EN       (1ULL << 28)
#define CTRL_GA_INT_EN       (1ULL << 29)

/* IOMMU Status Register bits (§3.4.16 "Command and Event Status Register",
 * MMIO Offset 2020h). The two Run bits say whether the IOMMU is still using
 * the buffer whose base register is about to move; EventOverflow is RW1C and
 * §2.5.1 has software clear it as part of restarting the log, because while
 * it stands "no new event log entries are written". */
#define STATUS_EVT_OVERFLOW  (1ULL << 0)
#define STATUS_EVT_LOG_RUN   (1ULL << 3)
#define STATUS_CMD_BUF_RUN   (1ULL << 4)

/* Command opcodes. */
#define CMD_OP_COMPLETION_WAIT      0x01
#define CMD_OP_INVALIDATE_DEV_TAB   0x02
#define CMD_OP_INVALIDATE_IOMMU_PG  0x03
#define CMD_OP_INVALIDATE_IOTLB_PG  0x04
#define CMD_OP_INVALIDATE_ALL       0x08

/* Device Table Base Address Register, bits 8:0 = Size: the table the IOMMU
 * will fetch DTEs from spans 4 KiB x (Size + 1). One full PCI segment is
 * 64 K device IDs of 32 bytes = 2 MiB = 512 pages, so Size = 0x1FF. Linux
 * writes the same quantity as `entry |= (dev_table_size >> 12) - 1`
 * (drivers/iommu/amd/init.c, iommu_set_device_table) and FreeBSD as
 * `rval = phys | (atop(seg_sz) - 1)` (sys/x86/iommu/amd_drv.c).
 *
 * The 0xF that stood here, with the comment calling it "1 MB (64 K entries)",
 * described 16 pages = 64 KiB = 2048 entries. The DTE of every device whose
 * devid was 2048 or higher would have been fetched from past the end of the
 * only region the IOMMU had been told about. */
#define DEV_TAB_BYTES       (256ULL * 1024 * 8)   /* 2 MiB total */
#define DEV_TAB_SIZE_CLASS  ((DEV_TAB_BYTES / 4096) - 1)   /* 0x1FF */

#define CMD_BUF_BYTES       4096u
#define EVT_LOG_BYTES       4096u

typedef struct {
    uint64_t  reg_phys;
    volatile uint8_t* reg;
    void*     dev_tab;
    uint64_t  dev_tab_phys;
    void*     cmd_buf;
    uint64_t  cmd_buf_phys;
    uint32_t  cmd_tail;
    void*     evt_log;
    uint64_t  evt_log_phys;
    /* The COMPLETION_WAIT semaphore: the IOMMU stores into it by physical
     * address, this kernel reads it through the pull map, and every wait
     * stores a fresh sequence number so a late store cannot satisfy the
     * wait that came after it. */
    volatile uint64_t* sem;
    uint64_t  sem_phys;
    uint64_t  sem_seq;
    uint16_t  pci_segment;
    /* EFR[IASup], read once out of this unit's own feature register. */
    bool      invalidate_all_supported;
    bool      online;
} amdvi_unit_t;

#define AMDVI_MAX_UNITS  ACPI_IVRS_MAX_IVHD
static amdvi_unit_t g_units[AMDVI_MAX_UNITS];
static uint8_t      g_unit_count = 0;
static uint64_t*    g_id_root      = NULL;
static uint64_t     g_id_root_phys = 0;

/*
 * An AMD-Vi structure is two addresses, and they are not the same number.
 *
 * The IOMMU walks the device table, the command buffer and the I/O page
 * tables with its own engine, which knows nothing of the kernel's page
 * tables: it is given the PHYSICAL address. The CPU must use the kernel's
 * alias for that memory, and after vmm_init the identity window is gone --
 * the only alias left is the pull map at PULL_MAP_BASE (vmm.h, vmm.c
 * "After this: identity mapping is GONE"). pmm_alloc returns physical.
 *
 * This function used to return that physical value as the CPU pointer and
 * memset through it. That is the defect the VT-d side of this kernel carried
 * until a Gigabyte B365 took a kernel #PF inside memset on 2026-08-24 (the
 * story is written out in vtd.c above alloc_table_4k); it survived here for
 * the same reason it survived there -- QEMU exposes no AMD IOMMU unless it is
 * started with -device amd-iommu, so the first AMD board with an IVRS in its
 * firmware would have been the first machine ever to run this line.
 */
static void* alloc_aligned_4k(uint64_t* phys_out, size_t size_bytes) {
    /* Round up to 4 KB. */
    size_t pages = (size_bytes + 4095) / 4096;
    void* p = pmm_alloc(pages);
    if (!p) { *phys_out = 0; return NULL; }

    uintptr_t phys = (uintptr_t)p;
    void*     virt = vmm_phys_to_virt(phys);
    if (!virt) { pmm_free(p, pages); *phys_out = 0; return NULL; }

    *phys_out = (uint64_t)phys;
    memset(virt, 0, pages * 4096);
    return virt;
}

/* An AMD-Vi v1 host I/O page-table entry is not an x86_64 page-table entry.
 *
 * The spec does bless handing an AMD64 long-mode table to the IOMMU, but only
 * "provided the Next Level fields ... are properly initialized according to
 * their level" -- and the permission bits do not coincide at all. Of the low
 * bits only PR (bit 0) sits where x86_64 keeps P; bits 4:1, where x86_64
 * keeps R/W and U/S, are Ignored. Permission lives at the top of the entry:
 * IW is bit 62, IR bit 61, FC bit 60, U bit 59. Which level the entry points
 * at lives in bits 11:9 (NextLevel); an entry whose NextLevel is 0 or 7 is a
 * page TRANSLATION entry, and the hardware stops walking there.
 *
 * IR and IW are ANDed at every step of the walk and with the DTE's own IR/IW,
 * so a zero anywhere along the path target-aborts the transaction: writing
 * permission into bits 1 and 2, as this file did, grants nothing at all.
 *
 * Cross-checked against Linux (IOMMU_PTE_PR/IR/IW, PM_LEVEL_ENC, PM_PTE_LEVEL
 * in drivers/iommu/amd/amd_iommu_types.h) and FreeBSD (AMDIOMMU_PTE_IR/IW,
 * AMDIOMMU_PTE_NLVL_MASK in sys/x86/iommu/amd_reg.h). */
#define AMD_PTE_PR          (1ULL << 0)
#define AMD_PTE_IR          (1ULL << 61)
#define AMD_PTE_IW          (1ULL << 62)
#define AMD_PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define AMD_PTE_NEXT_LVL(l) (((uint64_t)(l) << 9) & 0xE00ULL)
#define AMD_PTE_LEVEL_OF(e) (((e) >> 9) & 0x7ULL)

/* The DTE Mode field names the number of levels. Mode = 4 is a 4-level table:
 * entries in the root carry NextLevel = 3, their children 2, theirs 1, and
 * only the page translation entries at the bottom carry 0. */
#define AMD_PT_MODE         4u

/* Descend one level. `next_level` is what a directory entry at THIS level
 * must carry in bits 11:9 -- AMD_PT_MODE-1 under the root, then one less per
 * level. An entry already there with a different level is not a directory
 * this walk can follow, and there is nothing below it to hand back. */
static uint64_t* amd_pt_walk(uint64_t* parent, unsigned idx,
                             unsigned next_level, bool create) {
    uint64_t e = parent[idx];
    if (!(e & AMD_PTE_PR)) {
        if (!create) return NULL;
        uint64_t child_phys;
        void* child = alloc_aligned_4k(&child_phys, 4096);
        if (!child) return NULL;
        parent[idx] = (child_phys & AMD_PTE_ADDR_MASK)
                    | AMD_PTE_NEXT_LVL(next_level)
                    | AMD_PTE_PR | AMD_PTE_IR | AMD_PTE_IW;
        return (uint64_t*)child;
    }
    if (AMD_PTE_LEVEL_OF(e) != next_level) return NULL;
    return (uint64_t*)vmm_phys_to_virt((uintptr_t)(e & AMD_PTE_ADDR_MASK));
}

#define AMD_2M   0x200000ULL

/* The whole of what a PCI master can address in one address phase — the
 * identity map's FLOOR, and a property of the bus rather than a choice.
 *
 * PCI Local Bus Specification Rev. 3.0 §3.9 "64-Bit Bus Extension": "The
 * standard PCI bus transaction supports a 32-bit address, Single Address
 * Cycle (SAC), where the address is valid for a single clock when FRAME# is
 * first sampled asserted", and "Devices that support only 32-bit addresses
 * are mapped into the low 4 GB of the address space". Thirty-two address bits
 * is 2^32 bytes: every address a device without Dual Address Cycle can put on
 * the bus lies below this line, and none above it. Below it the map is
 * complete whatever the memory map says; above it, what the memory map and
 * the IVMD blocks say is what gets mapped. */
#define AMDVI_SAC_SPAN   (1ULL << 32)

/* ----- What firmware says a peripheral still needs to reach -----
 *
 * §5.2.2.2 "I/O Virtualization Memory Definition (IVMD) Block": "Platform
 * firmware can inform system software of memory usage restrictions or
 * requirements by using I/O Virtualization Memory Definition (IVMD) blocks
 * ... System software is expected to use the information in the IVMD blocks
 * when it programs the IOMMU."
 *
 * Table 102 gives the flags: Unity at bit 0, "Unit address mapping. 1b =
 * virtual addresses must be the same value as physical addresses", IR at bit
 * 1, IW at bit 2, ExclusionRange at bit 3 ("IR, IW, and Unity are ignored
 * when ExclusionRange = 1b"). A block with IR = 0 and IW = 0 is the opposite
 * request — "the memory range is not to be mapped into the peripheral device
 * address space" — so it is deliberately NOT mapped and said out loud, since
 * this driver has one identity domain and cannot carve a hole for some
 * devices only.
 *
 * Table 100 gives the layout: Type at +0 (20h all peripherals, 21h one
 * DeviceID, 22h a DeviceID range), Flags at +1, Length at +2, start address
 * at +16, memory block length at +24 — a LENGTH, unlike VT-d's inclusive
 * limit. The walk is a callback rather than an array because the number of
 * blocks is the firmware's to decide. */
#define IVMD_FLAG_UNITY      (1u << 0)
#define IVMD_FLAG_IR         (1u << 1)
#define IVMD_FLAG_IW         (1u << 2)
#define IVMD_FLAG_EXCLUSION  (1u << 3)
#define IVRS_BLOCK_IVMD_LOW   0x20
#define IVRS_BLOCK_IVMD_HIGH  0x22

typedef void (*AmdIvmdFn)(uint64_t base, uint64_t length, uint8_t flags,
                          void* ctx);

static void amdvi_walk_ivmd(AmdIvmdFn fn, void* ctx) {
    acpi_sdt_header_t* hdr = acpi_find_table("IVRS");
    if (!hdr) return;
    uint8_t* ptr = (uint8_t*)hdr + sizeof(acpi_ivrs_t);
    uint8_t* end = (uint8_t*)hdr + hdr->length;

    /* Every IVDB carries type at +0 and a 16-bit length at +2, which is what
     * makes the list walkable without knowing each block's shape. */
    while (ptr + 4 <= end) {
        uint8_t  type = ptr[0];
        uint8_t  flags = ptr[1];
        uint16_t len  = *(uint16_t*)(ptr + 2);
        if (len < 4 || ptr + len > end) break;
        if (type >= IVRS_BLOCK_IVMD_LOW && type <= IVRS_BLOCK_IVMD_HIGH &&
            len >= 32) {
            uint64_t base   = *(uint64_t*)(ptr + 16);
            uint64_t length = *(uint64_t*)(ptr + 24);
            if (length) fn(base, length, flags, ctx);
        }
        ptr += len;
    }
}

/* The identity map, whole — and both of its dimensions come from facts rather
 * than from numbers in this file.
 *
 * EXTENT. pmm_get_mem_end() is one past the last byte of the highest usable
 * E820 region, clamped to this part's MAXPHYADDR: the top of everything a
 * device could be handed a pointer into. The bottom 4 GiB is mapped whether
 * or not RAM reaches that high, because what is down there is not only RAM —
 * the reserved regions an IVRS IVMD points at and low MMIO live below 4 GiB,
 * and a device that touches one of them with the unit on and no entry for it
 * gets an IO_PAGE_FAULT and stops working.
 *
 * GRANULARITY. A page translation entry is "a page table entry with the Next
 * Level field set to 0h or 7h" (§2.2.3), and Table 15 "Page Table Level
 * Parameters" gives the default page size of a translation entry fetched from
 * a level-2 table as 2^21. So an entry written INTO the level-2 table with
 * NextLevel = 0 is a 2 MiB page, architecturally, with no capability bit to
 * consult — unlike Intel's SSLPS, AMD's page size is a property of the level
 * the entry sits at. The one rule to honour is alignment: §2.2.3, "if a page
 * translation entry's physical address is not aligned to a multiple of the
 * appropriate page size for the current page table entry page table's level
 * ... translation terminates with an IO_PAGE_FAULT". An identity map walked
 * in 2 MiB steps from zero is aligned at every step.
 *
 * On a 16 GiB machine this is 16 level-2 tables, one level-3 and one level-4:
 * 18 pages, against the 8192 that 4 KiB leaves would have cost for the same
 * coverage.
 *
 * Unlike the VT-d side, the interrupt address range is NOT cut out here. The
 * sentence that makes Intel cut it — "software must not program
 * paging-structure entries to remap any address to the interrupt address
 * range", §3.15 of the VT-d specification — has no counterpart in this
 * document, and on this hardware the interrupt path is decided by the DTE's
 * IV and IntCtl fields (Table 9) before any page table is consulted. Cutting
 * a hole on the strength of another vendor's rule would be inventing one. */
/* Identity-map [first, last] at 2 MiB granularity, rounding outward. */
static int amd_identity_range(uint64_t first, uint64_t last) {
    uint64_t a   = first & ~(AMD_2M - 1);
    uint64_t end = (last | (AMD_2M - 1)) + 1;
    for (; a < end; a += AMD_2M) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        uint64_t* l3 = amd_pt_walk(g_id_root, i4, AMD_PT_MODE - 1, true);
        if (!l3) return -1;
        uint64_t* l2 = amd_pt_walk(l3, i3, AMD_PT_MODE - 2, true);
        if (!l2) return -1;
        /* Page translation entry in the level-2 table: NextLevel stays 0, so
         * Table 15 makes this one 2 MiB page. Read and write granted. */
        l2[i2] = (a & AMD_PTE_ADDR_MASK) | AMD_PTE_PR | AMD_PTE_IR | AMD_PTE_IW;
    }
    return 0;
}

/* Does the table the IOMMU will walk actually translate this address?
 *
 * A READ of what was written, through the levels and the PR bits the hardware
 * uses. It replaces a comparison of the build's own recorded span against the
 * number the build had been given — the same call, rounded up, in the same
 * boot, which could never come back false. This one can. */
static bool amd_identity_covers(uint64_t addr) {
    if (!g_id_root) return false;
    uint64_t* l3 = amd_pt_walk(g_id_root, (unsigned)((addr >> 39) & 0x1FF),
                               AMD_PT_MODE - 1, false);
    if (!l3) return false;
    uint64_t* l2 = amd_pt_walk(l3, (unsigned)((addr >> 30) & 0x1FF),
                               AMD_PT_MODE - 2, false);
    if (!l2) return false;
    uint64_t e = l2[(addr >> 21) & 0x1FF];
    /* A translation entry is one whose Next Level is 0h or 7h (§2.2.3); this
     * build writes 0. Anything else at this slot is a directory, which means
     * the 2 MiB page this check is asking about was never laid down. */
    return (e & AMD_PTE_PR) && AMD_PTE_LEVEL_OF(e) == 0;
}

static void amd_map_ivmd(uint64_t base, uint64_t length, uint8_t flags,
                         void* ctx) {
    int* rc = (int*)ctx;
    if (flags & IVMD_FLAG_EXCLUSION) {
        /* Table 102: "IR, IW, and Unity are ignored when ExclusionRange =
         * 1b." An exclusion range is the IOMMU's own bypass window, set
         * through the Exclusion Base/Limit registers — and this driver has
         * already given that register pair to the completion-wait semaphore
         * on an SNP part (§2.12.2.2). Two owners for one pair of registers is
         * not something to resolve silently. */
        kprintf("[AMD-Vi] IVRS asks for an EXCLUSION range at 0x%lx+0x%lx; "
                "this driver uses the Exclusion Base/Limit pair for the "
                "completion-wait store window and cannot give it away, so the "
                "range is identity-mapped instead and reached through the "
                "tables rather than around them\n",
                (unsigned long)base, (unsigned long)length);
        if (amd_identity_range(base, base + length - 1) < 0) *rc = -1;
        return;
    }
    if (!(flags & (IVMD_FLAG_IR | IVMD_FLAG_IW))) {
        /* "If IR = 0b and IW = 0b in the IVMD flags field, then the memory
         * range is not to be mapped into the peripheral device address
         * space." The whole machine shares one identity domain here, so
         * honouring that for one device would take the range from all of
         * them. It is left mapped and the disagreement is said, not hidden. */
        kprintf("[AMD-Vi] IVRS marks 0x%lx+0x%lx as unreachable by a "
                "peripheral (IR=0, IW=0). This driver keeps one identity "
                "domain for the whole machine, so the range stays mapped for "
                "every device; per-device domains are what would honour it\n",
                (unsigned long)base, (unsigned long)length);
        return;
    }
    kprintf("[AMD-Vi] IVRS names memory block 0x%lx+0x%lx (flags 0x%02x%s) as "
            "one a peripheral needs — mapping it identically, because a "
            "device that reaches it after IommuEn with no entry takes an "
            "IO_PAGE_FAULT and stops\n",
            (unsigned long)base, (unsigned long)length, (unsigned)flags,
            (flags & IVMD_FLAG_UNITY) ? ", Unity" : "");
    if (amd_identity_range(base, base + length - 1) < 0) *rc = -1;
}

static void amd_check_ivmd(uint64_t base, uint64_t length, uint8_t flags,
                           void* ctx) {
    bool* ok = (bool*)ctx;
    if (!(flags & (IVMD_FLAG_IR | IVMD_FLAG_IW)) &&
        !(flags & IVMD_FLAG_EXCLUSION))
        return;                      /* deliberately not required to be there */
    if (!amd_identity_covers(base) || !amd_identity_covers(base + length - 1)) {
        kprintf("[AMD-Vi] the memory block 0x%lx+0x%lx that IVRS says a "
                "peripheral needs is NOT in the tables the IOMMU is about to "
                "walk — that device would take an IO_PAGE_FAULT the instant "
                "IommuEn goes on, so translation stays OFF everywhere\n",
                (unsigned long)base, (unsigned long)length);
        *ok = false;
    }
}

static int amd_identity_build(void) {
    if (g_id_root) return 0;
    void* p = alloc_aligned_4k(&g_id_root_phys, 4096);
    if (!p) return -1;
    g_id_root = (uint64_t*)p;

    uint64_t span = pmm_get_mem_end();
    if (span < AMDVI_SAC_SPAN) span = AMDVI_SAC_SPAN;
    span = (span + (AMD_2M - 1)) & ~(AMD_2M - 1);

    if (amd_identity_range(0, span - 1) < 0) return -1;

    int rc = 0;
    amdvi_walk_ivmd(amd_map_ivmd, &rc);
    if (rc < 0) {
        kprintf("[AMD-Vi] a memory block the IVRS names could not be mapped — "
                "the identity tables are incomplete and no unit will be "
                "switched on\n");
        return -1;
    }

    kprintf("[AMD-Vi] identity map covers 0x0-0x%lx plus every IVMD block, at "
            "2 MiB leaves — the top of the memory map or the reach of a "
            "Single Address Cycle, whichever is higher\n",
            (unsigned long)span);
    return 0;
}

/* Device Table Entry field bits (AMD-Vi Table 7 "Device Table Entry (DTE)
 * Field Definitions"); the same four Linux calls DTE_FLAG_V / _TV / _IR / _IW
 * in drivers/iommu/amd/amd_iommu_types.h. */
#define DTE_FLAG_V   (1ULL << 0)
#define DTE_FLAG_TV  (1ULL << 1)
#define DTE_FLAG_IR  (1ULL << 61)
#define DTE_FLAG_IW  (1ULL << 62)

/* Encode a DTE: V=1, TV=1, host page table = caller-supplied, mode = 4
 * (4-level), read and write permitted, caller-supplied DomainID.
 *
 * IR and IW are not optional decoration. Table 7 on bit 61: "0 = Device
 * initiated DMA read transactions are target aborted"; on bit 62 the same for
 * writes and atomics. The IOMMU evaluates the DTE before it ever consults a
 * page table, so a DTE written without them makes an attached device's every
 * transaction abort -- and this entry says V=1/TV=1, so the abort is what the
 * device gets instead of the untranslated pass-through it had before. Linux
 * ORs DTE_FLAG_IR|DTE_FLAG_IW into every entry it writes, unconditionally. */
static void amdvi_dte_program(uint8_t* dte_base, uint16_t devid,
                                uint64_t host_pt_phys, uint16_t domain_id) {
    uint64_t* dte = (uint64_t*)(dte_base + (size_t)devid * 32);
    dte[0] = (host_pt_phys & AMD_PTE_ADDR_MASK)
           | ((uint64_t)AMD_PT_MODE << 9)
           | DTE_FLAG_IR | DTE_FLAG_IW | DTE_FLAG_TV | DTE_FLAG_V;
    dte[1] = (uint64_t)domain_id;
    dte[2] = 0;
    dte[3] = 0;
}

/* ----- Command buffer ----- */

static void amdvi_cmd_push(amdvi_unit_t* u, uint64_t cmd0, uint64_t cmd1) {
    if (!u->cmd_buf) return;
    uint64_t* slot = (uint64_t*)((uint8_t*)u->cmd_buf + u->cmd_tail);
    slot[0] = cmd0;
    slot[1] = cmd1;
    u->cmd_tail = (u->cmd_tail + 16) & (CMD_BUF_BYTES - 1);
    *(volatile uint64_t*)(u->reg + MMIO_CMD_TAIL) = u->cmd_tail;
}

/* How long the IOMMU gets to store its COMPLETION_WAIT semaphore.
 *
 * DECLARED POLICY, not a spec quantity. AMD-Vi §2.4.3 defines COMPLETION_WAIT
 * as completing once every previously queued command has, and names neither a
 * maximum time nor an iteration count — there is nothing to quote. One second
 * is this kernel's patience, and it is deliberately the same number the VT-d
 * side of this kernel gives its own remap units, so there is one number to
 * argue with rather than two.
 *
 * The million iterations that stood here were not time: that is a couple of
 * milliseconds of cached loads on a fast part and most of a second on a slow
 * one, and on expiry it said so only through a debug_printf that a shipped
 * build compiles away. */
#define AMDVI_COMPLETION_PATIENCE_US   1000000u

/* COMPLETION_WAIT, 128 bits: bit 0 of qword 0 is Store (s), bit 1 is
 * Completion Interrupt (i), bits 51:3 are the SPA the IOMMU stores to, bits
 * 63:60 are the opcode -- and qword 1 is the VALUE that gets stored. Linux's
 * build_completion_wait (data[0] = lower_32(paddr) | CMD_COMPL_WAIT_STORE_MASK,
 * data[1] = upper_32(paddr) | op<<28, data[2..3] = the value) and FreeBSD's
 * struct amdiommu_cmd_completion_wait (s:1, i:1, f:1, address0:29,
 * address1:20, rsrv:8, op:4, data0:32, data1:32) agree bit for bit.
 *
 * What stood here asked for the completion INTERRUPT, never set Store, and put
 * the semaphore's address in qword 1 -- the value field. The IOMMU was given
 * no address to store to, so the store could never happen and the wait could
 * only ever run out. The semaphore also pointed into the command buffer at the
 * exact qword the command about to be pushed would occupy. */
#define AMDVI_CW_STORE       (1ULL << 0)
#define AMDVI_CW_ADDR_MASK   0x000FFFFFFFFFFFF8ULL   /* bits 51:3 */

/* Returns whether the semaphore landed.
 *
 * That answer is the only EVENT this driver has that proves the IOMMU is
 * running: §3.4.1 bit 12 has the engine fetch commands only "When CmdBufEn =
 * 1b and IommuEn = 1b", so a COMPLETION_WAIT that completes is the hardware
 * saying both bits are in force and it has worked through everything queued
 * ahead of it. amdvi_take_the_wheel uses it for exactly that. */
static bool amdvi_completion_wait(amdvi_unit_t* u) {
    /* The exit is the IOMMU's store into this unit's semaphore and nothing
     * else. Each wait stores a sequence number that has never been stored
     * before, so a store that lands after its own wait gave up cannot be
     * mistaken for the next wait's answer.
     *
     * Nothing is undone on expiry, and that is deliberate: a completion wait
     * that has not landed does not mean the queued commands failed, only that
     * they have not finished, and withdrawing anything here would be acting on
     * a fact this code does not have. What it CAN do is say so where a shipped
     * build can see it.
     *
     * One caller-visible trap, and it is the hardware's, not this wait's:
     * §3.4.1 bit 12 has the IOMMU fetch commands only "When CmdBufEn = 1b and
     * IommuEn = 1b". Before amdvi_take_the_wheel has run, every path into
     * this function -- attach, detach, invalidate -- runs with IommuEn still
     * clear, so the command is written into a buffer nobody reads and this
     * wait can only ever run its span out and say the semaphore never landed.
     * That is a true report of a real condition, which is why it is left to
     * say it rather than short-circuited on a bit read here -- and why the
     * bulk seating path writes DTEs without pushing a single command. */
    if (!u->cmd_buf || !u->sem) return false;

    uint64_t want = ++u->sem_seq;
    amdvi_cmd_push(u,
        ((uint64_t)CMD_OP_COMPLETION_WAIT << 60)
            | (u->sem_phys & AMDVI_CW_ADDR_MASK) | AMDVI_CW_STORE,
        want);

    HourGlass glass;
    if (!HourGlassTurn(&glass, AMDVI_COMPLETION_PATIENCE_US)) {
        kprintf("[AMD-Vi] no time source — waiting for the completion "
                "semaphore on the fact itself, with no bound\n");
        while (*u->sem != want) cpu_pause();
        return true;
    }
    for (;;) {
        if (*u->sem == want) return true;
        if (HourGlassRunOut(&glass)) break;
        cpu_pause();
    }
    kprintf("[AMD-Vi] the completion semaphore never landed — %lu us on the "
            "%s and the %s; the invalidations it was guarding may still be in "
            "flight\n",
            (unsigned long)HourGlassElapsedUs(&glass),
            HourGlassRulerName(&glass),
            HourGlassSourceDied(&glass) ? "ruler died mid-wait"
                                        : "span ran out");
    return false;
}

/* ----- Per-unit init ----- */

/* How long a unit that has just been told to stop gets to finish the commands
 * it had already fetched and the log entry it was already writing.
 *
 * DECLARED POLICY, not a spec quantity. §2.4 and §2.5.1 say to wait for the
 * Run bits and name no bound; one second is this kernel's patience, and it is
 * deliberately the same second AMDVI_COMPLETION_PATIENCE_US gives the
 * completion semaphore and VTD_HANDSHAKE_PATIENCE_US gives the remap units, so
 * there is one number to argue with rather than three. */
#define AMDVI_QUIESCE_PATIENCE_US   1000000u

/* Wait for status bits to read 0. Returns false when they never did, and the
 * caller must then refuse the unit: moving a base register out from under a
 * running IOMMU is what the spec calls undefined, so "carried on as though it
 * had stopped" is the one answer this wait must not produce. */
static bool amdvi_await_status_clear(amdvi_unit_t* u, uint64_t bits,
                                     const char* what) {
    volatile uint64_t* status = (volatile uint64_t*)(u->reg + MMIO_STATUS);
    HourGlass glass;
    if (!HourGlassTurn(&glass, AMDVI_QUIESCE_PATIENCE_US)) {
        kprintf("[AMD-Vi] no time source — waiting for %s on the fact itself, "
                "with no bound\n", what);
        while (*status & bits) cpu_pause();
        return true;
    }
    for (;;) {
        if (!(*status & bits)) return true;
        if (HourGlassRunOut(&glass)) break;
        cpu_pause();
    }
    kprintf("[AMD-Vi] %s never cleared — %lu us on the %s and the %s\n",
            what, (unsigned long)HourGlassElapsedUs(&glass),
            HourGlassRulerName(&glass),
            HourGlassSourceDied(&glass) ? "ruler died mid-wait"
                                        : "span ran out");
    return false;
}

/* Give back everything amdvi_unit_init took, in the reverse order it took it.
 *
 * Every failure after vmm_map_mmio used to return with the register window
 * still mapped and whatever tables had already been bought still out of the
 * PMM's hands -- as much as 2 MiB of device table plus three pages, per failed
 * IVHD, on a machine that is then told it has no IOMMU there. Nothing else in
 * this driver ever comes back for them: a unit that does not reach
 * `u->online = true` is not counted, not indexed and not reachable.
 *
 * The tables are safe to hand back at every one of those points because none
 * of them has been published to the hardware yet: DEV_TAB_BAR, CMD_BUF_BAR
 * and EVT_LOG_BAR are written further down, and until they are the IOMMU is
 * still fetching from whatever firmware gave it. That holds for the quiesce
 * failure too -- the unit would not stop, but what it would not stop USING is
 * firmware's buffers, not ours.
 *
 * The struct is left blank, because amdvi_init hands the same slot to the next
 * IVHD and a half-filled one would give that attempt the last one's pointers
 * on any path that returns before overwriting them. */
static void amdvi_unit_unwind(amdvi_unit_t* u) {
    if (u->sem)     pmm_free((void*)(uintptr_t)u->sem_phys, 1);
    if (u->evt_log) pmm_free((void*)(uintptr_t)u->evt_log_phys,
                             EVT_LOG_BYTES / 4096);
    if (u->cmd_buf) pmm_free((void*)(uintptr_t)u->cmd_buf_phys,
                             CMD_BUF_BYTES / 4096);
    if (u->dev_tab) pmm_free((void*)(uintptr_t)u->dev_tab_phys,
                             (size_t)(DEV_TAB_BYTES / 4096));
    if (u->reg)     vmm_unmap_mmio(u->reg, 16384);
    memset(u, 0, sizeof(*u));
}

static int amdvi_unit_init(amdvi_unit_t* u, uint64_t reg_phys,
                            uint16_t seg) {
    u->reg_phys     = reg_phys;
    u->pci_segment  = seg;
    u->reg = (volatile uint8_t*)vmm_map_mmio((uintptr_t)reg_phys, 16384,
                  VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                  VMM_FLAG_CACHE_DISABLE);
    if (!u->reg) return -1;   /* nothing taken yet but the failed mapping */

    /* Allocate tables. */
    u->dev_tab = alloc_aligned_4k(&u->dev_tab_phys, (size_t)DEV_TAB_BYTES);
    if (!u->dev_tab) { amdvi_unit_unwind(u); return -2; }
    u->cmd_buf = alloc_aligned_4k(&u->cmd_buf_phys, CMD_BUF_BYTES);
    if (!u->cmd_buf) { amdvi_unit_unwind(u); return -3; }
    u->evt_log = alloc_aligned_4k(&u->evt_log_phys, EVT_LOG_BYTES);
    if (!u->evt_log) { amdvi_unit_unwind(u); return -4; }
    /* The completion semaphore gets a page of its own: the IOMMU stores into
     * it by physical address while this unit's command buffer keeps being
     * written by the CPU, and the two must not share a qword. */
    u->sem = (volatile uint64_t*)alloc_aligned_4k(&u->sem_phys, sizeof(uint64_t));
    if (!u->sem) { amdvi_unit_unwind(u); return -5; }
    u->sem_seq = 0;

    /* The device table arrives zeroed, so every entry reads V = 0 and Table 8
     * forwards that device's addresses "without translation". It stays that
     * way until amdvi_seat_device writes the entries for the functions PCI
     * actually reports, which happens once, before IommuEn. Filling all 65536
     * entries here would seat devices that do not exist. */

    /* Stop the unit before any of its tables move.
     *
     * §2.4 for the command buffer ("CmdBufRun must be 0b to modify the command
     * buffer registers properly") and §2.5.1 for the event log ("EventLogRun
     * must be 0b to modify the event log registers safely") both require it,
     * and MMIO Offset 0010h carries the note in its own right: "If EventLen or
     * EventBase is changed while the EventLogRun = 1, the IOMMU behavior is
     * undefined."
     *
     * Both procedures are written for a unit the hardware has ALREADY halted,
     * so they read "wait for the Run bit, then clear the enable". A unit
     * firmware hands over running never halts by itself — 2020h[CmdBufRun]:
     * "once set to 1, it does not change to 0 until command processing stops
     * for cause" — and the cause software has is 0018h[CmdBufEn] = 0, which
     * "causes the IOMMU to cease fetching new commands although commands
     * previously fetched are completed". So the enables are cleared first and
     * the wait follows them; that write is what lets the wait terminate.
     *
     * What stood here did neither. It wrote the three base registers first and
     * only then read CTRL and ORed two enable bits into it, so a firmware-set
     * IommuEn survived untouched and every base register was reprogrammed on a
     * live IOMMU. The header of this file claimed init left translation off; it
     * could not, because nothing here ever cleared bit 0. */
    uint64_t ctrl = *(volatile uint64_t*)(u->reg + MMIO_CTRL);
    if (ctrl & CTRL_IOMMU_EN) {
        kprintf("[AMD-Vi] seg=%u base=0x%lx: firmware handed this IOMMU over "
                "ENABLED — stopping it to install this kernel's tables; the "
                "DMA protection firmware had established ends here\n",
                (unsigned)seg, (unsigned long)reg_phys);
    }
    ctrl &= ~(CTRL_IOMMU_EN | CTRL_CMD_BUF_EN | CTRL_EVT_LOG_EN |
              CTRL_EVT_INT_EN | CTRL_PPR_LOG_EN | CTRL_PPR_INT_EN |
              CTRL_GA_LOG_EN | CTRL_GA_INT_EN);
    *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl;

    if (!amdvi_await_status_clear(u, STATUS_CMD_BUF_RUN | STATUS_EVT_LOG_RUN,
                                  "CmdBufRun and EventLogRun")) {
        kprintf("[AMD-Vi] seg=%u base=0x%lx: left disabled rather than have "
                "its base registers moved while it is still using them\n",
                (unsigned)seg, (unsigned long)reg_phys);
        amdvi_unit_unwind(u);
        return -6;
    }

    /* Program base registers on a stopped unit.
     *
     * ComLen and EventLen are bits 59:56, not a low-order size class, and they
     * count ENTRIES in powers of two rather than encoding log2 of a byte count:
     * §3.4.1 gives 1000b = 256 entries = 4 Kbytes as the minimum, and values
     * below 1000b as reserved. 8 is that minimum, which is what both buffers
     * are. */
    *(volatile uint64_t*)(u->reg + MMIO_DEV_TAB_BAR) =
        u->dev_tab_phys | DEV_TAB_SIZE_CLASS;
    *(volatile uint64_t*)(u->reg + MMIO_CMD_BUF_BAR) =
        u->cmd_buf_phys | (8ULL << 56);
    *(volatile uint64_t*)(u->reg + MMIO_EVT_LOG_BAR) =
        u->evt_log_phys | (8ULL << 56);

    /* The head and tail pointers belong to the same two procedures as the base
     * registers, and the two buffers are not in the same position here.
     *
     * The command buffer's pair is reset by the write just above -- §2.4: "When
     * the Command Buffer Base Address Register [MMIO Offset 0008h] register is
     * written, the Command Buffer Head Pointer Register [MMIO Offset 2000h] and
     * the Command Buffer Tail Pointer Register [MMIO Offset 2008h] are reset to
     * the 0." Writing them is the procedure's own step and costs nothing, and
     * the mirror in u->cmd_tail must be brought to the same 0 in any case.
     *
     * The event log's pair carries no such sentence, and EvtHeadPtr keeps
     * firmware's offset into a buffer that no longer exists. That one has to be
     * zeroed by hand, which is why §2.5.1 lists both pointers beside the base
     * and why Linux writes them explicitly (iommu_enable_event_buffer). */
    *(volatile uint64_t*)(u->reg + MMIO_CMD_HEAD) = 0;
    *(volatile uint64_t*)(u->reg + MMIO_CMD_TAIL) = 0;
    *(volatile uint64_t*)(u->reg + MMIO_EVT_HEAD) = 0;
    *(volatile uint64_t*)(u->reg + MMIO_EVT_TAIL) = 0;
    u->cmd_tail = 0;

    /* Where this unit is allowed to store a COMPLETION_WAIT.
     *
     * §2.12.2.2 "Completion Wait Store Restrictions": on a part that supports
     * SNP the exclusion range is gone and the Exclusion Base / Exclusion
     * Range Limit registers ARE the Completion Store Base and Limit. A
     * COMPLETION_WAIT whose store address falls outside that window is not
     * stored, and this driver's whole exit from amdvi_completion_wait is that
     * store: every invalidation would spend a full second discovering that
     * the semaphore never landed, and say so, on every AMD board with SNP
     * silicon. Firmware's window is not ours — the semaphore is a page this
     * kernel bought a moment ago.
     *
     * Base equal to Limit names exactly one 4 KiB page, which is exactly what
     * the semaphore was given. Linux writes the same two registers with the
     * same masked address in iommu_set_cwwb_range, and keys it off the same
     * EFR bit rather than off any MSR: the register pair belongs to the
     * IOMMU, and the IOMMU's own feature register is what says the pair has
     * changed meaning.
     *
     * The write is harmless where SNPSup is set but SNP is not in force: the
     * low bits of the Exclusion Base carry ExEn and Allow, both left 0 here,
     * so read as an exclusion range the pair is disabled. */
    uint64_t efr = *(volatile uint64_t*)(u->reg + MMIO_EXT_FEAT);
    u->invalidate_all_supported = (efr & EFR_IA_SUP) != 0;
    if (efr & EFR_SNP_SUP) {
        uint64_t store = u->sem_phys & AMDVI_STORE_ADDR_MASK;
        *(volatile uint64_t*)(u->reg + MMIO_EXCL_BASE)  = store;
        *(volatile uint64_t*)(u->reg + MMIO_EXCL_LIMIT) = store;
        kprintf("[AMD-Vi] seg=%u base=0x%lx: SNPSup — completion store window "
                "set to the one page holding this unit's semaphore "
                "(0x%lx)\n", (unsigned)seg, (unsigned long)reg_phys,
                (unsigned long)store);
    }

    /* §2.5.1, before the log is restarted: "Write the IOMMU Status Register
     * with EventOverflow = 1b to clear the bit (W1C)." An EventOverflow left
     * standing by firmware would keep the IOMMU from writing a single new entry
     * however EventLogRun reads. Every other bit written here is 0: the RW1C
     * bits ignore it and the RO bits cannot be written at all. */
    *(volatile uint64_t*)(u->reg + MMIO_STATUS) = STATUS_EVT_OVERFLOW;

    /* Now the enables, onto the value this function stopped the unit with —
     * never onto a fresh read, which is how IommuEn survived before. IommuEn
     * stays clear: amdvi_take_the_wheel() is the only place this kernel sets it,
     * and it runs only once every present device has an entry in the table
     * this function just published. */
    ctrl |= CTRL_CMD_BUF_EN | CTRL_EVT_LOG_EN;
    *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl;

    u->online = true;
    return 0;
}

/* Empty every cache this unit holds, and wait until it says it has.
 *
 * One command does it where the unit reports EFR[IASup] (§2.4.8: "At the
 * completion of an INVALIDATE_IOMMU_ALL command, all IOMMU caches are
 * empty"). Where it does not, this is not called at all: amdvi_take_the_wheel
 * refuses such a unit before IommuEn is ever set, because §2.4.8 also says
 * the command is "reserved and causes an ILLEGAL_COMMAND_ERROR" there, and
 * the alternative Linux uses — an INVALIDATE_DEVTAB_ENTRY for each of 65536
 * device IDs — needs a command ring this driver does not manage: 65536
 * sixteen-byte commands do not fit in a 256-entry buffer, and a push that
 * waits for the head to move is a mechanism that has to be built and proven,
 * not guessed at with no AMD board to prove it on. */
static bool amdvi_flush_all(amdvi_unit_t* u) {
    amdvi_cmd_push(u, ((uint64_t)CMD_OP_INVALIDATE_ALL << 60), 0);
    return amdvi_completion_wait(u);
}

static uint8_t amdvi_units_online(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < g_unit_count; i++)
        if (g_units[i].online) n++;
    return n;
}

/* The PCI segment of the index-th online unit. iommu.c walks config space
 * once per distinct segment a unit lives in, and nowhere else. */
static bool amdvi_unit_segment(uint8_t index, uint16_t* seg_out) {
    uint8_t seen = 0;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!g_units[i].online) continue;
        if (seen == index) { *seg_out = g_units[i].pci_segment; return true; }
        seen++;
    }
    return false;
}

/* Write this function's Device Table Entry, in every unit of its segment, and
 * issue NO command. See the note at the top of this file on why the seating
 * and the switching are two steps: with IommuEn clear the command engine does
 * not run (§3.4.1 bit 12), so a per-device COMPLETION_WAIT here could only
 * ever run its span out.
 *
 * DomainID 0 is used, and on this hardware that is legal with nothing to
 * shift past: Table 7 on DTE bits 79:64 says only "The DomainID is a 16-bit
 * integer chosen by software that the IOMMU must use", with the single rule
 * that "I/O devices that share the same DTE[DomainID] must have the same
 * settings in the DTE[Mode] and page table root" — which every entry written
 * here does, since they all name the one identity table. AMD reserves no
 * value the way VT-d reserves 0 for a CM=1 unit's fault-tagged entries. */
static int amdvi_seat_device(uint16_t seg, uint8_t bus, uint8_t devfn) {
    uint16_t devid = (uint16_t)(((uint16_t)bus << 8) | devfn);
    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online || u->pci_segment != seg) continue;
        if (!u->dev_tab) return -1;
        amdvi_dte_program((uint8_t*)u->dev_tab, devid, g_id_root_phys, 0);
    }
    return 0;
}

/* Take IommuEn back off every unit that has it — the undo half of
 * all-or-nothing. A machine where some IOMMUs are on and others are not is a
 * machine whose DMA protection depends on which socket a device hangs off. */
static void amdvi_wheel_release(void) {
    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online) continue;
        uint64_t ctrl = *(volatile uint64_t*)(u->reg + MMIO_CTRL);
        if (!(ctrl & CTRL_IOMMU_EN)) continue;
        *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl & ~CTRL_IOMMU_EN;
    }
}

/* Switch DMA translation on, on every online unit, or on none of them.
 *
 * Order, and every step of it is the specification's:
 *
 *   1. Device Table Base, Command Buffer Base and Event Log Base are already
 *      published on a stopped unit and their head/tail pointers reset —
 *      amdvi_unit_init does that under §2.4 and §2.5.1, which allow those
 *      registers to move only while the matching Run bit reads 0.
 *   2. Every PCI function on the machine has a DTE by now: iommu.c walks
 *      config space and calls amdvi_seat_device for each one BEFORE calling
 *      this. A function with no entry reads V = 0, and Table 8 has the IOMMU
 *      forward its addresses "without translation" — the device keeps
 *      working and is simply outside the protection, which is why seating is
 *      all-or-nothing rather than best-effort.
 *   3. IommuEn, via MMIO Offset 0018h, onto a fresh read of the control
 *      register so nothing else in it is disturbed, then read back. The
 *      readback proves only that the write reached the register — this is a
 *      RW field with no status bit beside it, and a read of what one just
 *      wrote is not evidence that the unit started.
 *   4. INVALIDATE_IOMMU_ALL and a COMPLETION_WAIT on THAT unit, immediately,
 *      before the next one is touched. Two things ride on it. It clears
 *      whatever firmware left cached from its own device table — which is
 *      what step 4 exists for, and doing every enable first would leave each
 *      unit translating out of those caches for the length of the other
 *      loop. And the semaphore landing is the real proof that the unit came
 *      up: §3.4.1 bit 12, "When CmdBufEn = 1b and IommuEn = 1b, the IOMMU
 *      starts fetching commands", so a command that COMPLETES is the
 *      hardware's own statement that IommuEn is in force. A unit whose
 *      semaphore never lands takes the whole machine back off.
 *
 * Returns the number of units translating, which is either every online unit
 * or, on any refusal, a negative number with nothing left switched on. */
static int amdvi_take_the_wheel(uint8_t* units_total) {
    uint8_t online = amdvi_units_online();
    if (units_total) *units_total = online;

    if (online == 0) {
        kprintf("[AMD-Vi] no IOMMU is online — nothing to switch on\n");
        return -1;
    }
    if (!g_id_root) {
        kprintf("[AMD-Vi] no identity map was built — translation stays off\n");
        return -1;
    }

    /* Is the table the IOMMU is about to walk actually populated where it has
     * to be? Answered by READING the entries at the three addresses that
     * bound what a device can name — the bottom, the top of what a Single
     * Address Cycle reaches, the top of the memory map — and then at both
     * ends of every block the IVRS says a peripheral needs. What stood here
     * compared the build's own recorded span against the number the build had
     * been given, which is the same number rounded up and can never
     * disagree. */
    uint64_t top = pmm_get_mem_end();
    if (top < AMDVI_SAC_SPAN) top = AMDVI_SAC_SPAN;
    if (!amd_identity_covers(0) ||
        !amd_identity_covers(AMDVI_SAC_SPAN - 4096) ||
        !amd_identity_covers(top - 4096)) {
        kprintf("[AMD-Vi] the I/O page tables do not translate one of 0x0, "
                "0x%lx or 0x%lx — the map is not what this kernel believes it "
                "built, so translation stays OFF everywhere rather than run a "
                "device into a hole\n",
                (unsigned long)(AMDVI_SAC_SPAN - 4096),
                (unsigned long)(top - 4096));
        return -1;
    }
    bool reserved_ok = true;
    amdvi_walk_ivmd(amd_check_ivmd, &reserved_ok);
    if (!reserved_ok) return -1;

    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online || u->invalidate_all_supported) continue;
        kprintf("[AMD-Vi] IVHD[%u] reports EFR[IASup] = 0 (MMIO 0030h bit 6), "
                "so INVALIDATE_IOMMU_ALL would be an ILLEGAL_COMMAND_ERROR on "
                "it (§2.4.8) and this kernel has no other bounded way to empty "
                "what firmware may have cached from its device table. "
                "Translation is switched on NOWHERE on this machine rather "
                "than on the units that could take it\n", i);
        return -1;
    }

    uint8_t on = 0;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online) continue;
        uint64_t ctrl = *(volatile uint64_t*)(u->reg + MMIO_CTRL);
        *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl | CTRL_IOMMU_EN;
        if (!(*(volatile uint64_t*)(u->reg + MMIO_CTRL) & CTRL_IOMMU_EN)) {
            kprintf("[AMD-Vi] IVHD[%u] at 0x%lx did not keep IommuEn — taking "
                    "it back off the %u unit(s) that did, so this machine "
                    "translates nowhere rather than in places\n",
                    i, (unsigned long)u->reg_phys, on);
            amdvi_wheel_release();
            return -1;
        }
        /* This unit's caches emptied before the next one is touched, and the
         * semaphore landing is what says the unit is genuinely running. */
        if (!amdvi_flush_all(u)) {
            kprintf("[AMD-Vi] IVHD[%u] at 0x%lx did not complete the "
                    "INVALIDATE_IOMMU_ALL that empties what firmware left "
                    "cached — with no completion there is no evidence the "
                    "unit is fetching commands at all, so translation is "
                    "taken back off every unit\n",
                    i, (unsigned long)u->reg_phys);
            amdvi_wheel_release();
            return -1;
        }
        on++;
        debug_printf("[AMD-Vi] IVHD[%u] IommuEn=1, caches empty\n", i);
    }

    return on;
}

/* ----- ops vtable ----- */

struct iommu_domain {
    uint32_t id;
    uint64_t* host_pt;
    uint64_t  host_pt_phys;
    bool      is_identity;
};
#define AMDVI_MAX_DOMAINS  64
static struct iommu_domain g_domains[AMDVI_MAX_DOMAINS];
static uint8_t             g_domain_count = 1;

static int amdvi_init(void) {
    const acpi_ivrs_info_t* i = acpi_get_ivrs();
    if (!i) return -1;
    if (amd_identity_build() < 0) return -2;
    g_unit_count = 0;
    for (uint8_t k = 0; k < i->ivhd_count && g_unit_count < AMDVI_MAX_UNITS; k++) {
        if (amdvi_unit_init(&g_units[g_unit_count],
                            i->ivhd[k].iommu_base,
                            i->ivhd[k].pci_segment) == 0) {
            debug_printf("[AMD-Vi] IVHD[%u] online: seg=%u base=0x%lx\n",
                         k, i->ivhd[k].pci_segment,
                         (unsigned long)i->ivhd[k].iommu_base);
            g_unit_count++;
        }
    }
    g_domains[0].id           = 0;
    g_domains[0].host_pt      = g_id_root;
    g_domains[0].host_pt_phys = g_id_root_phys;
    g_domains[0].is_identity  = true;
    g_domain_count            = 1;
    debug_printf("[AMD-Vi] %u IOMMU(s) ready; IommuEn waits on "
                 "iommu_take_the_wheel(), after PCI is enumerated\n",
                 g_unit_count);
    return (g_unit_count > 0) ? 0 : -3;
}

static iommu_domain_t* amdvi_domain_alloc(void) {
    if (g_domain_count >= AMDVI_MAX_DOMAINS) return NULL;
    struct iommu_domain* d = &g_domains[g_domain_count];
    void* p = alloc_aligned_4k(&d->host_pt_phys, 4096);
    if (!p) return NULL;
    d->host_pt    = (uint64_t*)p;
    d->id         = g_domain_count;
    d->is_identity = false;
    g_domain_count++;
    return d;
}
static void amdvi_domain_free(iommu_domain_t* d) {
    if (!d || d->id == 0) return;
    d->host_pt = NULL;
    d->host_pt_phys = 0;
}

static int amdvi_device_attach(iommu_domain_t* d, uint16_t seg,
                                uint8_t bus, uint8_t devfn) {
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (g_units[i].pci_segment != seg) continue;
        uint16_t devid = (uint16_t)(((uint16_t)bus << 8) | devfn);
        uint64_t pt   = d ? d->host_pt_phys : g_id_root_phys;
        uint16_t did  = d ? (uint16_t)d->id : 0;
        amdvi_dte_program((uint8_t*)g_units[i].dev_tab, devid, pt, did);
        amdvi_cmd_push(&g_units[i],
            ((uint64_t)CMD_OP_INVALIDATE_DEV_TAB << 60) | devid, 0);
        amdvi_completion_wait(&g_units[i]);
        return 0;
    }
    return -1;
}
static int amdvi_device_detach(iommu_domain_t* d, uint16_t seg,
                                uint8_t bus, uint8_t devfn) {
    (void)d;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (g_units[i].pci_segment != seg) continue;
        uint16_t devid = (uint16_t)(((uint16_t)bus << 8) | devfn);
        uint8_t* dte_base = (uint8_t*)g_units[i].dev_tab;
        uint64_t* dte = (uint64_t*)(dte_base + (size_t)devid * 32);
        dte[0] = 0; dte[1] = 0; dte[2] = 0; dte[3] = 0;
        amdvi_cmd_push(&g_units[i],
            ((uint64_t)CMD_OP_INVALIDATE_DEV_TAB << 60) | devid, 0);
        amdvi_completion_wait(&g_units[i]);
        return 0;
    }
    return -1;
}

static int amdvi_map(iommu_domain_t* d, uint64_t iova, uint64_t phys,
                      uint64_t sz, uint32_t perm) {
    if (!d || d->is_identity) {
        if (iova != phys) return -1;
        return 0;
    }
    /* A present entry granting neither read nor write is not a permissive
     * mapping, it is one that target-aborts everything that touches it, so a
     * request that names no permission is refused rather than written. */
    if (!(perm & (IOMMU_PERM_READ | IOMMU_PERM_WRITE))) return -1;

    /* Per-domain map: walk d->host_pt; reuse amd_pt_walk. */
    uint64_t end = iova + sz;
    for (uint64_t a = iova & ~0xFFFULL; a < end; a += 4096) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
        uint64_t* l3 = amd_pt_walk(d->host_pt, i4, AMD_PT_MODE - 1, true);
        if (!l3) return -1;
        uint64_t* l2 = amd_pt_walk(l3, i3, AMD_PT_MODE - 2, true);
        if (!l2) return -1;
        uint64_t* l1 = amd_pt_walk(l2, i2, AMD_PT_MODE - 3, true);
        if (!l1) return -1;
        uint64_t pte = (phys + (a - (iova & ~0xFFFULL))) & AMD_PTE_ADDR_MASK;
        pte |= AMD_PTE_PR;
        if (perm & IOMMU_PERM_READ)  pte |= AMD_PTE_IR;
        if (perm & IOMMU_PERM_WRITE) pte |= AMD_PTE_IW;
        l1[i1] = pte;
    }
    return 0;
}
/* TME-MK aware map. AMD-Vi rev 4 "Cache Coherent Memory" mode uses the
 * same SL-PTE phys-field layout as the CPU PTE — bits [51:12] hold the
 * phys with the upper num_keyid_bits portion encoding the KeyID. The
 * existing amdvi_map keeps that whole field via `phys & AMD_PTE_ADDR_MASK`
 * (51:12 — the bits above it are the entry's own IR/IW/FC/U, not address),
 * so we just route through it for non-identity domains. Identity domain
 * is rejected: per-KeyID DMA only makes sense in a per-device domain. */
static int amdvi_map_with_keyid(iommu_domain_t* d, uint64_t iova,
                                  uint64_t phys_with_keyid, uint64_t sz,
                                  uint32_t perm) {
    if (!d || d->is_identity) return -1;
    return amdvi_map(d, iova, phys_with_keyid, sz, perm);
}

static int amdvi_unmap(iommu_domain_t* d, uint64_t iova, uint64_t size) {
    if (!d || d->is_identity) return 0;
    uint64_t end = iova + size;
    for (uint64_t a = iova & ~0xFFFULL; a < end; a += 4096) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
        uint64_t* l3 = amd_pt_walk(d->host_pt, i4, AMD_PT_MODE - 1, false);
        if (!l3) continue;
        uint64_t* l2 = amd_pt_walk(l3, i3, AMD_PT_MODE - 2, false);
        if (!l2) continue;
        uint64_t* l1 = amd_pt_walk(l2, i2, AMD_PT_MODE - 3, false);
        if (!l1) continue;
        l1[i1] = 0;
    }
    return 0;
}

/* The 4-bit EventCode of an event log entry, named.
 *
 * The names come from the per-event figures (§2.5.2 Figure 52 through §2.5.15
 * Figure 66), not from Table 42 "Event Type Summary", because Table 42 is
 * stale inside its own document: it gives EVENT_COUNTER_ZERO the same 1001b
 * as INVALID_PPR_REQUEST, and it calls "1010b-1111b Reserved; not used" while
 * five of those codes have a figure and a field table of their own -- 1010b
 * EVENT_COUNTER_ZERO (Figure 62), 1011b GUEST_EVENT_FAULT (Figure 63), 1100b
 * VIOMMU_HARDWARE_ERROR (Figure 64), 1101b RMP_PAGE_FAULT (Figure 65), 1110b
 * RMP_HARDWARE_ERROR (Figure 66). The figure is normative for the entry it
 * describes; the summary row is not.
 *
 * The name printed is the spec's own spelling, not an abbreviation borrowed
 * from another driver. Two codes no figure defines are left unnamed -- 0000b,
 * which Table 42 does call Reserved, and 1111b, which nothing in the document
 * mentions -- and for those the number is all that is said. This function
 * makes no claim about what the specification does or does not contain: it
 * reports only whether THIS driver has a name. */
static const char* amdvi_event_name(uint32_t code) {
    switch (code) {
    case 0x1: return "ILLEGAL_DEV_TABLE_ENTRY";   /* Figure 52 */
    case 0x2: return "IO_PAGE_FAULT";             /* Figure 53 */
    case 0x3: return "DEV_TAB_HARDWARE_ERROR";    /* Figure 54 */
    case 0x4: return "PAGE_TAB_HARDWARE_ERROR";   /* Figure 55 */
    case 0x5: return "ILLEGAL_COMMAND_ERROR";     /* Figure 56 */
    case 0x6: return "COMMAND_HARDWARE_ERROR";    /* Figure 57 */
    case 0x7: return "IOTLB_INV_TIMEOUT";         /* Figure 58 */
    case 0x8: return "INVALID_DEVICE_REQUEST";    /* Figure 59 */
    case 0x9: return "INVALID_PPR_REQUEST";       /* Figures 60, 61 */
    case 0xA: return "EVENT_COUNTER_ZERO";        /* Figure 62 */
    case 0xB: return "GUEST_EVENT_FAULT";         /* Figure 63 */
    case 0xC: return "VIOMMU_HARDWARE_ERROR";     /* Figure 64 */
    case 0xD: return "RMP_PAGE_FAULT";            /* Figure 65 */
    case 0xE: return "RMP_HARDWARE_ERROR";        /* Figure 66 */
    default:  return "(this driver has no name for this code)";
    }
}

/* Whether dword +00 bits 15:0 of an entry with this EventCode is a DeviceID.
 *
 * Figure 51 calls that dword nothing more specific than "First event code
 * dependent operand [31:0]", and for five codes it is not a device:
 * ILLEGAL_COMMAND_ERROR (Figure 56 / Table 61) and COMMAND_HARDWARE_ERROR
 * (Figure 57 / Table 62) leave +00 entirely Reserved and put the offending
 * command's system physical address in +04/+08; EVENT_COUNTER_ZERO (Figure 62
 * / Table 67) and GUEST_EVENT_FAULT (Figure 63) leave it Reserved as well;
 * and VIOMMU_HARDWARE_ERROR (Figure 64) puts a GuestID in those sixteen bits,
 * which is not a device at all. Printing a bus:dev.fn for any of them names a
 * device that did nothing -- in practice 00:00.0, the host bridge -- while
 * hiding the operand that actually says what happened. */
static bool amdvi_event_has_devid(uint32_t code) {
    switch (code) {
    case 0x1: case 0x2: case 0x3: case 0x4:   /* Figures 52, 53, 54, 55 */
    case 0x7: case 0x8: case 0x9:             /* Figures 58, 59, 60/61   */
    case 0xD: case 0xE:                       /* Figures 65, 66          */
        return true;
    default:
        return false;
    }
}

/* Drain the event log on every IOMMU.
 *
 * An entry is 16 bytes and its EventCode is the top nibble of the SECOND
 * dword -- AMD-Vi §2.5 "Generic Event Log Buffer Entry" (Figure 51 in
 * revision 3.06): dword +00 carries "First event code dependent operand
 * [31:0]" and nothing more specific than that, and dword +04 carries
 * EventCode in bits 31:28 above the rest of that operand. What the first
 * operand MEANS is settled by the figure for the individual event and by
 * nothing else, which is why amdvi_event_has_devid exists rather than a
 * blanket devid read. The top nibble of
 * dword +00, which this code used to read, is a set of operand-validity
 * flags -- so a real ILLEGAL_DEV_TABLE_ENTRY with one of them set decoded
 * here as 0x2, which is the EventCode of IO_PAGE_FAULT. Not a nonsense
 * number: a plausible, wrong event. Linux reads the nibble this reads now:
 * `type = (event[1] >> EVENT_TYPE_SHIFT) & EVENT_TYPE_MASK` with
 * EVENT_TYPE_SHIFT = 28. (The "§3.1.6" the old comment cited names nothing
 * in this specification.)
 *
 * EventOverflow is the other half of the channel, and it is the half that
 * decides how long the channel lasts. §3.4.16, MMIO 2020h bit 0: "No new
 * event log entries are written while this bit is set." §2.5.1 restarts the
 * log -- clear EventOverflow (W1C) and take 0018h[EventLogEn] back to 1, and
 * the hardware wants that bit driven to 0 first, which is what Linux's
 * amd_iommu_restart_event_logging does. A 4 KiB log holds 255 usable
 * entries, so one misconfigured device faulting at DMA rate fills it in
 * microseconds; without this, the IOMMU's only way of reporting
 * IO_PAGE_FAULT, ILLEGAL_DEV_TABLE_ENTRY and DEV_TAB_HARDWARE_ERROR is dead
 * for the rest of the boot and nothing says so.
 *
 * Not callable from an interrupt handler: TouchPublish is not IRQ-safe
 * (touch.h -- interrupt context uses TouchPublishIrqPair), and this routine
 * waits on EventLogRun with an HourGlass. The comment that stood here said
 * "callers run this from the SCI handler or as a periodic poll"; an SCI
 * handler is the one place it may not run from. It is reached now as this
 * backend's drain_faults, by way of amdvi_drain_faults.
 *
 * It is reached through amdvi_drain_faults, and only for the units whose
 * IommuEn is actually set, because with that bit clear this function has
 * provably nothing to do -- see amdvi_drain_faults for the proof, which is
 * in this file and not in the specification. The sentence that stood here --
 * that amdvi_unit_init arms the log so it fills whether or not anybody
 * drains it -- was wrong. MMIO Offset 0018h bit 2 EventLogEn: "all events
 * detected are written to the event log when IommuEn has also been set", and
 * "Events are discarded when the event log is not enabled". amdvi_unit_init
 * sets EventLogEn and deliberately leaves IommuEn clear, so on AMD the eyes
 * do not open until IommuEn does. */
void amdvi_poll_events(void) {
    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online) continue;

        volatile uint64_t* status = (volatile uint64_t*)(u->reg + MMIO_STATUS);
        bool     overflowed = (*status & STATUS_EVT_OVERFLOW) != 0;
        uint64_t ctrl       = 0;

        if (overflowed) {
            kprintf("[AMD-Vi] IVHD[%u] event log OVERFLOWED — every fault the "
                    "IOMMU tried to report after it filled was discarded, and "
                    "it has written nothing since\n", i);
            /* The log is restarted around the drain, and the enable goes off
             * first for the reason amdvi_unit_init spells out: EventLogRun
             * does not fall until the IOMMU has a cause to stop, and
             * 0018h[EventLogEn] = 0 is the cause software has. */
            ctrl = *(volatile uint64_t*)(u->reg + MMIO_CTRL);
            *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl & ~CTRL_EVT_LOG_EN;
            if (!amdvi_await_status_clear(u, STATUS_EVT_LOG_RUN,
                                          "EventLogRun")) {
                /* EventLogEn goes back exactly as it was found. A routine
                 * whose whole job is to READ the log must not be the thing
                 * that switches the log off: the drain is given up, but the
                 * unit is left in the configuration it arrived in. The
                 * channel stays silent all the same, because EventOverflow
                 * is still standing and §3.4.16 says no new entry is written
                 * while it is -- that is the IOMMU's doing, not ours. */
                *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl;
                kprintf("[AMD-Vi] IVHD[%u] would not stop writing its event "
                        "log — the head pointer is left where it is rather "
                        "than moved under a running IOMMU, EventLogEn is put "
                        "back as it was, and this unit reports no further "
                        "faults while EventOverflow stands\n", i);
                continue;
            }
        }

        uint64_t head = *(volatile uint64_t*)(u->reg + MMIO_EVT_HEAD);
        uint64_t tail = *(volatile uint64_t*)(u->reg + MMIO_EVT_TAIL);
        while (head != tail) {
            const uint8_t* slot =
                (const uint8_t*)u->evt_log + (head & (EVT_LOG_BYTES - 1));
            uint32_t code  = (*(const uint32_t*)(slot + 4) >> 28) & 0xFu;
            if (amdvi_event_has_devid(code)) {
                unsigned devid = *(const uint32_t*)slot & 0xFFFFu;
                kprintf("[AMD-Vi] IVHD[%u] event 0x%x %s from %02x:%02x.%x\n",
                        i, code, amdvi_event_name(code),
                        devid >> 8, (devid >> 3) & 0x1Fu, devid & 0x7u);
            } else {
                /* No device to name, so the two operands are said as the
                 * hardware wrote them. For ILLEGAL_COMMAND_ERROR and
                 * COMMAND_HARDWARE_ERROR the second one is the system
                 * physical address of the command the IOMMU choked on
                 * (Figures 56, 57) -- the whole of what the entry has to
                 * tell, and what naming a bus:dev.fn used to hide. */
                kprintf("[AMD-Vi] IVHD[%u] event 0x%x %s — operands "
                        "0x%lx 0x%lx\n",
                        i, code, amdvi_event_name(code),
                        (unsigned long)*(const uint64_t*)slot,
                        (unsigned long)*(const uint64_t*)(slot + 8));
            }
            /* Per-event broadcast — userspace logger / fault recoverer
             * subscribes by `iommu:fault` and decodes the 16-byte slot
             * we shipped as payload. */
            TouchPublish("iommu:fault", slot, 16);
            head = (head + 16) & (EVT_LOG_BYTES - 1);
        }
        *(volatile uint64_t*)(u->reg + MMIO_EVT_HEAD) = tail;

        if (overflowed) {
            /* §2.5.1, in this order: the overflow bit cleared (W1C) now that
             * the log is empty, then EventLogEn asserted again. Every other
             * bit written to 2020h is 0 — the RW1C bits ignore it and the RO
             * bits cannot be written at all. */
            *status = STATUS_EVT_OVERFLOW;
            *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl | CTRL_EVT_LOG_EN;
            kprintf("[AMD-Vi] IVHD[%u] event log restarted; what it held "
                    "before the overflow is all there is of what was lost\n",
                    i);
        }
    }
}
static void amdvi_invalidate(iommu_domain_t* d) {
    (void)d;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!g_units[i].online) continue;
        /* §2.4.8: on a unit that does not report EFR[IASup] this command "is
         * reserved and causes an ILLEGAL_COMMAND_ERROR". Issuing it anyway
         * used to be harmless only because IommuEn was never set and the
         * command was never fetched; the moment translation is on it would be
         * a real event in the log and a real invalidation that never
         * happened. The refusal is said, not swallowed. */
        if (!g_units[i].invalidate_all_supported) {
            kprintf("[AMD-Vi] IVHD[%u] does not support INVALIDATE_IOMMU_ALL "
                    "(EFR[IASup] = 0) — its caches were NOT invalidated\n", i);
            continue;
        }
        amdvi_flush_all(&g_units[i]);
    }
}

/* Phase 2G — opaque domain → ID accessor. Trivial field read. */
static uint32_t amdvi_domain_id(iommu_domain_t* d) {
    return d ? ((struct iommu_domain*)d)->id : 0xFFFFFFFFu;
}

/* This backend's drain_faults.
 *
 * What it exists to avoid is a call that LOOKS like an inspection and is a
 * no-op by construction. Wiring amdvi_poll_events straight into the vtable
 * would be exactly that, and the proof is three lines of this file rather
 * than anything in the specification:
 *
 *   - amdvi_unit_init writes 0 to MMIO_EVT_HEAD and 0 to MMIO_EVT_TAIL, so
 *     on entry head == tail and the drain loop does not run once;
 *   - amdvi_unit_init writes STATUS_EVT_OVERFLOW (W1C) before it brings the
 *     unit online, so firmware's overflow is already gone and the overflow
 *     branch -- the one that stops the log and waits on EventLogRun -- is
 *     unreachable from here;
 *   - the hardware cannot have refilled either, because amdvi_unit_init
 *     leaves IommuEn clear and MMIO Offset 0018h bit 2 EventLogEn says the
 *     events are written "when IommuEn has also been set", with "events are
 *     discarded when the event log is not enabled".
 *
 * So on every AMD board today this channel is empty, and it is empty for a
 * reason the board should hear said out loud rather than infer from a
 * missing line. That is what this does: it reads the bit -- not a guess, not
 * a cached flag, the live IommuEn out of the unit's own control register --
 * and reports the unit as deaf by configuration. Where the bit IS set, which
 * is only after amdvi_take_the_wheel, the log is real and amdvi_poll_events
 * reads it. */
static void amdvi_drain_faults(void) {
    bool any_live = false;

    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online) continue;
        if (*(volatile uint64_t*)(u->reg + MMIO_CTRL) & CTRL_IOMMU_EN) {
            any_live = true;
            continue;
        }
        kprintf("[AMD-Vi] IVHD[%u] has nothing to report and cannot have: "
                "IommuEn is clear in its control register, and 0018h[2] "
                "writes events to the log only \"when IommuEn has also been "
                "set\" -- everything this unit detects is discarded before "
                "it is written. This is not an empty log, it is a closed "
                "eye, and amdvi_take_the_wheel is what opens it\n", i);
    }

    /* Before iommu_take_the_wheel has run, no unit is live and this is where
     * the AMD path ends. Afterwards the same call reads a channel that is
     * genuinely filling -- which is why iommu_take_the_wheel ends by calling
     * this one more time, at the first instant an IO_PAGE_FAULT is possible. */
    if (any_live) amdvi_poll_events();
}

const iommu_ops_t amdvi_ops = {
    .name           = "AMD-Vi",
    .init           = amdvi_init,
    .domain_alloc   = amdvi_domain_alloc,
    .domain_free    = amdvi_domain_free,
    .device_attach  = amdvi_device_attach,
    .device_detach  = amdvi_device_detach,
    .map            = amdvi_map,
    .unmap          = amdvi_unmap,
    .invalidate     = amdvi_invalidate,
    .domain_id      = amdvi_domain_id,
    .map_with_keyid = amdvi_map_with_keyid,
    .drain_faults   = amdvi_drain_faults,
    .unit_segment   = amdvi_unit_segment,
    .seat_device    = amdvi_seat_device,
    .take_the_wheel = amdvi_take_the_wheel,
};
