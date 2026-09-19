#include "iommu.h"
#include "acpi.h"
#include "klib.h"
#include "touch.h"
#include "memtag.h"  /* Phase 2G — auto-tag DMA-mapped phys with iommu:domain:N */
#include "pmm.h"     /* PMM_PAGE_SIZE for page-count derivation */
#include "tme.h"     /* TME-MK aware DMA — KeyID encoding */

/*
 * IOMMU subsystem entry point. Picks a backend at boot:
 *   - VT-d  when ACPI DMAR is present (Intel)
 *   - AMD-Vi when ACPI IVRS is present (AMD)
 *
 * The backend is chosen HERE, out of the two vtables declared below.
 * There is no `iommu_set_ops()` in this kernel and there never was, so a
 * backend does not register itself and a third one cannot arrive without
 * a line in iommu_init.
 *
 * Neither backend is a skeleton either -- iommu.h says what each one
 * builds. What iommu_init still does not do is turn translation on: the
 * units come up with it off, and it stays off until something calls the
 * backend's own enable, which nothing in this tree does.
 */

static const iommu_ops_t* g_ops = NULL;

/* What iommu_init ended on. `g_ops == NULL` is two different machines: one
 * where ACPI described no remap hardware at all, and one where it described
 * some, the backend touched it, and not a single unit came up. The second is
 * the worse machine of the two — bringing a unit up is what takes firmware's
 * pre-boot DMA protection off, and a failed bring-up takes it off and leaves
 * nothing in its place — so the boot line must not report it as the first.
 * The pointer alone cannot tell them apart, so the outcome is kept. */
typedef enum {
    IommuOutcomeNotRun = 0,      /* iommu_init has not been called */
    IommuOutcomeNoTables,        /* no DMAR and no IVRS in ACPI */
    IommuOutcomeBackendFailed,   /* described, and no unit came online */
    IommuOutcomeUp               /* backend chosen and initialised */
} IommuOutcome;

static IommuOutcome g_outcome  = IommuOutcomeNotRun;
/* The backend ACPI named, kept whether or not it then came up, so the failed
 * case can say WHICH hardware was described and left unarmed. */
static const char*  g_described = NULL;

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
        g_outcome = IommuOutcomeNoTables;
        return -1;
    }
    g_described = g_ops->name;
    if (g_ops->init && g_ops->init() < 0) {
        debug_printf("[IOMMU] backend init failed — disabling\n");
        g_ops     = NULL;
        g_outcome = IommuOutcomeBackendFailed;
        return -2;
    }
    g_outcome = IommuOutcomeUp;
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

/* ─── TME-MK aware DMA path (Intel VT-d Spec rev 3.4 §9.4.3 /
 * AMD-Vi rev 4 "Cache Coherent Memory" mode) ─────────────────────
 *
 * The SL-PTE (second-level page-table entry) in both vendors holds
 * the same phys-address layout as a CPU PTE: bits [51:12] are the
 * address field; under TME-MK the upper num_keyid_bits portion
 * encodes the KeyID. When the IOMMU walks the SL-PTE for a DMA
 * access, it submits the KeyID-bearing phys to the memory
 * encryption engine — the device transparently sees plaintext that
 * matches what the CPU sees through its KeyID-bearing CPU PTE.
 *
 * The map_with_keyid op (per-backend) is responsible for writing
 * the full phys_with_keyid into the SL-PTE without stripping. When
 * a backend doesn't yet implement it (NULL slot), the wrapper
 * rejects KeyID != 0 to avoid silently routing DMA through a
 * KeyID-0 mapping that would read ciphertext.
 */
