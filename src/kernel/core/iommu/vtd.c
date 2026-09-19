#include "iommu.h"
#include "acpi.h"
/* acpi_find_table — the DMAR's RMRR entries are read HERE rather than in the
 * ACPI parser, because acpi_dmar_info_t stores DRHDs and nothing else, and a
 * reserved region this driver does not map is a device that stops the instant
 * GCMD.TE goes on. The parser walks the same table for its own reasons; this
 * walk asks it a different question and keeps nothing. */
#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "hourglass.h"
#include "atomics.h"
#include "touch.h"  /* iommu:fault — the units' own account of what they refused */
#include "cpu.h"     /* cpu_cpuid — the CLFLUSH line size, for ECAP.C = 0 */

/* =====================================================================
 * Intel VT-d driver — production implementation.
 *
 * Reference: Intel Virtualization Technology for Directed I/O,
 *            Revision 5.0+ (the "VT-d" spec). Sections cited inline.
 *
 * Responsibilities
 * ----------------
 *   * Map each DRHD register block — as long as the unit says it is
 *   * Read CAP/ECAP; take the widest adjusted guest address width this
 *     driver can serve out of the unit's SAGAW — 48-bit with the 4-level
 *     table built below, or 39-bit with that table's own second level
 *     handed over as the root — and refuse a unit that reports neither
 *   * Where a unit does not snoop the structures it walks (ECAP.C = 0),
 *     put every structure this driver writes back to memory itself
 *   * Build a *single* second-level page table that identity-maps every
 *     physical address a device on this machine can name: the top of the
 *     memory map (pmm_get_mem_end()), never below the whole of what a Single
 *     Address Cycle can address, and every reserved region the DMAR's own
 *     RMRR entries publish — read out of the table, not assumed
 *   * Allocate one root table per remap unit; populate every context
 *     entry of buses we have devices on to point at the shared SLPT
 *   * Set RTADDR_REG, drain context cache + IOTLB
 *   * `vtd_take_the_wheel()` flips GCMD.TE on every online unit, and only
 *     after every PCI function on the machine has a context entry. It is
 *     ALL-OR-NOTHING: a unit that will not begin translating takes the
 *     others back off with it, because half a machine translating is worse
 *     than none of it. iommu.c is the only caller and it seats the devices
 *     first; see iommu_take_the_wheel.
 *
 * Public surface (iommu_ops_t) is in iommu.h; we register `vtd_ops`
 * at the bottom of this file.
 * ===================================================================== */

/* ----- VT-d register offsets (§10.4) ----- */
#define VTD_REG_VER       0x000
#define VTD_REG_CAP       0x008
#define VTD_REG_ECAP      0x010
#define VTD_REG_GCMD      0x018
#define VTD_REG_GSTS      0x01C
#define VTD_REG_RTADDR    0x020
#define VTD_REG_CCMD      0x028
#define VTD_REG_FSTS      0x034

/* Fault Status Register bits (§11.4.7.1). All three are RW1C: a 1 written
 * back into one is how it is cleared, and nothing else clears it.
 *
 *   IQE — hardware would not take a descriptor this driver wrote. §6.5.2.11:
 *         "The Head pointer Register is not incremented, and references the
 *         descriptor associated with the queue error. No new descriptors are
 *         fetched from the Invalidation Queue until software clears the IQE
 *         field in the Fault Status Register. Tail pointer Register updates
 *         by software while the IQE field is Set does not cause descriptor
 *         fetches by hardware." An IQE nobody clears is a queue that is dead
 *         for the rest of the machine's life, including for whoever enables
 *         it next.
 *   ICE — a device answered a Device-TLB invalidation with an invalid
 *         completion.
 *   ITE — a Device-TLB invalidation timed out, and every invalidation wait
 *         still outstanding is abandoned with it.
 *
 * Which descriptor, and why, is in IQERCD (§11.4.9.9): IQEI in bits 3:0, the
 * timed-out source id in 47:32, the offending completion's source id in
 * 63:48. EDK2 names the same three bits B_FSTS_REG_IQE/ICE/ITE at BIT4/5/6
 * and the record register R_IQERCD_REG at 0xB0. */
#define FSTS_IQE          (1u << 4)
#define FSTS_ICE          (1u << 5)
#define FSTS_ITE          (1u << 6)
#define FSTS_QI_ERRORS    (FSTS_IQE | FSTS_ICE | FSTS_ITE)
#define IQERCD_IQEI(r)    ((unsigned)((r) & 0xFULL))
#define IQERCD_ITESID(r)  ((unsigned)(((r) >> 32) & 0xFFFFULL))
#define IQERCD_ICESID(r)  ((unsigned)(((r) >> 48) & 0xFFFFULL))

/* The OTHER half of the Fault Status Register: primary fault logging
 * (§7.2.1), which is the channel every non-recoverable translation fault and
 * every interrupt-remapping fault arrives on. Nothing in this driver read it
 * until now, and two of its properties make that a silence that lasts:
 *
 *   PFO (bit 0, RW1CS) — §11.4.7.1: "Hardware sets this field to indicate
 *   overflow of the fault recording registers. When this field is Set,
 *   hardware does not record any new faults until software clears this
 *   field." A PFO this kernel inherits from firmware and never clears is a
 *   unit that will record nothing for the rest of the machine's life.
 *
 *   PPF (bit 1, ROS) — the logical OR of the F fields across every fault
 *   recording register. It is READ-ONLY: it goes away when the F bits do and
 *   not before, so it is never written back. (Linux writes PPF and the
 *   deprecated PRO alongside PFO; rev 5.0 has retired PRO from bit 7 and
 *   calls that bit RsvdZ, which software writes as zero, so neither is
 *   written here.)
 *
 * And §7.3 makes the pair load-bearing for the interrupt that does not exist
 * yet: "Hardware checks if there are any previously reported interrupt
 * conditions that are yet to be serviced by software ... If hardware detects
 * any interrupt condition yet to be serviced by software, the Fault Event
 * interrupt is not generated." A stale PFO or F bit therefore also gags the
 * Fault Event the moment anybody arms it. */
#define FSTS_PFO          (1u << 0)
#define FSTS_PPF          (1u << 1)
#define FSTS_FRI(s)       ((uint32_t)(((s) >> 8) & 0xFFu))

/* A Fault Recording Register (§11.4.7.6) is 128 bits, and its fields do not
 * line up with the two qwords. Read as three pieces, which is also the order
 * the register's own footnote 1 demands: "software may read the highest
 * doubleword in a fault recording register and check if the Fault (F) field
 * is Set before reading the rest of the data fields in that register",
 * because "hardware updates to this register may be disassembled as multiple
 * doubleword writes".
 *
 *   +0Ch, bits 127:96 — F (127), T1 (126), AT (125:124), PV (123:104),
 *                       FR (103:96)
 *   +08h, bits  95:64 — PP (95), EXE (94), PRIV (93), T2 (92), SID (79:64)
 *   +00h, bits  63:0  — FI (63:12), the faulting page address
 *
 * T1 and T2 together name the request (§11.4.7.6, T2's table): 00 write,
 * 01 page request, 10 read, 11 AtomicOp. */
#define FRCD_HI_F         (1u << 31)                        /* bit 127      */
#define FRCD_HI_T1(d)     (((d) >> 30) & 1u)                /* bit 126      */
#define FRCD_HI_FR(d)     ((uint8_t)((d) & 0xFFu))          /* bits 103:96  */
#define FRCD_HI_PV(d)     (((d) >> 8) & 0xFFFFFu)           /* bits 123:104 */
#define FRCD_MID_SID(d)   ((uint16_t)((d) & 0xFFFFu))       /* bits  79:64  */
#define FRCD_MID_PP(d)    (((d) >> 31) & 1u)                /* bit 95       */
#define FRCD_MID_T2(d)    (((d) >> 28) & 1u)                /* bit 92       */
#define FRCD_FI_MASK      0xFFFFFFFFFFFFF000ULL             /* bits  63:12  */

/* The IOTLB registers have no fixed offset: ECAP.IRO (bits 17:8, §11.4.3)
 * reports it — "If the register base address is X, and the value reported in
 * this field is Y, the address for the IOTLB registers is calculated as
 * X+(16*Y)". IVA_REG sits at that offset and IOTLB_REG at +8.
 *
 * The field lives in ECAP and nowhere else. CAP bits 17:8 are SAGAW[12:8],
 * three reserved bits and the bottom two bits of MGAW, so reading the offset
 * out of CAP yields a number that grows with the unit's address width — on a
 * part reporting MGAW=39 and 48-bit AGAW it comes to 0x2040, past the end of
 * any grant this driver ever asked for. */
#define ECAP_IRO_OFF(e)        ((uint32_t)((((e) >> 8) & 0x3FFu) * 16u))
#define ECAP_IOTLB_REG_END(e)  (ECAP_IRO_OFF(e) + 16u)

/* CAP.FRO (bits 33:24) and CAP.NFR (bits 47:40, the count minus one), §11.4.2:
 * where the fault-recording registers start and how many there are. Together
 * with the IOTLB block they say how long this unit's register block is, which
 * is legitimately more than one page. */
#define CAP_FRO_OFF(c)         ((uint32_t)((((c) >> 24) & 0x3FFu) * 16u))
#define CAP_NUM_FAULT_REGS(c)  ((uint32_t)((((c) >> 40) & 0xFFu) + 1u))
#define CAP_FAULT_REG_END(c)   (CAP_FRO_OFF(c) + CAP_NUM_FAULT_REGS(c) * 16u)

/* CAP bits we read. */
#define CAP_SAGAW_SHIFT   8        /* bits 12:8 */
#define CAP_SAGAW_MASK    0x1Fu
/* AGAW index 2 — 48-bit adjusted guest address width, a 4-level second-level
 * table, and AW = 010b in the context entry (§11.4.2 SAGAW, §9.3 AW). */
#define VTD_AGAW_48BIT    2u
/* AGAW index 1 — 39-bit, a 3-level table, AW = 001b. The levels this driver
 * builds are always four; a unit that can only walk three is given the
 * SECOND level of that same table as its root (vtd_slpt_root_for). */
#define VTD_AGAW_39BIT    1u

/* ECAP.C, bit 0 — "Page-walk Coherency" (§11.4.3): whether this unit's walks
 * of the root, context, second-level paging and interrupt-remap structures
 * are snooped. Linux reads the same bit as ecap_coherent(e) = (e) & 1. */
#define ECAP_C            (1ULL << 0)

/* CAP.DWD (bit 54) and CAP.DRD (bit 55), §11.4.2: this unit can drain
 * in-flight writes / reads as part of an IOTLB invalidation. Asking a unit
 * that does not report them for a drain sets a field it treats as reserved, so
 * both ride on the capability. */
#define CAP_DWD           (1ULL << 54)
#define CAP_DRD           (1ULL << 55)

/* CAP.SSLPS, bits 37:34 — "Second Stage Large Page Support" (§11.4.2): "A
 * value of 1 in any of these bits indicates the corresponding large-page size
 * is supported. ... 0: 21-bit offset to page frame (2MB) ... 1: 30-bit offset
 * to page frame (1GB)".
 *
 * Only the 2 MiB bit is read. A 2 MiB leaf already takes the identity map of
 * a 16 GiB machine from 4194304 entries in 8192 leaf tables down to 8192
 * entries in 16 page directories; 1 GiB would take 8192 down to 16, which is
 * no longer a cost anyone can measure, and every added granularity is another
 * set of reserved-address bits to get right in an entry the hardware faults
 * on rather than complains about. */
#define CAP_SSLPS_2M      (1ULL << 34)

/* CAP.CM, bit 7 — "Caching Mode" (§11.4.2), and CAP.ND, bits 2:0 — "Number of
 * domains supported".
 *
 * These two decide which domain-id a context entry may legally carry, and
 * getting it wrong is not a diagnostic problem — it is a device whose every
 * transaction is tagged with the number the hardware reserves for the entries
 * it intends to FAULT.
 *
 * §6.2.2: "For implementations reporting Caching Mode (CM) as 1 in the
 * Capability Register, the implementation may cache entries that would result
 * in a fault, such as entries with P=0. ... In legacy mode, the reserved
 * domain-id value of 0 is used to tag the cached context entries that would
 * result in a fault."
 *
 * §6.2.2.1, in the list of what software must do in Legacy Mode: "Software
 * must not use domain-id value of 0 on when programming context-entries on
 * implementations reporting CM=1 in the Capability register."
 *
 * So on a CM=1 part every domain-id this driver writes is shifted up by one
 * (g_did_bias), and the shift is global rather than per unit because a
 * domain-id names the same domain wherever it appears. QEMU's intel-iommu
 * reports CM=1 by default, which is precisely the part this path would first
 * be exercised on.
 *
 * ND is a code, not a count: 000b is 4-bit domain-ids (16 domains) and each
 * step adds two bits, up to 110b = 16-bit (64K); 111b is Reserved and this
 * driver treats it as a unit it cannot size. A domain-id at or above what a
 * unit supports is a reserved-field violation in the context entry (LCT.3),
 * so it is refused rather than written. */
#define CAP_CM            (1ULL << 7)
#define CAP_ND_CODE(c)    ((unsigned)((c) & 0x7ULL))

/* The interrupt address range, §3.15: "On Intel architecture platforms,
 * physical address range FEEx_xxxxh is designated as the interrupt address
 * range. Single-DWORD length write requests without PASID to this range are
 * treated as interrupt requests and are not subjected to DMA remapping (even
 * if translation structures specify a mapping for this range)."
 *
 * That sentence is why MSI still reaches a LAPIC with GCMD.TE set and
 * interrupt remapping off — the writes never enter the second-level walk. The
 * NEXT sentence is why this range is cut out of the identity map anyway:
 * "Software must not program paging-structure entries to remap any address to
 * the interrupt address range." An identity entry for FEE00000h remaps an
 * address TO that range, which is the thing the specification forbids, and
 * nothing legitimate ever walks it. */
#define VTD_INTR_RANGE_BASE  0xFEE00000ULL
#define VTD_INTR_RANGE_END   0xFEF00000ULL

/* The whole of what a PCI master can address in one address phase.
 *
 * This is the identity map's FLOOR, and it is a property of the bus rather
 * than a choice. PCI Local Bus Specification Rev. 3.0 §3.9 "64-Bit Bus
 * Extension": "The standard PCI bus transaction supports a 32-bit address,
 * Single Address Cycle (SAC), where the address is valid for a single clock
 * when FRAME# is first sampled asserted", and "Devices that support only
 * 32-bit addresses are mapped into the low 4 GB of the address space". Thirty-
 * two address bits is 2^32 bytes: every address a device without Dual Address
 * Cycle can ever put on the bus lies below this line, and none above it.
 *
 * So below this line the map must be complete whatever the memory map says,
 * because a 32-bit master can name any of it and this kernel does not know
 * which of them will. Above it, what a device may be pointed at is what the
 * memory map and the RMRR entries say, and both are read rather than assumed. */
#define VTD_SAC_SPAN         (1ULL << 32)

/* GCMD bits (VT-d Rev 5.x §11.4.4.1 "Global Command Register";
 * §10.4.4 in the Rev 3.x numbering). GSTS reports the persistent ones at
 * the same bit positions, which is what makes the shadow below possible. */
