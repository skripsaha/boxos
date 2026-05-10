#include "iommu.h"
#include "acpi.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "touch.h"

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
 *   * Program IOMMU base registers (DEV_TAB_BAR, CMD_BUF_BAR, EVT_LOG_BAR)
 *   * Issue COMPLETION_WAIT to drain command buffer
 *   * `amdvi_enable()` flips IOMMU_CTRL.IommuEn; init() leaves it off
 *     so untranslated DMA continues to work without OS intervention
 *
 * The DTE format used is "V=1, host-page-table set to identity-map"
 * (DTE bits 1:0 = 11, DomainID=0, host page table pointer = SLPT root,
 * mode=4 for 4-level paging). This is the same model used by Linux's
 * "iommu=pt" mode.
 * ===================================================================== */

/* IOMMU MMIO registers (AMD-Vi §3.1.6). */
#define MMIO_DEV_TAB_BAR     0x0000   /* 64-bit */
#define MMIO_CMD_BUF_BAR     0x0008
#define MMIO_EVT_LOG_BAR     0x0010
#define MMIO_CTRL            0x0018
#define MMIO_EXCL_BASE       0x0020
#define MMIO_EXCL_LIMIT      0x0028
#define MMIO_EXT_FEAT        0x0030
#define MMIO_CMD_HEAD        0x2000
#define MMIO_CMD_TAIL        0x2008
#define MMIO_EVT_HEAD        0x2010
#define MMIO_EVT_TAIL        0x2018
#define MMIO_STATUS          0x2020

/* CTRL bits. */
#define CTRL_IOMMU_EN        (1ULL << 0)
#define CTRL_HT_TUN_EN       (1ULL << 1)
#define CTRL_EVT_LOG_EN      (1ULL << 2)
#define CTRL_CMD_BUF_EN      (1ULL << 12)

/* Command opcodes. */
#define CMD_OP_COMPLETION_WAIT      0x01
#define CMD_OP_INVALIDATE_DEV_TAB   0x02
#define CMD_OP_INVALIDATE_IOMMU_PG  0x03
#define CMD_OP_INVALIDATE_IOTLB_PG  0x04
#define CMD_OP_INVALIDATE_ALL       0x08

/* DEV_TAB_BAR low bits encode size class:
 *   0x0 = 4 KB (256 entries, single bus), 0xF = 1 MB (64 K entries).
 * We choose 0xF (full segment) for production. */
#define DEV_TAB_SIZE_CLASS  0xF
#define DEV_TAB_BYTES       (256ULL * 1024 * 8)   /* 2 MiB total */

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
    uint16_t  pci_segment;
    bool      online;
} amdvi_unit_t;

#define AMDVI_MAX_UNITS  ACPI_IVRS_MAX_IVHD
static amdvi_unit_t g_units[AMDVI_MAX_UNITS];
static uint8_t      g_unit_count = 0;
static uint64_t*    g_id_root      = NULL;
static uint64_t     g_id_root_phys = 0;

static void* alloc_aligned_4k(uint64_t* phys_out, size_t size_bytes) {
    /* Round up to 4 KB. */
    size_t pages = (size_bytes + 4095) / 4096;
    void* p = pmm_alloc(pages);
    if (!p) { *phys_out = 0; return NULL; }
    *phys_out = (uint64_t)(uintptr_t)p;
    memset(p, 0, pages * 4096);
    return p;
}

/* AMD-Vi long-mode page-table entries share the bit layout with x86_64
 * paging. Reuse the canonical bits. */
#define AMD_PTE_P  (1ULL << 0)
#define AMD_PTE_W  (1ULL << 1)
#define AMD_PTE_U  (1ULL << 2)

static uint64_t* amd_pt_walk(uint64_t* parent, unsigned idx, bool create) {
    uint64_t e = parent[idx];
    if (!(e & AMD_PTE_P)) {
        if (!create) return NULL;
        uint64_t child_phys;
        void* child = alloc_aligned_4k(&child_phys, 4096);
        if (!child) return NULL;
        parent[idx] = child_phys | AMD_PTE_P | AMD_PTE_W | AMD_PTE_U;
        return (uint64_t*)child;
    }
    return (uint64_t*)(uintptr_t)(e & ~0xFFFULL);
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
        uint64_t* l3 = amd_pt_walk(g_id_root, i4, true); if (!l3) return -1;
        uint64_t* l2 = amd_pt_walk(l3, i3, true);         if (!l2) return -1;
        uint64_t* l1 = amd_pt_walk(l2, i2, true);         if (!l1) return -1;
        l1[i1] = a | AMD_PTE_P | AMD_PTE_W | AMD_PTE_U;
    }
    return 0;
}

/* Encode a DTE: V=1, TV=1, host page table = caller-supplied,
 * mode = 4 (4-level), caller-supplied DomainID. AMD-Vi §2.2.2.1. */
