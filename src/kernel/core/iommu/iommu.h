#ifndef IOMMU_H
#define IOMMU_H

#include "ktypes.h"

/*
 * IOMMU subsystem — abstraction over Intel VT-d (DMAR-described) and
 * AMD-Vi (IVRS-described) hardware.
 *
 * This header defines the operations every IOMMU backend must
 * implement. The kernel calls into a `iommu_ops_t` chosen at boot
 * based on what ACPI exposed (DMAR vs IVRS). Translation is built on
 * top of a per-segment domain table; each PCI(e) function lives in
 * exactly one domain at any time.
 *
 * Current status: skeleton + VT-d register-level helpers + identity-map
 * scaffolding. Translation is NOT enabled until the follow-up audit
 * implements:
 *   - second-level page table builder (4 KB / 2 MB / 1 GB)
 *   - root + context entry programming
 *   - invalidation queue + completion polling
 *   - interrupt remap table
 *
 * The skeleton lets driver code call `iommu_map(domain, iova, phys,
 * size, perm)` against an abstract handle without caring which vendor
 * sits behind it; the real backends fill in the table walks later.
 */

#define IOMMU_PERM_READ    (1u << 0)
#define IOMMU_PERM_WRITE   (1u << 1)
#define IOMMU_PERM_EXECUTE (1u << 2)

typedef struct iommu_domain iommu_domain_t;
typedef struct iommu_dev    iommu_dev_t;

typedef struct iommu_ops {
    const char* name;
    int  (*init)(void);
    iommu_domain_t* (*domain_alloc)(void);
    void (*domain_free)(iommu_domain_t*);
    int  (*device_attach)(iommu_domain_t*, uint16_t segment,
                          uint8_t bus, uint8_t devfn);
    int  (*device_detach)(iommu_domain_t*, uint16_t segment,
                          uint8_t bus, uint8_t devfn);
    int  (*map)(iommu_domain_t*, uint64_t iova, uint64_t phys,
                uint64_t size, uint32_t perm);
    int  (*unmap)(iommu_domain_t*, uint64_t iova, uint64_t size);
    void (*invalidate)(iommu_domain_t*);
} iommu_ops_t;

/* Probe ACPI; pick backend; call ops->init. Returns 0 if any IOMMU
 * came online, -1 if none detected, -2 if init failed. */
int  iommu_init(void);
bool iommu_present(void);

const iommu_ops_t* iommu_get_ops(void);

/* Convenience wrappers — forward to ops if backend present, else
 * succeed silently so callers can be IOMMU-agnostic. */
iommu_domain_t* iommu_domain_alloc(void);
void            iommu_domain_free(iommu_domain_t*);
int             iommu_device_attach(iommu_domain_t*, uint16_t seg,
                                     uint8_t bus, uint8_t devfn);
int             iommu_map(iommu_domain_t*, uint64_t iova, uint64_t phys,
                          uint64_t size, uint32_t perm);
int             iommu_unmap(iommu_domain_t*, uint64_t iova, uint64_t size);

#endif /* IOMMU_H */