#define GCMD_TE     (1u << 31)     /* Translation Enable */
#define GCMD_SRTP   (1u << 30)     /* Set Root Table Pointer */
#define GCMD_SFL    (1u << 29)     /* Set Fault Log */
#define GCMD_EAFL   (1u << 28)     /* Enable Adv Fault Log */
#define GCMD_WBF    (1u << 27)     /* Write Buffer Flush */
#define GCMD_QIE    (1u << 26)     /* Queued Invalidation Enable */
#define GCMD_IRE    (1u << 25)     /* Interrupt Remap Enable */
#define GCMD_SIRTP  (1u << 24)     /* Set IR Table Pointer */
#define GCMD_CFI    (1u << 23)     /* Compatibility Format Interrupt */

/* GSTS bits. */
#define GSTS_TES    (1u << 31)
#define GSTS_RTPS   (1u << 30)
#define GSTS_CFIS   (1u << 23)
#define GSTS_IRES   (1u << 25)     /* Interrupt Remapping Enable Status */
#define GSTS_IRTPS  (1u << 24)     /* Interrupt Remap Table Pointer Status */
#define GSTS_QIES   (1u << 26)     /* Queued Invalidation Enable Status */

/* CCMD bits (§10.4.7). */
#define CCMD_ICC          (1ULL << 63)
#define CCMD_CIRG_GLOBAL  (1ULL << 61)

/* Second-level paging entry bits (§9.1). */
#define SLPTE_R     (1ULL << 0)    /* Read */
#define SLPTE_W     (1ULL << 1)    /* Write */
#define SLPTE_PS    (1ULL << 7)    /* Page Size — only valid at PML4/PDPT/PD */

/* ----- Per-DRHD state ----- */
typedef struct {
    uint64_t  reg_phys;
    /* The PCI segment this DRHD belongs to (DMAR §8.3, the DRHD's own Segment
     * Number field). A context entry is reachable only through the root table
     * of a unit in the same segment, so seating a device means finding the
     * units whose segment matches it — and on a single-segment machine, which
     * is nearly all of them, that is every unit. */
    uint16_t  segment;
    volatile uint8_t* reg;
    /* How much of the register block is mapped at u->reg. It starts at one
     * page — the only length that can be assumed before CAP and ECAP are
     * read — and becomes whatever those two say the block is. Whoever gives
     * the window back has to give back the length that was taken, and after
     * the remap below that is no longer 4096. */
    size_t    reg_size;
    uint64_t  cap;
    uint64_t  ecap;
    uint32_t  gcmd;                /* shadow of GCMD's persistent fields */
    uint8_t   sagaw_levels;        /* second-level table levels; AW = this - 2 */
    void*     root_table;          /* virt; 4 KB phys */
    uint64_t  root_phys;
    bool      online;
    bool      qi_active;
    bool      ir_active;
    void*     qi_ring;             /* 4 KB QI descriptor ring */
    uint64_t  qi_ring_phys;
    uint32_t  qi_tail;             /* in bytes; ring is 256 × 16 */
    /* The Invalidation Wait Descriptor's landing place: the unit writes the
     * descriptor's Status Data DWORD here, by physical address, once every
     * invalidation queued ahead of it has COMPLETED. It gets a page of its
     * own because the ring is exactly the 4 KiB the hardware was given and
     * has no spare qword in it. */
    volatile uint32_t* qi_status;
    uint64_t  qi_status_phys;
    void*     ir_table;            /* 4 KB Interrupt Remap Table */
    uint64_t  ir_table_phys;
    uint16_t  ir_entries;          /* number of remap entries */
} vtd_unit_t;

#define VTD_QI_ENTRIES   256
#define VTD_QI_BYTES     (VTD_QI_ENTRIES * 16)
#define VTD_IR_ENTRIES   256
#define VTD_IR_BYTES     (VTD_IR_ENTRIES * 16)

#define VTD_REG_IQH      0x080
#define VTD_REG_IQT      0x088
#define VTD_REG_IQA      0x090
#define VTD_REG_IQERCD   0x0B0
#define VTD_REG_IRTA     0x0B8

#define VTD_MAX_UNITS  ACPI_DMAR_MAX_DRHD
static vtd_unit_t g_units[VTD_MAX_UNITS];
static uint8_t    g_unit_count = 0;

/* ----- Shared identity-map second-level page tables -----
 *
 * One PML4 (the "top level") covers the entire DMA address space. Lower
 * levels are allocated as the walk reaches them.
 *
 * The page tables themselves come from PMM physical pages mapped into
 * the kernel via vmm_phys_to_virt (provided by the VMM). */
static uint64_t* g_id_pml4    = NULL;
static uint64_t  g_id_pml4_phys = 0;
/* The leaf this machine's units can all walk: 21 when every one of them
 * reports CAP.SSLPS 2 MiB, 12 otherwise. Not a tuning knob — a unit told to
 * walk a size it does not report faults the transaction (LSS.2, a reserved
 * field set in a second-stage paging entry), so the answer is the AND over
 * the units and nothing else. */
static unsigned  g_id_leaf_shift = 12;

/* What every domain-id this driver writes is shifted by: 1 where any online
 * unit reports CAP.CM, 0 otherwise. See CAP_CM above for the two sentences
 * that require it. Settled once, in vtd_build_identity, after every unit has
 * spoken and before a single context entry is written. */
static unsigned  g_did_bias = 0;

/* Pages the PMM has handed this driver for translation structures. Not a
 * limit and not a budget — the identity build reports what it cost, because a
 * machine whose units cannot walk a 2 MiB leaf pays for the same coverage in
 * 4 KiB leaves and the difference is four orders of magnitude. */
static uint64_t  g_tables_bought = 0;

/* ----- Putting a structure where a unit that does not snoop can see it -----
 *
 * ECAP.C (bit 0, "Page-walk Coherency") says whether a unit's walks of the
 * root, context, second-level paging and interrupt-remap structures are
 * coherent with the processor caches. Where it reads 0 they are not: a store
 * this kernel makes can sit in a cache line the remap unit never looks in
 * while the unit walks whatever DRAM holds — a root entry that reads
 * not-present, a leaf that reads as the zero it was allocated with. Software
 * has to put the line back itself, which is exactly what Linux does
 * (__iommu_flush_cache: `if (!ecap_coherent(iommu->ecap)) clflush_cache_range`).
 *
 * The flag is ONE for the whole driver, not one per unit, because the
 * second-level tables are SHARED by every unit on the machine: a structure
 * must be flushed if ANY unit that walks it fails to snoop. Flushing for a
 * unit that does snoop costs a few cache lines and changes nothing else.
 *
 * The invalidation queue is deliberately not flushed. This field is about the
 * structures hardware WALKS, the descriptor ring is not one of them, and
 * Linux's qi_submit_sync does not flush its descriptors either. */
static bool g_walk_snooped = true;

/* The CLFLUSH line size, CPUID.01H:EBX bits 15:8 counted in eight-byte
 * quantums. Read once. A part that answers 0 — which no x86_64 part does —
 * gets this kernel's declared line size rather than a number invented here. */
static size_t vtd_clflush_line(void)
{
    static size_t line = 0;
    if (!line) {
        uint32_t a = 0, b = 0, c = 0, d = 0;
        cpu_cpuid(1, 0, &a, &b, &c, &d);
        size_t said = (size_t)((b >> 8) & 0xFFu) * 8u;
        line = said ? said : (size_t)CONFIG_CACHE_LINE_SIZE;
    }
    return line;
}

/* Hand `bytes` at `addr` back to memory for a unit that will not snoop them.
 * A no-op on a machine whose every unit reports ECAP.C. The MFENCE either
 * side is the ordering CLFLUSH has (Intel SDM: CLFLUSH is ordered by MFENCE),
 * and the shape of Linux's clflush_cache_range. */
