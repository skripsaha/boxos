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
    /* Phase 2G — opaque domain → integer ID accessor. Each backend's
     * struct iommu_domain hides the layout but exposes a stable uint32_t
     * id. Used by the iommu_map wrapper to derive `iommu:domain:N` tag
     * strings without leaking backend struct layout to MemTag. */
    uint32_t (*domain_id)(iommu_domain_t*);
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

/* Phase 2G — opaque domain-ID accessor. Returns 0xFFFFFFFF for NULL
 * domain or when no backend is active. */
uint32_t        iommu_domain_id(iommu_domain_t*);

/* Phase 2G — boot-time audit. Logs the active backend name, # of
 * remap units, # of domains carved, and identity-map state. Idempotent
 * and free of side effects beyond debug_printf. Called from main.c
 * right after iommu_init so the boot log carries the current IOMMU
 * landscape next to the existing iommu:ready Touch event. */
void            iommu_audit_dump(void);

/* Phase 2G — DMA buffer auto-allocation wrapper.
 *
 * Combines pmm_alloc(pages, PHYS_TAG_DMA32) + iommu_map(domain, iova,
 * phys, size, perm) into a single call. Returns the phys address of the
 * allocated buffer (also the IOVA, since identity-map domain is used by
 * default). NULL on failure.
 *
 * When IOMMU is dormant (no DMAR/IVRS or g_ops==NULL), falls back to
 * plain pmm_alloc — driver code stays IOMMU-agnostic. Perm = read|write
 * by default; pass IOMMU_PERM_* bits to constrain. */
void *iommu_dma_alloc(iommu_domain_t *domain, size_t pages, uint32_t perm);
void  iommu_dma_free (iommu_domain_t *domain, void *phys, size_t pages);

#endif /* IOMMU_H */