static void amdvi_dte_program(uint8_t* dte_base, uint16_t devid,
                                uint64_t host_pt_phys, uint16_t domain_id) {
    uint64_t* dte = (uint64_t*)(dte_base + (size_t)devid * 32);
    dte[0] = (host_pt_phys & ~0xFFFULL) | (4ULL << 9) | (1ULL << 1) | (1ULL << 0);
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

static void amdvi_completion_wait(amdvi_unit_t* u) {
    /* Op[63:60]=0x1, Store completion bit 0. Use a poll loop on a flag
     * inside the command buffer itself — bit 0 of qword 1 toggles when
     * the IOMMU finishes the wait. */
    volatile uint64_t* wait_slot =
        (volatile uint64_t*)((uint8_t*)u->cmd_buf + u->cmd_tail + 8);
    *wait_slot = 0;
    amdvi_cmd_push(u, ((uint64_t)CMD_OP_COMPLETION_WAIT << 60) | (1ULL << 1),
                       (uint64_t)(uintptr_t)wait_slot | 1);
    for (int i = 0; i < 1000000; i++) {
        if (*wait_slot & 1) return;
    }
    debug_printf("[AMD-Vi] completion-wait timeout\n");
}

/* ----- Per-unit init ----- */

static int amdvi_unit_init(amdvi_unit_t* u, uint64_t reg_phys,
                            uint16_t seg) {
    u->reg_phys     = reg_phys;
    u->pci_segment  = seg;
    u->reg = (volatile uint8_t*)vmm_map_mmio((uintptr_t)reg_phys, 16384,
                  VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                  VMM_FLAG_CACHE_DISABLE);
    if (!u->reg) return -1;

    /* Allocate tables. */
    u->dev_tab = alloc_aligned_4k(&u->dev_tab_phys, (size_t)DEV_TAB_BYTES);
    if (!u->dev_tab) return -2;
    u->cmd_buf = alloc_aligned_4k(&u->cmd_buf_phys, CMD_BUF_BYTES);
    if (!u->cmd_buf) return -3;
    u->evt_log = alloc_aligned_4k(&u->evt_log_phys, EVT_LOG_BYTES);
    if (!u->evt_log) return -4;

    /* Pre-fill DTE with V=0 (not valid) — devices become valid on
     * device_attach(). For an initial pass-everything-through model
     * we could instead V=1 every entry now; we keep V=0 to avoid
     * surprising firmware before drivers are ready. */

    /* Program base registers. */
    *(volatile uint64_t*)(u->reg + MMIO_DEV_TAB_BAR) =
        u->dev_tab_phys | DEV_TAB_SIZE_CLASS;
    /* CMD_BUF_BAR: size class in low 4 bits (encoded log2(size) - 12). */
    /* 4 KB = log2(4096)-12 = 0. */
    *(volatile uint64_t*)(u->reg + MMIO_CMD_BUF_BAR) =
        u->cmd_buf_phys | (8ULL << 56);    /* size = 8 -> 2^8 entries * 16 = 4 KB */
    *(volatile uint64_t*)(u->reg + MMIO_EVT_LOG_BAR) =
        u->evt_log_phys | (8ULL << 56);

    /* Enable command buffer + event log; keep IOMMU_EN off. */
    uint64_t ctrl = *(volatile uint64_t*)(u->reg + MMIO_CTRL);
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
    /* Per-domain map: walk d->host_pt; reuse amd_pt_walk. */
    uint64_t end = iova + sz;
    for (uint64_t a = iova & ~0xFFFULL; a < end; a += 4096) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
        uint64_t* l3 = amd_pt_walk(d->host_pt, i4, true); if (!l3) return -1;
        uint64_t* l2 = amd_pt_walk(l3, i3, true);         if (!l2) return -1;
        uint64_t* l1 = amd_pt_walk(l2, i2, true);         if (!l1) return -1;
        uint64_t pte = (phys + (a - (iova & ~0xFFFULL))) & ~0xFFFULL;
        pte |= AMD_PTE_P | AMD_PTE_U;
        if (perm & IOMMU_PERM_WRITE) pte |= AMD_PTE_W;
        l1[i1] = pte;
    }
    return 0;
}
static int amdvi_unmap(iommu_domain_t* d, uint64_t iova, uint64_t size) {
    if (!d || d->is_identity) return 0;
    uint64_t end = iova + size;
    for (uint64_t a = iova & ~0xFFFULL; a < end; a += 4096) {
        unsigned i4 = (unsigned)((a >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((a >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((a >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((a >> 12) & 0x1FF);
        uint64_t* l3 = amd_pt_walk(d->host_pt, i4, false); if (!l3) continue;
        uint64_t* l2 = amd_pt_walk(l3, i3, false);         if (!l2) continue;
        uint64_t* l1 = amd_pt_walk(l2, i2, false);         if (!l1) continue;
        l1[i1] = 0;
    }
    return 0;
}

/* Drain the event log on every IOMMU. AMD-Vi §3.1.6 event entries are
 * 16 bytes; type code lives in bits 31:28 of qword 0. We log every
 * pending event then advance the head to the tail. Callers run this
 * from the SCI handler or as a periodic poll. */
void amdvi_poll_events(void) {
    for (uint8_t i = 0; i < g_unit_count; i++) {
        amdvi_unit_t* u = &g_units[i];
        if (!u->online) continue;
        uint64_t head = *(volatile uint64_t*)(u->reg + MMIO_EVT_HEAD);
        uint64_t tail = *(volatile uint64_t*)(u->reg + MMIO_EVT_TAIL);
        while (head != tail) {
            uint8_t* slot = (uint8_t*)u->evt_log + (head & (EVT_LOG_BYTES - 1));
            uint32_t code = ((*(uint32_t*)slot) >> 28) & 0xF;
            debug_printf("[AMD-Vi] IVHD[%u] event code=0x%x\n", i, code);
            /* Per-event broadcast — userspace logger / fault recoverer
             * subscribes by `iommu:fault` and decodes the 16-byte slot
             * we shipped as payload. */
            TouchPublish("iommu:fault", slot, 16);
            head = (head + 16) & (EVT_LOG_BYTES - 1);
        }
        *(volatile uint64_t*)(u->reg + MMIO_EVT_HEAD) = tail;
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
};