static void vtd_structure_flush(const void* addr, size_t bytes)
{
    if (g_walk_snooped || !addr || !bytes) return;
    size_t    line = vtd_clflush_line();
    uintptr_t p    = (uintptr_t)addr & ~(uintptr_t)(line - 1);
    uintptr_t end  = (uintptr_t)addr + bytes;
    __asm__ volatile ("mfence" ::: "memory");
    for (; p < end; p += line)
        __asm__ volatile ("clflush (%0)" :: "r"((const void*)p) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

/* One run of entries just written into the same leaf, [from, to). */
static void vtd_structure_flush_run(const uint64_t* from, const uint64_t* to)
{
    if (from && to > from)
        vtd_structure_flush(from, (size_t)((const uint8_t*)to -
                                           (const uint8_t*)from));
}

/*
 * A VT-d table is two addresses, and they are not the same number.
 *
 * The hardware is given the PHYSICAL one — it walks these tables with its own
 * DMA remapping engine, which knows nothing of the kernel's page tables. The
 * CPU must use the VIRTUAL one, and the kernel's alias for physical memory is
 * the pull map at PULL_MAP_BASE, not the identity of the address with itself.
 *
 * This function used to return the physical address as a pointer, with a
 * comment asserting that "PMM returns physical address aliased as kernel
 * virtual via the pull map". It does not: pmm_alloc returns physical, and the
 * pull map aliases it at PULL_MAP_BASE + phys. The code worked in QEMU for one
 * reason only — QEMU exposes no DMAR unless asked for one, so vtd_init()
 * returned on its first line and none of this ever ran. The first machine with
 * VT-d in its firmware took a kernel #PF writing to 0x3fc05000 inside memset,
 * four instructions in, and that machine was a Gigabyte B365 on 2026-08-24.
 */
static void* alloc_table_4k(uint64_t* phys_out) {
    void* p = pmm_alloc(1);
    if (!p) { *phys_out = 0; return NULL; }

    uintptr_t phys = (uintptr_t)p;
    void*     virt = vmm_phys_to_virt(phys);
    if (!virt) { pmm_free(p, 1); *phys_out = 0; return NULL; }

    *phys_out = (uint64_t)phys;
    g_tables_bought++;
    memset(virt, 0, 4096);
    /* The zeroes are as much a structure as the entries written into them
     * later: a unit that does not snoop would otherwise walk whatever the
     * PMM's previous owner left in DRAM. */
    vtd_structure_flush(virt, 4096);
    return virt;
}

/* The CPU-side pointer for a table the hardware knows by physical address.
 * Every descent through these structures goes through here, because every one
 * of them stores what the IOMMU needs and none of them store what we do. */
static inline void* vtd_table_virt(uint64_t entry) {
    return vmm_phys_to_virt((uintptr_t)(entry & ~0xFFFULL));
}

static uint64_t* slpte_walk(uint64_t* parent, unsigned idx, bool create) {
    uint64_t e = parent[idx];
    if (!(e & SLPTE_R)) {
        if (!create) return NULL;
        uint64_t child_phys;
        void* child = alloc_table_4k(&child_phys);
        if (!child) return NULL;
        parent[idx] = child_phys | SLPTE_R | SLPTE_W;
        vtd_structure_flush(&parent[idx], sizeof(parent[idx]));
        return (uint64_t*)child;
    }
    if (e & SLPTE_PS) return NULL;       /* super-page; cannot descend */
    return (uint64_t*)vtd_table_virt(e);
}

#define VTD_2M   0x200000ULL

/* Identity-map [phys, phys+size) at IOVA == phys with R+W, at whatever leaf
 * every unit on this machine can walk.
 *
 * Two things are skipped rather than mapped, and both are skips the hardware
 * asks for:
 *   - the interrupt address range (§3.15, see VTD_INTR_RANGE_BASE);
 *   - an address a 2 MiB leaf already covers identically, which is how the
 *     incremental path (vtd_map on the identity domain) stays correct after
 *     the bulk build has laid super-pages down. A super-page at A maps
 *     A..A+2MiB to itself; a later request to identity-map any part of that
 *     range is already satisfied, and descending into it is impossible
 *     (§9.1, PS at the page-directory level ends the walk).
 *
 * A request whose WHOLE range falls in the interrupt hole is refused, not
 * reported as done. The skip is the specification's; telling the caller its
 * mapping exists when this function deliberately declined to write one is
 * this file's, and it would be a lie the caller cannot detect. */
static int vtd_identity_map(uint64_t phys, uint64_t size) {
    if (!g_id_pml4 || !size) return -1;
    const uint64_t end = phys + size;
    bool anything_covered = false;
    /* Leaf writes are handed back to memory a RUN at a time rather than one
     * entry at a time: the walk is linear, so consecutive entries share a
     * cache line and a whole leaf table is one run. Linux batches the same
     * way (domain_flush_cache over first_pte..pte). */
    const uint64_t* run_from = NULL;
    const uint64_t* run_to   = NULL;
    int rc = 0;

    uint64_t a = phys & ~0xFFFULL;
    while (a < end) {
        if (a >= VTD_INTR_RANGE_BASE && a < VTD_INTR_RANGE_END) {
            vtd_structure_flush_run(run_from, run_to);
            run_from = run_to = NULL;
            a = VTD_INTR_RANGE_END;
            continue;
        }

        unsigned pml4_i = (unsigned)((a >> 39) & 0x1FF);
        unsigned pdpt_i = (unsigned)((a >> 30) & 0x1FF);
        unsigned pd_i   = (unsigned)((a >> 21) & 0x1FF);
        unsigned pt_i   = (unsigned)((a >> 12) & 0x1FF);

        uint64_t* pdpt = slpte_walk(g_id_pml4, pml4_i, true);
        if (!pdpt) { rc = -2; break; }
        uint64_t* pd   = slpte_walk(pdpt, pdpt_i, true);
        if (!pd)   { rc = -2; break; }

        uint64_t  pde  = pd[pd_i];

        if ((pde & SLPTE_R) && (pde & SLPTE_PS)) {
            /* Already a 2 MiB leaf. It is this map's own, so it identity-maps
             * its own base; anything else came from somewhere this function
             * cannot speak for and is refused rather than written over. */
            if ((pde & ~0xFFFULL) != (a & ~(VTD_2M - 1))) { rc = -2; break; }
            vtd_structure_flush_run(run_from, run_to);
            run_from = run_to = NULL;
            a = (a & ~(VTD_2M - 1)) + VTD_2M;
            anything_covered = true;
            continue;
        }

        uint64_t* slot;
        /* A 2 MiB leaf cannot straddle the interrupt range: that range begins
         * at FEE00000h, which is itself 2 MiB aligned, so the walk arrives at
         * its first byte and the skip above takes it. */
        if (g_id_leaf_shift == 21 && !(pde & SLPTE_R) &&
            (a & (VTD_2M - 1)) == 0 && (end - a) >= VTD_2M) {
            pd[pd_i] = a | SLPTE_R | SLPTE_W | SLPTE_PS;
            slot = &pd[pd_i];
            a += VTD_2M;
        } else {
            uint64_t* pt = slpte_walk(pd, pd_i, true);
            if (!pt) { rc = -2; break; }
            pt[pt_i] = a | SLPTE_R | SLPTE_W;
            slot = &pt[pt_i];
            a += 4096;
        }
        anything_covered = true;

        if (run_to != slot) {
            vtd_structure_flush_run(run_from, run_to);
            run_from = slot;
        }
        run_to = slot + 1;
    }
    vtd_structure_flush_run(run_from, run_to);
    if (rc == 0 && !anything_covered) {
        kprintf("[VT-d] a mapping of 0x%lx..0x%lx was asked for and every byte "
                "of it lies in the interrupt address range FEEx_xxxxh, which "
                "§3.15 forbids a paging-structure entry from naming — nothing "
                "was written and the request is refused rather than reported "
                "as done\n", (unsigned long)phys, (unsigned long)end);
        rc = -3;
    }
    return rc;
}

/* Does the table the hardware will walk actually translate this address?
 *
 * This is a READ of what was written, through the same levels and the same
 * present bits the remap unit uses, and it is the only coverage claim this
 * driver makes that can come back false. It replaces a comparison of the
 * build's own span against the number the build was given, which was the same
 * number rounded up and could never disagree with itself.
 *
 * The interrupt address range answers false, and truthfully: §3.15 has this
 * driver leave a hole there on purpose. The caller decides whether a hole at
 * that address is expected (the identity build) or a contradiction (an RMRR
 * that names it). */
static bool vtd_identity_covers(uint64_t addr) {
    if (!g_id_pml4) return false;
    uint64_t* pdpt = slpte_walk(g_id_pml4, (unsigned)((addr >> 39) & 0x1FF),
                                false);
    if (!pdpt) return false;
    uint64_t* pd = slpte_walk(pdpt, (unsigned)((addr >> 30) & 0x1FF), false);
    if (!pd) return false;
    uint64_t pde = pd[(addr >> 21) & 0x1FF];
    if (!(pde & SLPTE_R)) return false;
    if (pde & SLPTE_PS) return true;          /* 2 MiB leaf covers it */
    uint64_t* pt = (uint64_t*)vtd_table_virt(pde);
    if (!pt) return false;
    return (pt[(addr >> 12) & 0x1FF] & SLPTE_R) != 0;
}

/* ----- What firmware says a device still needs to reach -----
 *
 * §3.16 Handling Requests to Reserved System Memory, and §8.4: "BIOS may
 * report each such reserved memory region through the RMRR structures, along
 * with the devices that requires access to the specified reserved memory
 * region", and those regions are "expected to be used for legacy usages (such
 * as USB, UMA Graphics, etc.) requiring reserved memory".
 *
 * A region named there and not present in the second-level tables is a device
 * that works until GCMD.TE goes on and stops the instant it does — the exact
 * failure this whole ordering exists to avoid, and the one place where the
 * machine has already written down the answer.
 *
 * The walk is a callback rather than an array because the number of RMRRs is
 * the firmware's to decide and no ceiling here could be sourced. It is run
 * twice: once to map, once to verify what was mapped.
 *
 * Structure layout, §8.4 Table: Type at +0 (1 = RMRR), Length at +2, Segment
 * Number at +6, Reserved Memory Region Base Address at +8 and Limit Address
 * at +16 — the LAST address of the region, not one past it. */
typedef void (*VtdRmrrFn)(uint64_t base, uint64_t limit, uint16_t segment,
                          void* ctx);

static void vtd_walk_rmrr(VtdRmrrFn fn, void* ctx) {
    acpi_sdt_header_t* hdr = acpi_find_table("DMAR");
    if (!hdr) return;
    acpi_dmar_t* dmar = (acpi_dmar_t*)hdr;
    uint8_t* ptr = (uint8_t*)dmar + sizeof(acpi_dmar_t);
    uint8_t* end = (uint8_t*)dmar + dmar->header.length;

    while (ptr + sizeof(dmar_entry_header_t) <= end) {
        dmar_entry_header_t* eh = (dmar_entry_header_t*)ptr;
        if (eh->length < sizeof(dmar_entry_header_t)) break;
        if (ptr + eh->length > end) break;
        /* 24 bytes is the fixed part: the Device Scope entries that follow it
         * say WHICH devices need the region, and this driver seats every
         * device in the segment into one domain, so the region is needed by
         * whatever is in that domain and the scope adds nothing here. */
        if (eh->type == DMAR_TYPE_RMRR && eh->length >= 24) {
            uint16_t seg   = *(uint16_t*)(ptr + 6);
            uint64_t base  = *(uint64_t*)(ptr + 8);
            uint64_t limit = *(uint64_t*)(ptr + 16);
            if (limit >= base) fn(base, limit, seg, ctx);
        }
        ptr += eh->length;
    }
}

/* The coverage, whole — and every dimension of it is a fact from somewhere
 * outside this file.
 *
 * EXTENT, two facts and no constant of ours. pmm_get_mem_end() is one past
 * the last byte of the highest usable E820 region, clamped to this part's
 * MAXPHYADDR: the top of the RAM a device can be handed a pointer into.
 * VTD_SAC_SPAN is the whole of what a device without Dual Address Cycle can
 * put on the bus at all (PCI 3.0 §3.9, quoted at the define), so the map is
 * complete below that line whatever the memory map happens to say.
 *
 * RESERVED REGIONS, read rather than assumed. Everything a device still needs
 * outside RAM is in the DMAR's RMRR entries (§3.16, §8.4) — legacy USB
 * emulation buffers, graphics stolen memory — and those fields are 64-bit, so
 * one of them can sit above both lines above. They are mapped by name here,
 * not covered by hoping a floor reaches them.
 *
 * GRANULARITY, the units' own answer. 2 MiB where every unit reports
 * CAP.SSLPS bit 34, 4 KiB otherwise, and the AND is not a preference: a unit
 * told to walk a size it does not report faults the transaction (LSS.2).
 *
 * Called AFTER the units have been brought up, which is the only order in
 * which it can know the three things it needs: whether any unit fails to
 * snoop what it walks (ECAP.C), whether every unit can walk a 2 MiB leaf
 * (CAP.SSLPS), and whether any unit reserves domain-id 0 (CAP.CM). Nothing in
 * vtd_unit_init reads these tables, so nothing is lost by building them
 * second. */
static void vtd_map_rmrr(uint64_t base, uint64_t limit, uint16_t segment,
                         void* ctx) {
    int* rc = (int*)ctx;
    uint64_t first = base & ~(uint64_t)0xFFF;
    uint64_t last  = limit | (uint64_t)0xFFF;
    kprintf("[VT-d] DMAR names reserved region 0x%lx-0x%lx in segment %u as a "
            "DMA target — mapping it identically, because a device that "
            "reaches it after GCMD.TE with no entry simply stops\n",
            (unsigned long)base, (unsigned long)limit, (unsigned)segment);
    int r = vtd_identity_map(first, last - first + 1);
    /* -3 is "every byte of it is in the interrupt address range", which is a
     * contradiction between §3.15 and §3.16 in the firmware's own table, not
     * a failure of this build. It is left to vtd_check_rmrr, which refuses
     * the wheel and names both sections — tearing the whole backend down
     * here would leave the machine with no protection at all instead. */
    if (r < 0 && r != -3) *rc = -1;
}

static void vtd_check_rmrr(uint64_t base, uint64_t limit, uint16_t segment,
                           void* ctx) {
    bool* ok = (bool*)ctx;
    if ((base < VTD_INTR_RANGE_END && limit >= VTD_INTR_RANGE_BASE)) {
        kprintf("[VT-d] the DMAR names reserved region 0x%lx-0x%lx (segment "
                "%u), which overlaps the interrupt address range FEEx_xxxxh. "
                "§3.15 forbids a paging-structure entry naming that range and "
                "§3.16 requires this region to be reachable; the two cannot "
                "both be honoured, so translation stays OFF\n",
                (unsigned long)base, (unsigned long)limit, (unsigned)segment);
        *ok = false;
        return;
    }
    if (!vtd_identity_covers(base & ~(uint64_t)0xFFF) ||
        !vtd_identity_covers(limit | (uint64_t)0xFFF)) {
        kprintf("[VT-d] reserved region 0x%lx-0x%lx (segment %u) is NOT in the "
                "second-level tables the units are about to walk — the device "
                "that needs it would stop the instant GCMD.TE goes on, so "
                "translation stays OFF everywhere\n",
                (unsigned long)base, (unsigned long)limit, (unsigned)segment);
        *ok = false;
    }
}

static int vtd_build_identity(void) {
    if (g_id_pml4) return 0;

    bool every_unit_walks_2m = (g_unit_count > 0);
    bool any_unit_reserves_did0 = false;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!(g_units[i].cap & CAP_SSLPS_2M)) every_unit_walks_2m = false;
        if (g_units[i].cap & CAP_CM)          any_unit_reserves_did0 = true;
    }
    g_id_leaf_shift = every_unit_walks_2m ? 21u : 12u;
    g_did_bias      = any_unit_reserves_did0 ? 1u : 0u;
    if (g_did_bias)
        kprintf("[VT-d] a unit on this machine reports CAP.CM — §6.2.2.1: "
                "\"Software must not use domain-id value of 0 on when "
                "programming context-entries on implementations reporting "
                "CM=1\", because §6.2.2 has the hardware use that value to tag "
                "the context entries it means to FAULT. Every domain-id this "
                "driver writes is shifted up by one; the identity domain is "
                "domain-id 1\n");

    uint64_t span = pmm_get_mem_end();
    if (span < VTD_SAC_SPAN) span = VTD_SAC_SPAN;
    span = (span + (VTD_2M - 1)) & ~(VTD_2M - 1);

    uint64_t before = g_tables_bought;
    void* p = alloc_table_4k(&g_id_pml4_phys);
    if (!p) return -1;
    g_id_pml4 = (uint64_t*)p;
    if (vtd_identity_map(0, span) < 0) return -1;

    int rmrr_rc = 0;
    vtd_walk_rmrr(vtd_map_rmrr, &rmrr_rc);
    if (rmrr_rc < 0) {
        kprintf("[VT-d] a reserved region the DMAR names could not be mapped — "
                "the identity tables are incomplete and no unit will be "
                "switched on\n");
        return -1;
    }

    kprintf("[VT-d] identity map covers 0x0-0x%lx plus every RMRR region, at "
            "%s leaves, for %lu page(s) of tables — the top of the memory map "
            "or the reach of a Single Address Cycle, whichever is higher\n",
            (unsigned long)span, g_id_leaf_shift == 21 ? "2 MiB" : "4 KiB",
            (unsigned long)(g_tables_bought - before));
    return 0;
}

/* ----- Root/context table helpers ----- */

/* Root entry (§9.1): 16 bytes — low qword has the context table pointer
 * and the Present bit in [0]. */
typedef struct { uint64_t lo; uint64_t hi; } __attribute__((packed))
vtd_root_entry_t;

/* Context entry: 16 bytes — low qword: Present[0], FPD[1], Translation
 * Type[3:2], AW[2:0 at upper 32]... actually layout:
 *   lo: ASR(slpt_phys)[51:12] | AW[63:..]
 *       AW shift is bit 6:7, T (translation type) bits 2:3, Present 0
 * (Use the canonical mask helpers below.) */
typedef struct { uint64_t lo; uint64_t hi; } __attribute__((packed))
vtd_context_entry_t;

/* The table this unit must actually be given as its second-level root.
 *
 * Every table this driver builds has four levels, AGAW index 2. A unit whose
 * SAGAW offers only the 39-bit width walks three, and §9.3 requires the
 * context entry's AW to name a width the unit's own SAGAW reports — so such a
 * unit cannot be given the top of the table. It is given the table's SECOND
 * level instead: entry 0 of a 4-level top covers IOVA 0 through 512 GiB, and
 * a 39-bit unit cannot produce an address above 512 GiB, so the two describe
 * exactly the same translations for every address that unit can ask about.
 * Linux performs the identical descent in domain_context_mapping_one —
 * `for (agaw = domain->agaw; agaw > iommu->agaw; agaw--)
 *      pgd = phys_to_virt(dma_pte_addr(pgd));`
 *
 * Returns 0 when the level cannot be reached: the descent has to create the
 * level if the table does not have it yet, and the PMM can refuse. */
static uint64_t vtd_slpt_root_for(const vtd_unit_t* u, uint64_t top_phys)
{
    if (!top_phys) return 0;
    uint64_t phys = top_phys;
    for (unsigned levels = VTD_AGAW_48BIT + 2;
         levels > u->sagaw_levels; levels--) {
        uint64_t* table = (uint64_t*)vtd_table_virt(phys);
        if (!table) return 0;
        if (!slpte_walk(table, 0, true)) return 0;
        phys = table[0] & ~0xFFFULL;
    }
    return phys;
}

/* How many domain-ids this unit will accept, from CAP.ND (§11.4.2): 000b is
 * 4-bit ids and every step adds two bits, so the count is 1 << (4 + 2*code).
 * 111b is Reserved — a unit whose ND this driver cannot read is a unit it
 * cannot size a domain-id for, and 0 is how that is said. */
/* The domain-id the hardware sees for a domain this driver numbers `id`.
 * Anything that names a domain to hardware — a context entry, or an
 * invalidation descriptor's DID field — must go through this, or the
 * invalidation names a domain nothing is tagged with. */
static inline uint32_t vtd_hw_did(uint32_t id) { return id + g_did_bias; }

static uint32_t vtd_domains_supported(const vtd_unit_t* u) {
    unsigned code = CAP_ND_CODE(u->cap);
    if (code > 6u) return 0;
    return 1u << (4u + 2u * code);
}

static int vtd_program_context(vtd_unit_t* u, uint8_t bus, uint8_t devfn,
                                uint64_t slpt_phys, uint32_t domain_id) {
    /* The domain-id the HARDWARE is given is not the one the caller names:
     * on a CM=1 part 0 belongs to the entries the unit intends to fault
     * (§6.2.2), so every id is shifted past it. The shift is settled once, in
     * vtd_build_identity, from the CAP of every unit on the machine. */
    uint32_t did = vtd_hw_did(domain_id);
    uint32_t nd  = vtd_domains_supported(u);
    if (did >= nd) {
        kprintf("[VT-d] the unit at 0x%lx supports %u domain-id(s) (CAP.ND) "
                "and this context entry would carry %u — that is a reserved "
                "value in the entry (LCT.3), so the entry is not written\n",
                (unsigned long)u->reg_phys, (unsigned)nd, (unsigned)did);
        return -1;
    }

    /* The address this unit is given is the level it can walk, which is not
     * always the level this driver built (vtd_slpt_root_for). */
    uint64_t asr = vtd_slpt_root_for(u, slpt_phys);
    if (!asr) return -1;

    vtd_root_entry_t* root = (vtd_root_entry_t*)u->root_table;
    vtd_root_entry_t* re = &root[bus];
    if (!(re->lo & 1)) {
        uint64_t ctx_phys;
        void* ctx = alloc_table_4k(&ctx_phys);
        if (!ctx) return -1;
        re->lo = ctx_phys | 1;
        re->hi = 0;
        vtd_structure_flush(re, sizeof(*re));
    }
    vtd_context_entry_t* ctx = (vtd_context_entry_t*)vtd_table_virt(re->lo);
    if (!ctx) return -1;
    uint8_t aw = (uint8_t)(u->sagaw_levels - 2);
    ctx[devfn].lo = asr | 1;
    ctx[devfn].hi = ((uint64_t)did << 8) | ((uint64_t)aw & 0x7);
    vtd_structure_flush(&ctx[devfn], sizeof(ctx[devfn]));
    return 0;
}

/* Default identity-map: every devfn on every bus points at the shared
 * SLPT. Used when no caller has explicitly attached a private domain. */
static int vtd_program_identity_bus(vtd_unit_t* u, uint8_t bus) {
    for (uint16_t df = 0; df < 256; df++) {
        if (vtd_program_context(u, bus, (uint8_t)df,
                                  g_id_pml4_phys, 0) < 0)
            return -1;
    }
    return 0;
}

/* ----- Handshakes with the remap unit ----- */

