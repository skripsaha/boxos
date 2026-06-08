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
    /* TME-MK aware map. phys carries KeyID in upper bits (see
     * tme_phys_with_keyid). When the backend doesn't implement this
     * (NULL slot), iommu_map_with_keyid falls back to ->map after
     * stripping the KeyID — DMA buffers would see ciphertext, so the
     * generic wrapper rejects KeyID != 0 in that fallback case. */
    int  (*map_with_keyid)(iommu_domain_t*, uint64_t iova,
                           uint64_t phys_with_keyid, uint64_t size,
                           uint32_t perm);
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

/* TME-MK aware DMA alloc — couples per-region encryption with device
 * DMA. The IOMMU SL-PTE stores `phys | (keyid << reduced_MAXPHYADDR)`
 * so that both the CPU's memory-encryption engine (via the user PTE
 * with the same KeyID) AND the device's IOMMU-translated DMA reads
 * decrypt with the same key — coherent plaintext on both sides.
 *
 * Per Intel VT-d Spec rev 3.4 §9.4.3 ("Second-Level Page-Table
 * Entries with Memory Encryption"): the SL-PTE phys-address field
 * holds bits [reduced_MAXPHYADDR-1 : 12]; upper bits up to raw
 * MAXPHYADDR-1 carry the KeyID, same layout as CPU PTE. AMD-Vi rev 4
 * uses the same encoding under "Cache Coherent Memory" mode.
 *
 *   keyid : reservation from tme_keyid_alloc(); MUST be > 0
 *           (KeyID 0 = platform default; iommu_dma_alloc covers it
 *            with the non-KeyID API for symmetry). 0 → NULL.
 *   perm  : IOMMU_PERM_READ|WRITE etc.
 *
 * Returns the RAW physical address (matches iommu_dma_alloc's
 * convention). Caller uses it as:
 *   - IOVA written into device DMA descriptors. The IOMMU
 *     translates IOVA→phys_with_keyid via the SL-PTE; the device
 *     transparently observes plaintext that matches the CPU's view.
 *   - input to tme_phys_with_keyid() when composing a CPU PTE via
 *     vmm_map_page_with_keyid() for kernel-side access.
 *
 * Pages are zero-filled through a KeyID-bearing kernel temp slot so
 * caller / device observes true zeros on first read (instead of
 * ciphertext_K0(zeros) decrypted-with-Kuser = garbage).
 *
 * Returns NULL when IOMMU/TME-MK is unavailable or any sub-step
 * fails. The KeyID itself is the caller's lifecycle responsibility
 * (Bay code frees via tme_keyid_free on last release). */
void *iommu_dma_alloc_with_keyid(iommu_domain_t *domain, size_t pages,
                                  uint16_t keyid, uint32_t perm);
void  iommu_dma_free_with_keyid (iommu_domain_t *domain, void *phys_with_keyid,
                                  size_t pages, uint16_t keyid);

/* Direct mapping form: caller already holds phys (raw) + keyid and
 * wants IOMMU translation set up. Builds SL-PTE with KeyID. Falls
 * through to ops->map when keyid==0. */
int   iommu_map_with_keyid(iommu_domain_t *domain, uint64_t iova,
                            uint64_t phys, uint64_t size, uint16_t keyid,
                            uint32_t perm);

#endif /* IOMMU_H */
