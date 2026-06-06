#include "iommu.h"
#include "acpi.h"
#include "klib.h"
#include "touch.h"
#include "memtag.h"  /* Phase 2G — auto-tag DMA-mapped phys with iommu:domain:N */
#include "pmm.h"     /* PMM_PAGE_SIZE for page-count derivation */

/*
 * IOMMU subsystem entry point. Picks a backend at boot:
 *   - VT-d  when ACPI DMAR is present (Intel)
 *   - AMD-Vi when ACPI IVRS is present (AMD)
 *
 * Backends register themselves through `iommu_set_ops()`. At present
 * both backends are skeletons (no translation enabled); the chosen
 * ops vtable lets future PCIe drivers compile against a stable API
 * while we flesh the backends out.
 */

static const iommu_ops_t* g_ops = NULL;

const iommu_ops_t* iommu_get_ops(void) { return g_ops; }
bool iommu_present(void)               { return g_ops != NULL; }

extern const iommu_ops_t vtd_ops;
extern const iommu_ops_t amdvi_ops;

int iommu_init(void) {
    if (acpi_get_dmar()) {
        g_ops = &vtd_ops;
        debug_printf("[IOMMU] backend = VT-d (DMAR present)\n");
    } else if (acpi_get_ivrs()) {
        g_ops = &amdvi_ops;
        debug_printf("[IOMMU] backend = AMD-Vi (IVRS present)\n");
    } else {
        debug_printf("[IOMMU] no DMAR/IVRS — running without IOMMU\n");
        return -1;
    }
    if (g_ops->init && g_ops->init() < 0) {
        debug_printf("[IOMMU] backend init failed — disabling\n");
        g_ops = NULL;
        return -2;
    }
    /* Tell userspace which IOMMU backend is online so DMA-buffer
     * services (`iommu:ready` subscriber) know whether they need to
     * route allocations through a domain or can DMA directly. */
    TouchPublish("iommu:ready", g_ops->name, 0);
    return 0;
}

iommu_domain_t* iommu_domain_alloc(void) {
    return (g_ops && g_ops->domain_alloc) ? g_ops->domain_alloc() : NULL;
}
void iommu_domain_free(iommu_domain_t* d) {
    if (g_ops && g_ops->domain_free) g_ops->domain_free(d);
}
uint32_t iommu_domain_id(iommu_domain_t* d) {
    if (!g_ops || !g_ops->domain_id) return 0xFFFFFFFFu;
    return g_ops->domain_id(d);
}

/* Phase 2G — Touch event payload for DMA / device-attach lifecycle.
 * Fits the 64 B Pocket envelope: subscribers receive precise BDF +
 * domain context without a follow-up syscall. */
typedef struct {
    uint16_t segment;
    uint8_t  bus;
    uint8_t  devfn;
    uint32_t domain_id;
    uint64_t iova;
    uint64_t phys;
    uint64_t size;
    uint32_t perm;
    uint32_t reserved;
} iommu_event_payload_t;

int iommu_device_attach(iommu_domain_t* d, uint16_t seg,
                         uint8_t bus, uint8_t devfn) {
    if (!g_ops || !g_ops->device_attach) return 0;
    int r = g_ops->device_attach(d, seg, bus, devfn);
    if (r == 0) {
        /* Publish only on success so subscribers don't see ghost attaches
         * from failed backend bring-up paths. */
        iommu_event_payload_t ev = {
            .segment = seg, .bus = bus, .devfn = devfn,
            .domain_id = iommu_domain_id(d),
            .iova = 0, .phys = 0, .size = 0,
            .perm = 0, .reserved = 0,
        };
        TouchPublish("iommu:domain:attached", &ev, sizeof(ev));
    }
    return r;
}