/* How long a remap unit gets to answer one Global Command write.
 *
 * DECLARED POLICY, not a spec quantity. VT-d Rev 5.x §11.4.4.1 is explicit that
 * software changes ONE field in GCMD and then waits for the matching bit in
 * GSTS, and it names neither a count nor a maximum anywhere. One second is
 * this kernel's patience, and it is deliberately THE SAME NUMBER at every
 * handshake in this file so there is one number to argue with rather than six.
 *
 * What it replaces was `for (int i = 0; i < 1000000; i++)` around a single
 * MMIO read — a few milliseconds on one machine and most of a second on
 * another, and silence on the log either way. */
#define VTD_HANDSHAKE_PATIENCE_US   1000000u

/* Wait for one GSTS bit to reach `want`. Returns false when it never did; the
 * caller then decides what to UNDO, because "carried on as though it worked" is
 * the one answer a failed handshake must not produce.
 *
 * Both directions are needed: an enable is answered by the bit APPEARING, and a
 * disable — including the ones this driver issues to take a unit back off
 * firmware — by the same bit GOING. */
static bool vtd_await_gsts_state(vtd_unit_t* u, uint32_t gsts_bit, bool want,
                                 const char* what)
{
    volatile uint32_t* gsts = (volatile uint32_t*)(u->reg + VTD_REG_GSTS);
    HourGlass glass;
    if (!HourGlassTurn(&glass, VTD_HANDSHAKE_PATIENCE_US)) {
        kprintf("[VT-d] no time source — waiting for %s on the fact itself, "
                "with no bound\n", what);
        while (((*gsts & gsts_bit) != 0) != want) cpu_pause();
        return true;
    }
    for (;;) {
        if (((*gsts & gsts_bit) != 0) == want) return true;
        if (HourGlassRunOut(&glass)) break;
        cpu_pause();
    }
    kprintf("[VT-d] %s never %s — %lu us on the %s and the %s\n",
            what, want ? "latched" : "cleared",
            (unsigned long)HourGlassElapsedUs(&glass),
            HourGlassRulerName(&glass),
            HourGlassSourceDied(&glass) ? "ruler died mid-wait"
                                        : "span ran out");
    return false;
}

static bool vtd_await_gsts(vtd_unit_t* u, uint32_t gsts_bit, const char* what)
{
    return vtd_await_gsts_state(u, gsts_bit, true, what);
}

static bool vtd_await_gsts_gone(vtd_unit_t* u, uint32_t gsts_bit,
                                const char* what)
{
    return vtd_await_gsts_state(u, gsts_bit, false, what);
}

/* The same wait, for the invalidation registers whose command bit is cleared
 * BY THE HARDWARE on completion (§6.5.1 CCMD.ICC, §6.5.2 IOTLB.IVT) rather
 * than mirrored into GSTS. */
static bool vtd_await_clear64(volatile uint64_t* reg, uint64_t bit,
                              const char* what)
{
    HourGlass glass;
    if (!HourGlassTurn(&glass, VTD_HANDSHAKE_PATIENCE_US)) {
        kprintf("[VT-d] no time source — waiting for %s on the fact itself, "
                "with no bound\n", what);
        while (*reg & bit) cpu_pause();
        return true;
    }
    for (;;) {
        if (!(*reg & bit)) return true;
        if (HourGlassRunOut(&glass)) break;
        cpu_pause();
    }
    kprintf("[VT-d] %s never completed — %lu us on the %s and the %s\n",
            what, (unsigned long)HourGlassElapsedUs(&glass),
            HourGlassRulerName(&glass),
            HourGlassSourceDied(&glass) ? "ruler died mid-wait"
                                        : "span ran out");
    return false;
}

/* ----- The one place GCMD is written -----
 *
 * GCMD is WRITE-ONLY and a write carries the WHOLE command word: every field a
 * bare literal leaves at zero is a field the hardware is being told to turn
 * off. VT-d Rev 5.x §11.4.4.1 spells the sequence out — read GSTS, mask off the
 * one-shot bits (the spec's own 96FFFFFFh), OR in the one field being changed,
 * write, then wait for GSTS to answer. Writing `GCMD_SIRTP` on its own does not
 * "set SIRTP"; it sets SIRTP and clears TE, EAFL, QIE, IRE and CFI in the same
 * word, which is how the SIRTP here used to switch off the queue that had been
 * enabled one line earlier.
 *
 * So the persistent half of the word lives in u->gcmd, seeded from GSTS when
 * the unit is taken over (the shadow Linux keeps in intel_iommu::gcmd, and the
 * same set EDK2's 96FFFFFFh mask preserves): TE, EAFL, QIE, IRE, CFI — the
 * fields GSTS reports back as ongoing state. SRTP, SFL, WBF and SIRTP are
 * one-shot commands; they ride along with a single write and are never
 * recorded, because writing one back would re-issue a pointer load nobody
 * asked for. */
#define VTD_GCMD_PERSISTENT  (GCMD_TE | GCMD_EAFL | GCMD_QIE | \
                              GCMD_IRE | GCMD_CFI)

/* Publish the shadow, optionally carrying ONE one-shot command with it. */
static void vtd_gcmd_publish(vtd_unit_t* u, uint32_t oneshot)
{
    *(volatile uint32_t*)(u->reg + VTD_REG_GCMD) = u->gcmd | oneshot;
}

/* Take the unit over from whoever had it before us.
 *
 * A remap unit does not necessarily arrive idle. Firmware that advertises
 * pre-boot DMA protection (DMAR flags bit 2) hands the OS a unit already
 * translating, with GSTS.TES set and a root table of its own; some hand over
 * interrupt remapping or an invalidation queue the same way. None of those
 * tables are ours: the root table this driver is about to publish has no
 * present entries yet, and the ring firmware is using is not one we drain.
 *
 * The decision is therefore to DISABLE what firmware left on, deliberately and
 * out loud, before repointing anything — the same choice Linux makes outside
 * kdump (iommu.c "Disable translation if already enabled prior to OS handover",
 * intel_iommu_init_qi's dmar_disable_qi, irq_remapping.c's pre-enabled path).
 * Adopting instead would mean translating live DMA through an empty table.
 *
 * One field per write, serialized as §11.4.4.1 requires, each answered by its
 * GSTS bit going. Returns false when a field would not go: the caller must then
 * leave the unit alone entirely, because firmware's tables are still in force
 * and overwriting RTADDR under them is the one move that cannot be undone. */
static bool vtd_adopt_from_firmware(vtd_unit_t* u)
{
    uint32_t sts = *(volatile uint32_t*)(u->reg + VTD_REG_GSTS);
    u->gcmd = sts & VTD_GCMD_PERSISTENT;

    if (u->gcmd & GCMD_TE) {
        kprintf("[VT-d] unit at 0x%lx arrived TRANSLATING — its root table is "
                "firmware's, not ours; switching translation off before "
                "replacing it\n", (unsigned long)u->reg_phys);
        u->gcmd &= ~GCMD_TE;
        vtd_gcmd_publish(u, 0);
        if (!vtd_await_gsts_gone(u, GSTS_TES,
                                 "firmware's translation (GCMD.TE -> GSTS.TES)"))
            return false;
    }
    if (u->gcmd & GCMD_IRE) {
        kprintf("[VT-d] unit at 0x%lx arrived REMAPPING INTERRUPTS — its remap "
                "table is firmware's; switching interrupt remapping off\n",
                (unsigned long)u->reg_phys);
        u->gcmd &= ~GCMD_IRE;
        vtd_gcmd_publish(u, 0);
        if (!vtd_await_gsts_gone(u, GSTS_IRES,
                                 "firmware's interrupt remapping "
                                 "(GCMD.IRE -> GSTS.IRES)"))
            return false;
    }
    if (u->gcmd & GCMD_QIE) {
        kprintf("[VT-d] unit at 0x%lx arrived with an invalidation queue we do "
                "not drain — switching it off before publishing our own ring\n",
                (unsigned long)u->reg_phys);
        u->gcmd &= ~GCMD_QIE;
        vtd_gcmd_publish(u, 0);
        if (!vtd_await_gsts_gone(u, GSTS_QIES,
                                 "firmware's invalidation queue "
                                 "(GCMD.QIE -> GSTS.QIES)"))
            return false;
    }
    return true;
}

/* Take Queued Invalidation back off. One field, through the shadow, answered by
 * GSTS.QIES going — the hardware also resets IQH to 0 as it goes (§6.5.2), so a
 * later re-enable starts from a head this driver agrees with. */
static void vtd_qi_disable(vtd_unit_t* u)
{
    u->gcmd &= ~GCMD_QIE;
    vtd_gcmd_publish(u, 0);
    (void)vtd_await_gsts_gone(u, GSTS_QIES,
                              "queued invalidation going off "
                              "(GCMD.QIE -> GSTS.QIES)");
    u->qi_active = false;
}

/* ----- Queued Invalidation (§6.5.2) ----- */

/* Invalidation descriptors. The Type field is seven bits — qw0 bits 11:9
 * concatenated with bits 3:0 — and every descriptor this driver issues has 0h
 * in the upper half, so the type is carried by bits 3:0 alone. G (granularity)
 * is at bits 5:4 in both descriptors below, DID at 31:16, SID at 47:32. */
#define QI_TYPE_CC        0x1ULL      /* Context-cache Invalidate (§6.5.2.1) */
#define QI_TYPE_IOTLB     0x2ULL      /* IOTLB Invalidate (§6.5.2.3) */
#define QI_TYPE_WAIT      0x5ULL      /* Invalidation Wait (§6.5.2.9) */
#define QI_GRAN_GLOBAL    (1ULL << 4)
#define QI_GRAN_DOMAIN    (2ULL << 4)
#define QI_GRAN_DEVICE    (3ULL << 4) /* context-cache; page-selective in IOTLB */
#define QI_DID(did)       (((uint64_t)(did) & 0xFFFFULL) << 16)
#define QI_SID(sid)       (((uint64_t)(sid) & 0xFFFFULL) << 32)
#define QI_IOTLB_DW       (1ULL << 6) /* drain in-flight writes */
#define QI_IOTLB_DR       (1ULL << 7) /* drain in-flight reads */

/* Invalidation Wait Descriptor fields (§6.5.2.9). SW at bit 5 asks hardware
 * to "indicate the invalidation wait descriptor completion by performing a
 * coherent DWORD write of the value in the Status Data field to the address
 * specified in the Status Address field"; Status Data is qw0 bits 63:32 and
 * the Status Address is qw1 bits 63:2. The other two flags are deliberately
 * left clear: IF (bit 4) asks for an interrupt this kernel has not armed, and
 * FN (bit 6) orders descriptors AFTER the wait, which is not what is being
 * asked for here — everything this driver submits is ahead of its own wait.
 * Linux and EDK2 build the same word: QI_IWD_STATUS_DATA(d) << 32,
 * QI_IWD_STATUS_WRITE = 1 << 5, QI_IWD_TYPE = 0x5. */
#define QI_IWD_SW         (1ULL << 5)
#define QI_IWD_STATUS(d)  (((uint64_t)(d)) << 32)

/* The two values the status DWORD ever holds. Any non-zero Status Data would
 * do; these are the ones Linux uses (QI_FREE / QI_DONE), and the word is put
 * back to PENDING before every submit so a store that lands after its own
 * wait gave up cannot be read as the next wait's answer. */
#define VTD_QI_WAIT_PENDING  0u
#define VTD_QI_WAIT_DONE     2u

/* One IOTLB Invalidate descriptor (§6.5.2.3).
 *
 * The type belongs in bits 3:0 and the granularity in 5:4, and swapping the
 * two does not produce a malformed descriptor — it produces a well-formed
 * DIFFERENT one: 0x21 is a domain-selective context-cache invalidate for
 * domain 0, which the hardware retires without complaint while the IOTLB keeps
 * every translation the caller believed it had just dropped.
 *
 * DR/DW ask the unit to drain reads/writes already in the fabric before it
 * reports the invalidation done, which is what makes "the old mapping is gone"
 * true rather than merely started; both are legal only on a unit that reports
 * them, so both ride on CAP. */
static uint64_t vtd_qi_iotlb_desc(const vtd_unit_t* u, uint64_t gran,
                                  uint32_t did)
{
    uint64_t qw0 = QI_TYPE_IOTLB | gran | QI_DID(did);
    if (u->cap & CAP_DWD) qw0 |= QI_IOTLB_DW;
    if (u->cap & CAP_DRD) qw0 |= QI_IOTLB_DR;
    return qw0;
}

/* Which of this unit's queue-error bits are standing right now. */
static uint32_t vtd_qi_fault_bits(vtd_unit_t* u)
{
    return *(volatile uint32_t*)(u->reg + VTD_REG_FSTS) & FSTS_QI_ERRORS;
}

static void vtd_qi_clear_faults(vtd_unit_t* u, uint32_t bits)
{
    *(volatile uint32_t*)(u->reg + VTD_REG_FSTS) = bits;   /* RW1C */
}

/* Everything the unit is willing to say about a queue error, said out loud.
 *
 * This register is the difference between "the queue is slow" and "the
 * hardware refused the descriptor this driver wrote, froze its head on it and
 * will not fetch another until someone clears a bit". Without reading it the
 * second is reported as the first, which is the wrong cause, the wrong fix and
 * a second of the boot spent finding it out.
 *
 * Read before the queue is switched off, because IQH points at the offending
 * descriptor only until then (§6.5.2.11), and disabling QIE resets it. */
static void vtd_qi_say_faults(vtd_unit_t* u, uint32_t bits, const char* when)
{
    uint64_t rec  = *(volatile uint64_t*)(u->reg + VTD_REG_IQERCD);
    uint32_t head = (uint32_t)(*(volatile uint64_t*)(u->reg + VTD_REG_IQH)
                               & (uint64_t)(VTD_QI_BYTES - 1));
    const uint64_t* bad = (const uint64_t*)((const uint8_t*)u->qi_ring + head);

    if (bits & FSTS_IQE)
        kprintf("[VT-d] the unit at 0x%lx REFUSED an invalidation descriptor "
                "%s — IQEI=%u at queue offset 0x%x, qw0=0x%lx qw1=0x%lx\n",
                (unsigned long)u->reg_phys, when, IQERCD_IQEI(rec), head,
                (unsigned long)bad[0], (unsigned long)bad[1]);
    if (bits & FSTS_ITE)
        kprintf("[VT-d] the unit at 0x%lx timed out on a device-TLB "
                "invalidation %s — source id 0x%04x; every invalidation wait "
                "still outstanding is abandoned with it\n",
                (unsigned long)u->reg_phys, when, IQERCD_ITESID(rec));
    if (bits & FSTS_ICE)
        kprintf("[VT-d] the unit at 0x%lx got an invalid device-TLB "
                "invalidation completion %s — source id 0x%04x\n",
                (unsigned long)u->reg_phys, when, IQERCD_ICESID(rec));
}

