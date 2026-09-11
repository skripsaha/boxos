#include "iommu.h"
#include "acpi.h"
#include "klib.h"
#include "touch.h"
#include "memtag.h"
#include "pmm.h"
#include "tme.h"


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

void *iommu_dma_alloc(iommu_domain_t *domain, size_t pages, uint32_t perm) {
    if (!pages) return NULL;
    if (perm == 0) perm = IOMMU_PERM_READ | IOMMU_PERM_WRITE;
    void *phys = pmm_alloc(pages, PHYS_TAG_DMA32);
    if (!phys) return NULL;
    (void)MemTagApplyByPhys((uintptr_t)phys, pages, "purpose:dma");
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

int iommu_map_with_keyid(iommu_domain_t *domain, uint64_t iova,
                          uint64_t phys, uint64_t size, uint16_t keyid,
                          uint32_t perm) {
    if (!g_ops) return 0;
    if (keyid == 0) {
        return iommu_map(domain, iova, phys, size, perm);
    }
    if (!g_tme.mk_active) {
        return -1;
    }
    uint64_t phys_with_keyid = tme_phys_with_keyid(phys, keyid);

    if (g_ops->map_with_keyid) {
        int r = g_ops->map_with_keyid(domain, iova, phys_with_keyid,
                                       size, perm);
        if (r == 0 && size > 0) {
            char tag[40];
            uint32_t did = iommu_domain_id(domain);
            ksnprintf(tag, sizeof(tag), "iommu:domain:%u:keyid:%u",
                      did, (unsigned)keyid);
            uintptr_t phys_aligned = phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1);
            size_t pgs = (size + (phys - phys_aligned) + PMM_PAGE_SIZE - 1)
                         / PMM_PAGE_SIZE;
            (void)MemTagApplyByPhys(phys_aligned, pgs, tag);
        }
        return r;
    }

    return -1;
}

void *iommu_dma_alloc_with_keyid(iommu_domain_t *domain, size_t pages,
                                  uint16_t keyid, uint32_t perm) {
    if (!pages || keyid == 0) return NULL;
    if (!g_tme.mk_active) return NULL;
    if (perm == 0) perm = IOMMU_PERM_READ | IOMMU_PERM_WRITE;

    void *phys = pmm_alloc_with_keyid(pages, keyid);
    if (!phys) return NULL;
    (void)MemTagApplyByPhys((uintptr_t)phys, pages, "purpose:dma:encrypted");

    error_t zrc = tme_zero_pages_with_keyid((uintptr_t)phys, pages,
                                             keyid, false);
    if (zrc != OK) {
        pmm_free_with_keyid(phys, pages, keyid);
        return NULL;
    }

    if (g_ops && g_ops->map && domain) {
        int r = iommu_map_with_keyid(domain,
                                      (uint64_t)(uintptr_t)phys,
                                      (uint64_t)(uintptr_t)phys,
                                      (uint64_t)pages * PMM_PAGE_SIZE,
                                      keyid, perm);
        if (r != 0) {
            pmm_free_with_keyid(phys, pages, keyid);
            return NULL;
        }
    }
    return phys;
}

void iommu_dma_free_with_keyid(iommu_domain_t *domain, void *phys,
                                size_t pages, uint16_t keyid) {
    if (!phys || !pages) return;

    if (g_ops && g_ops->unmap && domain) {
        (void)iommu_unmap(domain, (uint64_t)(uintptr_t)phys,
                          (uint64_t)pages * PMM_PAGE_SIZE);
    }
    pmm_free_with_keyid(phys, pages, keyid);
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