int iommu_map_with_keyid(iommu_domain_t *domain, uint64_t iova,
                          uint64_t phys, uint64_t size, uint16_t keyid,
                          uint32_t perm) {
    if (!g_ops) return 0;       /* dormant — driver-level DMA still works */
    if (keyid == 0) {
        /* KeyID 0 = platform default; the regular map path handles it. */
        return iommu_map(domain, iova, phys, size, perm);
    }
    if (!g_tme.mk_active) {
        /* Caller asked for KeyID-aware map but TME-MK isn't active.
         * Reject — silently dropping the KeyID would route DMA
         * through plaintext, breaking the caller's safety contract. */
        return -1;
    }
    uint64_t phys_with_keyid = tme_phys_with_keyid(phys, keyid);

    if (g_ops->map_with_keyid) {
        int r = g_ops->map_with_keyid(domain, iova, phys_with_keyid,
                                       size, perm);
        if (r == 0 && size > 0) {
            /* Same Touch + MemTag publish as the plain map path.
             * The tag includes the KeyID so userspace observers can
             * filter encrypted DMA buffers. */
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

    /* Backend lacks KeyID-aware map. Refuse — silently calling ->map
     * with the KeyID-stripped phys would expose ciphertext to the
     * device. */
    return -1;
}

void *iommu_dma_alloc_with_keyid(iommu_domain_t *domain, size_t pages,
                                  uint16_t keyid, uint32_t perm) {
    if (!pages || keyid == 0) return NULL;
    if (!g_tme.mk_active) return NULL;
    if (perm == 0) perm = IOMMU_PERM_READ | IOMMU_PERM_WRITE;

    /* DMA32 zone — same constraint as iommu_dma_alloc. */
    void *phys = pmm_alloc_with_keyid(pages, keyid);
    if (!phys) return NULL;
    (void)MemTagApplyByPhys((uintptr_t)phys, pages, "purpose:dma:encrypted");

    /* Zero-fill via the KeyID-bearing kernel temp slot so device DMA
     * reads see true zeros via K (not ciphertext_K0(zeros) decrypted
     * with K which gives garbage). Cold-path cost: bounded by `pages`. */
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
    /* Return the RAW phys — matches iommu_dma_alloc's convention so
     * callers can:
     *   - Use the returned address directly as IOVA in device DMA
     *     descriptors (IOMMU translates raw→phys_with_keyid in its
     *     SL-PTE → device sees KeyID-decrypted plaintext).
     *   - Compose the KeyID-bearing phys via tme_phys_with_keyid()
     *     when mapping into a CPU page table (vmm_map_page_with_keyid).
     * Caller passes the same value back to iommu_dma_free_with_keyid. */
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

/* Boot-time audit. The backends log their own per-DRHD/IVHD bring-up; this
 * adds the one line that says what the machine as a whole ended up with, at
 * the iommu_init call site.
 *
 * It reports the outcome iommu_init recorded, and nothing wider than that.
 * "DMA is untranslated" is a statement about the path this line stands at
 * the end of; "this machine has no IOMMU" is a statement about the hardware,
 * and only one of the four outcomes below is entitled to make it.
 *
 * kprintf, not debug_printf: this line exists to be read on a board, and
 * debug_printf compiles to nothing in a shipped build. */
void iommu_audit_dump(void) {
    switch (g_outcome) {
    case IommuOutcomeNotRun:
        kprintf("[IOMMU] iommu_init has not run, so nothing is known about "
                "this machine's remap hardware yet\n");
        return;
    case IommuOutcomeNoTables:
        kprintf("[IOMMU] ACPI describes neither DMAR nor IVRS — there is no "
                "remap hardware here to drive, and every device's DMA "
                "reaches memory untranslated\n");
        return;
    case IommuOutcomeBackendFailed:
        /* The hardware exists. It was reached, and it did not come up — and
         * the first thing a bring-up does is take the unit off whatever
         * firmware left running on it, so this machine's pre-boot DMA
         * protection is not something the rest of the boot may count on. */
        kprintf("[IOMMU] ACPI describes %s hardware and not one unit came "
                "online — the lines above say which refused and why. DMA "
                "reaches memory untranslated, and firmware's pre-boot DMA "
                "protection is not to be counted on: bringing a unit up "
                "starts by taking it off firmware's tables\n",
                g_described ? g_described : "remap");
        return;
    case IommuOutcomeUp:
        kprintf("[IOMMU] backend=%s, units up, and iommu_init left "
                "translation OFF — the domains, page tables and device "
                "entries this kernel wrote are not consulted by the hardware "
                "yet, so DMA still reaches memory untranslated. Touch "
                "subjects: iommu:ready, iommu:domain:attached, "
                "iommu:dma:mapped, iommu:dma:unmapped; MemTag labels every "
                "mapped page iommu:domain:N\n", g_ops->name);
        return;
    }
}