static void vtd_qi_init(vtd_unit_t* u) {
    /* ECAP.QI bit 1 — queued invalidation supported. */
    if (!(u->ecap & (1ULL << 1))) return;
    u->qi_ring = alloc_table_4k(&u->qi_ring_phys);
    if (!u->qi_ring) return;
    /* No status word, no way to learn that an invalidation finished — and
     * "submitted" is not "finished". A queue that cannot be waited on is not
     * enabled at all; the CCMD/IOTLB registers do the same work synchronously
     * and need nothing allocated. */
    u->qi_status = (volatile uint32_t*)alloc_table_4k(&u->qi_status_phys);
    if (!u->qi_status) {
        pmm_free((void*)(uintptr_t)u->qi_ring_phys, 1);
        u->qi_ring = NULL;
        u->qi_ring_phys = 0;
        kprintf("[VT-d] no page for the invalidation wait status word on the "
                "unit at 0x%lx — leaving queued invalidation off\n",
                (unsigned long)u->reg_phys);
        return;
    }
    *u->qi_status = VTD_QI_WAIT_PENDING;
    u->qi_tail = 0;

    /* A queue error outlives the queue that caused it: IQE/ICE/ITE are sticky
     * RW1C, and §6.5.2.11 makes an IQE left standing by firmware block every
     * descriptor fetch on this unit — ours included, and the IQT write that
     * would have started them included. So whatever is standing goes now,
     * before our ring is published. The descriptor it refers to was in
     * firmware's ring, which is not ours to read or print. */
    uint32_t stale = vtd_qi_fault_bits(u);
    if (stale) {
        uint64_t rec = *(volatile uint64_t*)(u->reg + VTD_REG_IQERCD);
        kprintf("[VT-d] the unit at 0x%lx arrived with invalidation queue "
                "error bits 0x%x standing — clearing them before publishing "
                "this kernel's ring\n", (unsigned long)u->reg_phys, stale);
        /* One IQERCD field answers for one bit and for no other (§11.4.9.9):
         * IQEI says why a descriptor was refused, ITESID names the device
         * that timed out, ICESID the one that answered badly. Quoting IQEI
         * under an ITE or an ICE would be reading a field the hardware did
         * not write for that event and calling the number a cause. */
        if (stale & FSTS_IQE)
            kprintf("[VT-d]   IQE — a descriptor was refused, IQEI=%u\n",
                    IQERCD_IQEI(rec));
        if (stale & FSTS_ITE)
            kprintf("[VT-d]   ITE — a device-TLB invalidation timed out, "
                    "source id 0x%04x\n", IQERCD_ITESID(rec));
        if (stale & FSTS_ICE)
            kprintf("[VT-d]   ICE — an invalid device-TLB invalidation "
                    "completion arrived, source id 0x%04x\n",
                    IQERCD_ICESID(rec));
        vtd_qi_clear_faults(u, stale);
    }

    /* IQA: low bits encode Queue Size = ring size in 4 KB units - 1.
     * We use exactly 4 KB → QS=0. */
    *(volatile uint64_t*)(u->reg + VTD_REG_IQA) = u->qi_ring_phys;
    *(volatile uint64_t*)(u->reg + VTD_REG_IQT) = 0;
    /* Enable QI via GCMD.QIE, then wait for GSTS.QIES to mirror it.
     *
     * On failure the enable is TAKEN BACK rather than left half-made. The
     * hazard here is one-directional: qi_active is set only after QIES has
     * latched, so the hardware can never have QI on while this kernel thinks
     * it is off — but leaving GCMD.QIE asserted on a unit whose QIES never
     * appeared would leave a queue this driver has decided not to use, with an
     * IQA still pointing at a ring nobody drains. */
    u->gcmd |= GCMD_QIE;
    vtd_gcmd_publish(u, 0);
    if (vtd_await_gsts(u, GSTS_QIES, "queued invalidation (GCMD.QIE -> GSTS.QIES)")) {
        u->qi_active = true;
    } else {
        kprintf("[VT-d] taking GCMD.QIE back off this unit — invalidation "
                "falls back to the CCMD/IOTLB registers\n");
        vtd_qi_disable(u);
        /* The ring and the wait status word were bought for a queue that
         * never came up, and nothing later in this driver's life comes back
         * for them. They go to the PMM here — but only once the unit itself
         * says QIES is gone, because IQA still holds the ring's physical
         * address and a queue somehow still running would be fetching
         * descriptors out of a page the PMM has since re-issued. Two leaked
         * pages are cheaper than that, and the leak is said out loud. */
        if (!(*(volatile uint32_t*)(u->reg + VTD_REG_GSTS) & GSTS_QIES)) {
            pmm_free((void*)(uintptr_t)u->qi_ring_phys, 1);
            pmm_free((void*)(uintptr_t)u->qi_status_phys, 1);
            u->qi_ring        = NULL;
            u->qi_ring_phys   = 0;
            u->qi_status      = NULL;
            u->qi_status_phys = 0;
        } else {
            kprintf("[VT-d] the unit at 0x%lx still reports GSTS.QIES with "
                    "GCMD.QIE off — its invalidation ring and wait status "
                    "page stay allocated rather than be handed back under a "
                    "queue that may still be fetching from them\n",
                    (unsigned long)u->reg_phys);
        }
    }
}

/* Submit one invalidation descriptor and wait until the unit says it
 * COMPLETED. Returns false when it never did.
 *
 * IQH catching IQT was never that proof. §6.5.2: the Invalidation Queue Head
 * Register is incremented by hardware after FETCHING a descriptor, and the
 * queue counts as quiesced only when head equals tail AND "the last descriptor
 * completed is an Invalidation Wait Descriptor (which indicates no
 * invalidation requests are pending in hardware)". The comment that stood
 * here — "Drained when the hardware's head catches the tail we just
 * published" — asserted the spec's insufficient condition as hardware fact,
 * and on that basis this function returned while the context-cache
 * invalidate, the IOTLB invalidate and any device-TLB round trip were still
 * in flight inside the remap unit. A caller that then hands the page back to
 * the PMM has a device still translating through the entry it just dropped.
 *
 * So a submit is a pair: the caller's descriptor, then an Invalidation Wait
 * Descriptor (§6.5.2.9) with SW=1, whose Status Data the unit writes into
 * this unit's status DWORD once everything ahead of it has completed. That
 * word is the exit, and the only one. Linux ends every qi_submit_sync the
 * same way, and reads FSTS on every turn of the poll for the same reason this
 * does: a descriptor the hardware refuses stops the queue dead, and no amount
 * of further waiting will change it. */
static bool vtd_qi_submit(vtd_unit_t* u, uint64_t qw0, uint64_t qw1) {
    if (!u->qi_active) return false;

    uint64_t* slot = (uint64_t*)((uint8_t*)u->qi_ring + u->qi_tail);
    slot[0] = qw0;
    slot[1] = qw1;
    u->qi_tail = (u->qi_tail + 16) & (VTD_QI_BYTES - 1);

    *u->qi_status = VTD_QI_WAIT_PENDING;
    uint64_t* wait = (uint64_t*)((uint8_t*)u->qi_ring + u->qi_tail);
    wait[0] = QI_TYPE_WAIT | QI_IWD_SW | QI_IWD_STATUS(VTD_QI_WAIT_DONE);
    wait[1] = u->qi_status_phys;
    u->qi_tail = (u->qi_tail + 16) & (VTD_QI_BYTES - 1);

    /* The two descriptors and the cleared status word are ordinary writeback
     * stores and the tail is a store to uncached MMIO, which x86 will not
     * move ahead of them. The unit cannot be handed a tail that points past a
     * descriptor it has not been given. */
    *(volatile uint64_t*)(u->reg + VTD_REG_IQT) = u->qi_tail;

    HourGlass glass;
    bool bounded = HourGlassTurn(&glass, VTD_HANDSHAKE_PATIENCE_US);
    if (!bounded)
        kprintf("[VT-d] no time source — waiting for the invalidation wait "
                "descriptor on the fact itself, with no bound\n");

    for (;;) {
        if (*u->qi_status == VTD_QI_WAIT_DONE) return true;

        uint32_t bad = vtd_qi_fault_bits(u);
        if (bad) {
            /* Said first, because IQH still points at the offending
             * descriptor; the queue switched off next, so the hardware cannot
             * re-fetch that descriptor and re-raise the bit behind the clear;
             * and the bit cleared last, because §6.5.2.11 makes an IQE left
             * standing block every fetch this unit will ever be asked for,
             * long after this call is forgotten. */
            vtd_qi_say_faults(u, bad, "while an invalidation was in flight");
            vtd_qi_disable(u);
            vtd_qi_clear_faults(u, bad);
            kprintf("[VT-d] queued invalidation withdrawn from the unit at "
                    "0x%lx — invalidation falls back to the CCMD/IOTLB "
                    "registers\n", (unsigned long)u->reg_phys);
            return false;
        }
        if (bounded && HourGlassRunOut(&glass)) break;
        cpu_pause();
    }

    /* A queue that stops finishing does not start again on its own, and every
     * later submit would spend the same second discovering that. So the
     * mechanism is withdrawn instead of degraded: QIE comes back off and the
     * caller falls through to the CCMD/IOTLB registers, which are always
     * present (§6.5.1) and need no queue at all. */
    kprintf("[VT-d] the invalidation wait descriptor never landed after %lu us "
            "on the %s (head=0x%lx tail=0x%x) and the unit reports no queue "
            "error — withdrawing queued invalidation\n",
            (unsigned long)HourGlassElapsedUs(&glass),
            HourGlassRulerName(&glass),
            (unsigned long)(*(volatile uint64_t*)(u->reg + VTD_REG_IQH)
                            & ~0xFULL),
            u->qi_tail);
    vtd_qi_disable(u);
    return false;
}

/* ----- Interrupt Remap (§5.1) ----- */

static void vtd_ir_init(vtd_unit_t* u) {
    /* ECAP.IR bit 3. */
    if (!(u->ecap & (1ULL << 3))) return;
    u->ir_table = alloc_table_4k(&u->ir_table_phys);
    if (!u->ir_table) return;
    u->ir_entries = VTD_IR_ENTRIES;
    /* IRTA: address | (size = log2(entries) - 1) in low 4 bits. */
    uint8_t size_enc = 0;
    uint32_t n = VTD_IR_ENTRIES;
    while (n > 2) { size_enc++; n >>= 1; }
    *(volatile uint64_t*)(u->reg + VTD_REG_IRTA) =
        u->ir_table_phys | size_enc;
    /* SIRTP is a one-shot: it rides on the shadow rather than replacing it, so
     * the queue enabled a moment ago in vtd_qi_init keeps its QIE bit. There is
     * nothing to take back when it does not complete, so the undo is simply
     * refusing to call interrupt remapping active. GCMD.IRE is never set here,
     * so nothing is remapping either way. */
    vtd_gcmd_publish(u, GCMD_SIRTP);
    if (vtd_await_gsts(u, GSTS_IRTPS,
                       "the interrupt-remap table pointer (GCMD.SIRTP -> GSTS.IRTPS)")) {
        u->ir_active = true;
    } else {
        kprintf("[VT-d] this unit keeps no interrupt-remap table\n");
    }
}

/* IOTLB Invalidate Register (§11.4.8.1) — the register path's half of the
 * same operation vtd_qi_iotlb_desc builds as a descriptor, and its bits are
 * NOT in the descriptor's places: IVT is bit 63, IIRG 61:60, and the drain
 * bits are DR at 49 and DW at 48, where the descriptor keeps DR at 7 and DW
 * at 6.
 *
 * The drain bits are what make "the old mapping is gone" true rather than
 * merely started — without them the unit may report the invalidation done
 * while reads and writes issued under the old translation are still in the
 * fabric. §11.4.8.1 has a unit that does not report DRD/DWD in CAP treat the
 * matching field as reserved, so both ride on the same CAP_DRD/CAP_DWD the
 * queued descriptor consults, and this path asks for exactly what that one
 * asks for. It used to ask for neither, which made the fallback a WEAKER
 * invalidation than the queue it stands in for — on precisely the units whose
 * queue had just been withdrawn.
 *
 * (Linux's __iommu_flush_iotlb sets only DMA_TLB_WRITE_DRAIN, from
 * cap_write_drain, and does not ask for read draining at all. The spec allows
 * both; this driver asks for both, here and in the descriptor, so the two
 * halves of one operation cannot differ in what they promise.) */
#define IOTLB_IVT           (1ULL << 63)
#define IOTLB_IIRG_GLOBAL   (1ULL << 60)
#define IOTLB_DR            (1ULL << 49)
#define IOTLB_DW            (1ULL << 48)

/* Drain context cache + global IOTLB after table changes (§6.5.5).
 *
 * Returns whether the hardware said both COMPLETED. That answer is not a
 * diagnostic: §6.6 makes this pair the precondition for translating at all on
 * a part that does not report ESRTPS, so a unit whose CCMD.ICC never cleared
 * would begin translating through whatever firmware left cached. It used to
 * be discarded — `(void)vtd_await_clear64(...)` twice — and the caller was
 * then free to set GCMD.TE on a unit that had invalidated nothing. */
static bool vtd_global_flush(vtd_unit_t* u) {
    if (u->qi_active) {
        /* Context cache first, then the IOTLB at a granularity no smaller —
         * the order §6.5.2.1 requires of the pair. */
        bool done = vtd_qi_submit(u, QI_TYPE_CC | QI_GRAN_GLOBAL, 0);
        /* Both results are folded into one flag rather than short-circuited,
         * because that flag is what decides whether the register path below
         * runs at all. There is no case in which the first fails and the
         * second still reaches hardware: every false return from
         * vtd_qi_submit has already taken the queue off this unit, so the
         * second call returns at its first line without writing a
         * descriptor. The `&& done` is bookkeeping, not a second attempt. */
        done = vtd_qi_submit(u, vtd_qi_iotlb_desc(u, QI_GRAN_GLOBAL, 0), 0)
               && done;
        if (done) return true;
        /* vtd_qi_submit withdraws queued invalidation from a unit whose queue
         * stopped finishing, and every submit after that returns having done
         * nothing. The flush itself is not optional, so a unit that lost its
         * queue halfway finishes through the registers below — which is what
         * vtd_qi_submit's own comment promises and what this function used to
         * return without doing. */
        kprintf("[VT-d] the invalidation queue did not finish the global "
                "invalidate — finishing it through the CCMD/IOTLB "
                "registers\n");
    }
    /* CCMD: write CIRG=01 (global), then wait for the hardware to clear ICC.
     * There is nothing to undo on failure — the register invalidation either
     * happened or the unit is not answering — but the answer is carried back
     * to the caller, because what the caller does next is decide whether this
     * unit may begin translating. */
    *(volatile uint64_t*)(u->reg + VTD_REG_CCMD) =
        CCMD_ICC | CCMD_CIRG_GLOBAL;
    bool ok = vtd_await_clear64((volatile uint64_t*)(u->reg + VTD_REG_CCMD),
                                CCMD_ICC, "the global context-cache invalidate");

    /* IVA_REG lives at ECAP.IRO * 16 and IOTLB_REG one qword past it
     * (§11.4.3); a global invalidate needs no IVA_REG write.
     *
     * Issued even when the context-cache invalidate did not complete: §6.6
     * names the order, not a permission to stop, and the caller is told the
     * whole pair failed either way. */
    uint32_t iotlb_off = ECAP_IRO_OFF(u->ecap);
    volatile uint64_t* iotlb_reg =
        (volatile uint64_t*)(u->reg + iotlb_off + 8);
    uint64_t iotlb_cmd = IOTLB_IVT | IOTLB_IIRG_GLOBAL;
    if (u->cap & CAP_DWD) iotlb_cmd |= IOTLB_DW;
    if (u->cap & CAP_DRD) iotlb_cmd |= IOTLB_DR;
    *iotlb_reg = iotlb_cmd;
    return vtd_await_clear64(iotlb_reg, IOTLB_IVT,
                             "the global IOTLB invalidate") && ok;
}

/* ----- Per-unit init ----- */

#define VTD_MMIO_FLAGS  (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | \
                         VMM_FLAG_CACHE_DISABLE)

