#include "iommu.h"
#include "acpi.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"

/* =====================================================================
 * Intel VT-d driver — production implementation.
 *
 * Reference: Intel Virtualization Technology for Directed I/O,
 *            Revision 5.0+ (the "VT-d" spec). Sections cited inline.
 *
 * Responsibilities
 * ----------------
 *   * Map each DRHD register block (one MMIO page per remap unit)
 *   * Read CAP/ECAP, derive SAGAW (supported adjusted guest address
 *     widths) and pick a 48-bit address width when available
 *   * Build a *single* second-level page table that identity-maps the
 *     first 4 GiB of physical RAM (covers every consumer DMA target);
 *     larger systems extend this through `vtd_identity_extend()`
 *   * Allocate one root table per remap unit; populate every context
 *     entry of buses we have devices on to point at the shared SLPT
 *   * Set RTADDR_REG, drain context cache + IOTLB
 *   * `vtd_enable_translation()` flips GCMD.TE — only callers that
 *     have explicitly opted-in to DMA translation should call this.
 *     init() does NOT enable translation by default, so booting with
 *     untranslated DMA remains the safe path.
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
#define VTD_REG_FECTL     0x038
#define VTD_REG_FEDATA    0x03C
#define VTD_REG_FEADDR    0x040
#define VTD_REG_IOTLB_OFF (((cap_reg >> 8) & 0x3FF) * 16)  /* macro-style helper below */
#define VTD_REG_PRECTL    0x028

/* CAP bits we read. */
#define CAP_SAGAW_SHIFT   8        /* bits 12:8 */
#define CAP_SAGAW_MASK    0x1Fu

/* GCMD bits (§10.4.16). */
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
    volatile uint8_t* reg;
    uint64_t  cap;
    uint64_t  ecap;
    uint8_t   sagaw_levels;        /* 4 (48-bit) or 3 (39-bit) */
    void*     root_table;          /* virt; 4 KB phys */
    uint64_t  root_phys;
    bool      online;
    bool      qi_active;
    bool      ir_active;
    void*     qi_ring;             /* 4 KB QI descriptor ring */
    uint64_t  qi_ring_phys;
    uint32_t  qi_tail;             /* in bytes; ring is 256 × 16 */
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
#define VTD_REG_IRTA     0x0B8

#define VTD_MAX_UNITS  ACPI_DMAR_MAX_DRHD
static vtd_unit_t g_units[VTD_MAX_UNITS];
static uint8_t    g_unit_count = 0;

/* ----- Shared identity-map second-level page tables -----
 *
 * One PML4 (the "top level") covers the entire DMA address space. We
 * lazily allocate lower levels and use 2-MiB super-pages where the
 * controller advertises support, otherwise 4-KiB pages.
 *
 * The page tables themselves come from PMM physical pages mapped into
 * the kernel via vmm_phys_to_virt (provided by the VMM). */
static uint64_t* g_id_pml4    = NULL;
static uint64_t  g_id_pml4_phys = 0;

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
    memset(virt, 0, 4096);
    return virt;
}

/* The CPU-side pointer for a table the hardware knows by physical address.
 * Every descent through these structures goes through here, because every one
 * of them stores what the IOMMU needs and none of them store what we do. */
static inline void* vtd_table_virt(uint64_t entry) {
    return vmm_phys_to_virt((uintptr_t)(entry & ~0xFFFULL));
}

static uint64_t* slpte_walk(uint64_t* parent, uint64_t parent_phys,
                             unsigned idx, bool create) {
    (void)parent_phys;
    uint64_t e = parent[idx];
    if (!(e & SLPTE_R)) {
        if (!create) return NULL;
        uint64_t child_phys;
        void* child = alloc_table_4k(&child_phys);
        if (!child) return NULL;
        parent[idx] = child_phys | SLPTE_R | SLPTE_W;
        return (uint64_t*)child;
    }
    if (e & SLPTE_PS) return NULL;       /* super-page; cannot descend */
    return (uint64_t*)vtd_table_virt(e);
}

