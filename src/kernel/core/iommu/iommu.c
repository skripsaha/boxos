#include "iommu.h"
#include "acpi.h"
#include "klib.h"
#include "touch.h"

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
int iommu_device_attach(iommu_domain_t* d, uint16_t seg,
                         uint8_t bus, uint8_t devfn) {
    if (!g_ops || !g_ops->device_attach) return 0;
    return g_ops->device_attach(d, seg, bus, devfn);
}
int iommu_map(iommu_domain_t* d, uint64_t iova, uint64_t phys,
               uint64_t size, uint32_t perm) {
    if (!g_ops || !g_ops->map) return 0;
    return g_ops->map(d, iova, phys, size, perm);
}
int iommu_unmap(iommu_domain_t* d, uint64_t iova, uint64_t size) {
    if (!g_ops || !g_ops->unmap) return 0;
    return g_ops->unmap(d, iova, size);
}