/* Give back everything vtd_unit_init took, in the reverse order it took it.
 *
 * Every refusal after the register window was mapped used to return with that
 * window still mapped — up to 16 KiB of it, once ECAP.IRO has been read and
 * the block remapped to its real length — and the root-table page, when one
 * had been bought, still out of the PMM's hands. Nothing comes back for them
 * later: a unit that does not reach `u->online = true` is not counted, not
 * indexed and not reachable through g_units.
 *
 * The root table is safe to hand back at the one refusal that holds one —
 * the root-table pointer the unit would not acknowledge. That point is past
 * vtd_adopt_from_firmware, which returns true only with GSTS.TES clear:
 * either it was never set or the hardware was watched clearing it. So the
 * unit is not translating, and RTADDR is walked only under GCMD.TE. Nothing
 * sets TE afterwards either — vtd_take_the_wheel walks the online units and
 * this one never becomes one.
 *
 * The struct is left blank, because vtd_init hands the same slot to the next
 * DRHD — g_unit_count only moves on success — and a half-filled one would
 * give that attempt this one's CAP, ECAP and pointers on any path that
 * returns before overwriting them. */
static void vtd_unit_unwind(vtd_unit_t* u) {
    if (u->root_table) pmm_free((void*)(uintptr_t)u->root_phys, 1);
    if (u->reg)        vmm_unmap_mmio(u->reg, u->reg_size);
    memset(u, 0, sizeof(*u));
}

static int vtd_unit_init(vtd_unit_t* u, uint64_t phys, uint16_t segment) {
    u->reg_phys = phys;
    u->segment  = segment;
    u->reg = (volatile uint8_t*)vmm_map_mmio((uintptr_t)phys, 4096,
                                             VTD_MMIO_FLAGS);
    if (!u->reg) { vtd_unit_unwind(u); return -1; }
    u->reg_size = 4096;
    u->cap  = *(volatile uint64_t*)(u->reg + VTD_REG_CAP);
    u->ecap = *(volatile uint64_t*)(u->reg + VTD_REG_ECAP);

    /* How long this unit's register block actually is. CAP and ECAP sit in the
     * first page and are read above; the IOTLB registers (ECAP.IRO) and the
     * fault-recording registers (CAP.FRO/CAP.NFR) are wherever the unit says,
     * and either block can begin past the first page — IRO is ten bits, so the
     * IOTLB registers can legally sit up to 0x3FF0 into the block. One page was
     * mapped to learn that; the rest is mapped now, before anything outside the
     * first page is touched. */
    uint32_t block_end = ECAP_IOTLB_REG_END(u->ecap);
    uint32_t fault_end = CAP_FAULT_REG_END(u->cap);
    if (fault_end > block_end) block_end = fault_end;
    size_t regs_size = ((size_t)block_end + 0xFFFu) & ~(size_t)0xFFFu;
    if (regs_size > 4096) {
        vmm_unmap_mmio(u->reg, 4096);
        u->reg      = NULL;
        u->reg_size = 0;
        u->reg = (volatile uint8_t*)vmm_map_mmio((uintptr_t)phys, regs_size,
                                                 VTD_MMIO_FLAGS);
        if (!u->reg) {
            kprintf("[VT-d] remap unit at 0x%lx publishes a 0x%lx-byte register "
                    "block this kernel could not map — leaving it alone\n",
                    (unsigned long)phys, (unsigned long)regs_size);
            vtd_unit_unwind(u);
            return -1;
        }
        u->reg_size = regs_size;
    }

    /* SAGAW (CAP bits 12:8, §11.4.2): bit N set means the unit supports the
     * adjusted guest address width whose index is N, and that width's page
     * table has N + 2 levels — bit 1 is 39-bit/3-level, bit 2 is 48-bit/4-level,
     * bit 3 is 57-bit/5-level. The context entry's AW field (§9.3, bits 66:64)
     * carries that same index and "must match an AGAW value supported by
     * hardware", so AW = levels - 2, and the table handed to the unit must have
     * exactly that many levels.
     *
     * This driver's second-level walkers index a>>39, a>>30, a>>21, a>>12 —
     * four levels, unconditionally — so the width it can tell the whole truth
     * about is 48-bit, AGAW index 2, AW = 010b. It can tell a narrower truth
     * as well: a unit reporting only the 39-bit width is given the SECOND
     * level of that same table and AW = 001b (vtd_slpt_root_for), which is
     * what Linux does with an iommu whose agaw is below the domain's. What
     * cannot be served is a unit reporting neither — a 57-bit-only SAGAW
     * would need a fifth level that does not exist here — and that one is
     * refused out loud rather than handed an AW absent from its own SAGAW
     * (the old bit table was one arm low throughout: a 48-bit-only unit was
     * told 39-bit, and a 39-bit-only unit was told 000b, which §9.3 lists as
     * Reserved).
     *
     * Widening what is accepted widens what is DRIVEN. The unit comes up
     * exactly as a 48-bit one does — firmware's tables off, this kernel's
     * root table published — and vtd_take_the_wheel then switches it on with
     * the rest, so on a machine whose units are 39-bit-only the change is
     * between "no unit is driven" and "every unit translates". */
    uint8_t sagaw = (uint8_t)((u->cap >> CAP_SAGAW_SHIFT) & CAP_SAGAW_MASK);
    if (sagaw & (1u << VTD_AGAW_48BIT)) {
        u->sagaw_levels = VTD_AGAW_48BIT + 2;
    } else if (sagaw & (1u << VTD_AGAW_39BIT)) {
        u->sagaw_levels = VTD_AGAW_39BIT + 2;
        kprintf("[VT-d] remap unit at 0x%lx reports SAGAW=0x%02x — 39-bit "
                "AGAW only, so it gets the second level of this kernel's "
                "table as its root and AW=001b, and comes up like every "
                "other unit: off firmware's tables, ready for the wheel\n",
                (unsigned long)phys, sagaw);
    } else {
        kprintf("[VT-d] remap unit at 0x%lx reports SAGAW=0x%02x — neither the "
                "48-bit nor the 39-bit adjusted guest address width, and this "
                "driver has no table of any other depth; leaving the unit "
                "alone\n", (unsigned long)phys, sagaw);
        vtd_unit_unwind(u);
        return -2;
    }

    /* Whether this unit snoops the structures it walks. One unit that does
     * not is enough: the second-level tables are shared, so from here on
     * every structure this driver writes is handed back to memory. */
    if (!(u->ecap & ECAP_C) && g_walk_snooped) {
        g_walk_snooped = false;
        kprintf("[VT-d] remap unit at 0x%lx reports ECAP.C = 0 — it does not "
                "snoop the structures it walks, so this kernel flushes every "
                "one it writes\n", (unsigned long)phys);
    }

    /* Whatever firmware left running on this unit comes off before anything of
     * ours is published, and u->gcmd starts mirroring the hardware here. Every
     * GCMD write from this point on goes through vtd_gcmd_publish. */
    if (!vtd_adopt_from_firmware(u)) {
        kprintf("[VT-d] remap unit at 0x%lx would not come off firmware's "
                "tables — leaving it exactly as it was\n", (unsigned long)phys);
        vtd_unit_unwind(u);
        return -5;
    }

    /* Allocate root table. */
    u->root_table = alloc_table_4k(&u->root_phys);
    if (!u->root_table) { vtd_unit_unwind(u); return -3; }

    /* Program RTADDR_REG with the table physical address.
     * Bit 11:10 = TT (translation table type) = 00 (legacy). */
    *(volatile uint64_t*)(u->reg + VTD_REG_RTADDR) = u->root_phys;
    /* Tell hardware to load the new RTADDR, and REFUSE THE UNIT if it never
     * says it did. u->online used to be set regardless, which meant every
     * later caller — device_attach, map, invalidate — programmed context
     * entries reachable only through a root table the hardware never latched,
     * and believed it had done so. A unit this kernel cannot hand a root table
     * to is a unit this kernel does not have. */
    vtd_gcmd_publish(u, GCMD_SRTP);
    if (!vtd_await_gsts(u, GSTS_RTPS,
                        "the root-table pointer (GCMD.SRTP -> GSTS.RTPS)")) {
        kprintf("[VT-d] remap unit at 0x%lx refused its root table — not "
                "bringing it online\n", (unsigned long)phys);
        vtd_unit_unwind(u);
        return -4;
    }

    /* Set up advanced features. */
    vtd_qi_init(u);
    vtd_ir_init(u);

    u->online = true;
    debug_printf("[VT-d]   QI=%s IR=%s\n",
                 u->qi_active ? "yes" : "no",
                 u->ir_active ? "yes" : "no");
    return 0;
}

/* ----- Seating devices, and taking the wheel ----- */

static uint8_t vtd_units_online(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < g_unit_count; i++)
        if (g_units[i].online) n++;
    return n;
}

/* The PCI segment of the index-th online unit. iommu.c walks config space
 * once per distinct segment a unit lives in, and nowhere else: a segment with
 * no remap unit has nothing to seat devices into. */
static bool vtd_unit_segment(uint8_t index, uint16_t* seg_out) {
    uint8_t seen = 0;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!g_units[i].online) continue;
        if (seen == index) { *seg_out = g_units[i].segment; return true; }
        seen++;
    }
    return false;
}

/* Write this function's context entry into every unit of its segment, and
 * invalidate NOTHING.
 *
 * That is the whole difference from vtd_device_attach, and it is the reason
 * this exists beside it. Seating the machine means several dozen functions;
 * doing a context-cache and an IOTLB invalidation per function is several
 * dozen round trips through the invalidation queue to reach a state that one
 * global invalidation — the one §6.6 requires between the root-table pointer
 * and GCMD.TE anyway — reaches in one. vtd_take_the_wheel issues it.
 *
 * Nothing is cached yet in any case: every unit here has RTPS latched on a
 * root table this kernel wrote and GCMD.TE clear, so the hardware has never
 * walked one of these entries. The invalidation before TE is for what
 * firmware may have left cached, not for what this loop writes. */
static int vtd_seat_device(uint16_t seg, uint8_t bus, uint8_t devfn) {
    for (uint8_t i = 0; i < g_unit_count; i++) {
        vtd_unit_t* u = &g_units[i];
        if (!u->online || u->segment != seg) continue;
        /* Domain 0 is this driver's identity domain; what reaches the entry
         * is vtd_hw_did(0), which is 1 on a CM=1 part (§6.2.2.1). */
        if (vtd_program_context(u, bus, devfn, g_id_pml4_phys, 0) < 0)
            return -1;
    }
    return 0;
}

/* Take translation back off every unit that has it. The undo half of
 * all-or-nothing: a machine where some units translate and others do not is a
 * machine where a driver's DMA works or aborts depending on which root port
 * its device hangs off, and that is a worse failure than no translation at
 * all because it looks like a device fault. */
static void vtd_wheel_release(void) {
    for (uint8_t i = 0; i < g_unit_count; i++) {
        vtd_unit_t* u = &g_units[i];
        if (!u->online || !(u->gcmd & GCMD_TE)) continue;
        u->gcmd &= ~GCMD_TE;
        vtd_gcmd_publish(u, 0);
        (void)vtd_await_gsts_gone(u, GSTS_TES,
                                  "translation going back off "
                                  "(GCMD.TE -> GSTS.TES)");
    }
}

/* Switch DMA translation on, on every online unit, or on none of them.
 *
 * Order, and every step of it is the specification's:
 *
 *   1. The root-table pointer is already published and acknowledged —
 *      vtd_unit_init writes RTADDR_REG, issues GCMD.SRTP and refuses the unit
 *      unless GSTS.RTPS answers. §6.6: "Software must always perform a Set
 *      Root-Table Pointer operation before enabling or re-enabling (after
 *      disabling) remapping hardware."
 *   2. Every PCI function on the machine has a context entry by now: iommu.c
 *      walks config space and calls vtd_seat_device for each one BEFORE
 *      calling this. A function with no present context entry faults the
 *      instant TE goes on (LCT.2), which for a live disk controller is a
 *      machine that stops mid-boot with no explanation.
 *   3. A global context-cache invalidation, then a global IOTLB invalidation,
 *      in that order. §6.6 for implementations reporting ESRTPS clear:
 *      "software must perform a global invalidate of the context-cache,
 *      PASID-cache (if applicable), IOTLB, and HPT-cache (if applicable), in
 *      that order." vtd_global_flush issues exactly that pair, and the order
 *      is also §11.4.9.2's own: "software must perform domain-selective (or
 *      global) invalidation of IOTLB after the context-cache invalidation has
 *      completed". Issued unconditionally rather than under ESRTPS, because a
 *      unit that does it in hardware loses nothing by being asked again.
 *      This is also what discharges §6.8: on a part reporting RWBF, hardware
 *      "must implicitly perform a write buffer flush before invalidating the
 *      context-cache", so the tables written above are visible without a
 *      separate GCMD.WBF. The unit's ANSWER to that pair is required, not
 *      merely requested: a unit whose CCMD.ICC or IOTLB IVT never cleared
 *      takes the whole machine's translation down with it rather than begin
 *      translating through what it still holds.
 *   4. GCMD.TE, one field through the shadow, answered by GSTS.TES.
 *
 * Returns the number of units translating, which is either every online unit
 * or, on any refusal, a negative number with nothing left switched on. */
static int vtd_take_the_wheel(uint8_t* units_total) {
    uint8_t online = vtd_units_online();
    if (units_total) *units_total = online;

    if (online == 0) {
        kprintf("[VT-d] no remap unit is online — nothing to switch on\n");
        return -1;
    }
    if (!g_id_pml4) {
        kprintf("[VT-d] no identity map was built — translation stays off\n");
        return -1;
    }

    /* Is the table the hardware is about to walk actually populated where it
     * has to be? The question is answered by READING the entries, through the
     * same levels and present bits the unit uses, at the three addresses that
     * bound what a device can name: the bottom, the top of what a Single
     * Address Cycle reaches, and the top of the memory map. Then at both ends
     * of every region the DMAR reserves.
     *
     * What stood here compared the span the build recorded against the number
     * the build had been given — the same call, rounded up, in the same boot.
     * It could not come back false, and a check that cannot fail is not a
     * proof of anything. These can: a level the PMM refused, an RMRR above
     * the map, a hole left by an earlier partial build all show up as a
     * missing present bit at one of these addresses. */
    uint64_t top = pmm_get_mem_end();
    if (top < VTD_SAC_SPAN) top = VTD_SAC_SPAN;
    if (!vtd_identity_covers(0) ||
        !vtd_identity_covers(VTD_SAC_SPAN - 4096) ||
        !vtd_identity_covers(top - 4096)) {
        kprintf("[VT-d] the second-level tables do not translate one of 0x0, "
                "0x%lx or 0x%lx — the map is not what this kernel believes it "
                "built, so translation stays OFF everywhere rather than run a "
                "device into a hole\n",
                (unsigned long)(VTD_SAC_SPAN - 4096),
                (unsigned long)(top - 4096));
        return -1;
    }
    bool reserved_ok = true;
    vtd_walk_rmrr(vtd_check_rmrr, &reserved_ok);
    if (!reserved_ok) return -1;

    uint8_t on = 0;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        vtd_unit_t* u = &g_units[i];
        if (!u->online) continue;
        if (!vtd_global_flush(u)) {
            kprintf("[VT-d] DRHD[%u] did not finish the global invalidation "
                    "§6.6 requires before translation may be enabled — it "
                    "would begin translating through whatever it still has "
                    "cached, so translation is switched on NOWHERE\n", i);
            vtd_wheel_release();
            return -1;
        }
        u->gcmd |= GCMD_TE;
        vtd_gcmd_publish(u, 0);
        if (!vtd_await_gsts(u, GSTS_TES,
                            "translation (GCMD.TE -> GSTS.TES)")) {
            /* TES never appeared, so the unit is not translating — and TE
             * comes back out of the command word, so a TE that is merely late
             * cannot come up under a kernel that has counted this unit out. */
            u->gcmd &= ~GCMD_TE;
            vtd_gcmd_publish(u, 0);
            kprintf("[VT-d] DRHD[%u] would not begin translating — taking "
                    "translation back off the %u unit(s) that had already "
                    "started, so this machine translates nowhere rather than "
                    "in places\n", i, on);
            vtd_wheel_release();
            return -1;
        }
        on++;
        debug_printf("[VT-d] DRHD[%u] translation enabled (TES=1)\n", i);
    }
    return on;
}