/* Identity-map [phys, phys+size) at IOVA == phys with R+W. */
static int vtd_identity_map(uint64_t phys, uint64_t size) {
    if (!g_id_pml4) return -1;
    uint64_t end = phys + size;
    /* 4-KiB granularity for safety. Production tuning would slot in
     * 2 MiB and 1 GiB super-pages for the bulk of RAM. */
    for (uint64_t a = phys & ~0xFFFULL; a < end; a += 4096) {
        unsigned pml4_i = (unsigned)((a >> 39) & 0x1FF);
        unsigned pdpt_i = (unsigned)((a >> 30) & 0x1FF);
        unsigned pd_i   = (unsigned)((a >> 21) & 0x1FF);
        unsigned pt_i   = (unsigned)((a >> 12) & 0x1FF);

        uint64_t* pdpt = slpte_walk(g_id_pml4, g_id_pml4_phys, pml4_i, true);
        if (!pdpt) return -2;
        uint64_t* pd   = slpte_walk(pdpt, 0, pdpt_i, true);
        if (!pd)   return -2;
        uint64_t* pt   = slpte_walk(pd, 0, pd_i, true);
        if (!pt)   return -2;
        pt[pt_i] = a | SLPTE_R | SLPTE_W;
    }
    return 0;
}

/* Initial coverage: bottom 4 GiB. Real-HW callers wanting larger
 * coverage call vtd_identity_extend(end_phys). */
