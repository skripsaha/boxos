#ifndef IOMMU_H
#define IOMMU_H

#include "ktypes.h"


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
    int  (*map_with_keyid)(iommu_domain_t*, uint64_t iova,
                           uint64_t phys_with_keyid, uint64_t size,
                           uint32_t perm);
    uint32_t (*domain_id)(iommu_domain_t*);
} iommu_ops_t;

int  iommu_init(void);
bool iommu_present(void);

const iommu_ops_t* iommu_get_ops(void);

iommu_domain_t* iommu_domain_alloc(void);
void            iommu_domain_free(iommu_domain_t*);
int             iommu_device_attach(iommu_domain_t*, uint16_t seg,
                                     uint8_t bus, uint8_t devfn);
int             iommu_map(iommu_domain_t*, uint64_t iova, uint64_t phys,
                          uint64_t size, uint32_t perm);
int             iommu_unmap(iommu_domain_t*, uint64_t iova, uint64_t size);

uint32_t        iommu_domain_id(iommu_domain_t*);

void            iommu_audit_dump(void);

void *iommu_dma_alloc(iommu_domain_t *domain, size_t pages, uint32_t perm);
void  iommu_dma_free (iommu_domain_t *domain, void *phys, size_t pages);

void *iommu_dma_alloc_with_keyid(iommu_domain_t *domain, size_t pages,
                                  uint16_t keyid, uint32_t perm);
void  iommu_dma_free_with_keyid (iommu_domain_t *domain, void *phys_with_keyid,
                                  size_t pages, uint16_t keyid);

int   iommu_map_with_keyid(iommu_domain_t *domain, uint64_t iova,
                            uint64_t phys, uint64_t size, uint16_t keyid,
                            uint32_t perm);

#endif