/* ----- Primary fault logging: what a unit refused, and why (§7.2.1) -----
 *
 * The fault recording registers are the only channel this hardware has for a
 * non-recoverable translation or interrupt-remapping fault, and nothing in
 * this driver had ever read one. That is not merely a missing diagnostic.
 *
 * §11.4.7.1 on PFO: "When this field is Set, hardware does not record any new
 * faults until software clears this field." §7.3 on the fault event: "If
 * hardware detects any interrupt condition yet to be serviced by software,
 * the Fault Event interrupt is not generated." So a unit handed over by
 * firmware with an overflow, or with one F bit standing, is a unit that will
 * record nothing and signal nothing for the rest of the boot -- and it stays
 * that way through whoever arms the Fault Event and whoever finally sets
 * GCMD.TE. Reading and clearing the channel at the handover is what makes the
 * unit able to speak at all.
 *
 * Registers, not memory: unlike AMD-Vi's event log, these live inside the
 * unit's own MMIO block, so what firmware recorded survives this kernel
 * taking the unit over and is still there to be read.
 */

/* Is this fault reason one of the interrupt-remapping conditions?
 *
 * Table 15 (§5.1.4.1) occupies 20h-2Bh; Table 30 (§7.1.3) occupies the codes
 * below 20h for legacy mode and 30h and above for scalable mode. The split
 * matters for more than the name: §11.4.7.6 gives the FI field a different
 * meaning on each side, so a driver that does not know which side it is on
 * prints an address where there is an interrupt index, or an index where
 * there is an address. */
static bool vtd_fault_is_intr_remap(uint8_t fr) {
    return fr >= 0x20u && fr <= 0x2Bu;
}

/* Does FI carry the interrupt_index for this interrupt-remapping reason?
 *
 * §11.4.7.6, FI: "When the Fault Reason (FR) field indicates interrupt-
 * remapping Fault Reason 20h, 25h, 29h, 2Ah, and 2Bh, contents of this field
 * is undefined. When the Fault Reason (FR) field indicates interrupt-
 * remapping fault conditions other than [those], bits 63:48 of this field
 * indicate the interrupt_index computed for the faulted interrupt request,
 * and bits 47:12 are cleared." Printing an index out of an undefined field
 * would be inventing one. */
static bool vtd_fault_has_intr_index(uint8_t fr) {
    switch (fr) {
    case 0x20: case 0x25: case 0x29: case 0x2A: case 0x2B:
        return false;
    default:
        return true;
    }
}

/* The fault reason, in the specification's own words.
 *
 * The wording is the condition text of Table 30 (§7.1.3) and Table 15
 * (§5.1.4.1), and each legacy row carries the table's own condition code so
 * the row can be found again. Only the legacy-mode rows are named: this
 * driver programs RTADDR_REG.TTM = 00b and builds nothing else, so the
 * scalable-mode reasons at 30h and above cannot come from its own tables --
 * but firmware's may still be sitting in the register, so those are reported
 * by number rather than guessed at. Two codes have no row at all in either
 * table (0h and 0Fh-1Fh), and they get the same treatment.
 *
 * This function makes no claim about what the specification does or does not
 * contain: it reports only whether THIS driver has a name. */
static const char* vtd_fault_reason_name(uint8_t fr) {
    switch (fr) {
    /* Table 30, legacy root-table faults. */
    case 0x8: return "LRT.1: a hardware attempt to access a root-entry "
                     "referenced through RTADDR_REG resulted in an error";
    case 0x1: return "LRT.2: the Present field in the root-entry used to "
                     "process the request is 0";
    case 0xA: return "LRT.3: non-zero reserved field in a root-entry with "
                     "Present set";
    /* Table 30, legacy context-table faults. */
    case 0x9: return "LCT.1: a hardware attempt to access a context-entry "
                     "referenced through the CTP field of a root-entry "
                     "resulted in an error";
    case 0x2: return "LCT.2: the Present field in the context-entry used to "
                     "process the request is 0";
    case 0xB: return "LCT.3: non-zero reserved field in a context-entry with "
                     "Present set";
    case 0x3: return "LCT.4: invalid programming of the context-entry used "
                     "to process the request (an Address-Width or "
                     "Translation-Type the unit does not support, or an "
                     "error reaching the table its SSPTPTR names)";
    case 0xD: return "LCT.5: the Translation-Type of a present context-entry "
                     "blocks translation requests without PASID and "
                     "translated requests";
    /* Table 30, legacy second-stage table faults. */
    case 0x7: return "LSS.1: a hardware attempt to access a second-stage "
                     "paging entry referenced through a preceding one "
                     "resulted in an error";
    case 0xC: return "LSS.2: non-zero reserved field in a second-stage "
                     "paging entry with Read or Write set";
    /* Table 30, legacy general faults. */
    case 0x4: return "LGN.1: address overflow in second-stage translation";
    case 0x5: return "LGN.2: a write or AtomicOp request encountered lack of "
                     "write permission";
    case 0x6: return "LGN.3: a read or AtomicOp request encountered lack of "
                     "read permission";
    case 0xE: return "LGN.4: the address after remapping fell in the "
                     "interrupt address range FEEx_xxxxh";
    /* Table 15, interrupt-remapping faults. */
    case 0x20: return "decoding the remappable interrupt request found one "
                      "or more reserved fields set";
    case 0x21: return "the interrupt_index computed for the request is "
                      "greater than the interrupt-remapping table size, or "
                      "its IRTE lies above the Host Address Width";
    case 0x22: return "the Present field in the IRTE for this "
                      "interrupt_index is clear";
    case 0x23: return "a hardware attempt to access the interrupt-remapping "
                      "table through IRTA_REG resulted in an error";
    case 0x24: return "one or more reserved fields not initialized to zero "
                      "in an IRTE with Present set";
    case 0x25: return "a compatibility-format interrupt request was blocked "
                      "(EIME set in IRTA_REG, or CFIS clear in GSTS)";
    case 0x26: return "a remappable interrupt request was blocked because "
                      "the requester's source-id failed the SID/SVT/SQ "
                      "verification in its IRTE";
    case 0x27: return "a hardware attempt to access the Posted Interrupt "
                      "Descriptor named by an IRTE resulted in an error";
    case 0x28: return "one or more reserved fields not initialized to zero "
                      "in a Posted Interrupt Descriptor";
    case 0x29: return "an untranslated request-without-PASID to the "
                      "interrupt address range FEEx_xxxxh that does not meet "
                      "the requirements of a valid interrupt";
    case 0x2A: return "EIME is 0 in IRTA_REG on an implementation that "
                      "reports Extended Interrupt Mode Enable Required";
    case 0x2B: return "an MSI request arrived while interrupt remapping is "
                      "disabled on an implementation that reports Interrupt "
                      "Remapping Required";
    default:   return "(this driver has no name for this fault reason)";
    }
}

/* Does this fault reason name one of the address translation conditions?
 *
 * The question is not cosmetic. §11.4.7.6 makes T1, T2 and AT "relevant only
 * when the F field is Set, and when the fault reason (FR) indicates one of
 * the address translation fault conditions", so calling a record a "read
 * request" on the strength of a bit that the specification does not say
 * carries one is inventing the answer.
 *
 * True for exactly the codes vtd_fault_reason_name has a legacy Table 30 row
 * for -- 1h through Eh. Not for the scalable-mode rows at 30h and above:
 * those ARE translation conditions, but this driver programs legacy mode and
 * has no table of their reasons, so a record carrying one came from
 * somewhere this driver cannot speak for and is reported raw instead. */
static bool vtd_fault_is_addr_translation(uint8_t fr) {
    return fr >= 0x1u && fr <= 0xEu;
}

/* T1 and T2 together (§11.4.7.6, the T2 table). Relevant only when the fault
 * reason names an address translation condition, which is why the caller asks
 * only then. */
static const char* vtd_fault_request_kind(uint32_t t1, uint32_t t2) {
    if (!t1) return t2 ? "page request" : "write request";
    return t2 ? "AtomicOp request" : "read request";
}

/* Read, say and clear one unit's fault recording registers.
 *
 * And, on a unit whose invalidation queue never came up, its queue-error bits
 * as well -- because §7.3 does not gate the Fault Event on the fault records
 * alone: hardware decides "by evaluating if any of the PPF, PFO, IQE, ICE and
 * ITE fields in the Fault Status Register is Set", and "if hardware detects
 * any interrupt condition yet to be serviced by software, the Fault Event
 * interrupt is not generated". One IQE left standing by firmware therefore
 * silences this unit's every future fault report, records and all.
 *
 * On a unit where the queue IS active those three bits are not this
 * function's to touch: vtd_qi_submit reads FSTS on every turn of its own
 * wait, names the error out of IQERCD while IQH still points at the
 * descriptor that caused it (§6.5.2.11), and clears it there. Clearing them
 * from here would steal that report and leave the submitter looking at a
 * queue that failed for no stated reason. Where the queue never came up --
 * ECAP.QI clear, or vtd_qi_init out of pages for the ring or the wait status
 * word -- nobody will ever clear them, and nobody has read them either. */
static void vtd_unit_drain_faults(vtd_unit_t* u, uint8_t idx) {
    volatile uint32_t* fsts = (volatile uint32_t*)(u->reg + VTD_REG_FSTS);
    uint32_t status = *fsts;

    if (!u->qi_active) {
        uint32_t stuck = status & FSTS_QI_ERRORS;
        if (stuck) {
            /* IQERCD only, never the ring: u->qi_ring is NULL on precisely
             * the units this branch is for, and the descriptor the bits
             * refer to was firmware's in any case. */
            uint64_t rec = *(volatile uint64_t*)(u->reg + VTD_REG_IQERCD);
            kprintf("[VT-d] DRHD[%u] carries invalidation queue error bits "
                    "0x%x with no queue of this kernel's running -- nothing "
                    "else will ever clear them, and §7.3 has the unit "
                    "withhold its Fault Event interrupt while any of them "
                    "stands\n", idx, stuck);
            if (stuck & FSTS_IQE)
                kprintf("[VT-d]   IQE - a descriptor was refused, IQEI=%u\n",
                        IQERCD_IQEI(rec));
            if (stuck & FSTS_ITE)
                kprintf("[VT-d]   ITE - a device-TLB invalidation timed out, "
                        "source id 0x%04x\n", IQERCD_ITESID(rec));
            if (stuck & FSTS_ICE)
                kprintf("[VT-d]   ICE - an invalid device-TLB invalidation "
                        "completion arrived, source id 0x%04x\n",
                        IQERCD_ICESID(rec));
            *fsts = stuck;   /* RW1C; every other bit written 0 */
        }
    }

    if (!(status & (FSTS_PFO | FSTS_PPF))) return;

    /* How many registers there are, and where they begin, is what CAP.NFR and
     * CAP.FRO say -- no ceiling of this driver's choosing is involved, and
     * CAP.NFR is a count minus one so the loop bound is at least 1. The whole
     * register block out to CAP_FAULT_REG_END was mapped in vtd_unit_init for
     * exactly this. */
    const uint32_t nfr  = CAP_NUM_FAULT_REGS(u->cap);
    const uint32_t base = CAP_FRO_OFF(u->cap);
    const bool overflowed = (status & FSTS_PFO) != 0;

    if (overflowed) {
        kprintf("[VT-d] DRHD[%u] fault log OVERFLOWED — every fault after it "
                "filled was discarded, and §11.4.7.1 has the unit record "
                "nothing further while the bit stands\n", idx);
    }

    /* Where to start. §7.2.1 for the ordinary case: "Software is expected to
     * process the non-recoverable faults reported through the Fault Recording
     * Registers in a circular FIFO fashion starting from the Fault Recording
     * Register referenced by the Fault Recording Index (FRI) field, until it
     * finds a Fault Recording Register with no faults (F field Clear)."
     *
     * Under overflow that walk is not enough: recovery there is "software
     * must first process the pending faults in EACH of the Fault Recording
     * Registers, Clear the Fault (F) field in all those registers, and Clear
     * the overflow status" -- so every register is visited, and a clear one
     * is skipped rather than taken as the end. FRI is not consulted either:
     * §11.4.7.1 says "The value read from this field is undefined when the
     * PPF field is Clear", and an overflow with every F already processed can
     * present exactly that. */
    uint32_t i = overflowed ? 0u : FSTS_FRI(status);
    /* FRI is eight bits and NFR need not fill them. An index past the last
     * fault recording register would have this loop read -- and then RW1C a
     * bit into -- a register that is not one, somewhere else in the block
     * this unit published. The bound is the unit's own CAP.NFR; nothing here
     * is chosen. */
    if (i >= nfr) i = 0u;
    uint32_t said = 0;

    for (uint32_t visited = 0; visited < nfr; visited++, i = (i + 1u) % nfr) {
        volatile uint8_t* rec = u->reg + base + (size_t)i * 16u;

        /* The highest doubleword first, and the rest only once F reads set --
         * §11.4.7.6 footnote 1, because "hardware updates to this register
         * may be disassembled as multiple doubleword writes". */
        uint32_t hi = *(volatile uint32_t*)(rec + 12);
        if (!(hi & FRCD_HI_F)) {
            if (overflowed) continue;
            break;
        }

        uint32_t mid = *(volatile uint32_t*)(rec + 8);
        uint64_t fi  = *(volatile uint64_t*)rec;

        uint8_t  fr  = FRCD_HI_FR(hi);
        uint16_t sid = FRCD_MID_SID(mid);

        if (vtd_fault_is_intr_remap(fr)) {
            if (vtd_fault_has_intr_index(fr)) {
                kprintf("[VT-d] DRHD[%u] fault 0x%02x from %02x:%02x.%x on "
                        "interrupt index %u — %s\n",
                        idx, fr, sid >> 8, (sid >> 3) & 0x1Fu, sid & 0x7u,
                        (unsigned)(fi >> 48), vtd_fault_reason_name(fr));
            } else {
                kprintf("[VT-d] DRHD[%u] fault 0x%02x from %02x:%02x.%x — "
                        "%s\n",
                        idx, fr, sid >> 8, (sid >> 3) & 0x1Fu, sid & 0x7u,
                        vtd_fault_reason_name(fr));
            }
        } else if (vtd_fault_is_addr_translation(fr)) {
            /* PASID is said only when the register says it is there: PV is
             * "relevant only when the PP field is Set" (§11.4.7.6). */
            if (FRCD_MID_PP(mid)) {
                kprintf("[VT-d] DRHD[%u] fault 0x%02x from %02x:%02x.%x "
                        "PASID 0x%x, %s at 0x%lx — %s\n",
                        idx, fr, sid >> 8, (sid >> 3) & 0x1Fu, sid & 0x7u,
                        (unsigned)FRCD_HI_PV(hi),
                        vtd_fault_request_kind(FRCD_HI_T1(hi),
                                               FRCD_MID_T2(mid)),
                        (unsigned long)(fi & FRCD_FI_MASK),
                        vtd_fault_reason_name(fr));
            } else {
                kprintf("[VT-d] DRHD[%u] fault 0x%02x from %02x:%02x.%x, "
                        "%s at 0x%lx — %s\n",
                        idx, fr, sid >> 8, (sid >> 3) & 0x1Fu, sid & 0x7u,
                        vtd_fault_request_kind(FRCD_HI_T1(hi),
                                               FRCD_MID_T2(mid)),
                        (unsigned long)(fi & FRCD_FI_MASK),
                        vtd_fault_reason_name(fr));
            }
        } else {
            /* A reason this driver cannot place: it is neither an interrupt-
             * remapping condition nor one of the legacy translation rows, so
             * nothing here knows what FI holds or whether T1/T2 mean
             * anything. The record goes out as the hardware wrote it, which
             * is the whole of what it has to say. */
            kprintf("[VT-d] DRHD[%u] fault 0x%02x from %02x:%02x.%x — %s; "
                    "record 0x%lx 0x%lx\n",
                    idx, fr, sid >> 8, (sid >> 3) & 0x1Fu, sid & 0x7u,
                    vtd_fault_reason_name(fr),
                    (unsigned long)fi,
                    (unsigned long)(((uint64_t)hi << 32) | (uint64_t)mid));
        }

        /* The register as the hardware wrote it, for whoever subscribed. The
         * subject is the one AMD-Vi already publishes on; which of the two
         * shapes the sixteen bytes carry is settled by the backend named in
         * iommu:ready, and a machine has one backend. */
        struct { uint64_t lo; uint64_t hi; } raw = {
            .lo = fi,
            .hi = ((uint64_t)hi << 32) | (uint64_t)mid,
        };
        TouchPublish("iommu:fault", &raw, sizeof(raw));

        /* F is RW1C and clearing it is what lets PPF -- which is read-only
         * and computed as the OR of every F -- fall. */
        *(volatile uint32_t*)(rec + 12) = FRCD_HI_F;
        said++;
    }

    if (overflowed) {
        /* §7.2.1, and only now that every F is clear: "Clear the overflow
         * status by writing a 1 to the Primary Fault Overflow (PFO) field.
         * Once the PFO field is cleared by software, hardware continues to
         * record new faults." Every other bit of the word is written 0: the
         * RW1C bits ignore that, PPF is read-only, and bit 7 is RsvdZ. */
        *fsts = FSTS_PFO;
        kprintf("[VT-d] DRHD[%u] fault recording restarted after %u record(s) "
                "— what stood before the overflow is all there is of what was "
                "lost\n", idx, said);
    }
}