static int vtd_build_identity(void) {
    if (g_id_pml4) return 0;
    void* p = alloc_table_4k(&g_id_pml4_phys);
    if (!p) return -1;
    g_id_pml4 = (uint64_t*)p;
    return vtd_identity_map(0, 1ULL << 32);
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

static int vtd_program_context(vtd_unit_t* u, uint8_t bus, uint8_t devfn,
                                uint64_t slpt_phys, uint32_t domain_id) {
    vtd_root_entry_t* root = (vtd_root_entry_t*)u->root_table;
    vtd_root_entry_t* re = &root[bus];
    if (!(re->lo & 1)) {
        uint64_t ctx_phys;
        void* ctx = alloc_table_4k(&ctx_phys);
        if (!ctx) return -1;
        re->lo = ctx_phys | 1;
        re->hi = 0;
    }
    vtd_context_entry_t* ctx = (vtd_context_entry_t*)vtd_table_virt(re->lo);
    if (!ctx) return -1;
    uint8_t aw = (uint8_t)(u->sagaw_levels - 2);
    ctx[devfn].lo = slpt_phys | 1;
    ctx[devfn].hi = ((uint64_t)domain_id << 8) | ((uint64_t)aw & 0x7);
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

/* ----- Queued Invalidation (§6.5.2) ----- */

static void vtd_qi_init(vtd_unit_t* u) {
    /* ECAP.QI bit 1 — queued invalidation supported. */
    if (!(u->ecap & (1ULL << 1))) return;
    u->qi_ring = alloc_table_4k(&u->qi_ring_phys);
    if (!u->qi_ring) return;
    u->qi_tail = 0;
    /* IQA: low bits encode Queue Size = ring size in 4 KB units - 1.
     * We use exactly 4 KB → QS=0. */
    *(volatile uint64_t*)(u->reg + VTD_REG_IQA) = u->qi_ring_phys;
    *(volatile uint64_t*)(u->reg + VTD_REG_IQT) = 0;
    /* Enable QI via GCMD.QIE. */
    uint32_t gcmd = GCMD_QIE;
    *(volatile uint32_t*)(u->reg + VTD_REG_GCMD) = gcmd;
    /* Wait QIES = 1 in GSTS bit 26. */
    for (int i = 0; i < 1000000; i++) {
        if (*(volatile uint32_t*)(u->reg + VTD_REG_GSTS) & (1u << 26)) {
            u->qi_active = true;
            break;
        }
    }
}

static void vtd_qi_submit(vtd_unit_t* u, uint64_t qw0, uint64_t qw1) {
    if (!u->qi_active) return;
    uint64_t* slot = (uint64_t*)((uint8_t*)u->qi_ring + u->qi_tail);
    slot[0] = qw0;
    slot[1] = qw1;
    u->qi_tail = (u->qi_tail + 16) & (VTD_QI_BYTES - 1);
    *(volatile uint64_t*)(u->reg + VTD_REG_IQT) = u->qi_tail;
    /* Poll head == tail (drained). */
    for (int i = 0; i < 1000000; i++) {
        if ((*(volatile uint64_t*)(u->reg + VTD_REG_IQH) & ~0xFULL) ==
            (uint64_t)u->qi_tail) return;
    }
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
    *(volatile uint32_t*)(u->reg + VTD_REG_GCMD) = GCMD_SIRTP;
    for (int i = 0; i < 1000000; i++) {
        if (*(volatile uint32_t*)(u->reg + VTD_REG_GSTS) & (1u << 24)) {
            u->ir_active = true;
            break;
        }
    }
}

/* Drain context cache + global IOTLB after table changes (§6.5.5). */
static void vtd_global_flush(vtd_unit_t* u) {
    if (u->qi_active) {
        /* QI context-cache global invalidate. */
        vtd_qi_submit(u, (1ULL << 4) | (1ULL << 0), 0);     /* CC, global */
        /* QI IOTLB global invalidate. */
        vtd_qi_submit(u, (2ULL << 4) | (1ULL << 0), 0);     /* IOTLB, global */
        return;
    }
    /* CCMD: write CIRG=01 (global), then poll ICC until 0 (cleared). */
    *(volatile uint64_t*)(u->reg + VTD_REG_CCMD) =
        CCMD_ICC | CCMD_CIRG_GLOBAL;
    for (int i = 0; i < 1000000; i++) {
        if (!(*(volatile uint64_t*)(u->reg + VTD_REG_CCMD) & CCMD_ICC)) break;
    }
    /* IOTLB invalidation lives at offset (CAP[16:8] * 16). */
    uint32_t iotlb_off = (uint32_t)(((u->cap >> 8) & 0x3FF) * 16);
    uint64_t iotlb_reg = (uint64_t)(uintptr_t)(u->reg + iotlb_off + 8);
    *(volatile uint64_t*)iotlb_reg =
        (1ULL << 63)   |              /* IVT (invalidate) */
        (1ULL << 60);                 /* IIRG=01 global */
    for (int i = 0; i < 1000000; i++) {
        if (!(*(volatile uint64_t*)iotlb_reg & (1ULL << 63))) break;
    }
}

/* ----- Per-unit init ----- */

static int vtd_unit_init(vtd_unit_t* u, uint64_t phys) {
    u->reg_phys = phys;
    u->reg = (volatile uint8_t*)vmm_map_mmio((uintptr_t)phys, 4096,
                  VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                  VMM_FLAG_CACHE_DISABLE);
    if (!u->reg) return -1;
    u->cap  = *(volatile uint64_t*)(u->reg + VTD_REG_CAP);
    u->ecap = *(volatile uint64_t*)(u->reg + VTD_REG_ECAP);

    /* SAGAW: bits 12:8 of CAP. We prefer 4-level (48-bit) — bit 11
     * (1 << 3) in SAGAW. Fall back to 3-level (39-bit, bit 10). */
    uint8_t sagaw = (uint8_t)((u->cap >> CAP_SAGAW_SHIFT) & CAP_SAGAW_MASK);
    if (sagaw & (1 << 3))       u->sagaw_levels = 4;
    else if (sagaw & (1 << 2))  u->sagaw_levels = 3;
    else if (sagaw & (1 << 1))  u->sagaw_levels = 2;
    else                         return -2;

    /* Allocate root table. */
    u->root_table = alloc_table_4k(&u->root_phys);
    if (!u->root_table) return -3;

    /* Program RTADDR_REG with the table physical address.
     * Bit 11:10 = TT (translation table type) = 00 (legacy). */
    *(volatile uint64_t*)(u->reg + VTD_REG_RTADDR) = u->root_phys;
    /* Tell hardware to load the new RTADDR. */
    *(volatile uint32_t*)(u->reg + VTD_REG_GCMD) = GCMD_SRTP;
    for (int i = 0; i < 1000000; i++) {
        if (*(volatile uint32_t*)(u->reg + VTD_REG_GSTS) & GSTS_RTPS) break;
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

/* Public: enable DMA translation on every initialised remap unit.
 * Safe iff every PCI(e) function the system uses has a context entry
 * pointing at an identity-map SLPT. Until callers explicitly opt in,
 * leave translation off so untranslated DMA works normally. */
int vtd_enable_translation(void) {
    int enabled = 0;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        vtd_unit_t* u = &g_units[i];
        if (!u->online) continue;
        vtd_global_flush(u);
        *(volatile uint32_t*)(u->reg + VTD_REG_GCMD) = GCMD_TE;
        for (int k = 0; k < 1000000; k++) {
            if (*(volatile uint32_t*)(u->reg + VTD_REG_GSTS) & GSTS_TES) break;
        }
        enabled++;
        debug_printf("[VT-d] DRHD[%u] translation enabled (TES=1)\n", i);
    }
    return enabled;
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
    if (vtd_build_identity() < 0) {
        debug_printf("[VT-d] identity-map build failed\n");
        return -1;
    }
    for (uint8_t i = 0; i < d->drhd_count && g_unit_count < VTD_MAX_UNITS; i++) {
        vtd_unit_t* u = &g_units[g_unit_count];
        if (vtd_unit_init(u, d->drhd[i].register_base) == 0) {
            g_unit_count++;
            debug_printf("[VT-d] DRHD[%u] online: seg=%u base=0x%lx levels=%u\n",
                         i, d->drhd[i].segment,
                         (unsigned long)d->drhd[i].register_base,
                         u->sagaw_levels);
        }
    }
    g_domains[0].id          = 0;
    g_domains[0].slpt        = g_id_pml4;
    g_domains[0].slpt_phys   = g_id_pml4_phys;
    g_domains[0].is_identity = true;
    g_domain_count           = 1;
    debug_printf("[VT-d] %u remap unit(s) ready; translation NOT enabled "
                 "(call vtd_enable_translation())\n", g_unit_count);
    return (g_unit_count > 0) ? 0 : -2;
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
    (void)seg;
    if (!d) return -1;
    uint64_t slpt = d->slpt_phys ? d->slpt_phys : g_id_pml4_phys;
    uint32_t did  = d->id;
    int rc = -1;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!g_units[i].online) continue;
        if (devfn == 0xFFu) {
            /* Wildcard: identity-map every devfn on this bus. */
            if (vtd_program_identity_bus(&g_units[i], bus) == 0) rc = 0;
        } else if (vtd_program_context(&g_units[i], bus, devfn, slpt, did) == 0) {
            rc = 0;
        }
        if (g_units[i].qi_active) {
            /* QI device-level invalidation: source-id = (bus<<8)|devfn. */
            uint64_t sid = ((uint64_t)bus << 8) | devfn;
            vtd_qi_submit(&g_units[i],
                (1ULL << 4) | (1ULL << 0) | (sid << 32) | (1ULL << 1), 0);
        }
    }
    return rc;
}
static int vtd_device_detach(iommu_domain_t* d, uint16_t seg,
                              uint8_t bus, uint8_t devfn) {
    (void)d; (void)seg;
    for (uint8_t i = 0; i < g_unit_count; i++) {
        if (!g_units[i].online) continue;
        vtd_program_context(&g_units[i], bus, devfn, g_id_pml4_phys, 0);
    }
    return 0;
}

/* Walk into a domain's SLPT to install (iova → phys) at 4 KB. */
static int domain_slpt_map(struct iommu_domain* d, uint64_t iova,
                             uint64_t phys, uint64_t size, uint32_t perm) {
    if (!d || !d->slpt) return -1;
    uint64_t end = iova + size;
    for (uint64_t a = iova & ~0xFFFULL; a < end; a += 4096) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
        uint64_t* l3 = slpte_walk(d->slpt, d->slpt_phys, i4, true);
        if (!l3) return -2;
        uint64_t* l2 = slpte_walk(l3, 0, i3, true); if (!l2) return -2;
        uint64_t* l1 = slpte_walk(l2, 0, i2, true); if (!l1) return -2;
        uint64_t pte = (phys + (a - (iova & ~0xFFFULL))) & ~0xFFFULL;
        if (perm & IOMMU_PERM_READ)  pte |= SLPTE_R;
        if (perm & IOMMU_PERM_WRITE) pte |= SLPTE_W;
        if (!(perm & (IOMMU_PERM_READ | IOMMU_PERM_WRITE)))
            pte |= SLPTE_R | SLPTE_W;
        l1[i1] = pte;
    }
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
            uint64_t* l3 = slpte_walk(d->slpt, d->slpt_phys, i4, false);
            if (!l3) continue;
            uint64_t* l2 = slpte_walk(l3, 0, i3, false); if (!l2) continue;
            uint64_t* l1 = slpte_walk(l2, 0, i2, false); if (!l1) continue;
            l1[i1] = 0;
        }
        return 0;
    }
    (void)d; (void)iova; (void)size;
    return 0;
}
static void vtd_invalidate(iommu_domain_t* d) {
    (void)d;
    for (uint8_t i = 0; i < g_unit_count; i++)
        if (g_units[i].online) vtd_global_flush(&g_units[i]);
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
};
