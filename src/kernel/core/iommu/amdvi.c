#include "iommu.h"
#include "acpi.h"
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
 *     the bottom 4 GiB (shared across all DTEs in "passthrough" mode)
 *   * Stop the unit, then program its base registers (DEV_TAB_BAR,
 *     CMD_BUF_BAR, EVT_LOG_BAR) and their head/tail pointers while it is
 *     stopped -- §2.4 and §2.5.1 permit those registers to move only while
 *     the matching Run bit in MMIO 2020h reads 0
 *   * Issue COMPLETION_WAIT to drain command buffer
 *   * `amdvi_enable()` flips IOMMU_CTRL.IommuEn. init() CLEARS it, together
 *     with CmdBufEn, EventLogEn and the PPR/GA log enables: an AMD board
 *     whose firmware hands the IOMMU over already running is the ordinary
 *     case, and its tables cannot be swapped underneath it
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
 * Translation is per DEVICE, in the DTE. Table 8, V = 0: "All addresses are
 * forwarded without translation; individual control fields are ignored."
 * Table 10, IV = 0: LINT0, LINT1, SMI, NMI, INIT and ExtInt "are passed
 * through unmapped". amdvi_unit_init leaves the device table zeroed and
 * nothing in this tree calls iommu_device_attach, so every entry reads V = 0
 * and IV = 0 today: IommuEn = 1 over this table would start the log and the
 * command engine and translate nothing at all. Whether to take that step is
 * the owner's call, not this file's.
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

static int amd_identity_build(void) {
    if (g_id_root) return 0;
    void* p = alloc_aligned_4k(&g_id_root_phys, 4096);
    if (!p) return -1;
    g_id_root = (uint64_t*)p;
    /* Bottom 4 GiB at 4 KB granularity. */
    for (uint64_t a = 0; a < (1ULL << 32); a += 4096) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
        uint64_t* l3 = amd_pt_walk(g_id_root, i4, AMD_PT_MODE - 1, true);
        if (!l3) return -1;
        uint64_t* l2 = amd_pt_walk(l3, i3, AMD_PT_MODE - 2, true);
        if (!l2) return -1;
        uint64_t* l1 = amd_pt_walk(l2, i2, AMD_PT_MODE - 3, true);
        if (!l1) return -1;
        /* Page translation entry: NextLevel stays 0, read and write granted. */
        l1[i1] = (a & AMD_PTE_ADDR_MASK) | AMD_PTE_PR | AMD_PTE_IR | AMD_PTE_IW;
    }
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

static void amdvi_completion_wait(amdvi_unit_t* u) {
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
     * IommuEn = 1b". Every path into this function except amdvi_enable --
     * attach, detach, invalidate -- runs with IommuEn still clear, so the
     * command is written into a buffer nobody reads and this wait can only
     * ever run its span out and say the semaphore never landed. That is a
     * true report of a real condition, which is why it is left to say it
     * rather than short-circuited on a bit read here. */
    if (!u->cmd_buf || !u->sem) return;

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
        return;
    }
    for (;;) {
        if (*u->sem == want) return;
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

    /* Pre-fill DTE with V=0 (not valid) — devices become valid on
     * device_attach(). For an initial pass-everything-through model
     * we could instead V=1 every entry now; we keep V=0 to avoid
     * surprising firmware before drivers are ready. */

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
     * stays clear: amdvi_enable() is the only place this kernel sets it. */
    ctrl |= CTRL_CMD_BUF_EN | CTRL_EVT_LOG_EN;
    *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl;

    u->online = true;
    return 0;
}

int amdvi_enable(void) {
    int en = 0;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online) continue;
        uint64_t ctrl = *(volatile uint64_t*)(u->reg + MMIO_CTRL);
        ctrl |= CTRL_IOMMU_EN;
        *(volatile uint64_t*)(u->reg + MMIO_CTRL) = ctrl;
        amdvi_completion_wait(u);
        en++;
        debug_printf("[AMD-Vi] IVHD[%u] IommuEn=1\n", i);
    }
    return en;
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
    debug_printf("[AMD-Vi] %u IOMMU(s) ready; IommuEn NOT set "
                 "(call amdvi_enable())\n", g_unit_count);
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
        amdvi_cmd_push(&g_units[i],
            ((uint64_t)CMD_OP_INVALIDATE_ALL << 60), 0);
        amdvi_completion_wait(&g_units[i]);
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
 * is only after amdvi_enable, the log is real and amdvi_poll_events reads
 * it. */
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
                "eye, and amdvi_enable is what opens it\n", i);
    }

    /* Nothing calls amdvi_enable in this tree yet, so today this is where
     * the AMD path ends. The moment one does, the same call reads a channel
     * that is genuinely filling. */
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
};