/* Public: read every online unit's fault recording registers, say what is in
 * them and clear them. Silent and harmless on a machine with no units, and on
 * a unit with nothing recorded. */
void vtd_drain_faults(void) {
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!g_units[i].online) continue;
        vtd_unit_drain_faults(&g_units[i], i);
    }
}

/* ----- ops vtable bodies ----- */

struct iommu_domain {
    uint32_t id;
    uint64_t* slpt;
    uint64_t  slpt_phys;
    bool      is_identity;
};
#define VTD_MAX_DOMAINS  64
static struct iommu_domain g_domains[VTD_MAX_DOMAINS];
static uint8_t             g_domain_count = 1;
/* g_domains[0] is the default identity-map shared domain. */

static int vtd_init(void) {
    const acpi_dmar_info_t* d = acpi_get_dmar();
    if (!d) return -1;
    g_unit_count = 0;
    for (uint8_t i = 0; i < d->drhd_count && g_unit_count < VTD_MAX_UNITS; i++) {
        vtd_unit_t* u = &g_units[g_unit_count];
        if (vtd_unit_init(u, d->drhd[i].register_base,
                          d->drhd[i].segment) == 0) {
            g_unit_count++;
            debug_printf("[VT-d] DRHD[%u] online: seg=%u base=0x%lx levels=%u\n",
                         i, d->drhd[i].segment,
                         (unsigned long)d->drhd[i].register_base,
                         u->sagaw_levels);
        }
    }
    if (g_unit_count == 0) return -2;

    /* Root tables were bought one unit at a time, so a unit that reported
     * ECAP.C = 0 may have arrived after an earlier unit's root page had
     * already been zeroed under the assumption that everything here snoops.
     * That page is walked by hardware, so it goes back to memory now that the
     * answer is final. The second-level tables need no such repair: they are
     * built below, after every unit has spoken. */
    if (!g_walk_snooped)
        for (uint8_t i = 0; i < g_unit_count; i++)
            vtd_structure_flush(g_units[i].root_table, 4096);

    if (vtd_build_identity() < 0) {
        /* This is the one refusal that arrives AFTER the units were taken
         * over, and taking a unit over is what ends firmware's pre-boot DMA
         * protection. Returning here with the units half-held would leave
         * every MMIO window mapped, every root page out of the PMM's hands,
         * every unit off firmware's tables and on ours with nothing walking
         * them — and iommu.c would then find no backend and say nothing at
         * all. So the units are handed back, one by one, and the state the
         * machine is left in is named rather than inferred. */
        kprintf("[VT-d] the identity second-level tables could not be built. "
                "%u unit(s) had already been taken off firmware's tables; "
                "they are being released now, and this machine runs with NO "
                "DMA protection at all — neither firmware's, which ended when "
                "the first unit was adopted, nor this kernel's, which is not "
                "going to start\n", g_unit_count);
        for (uint8_t i = 0; i < g_unit_count; i++)
            vtd_unit_unwind(&g_units[i]);
        g_unit_count = 0;
        return -2;
    }

    g_domains[0].id          = 0;
    g_domains[0].slpt        = g_id_pml4;
    g_domains[0].slpt_phys   = g_id_pml4_phys;
    g_domains[0].is_identity = true;
    g_domain_count           = 1;
    debug_printf("[VT-d] %u remap unit(s) ready; translation waits on "
                 "iommu_take_the_wheel(), after PCI is enumerated\n",
                 g_unit_count);
    return 0;
}

static iommu_domain_t* vtd_domain_alloc(void) {
    if (g_domain_count >= VTD_MAX_DOMAINS) return NULL;
    struct iommu_domain* d = &g_domains[g_domain_count];
    void* p = alloc_table_4k(&d->slpt_phys);
    if (!p) return NULL;
    d->slpt        = (uint64_t*)p;
    d->id          = g_domain_count;
    d->is_identity = false;
    g_domain_count++;
    return d;
}
static void vtd_domain_free(iommu_domain_t* d) {
    if (!d || d->id == 0 || d->id >= g_domain_count) return;
    /* Mark as free; full reclamation of the SLPT page tree is deferred
     * until a richer allocator is in place. */
    d->slpt = NULL;
    d->slpt_phys = 0;
}

static int vtd_device_attach(iommu_domain_t* d, uint16_t seg,
                              uint8_t bus, uint8_t devfn) {
    if (!d) return -1;
    uint64_t slpt = d->slpt_phys ? d->slpt_phys : g_id_pml4_phys;
    /* The number the DESCRIPTORS below must carry is the one the hardware was
     * given, not the one this driver counts with — vtd_program_context shifts
     * it past the reserved 0 on a CM=1 part, and a domain-selective
     * invalidation naming the unshifted id would name a domain nothing is
     * tagged with. */
    uint32_t did  = vtd_hw_did(d->id);
    int rc = -1;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        /* A context entry is reachable only through the root table of a unit
         * in the SAME segment (DMAR §8.3), which is what vtd_seat_device has
         * always honoured and this had not. */
        if (!g_units[i].online || g_units[i].segment != seg) continue;
        if (devfn == 0xFFu) {
            /* Wildcard: identity-map every devfn on this bus. */
            if (vtd_program_identity_bus(&g_units[i], bus) == 0) rc = 0;
        } else if (vtd_program_context(&g_units[i], bus, devfn, slpt,
                                       d->id) == 0) {
            rc = 0;
        }
        if (g_units[i].qi_active) {
            /* The context entries just rewritten are still cached in the unit,
             * so the context cache is invalidated for exactly what changed and
             * the IOTLB follows at a granularity no smaller, as §6.5.2.1
             * requires. What stood here was a type-03h descriptor — a
             * Device-TLB invalidate for IOVA 0 with a reserved bit set — which
             * invalidated no context cache at all and stalled the queue.
             *
             * The wildcard rewrote every devfn on the bus into the identity
             * domain; no device-selective descriptor covers that, so it is
             * invalidated globally. */
            bool done;
            if (devfn == 0xFFu) {
                done = vtd_qi_submit(&g_units[i], QI_TYPE_CC | QI_GRAN_GLOBAL,
                                     0);
                done = vtd_qi_submit(&g_units[i],
                                     vtd_qi_iotlb_desc(&g_units[i],
                                                       QI_GRAN_GLOBAL, 0), 0)
                       && done;
            } else {
                uint64_t sid = ((uint64_t)bus << 8) | devfn;
                done = vtd_qi_submit(&g_units[i], QI_TYPE_CC | QI_GRAN_DEVICE |
                                                  QI_DID(did) | QI_SID(sid), 0);
                done = vtd_qi_submit(&g_units[i],
                                     vtd_qi_iotlb_desc(&g_units[i],
                                                       QI_GRAN_DOMAIN, did), 0)
                       && done;
            }
            /* An invalidation that did not complete leaves the entries just
             * rewritten still cached in the unit, and the caller is about to
             * be told the device is attached. vtd_qi_submit has already taken
             * the queue off this unit and said why, so the global flush below
             * goes through the CCMD/IOTLB registers and finishes the job. */
            if (!done) (void)vtd_global_flush(&g_units[i]);
        }
    }
    return rc;
}
static int vtd_device_detach(iommu_domain_t* d, uint16_t seg,
                              uint8_t bus, uint8_t devfn) {
    (void)d;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!g_units[i].online || g_units[i].segment != seg) continue;
        vtd_program_context(&g_units[i], bus, devfn, g_id_pml4_phys, 0);
    }
    return 0;
}

/* Walk into a domain's SLPT to install (iova → phys) at 4 KB. */
static int domain_slpt_map(struct iommu_domain* d, uint64_t iova,
                             uint64_t phys, uint64_t size, uint32_t perm) {
    if (!d || !d->slpt) return -1;
    uint64_t end = iova + size;
    const uint64_t* run_from = NULL;
    const uint64_t* run_to   = NULL;
    for (uint64_t a = iova & ~0xFFFULL; a < end; a += 4096) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
        uint64_t* l3 = slpte_walk(d->slpt, i4, true);
        if (!l3) { vtd_structure_flush_run(run_from, run_to); return -2; }
        uint64_t* l2 = slpte_walk(l3, i3, true);
        if (!l2) { vtd_structure_flush_run(run_from, run_to); return -2; }
        uint64_t* l1 = slpte_walk(l2, i2, true);
        if (!l1) { vtd_structure_flush_run(run_from, run_to); return -2; }
        uint64_t pte = (phys + (a - (iova & ~0xFFFULL))) & ~0xFFFULL;
        if (perm & IOMMU_PERM_READ)  pte |= SLPTE_R;
        if (perm & IOMMU_PERM_WRITE) pte |= SLPTE_W;
        if (!(perm & (IOMMU_PERM_READ | IOMMU_PERM_WRITE)))
            pte |= SLPTE_R | SLPTE_W;
        l1[i1] = pte;
        if (run_to != &l1[i1]) {
            vtd_structure_flush_run(run_from, run_to);
            run_from = &l1[i1];
        }
        run_to = &l1[i1] + 1;
    }
    vtd_structure_flush_run(run_from, run_to);
    return 0;
}

static int vtd_map(iommu_domain_t* d, uint64_t iova, uint64_t phys,
                    uint64_t size, uint32_t perm) {
    if (d && !d->is_identity) {
        return domain_slpt_map(d, iova, phys, size, perm);
    }
    /* Default identity-domain path. */
    if (iova != phys) return -1;
    return vtd_identity_map(phys, size);
}

/* TME-MK aware map. phys_with_keyid carries the KeyID in upper bits
 * (above the reduced MAXPHYADDR). Per Intel VT-d Spec rev 3.4 §9.4.3
 * the SL-PTE phys-address field is bits [51:12] and the encryption
 * engine reads KeyID from the same upper bits the CPU PTE uses —
 * domain_slpt_map already preserves them via the `phys & ~0xFFF`
 * extraction (no upper-bit clamp). For identity-domain maps we
 * refuse: KeyID encryption only makes sense in a per-process / per-
 * Bay domain where the device's KeyID matches the CPU's. */
static int vtd_map_with_keyid(iommu_domain_t* d, uint64_t iova,
                               uint64_t phys_with_keyid, uint64_t size,
                               uint32_t perm) {
    if (!d || d->is_identity) return -1;
    return domain_slpt_map(d, iova, phys_with_keyid, size, perm);
}
static int vtd_unmap(iommu_domain_t* d, uint64_t iova, uint64_t size) {
    if (d && !d->is_identity) {
        /* Clear PTEs in domain's SLPT. */
        if (!d->slpt) return -1;
        uint64_t end = iova + size;
        for (uint64_t a = iova & ~0xFFFULL; a < end; a += 4096) {
            unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
            unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
            unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
            unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
            uint64_t* l3 = slpte_walk(d->slpt, i4, false);
            if (!l3) continue;
            uint64_t* l2 = slpte_walk(l3, i3, false); if (!l2) continue;
            uint64_t* l1 = slpte_walk(l2, i2, false); if (!l1) continue;
            l1[i1] = 0;
            /* Per entry rather than per run: an unmap walks a range the
             * caller chose, skips whatever is not mapped, and is nothing
             * like the linear fill the identity map performs. */
            vtd_structure_flush(&l1[i1], sizeof(l1[i1]));
        }
        return 0;
    }
    (void)d; (void)iova; (void)size;
    return 0;
}
static void vtd_invalidate(iommu_domain_t* d) {
    (void)d;
    for (uint8_t i = 0; i < g_unit_count; i++)
        if (g_units[i].online) (void)vtd_global_flush(&g_units[i]);
}

/* Phase 2G — opaque domain → ID accessor. Trivial field read. */
static uint32_t vtd_domain_id(iommu_domain_t* d) {
    return d ? ((struct iommu_domain*)d)->id : 0xFFFFFFFFu;
}

const iommu_ops_t vtd_ops = {
    .name           = "Intel VT-d",
    .init           = vtd_init,
    .domain_alloc   = vtd_domain_alloc,
    .domain_free    = vtd_domain_free,
    .device_attach  = vtd_device_attach,
    .device_detach  = vtd_device_detach,
    .map            = vtd_map,
    .unmap          = vtd_unmap,
    .invalidate     = vtd_invalidate,
    .domain_id      = vtd_domain_id,
    .map_with_keyid = vtd_map_with_keyid,
    .drain_faults   = vtd_drain_faults,
    .unit_segment   = vtd_unit_segment,
    .seat_device    = vtd_seat_device,
    .take_the_wheel = vtd_take_the_wheel,
};