int iommu_map(iommu_domain_t* d, uint64_t iova, uint64_t phys,
               uint64_t size, uint32_t perm) {
    if (!g_ops || !g_ops->map) return 0;
    int r = g_ops->map(d, iova, phys, size, perm);
    if (r == 0 && size > 0) {
        /* Tag every PMM page covered by the mapping with the domain
         * label. Userspace observers can now `MemTagAnd("iommu:domain:5")`
         * to enumerate every buffer routed through that IOMMU domain.
         *
         * The phys is page-aligned by IOMMU mapping requirements (Intel
         * VT-d §3.5: minimum page granularity = 4 KiB). size rounded up
         * to the page is the conservative tag span. */
        char tag[32];
        uint32_t did = iommu_domain_id(d);
        ksnprintf(tag, sizeof(tag), "iommu:domain:%u", did);
        uintptr_t phys_aligned = phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1);
        size_t pages = (size + (phys - phys_aligned) + PMM_PAGE_SIZE - 1)
                       / PMM_PAGE_SIZE;
        (void)MemTagApplyByPhys(phys_aligned, pages, tag);

        iommu_event_payload_t ev = {
            .segment = 0, .bus = 0, .devfn = 0,
            .domain_id = did,
            .iova = iova, .phys = phys, .size = size,
            .perm = perm, .reserved = 0,
        };
        TouchPublish("iommu:dma:mapped", &ev, sizeof(ev));
    }
    return r;
}

int iommu_unmap(iommu_domain_t* d, uint64_t iova, uint64_t size) {
    if (!g_ops || !g_ops->unmap) return 0;
    /* We don't know the phys here (unmap takes iova only); skip the
     * tag clear — when the underlying region is freed via pmm_free,
     * MemTagPmmFreed reaps the region entry. Touch publish still fires
     * so subscribers can refresh their view. */
    int r = g_ops->unmap(d, iova, size);
    if (r == 0) {
        iommu_event_payload_t ev = {
            .segment = 0, .bus = 0, .devfn = 0,
            .domain_id = iommu_domain_id(d),
            .iova = iova, .phys = 0, .size = size,
            .perm = 0, .reserved = 0,
        };
        TouchPublish("iommu:dma:unmapped", &ev, sizeof(ev));
    }
    return r;
}

/* Phase 2G — boot-time audit. The IOMMU backends (vtd.c / amdvi.c) log
 * their own per-DRHD/IVHD bring-up; this wrapper adds a one-line
 * summary at the iommu_init call site so the boot log records the
 * MemTag/Touch surface state next to the backend log. */
void *iommu_dma_alloc(iommu_domain_t *domain, size_t pages, uint32_t perm) {
    if (!pages) return NULL;
    /* Default to read|write when caller passes 0. */
    if (perm == 0) perm = IOMMU_PERM_READ | IOMMU_PERM_WRITE;
    /* DMA32 zone — most real DMA controllers can address up to 4 GiB
     * by default; drivers that handle 64-bit DMA can extend later. */
    void *phys = pmm_alloc(pages, PHYS_TAG_DMA32);
    if (!phys) return NULL;
    /* Tag the underlying region so userspace sees the lifecycle. */
    (void)MemTagApplyByPhys((uintptr_t)phys, pages, "purpose:dma");
    /* IOMMU map identity-style: iova == phys. When IOMMU is dormant
     * (no backend), iommu_map is a no-op that returns 0; the buffer is
     * still usable for legacy in-kernel DMA. */
    if (g_ops && g_ops->map && domain) {
        int r = iommu_map(domain, (uint64_t)(uintptr_t)phys,
                          (uint64_t)(uintptr_t)phys,
                          (uint64_t)pages * PMM_PAGE_SIZE, perm);
        if (r != 0) {
            pmm_free(phys, pages);
            return NULL;
        }
    }
    return phys;
}

void iommu_dma_free(iommu_domain_t *domain, void *phys, size_t pages) {
    if (!phys || !pages) return;
    if (g_ops && g_ops->unmap && domain) {
        (void)iommu_unmap(domain, (uint64_t)(uintptr_t)phys,
                          (uint64_t)pages * PMM_PAGE_SIZE);
    }
    pmm_free(phys, pages);
}

void iommu_audit_dump(void) {
    if (!g_ops) {
        debug_printf("[IOMMU] audit: no backend active (DMA bypasses IOMMU)\n");
        return;
    }
    debug_printf("[IOMMU] audit: backend=%s domain_id getter=%s "
                 "TouchPublish surface active (iommu:domain:N / "
                 "iommu:dma:mapped / iommu:dma:unmapped / "
                 "iommu:domain:attached)\n",
                 g_ops->name,
                 g_ops->domain_id ? "yes" : "no");
}
