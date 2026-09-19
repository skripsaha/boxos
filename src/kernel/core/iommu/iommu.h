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
 * What is built, and what that still leaves
 * -----------------------------------------
 * Neither backend is scaffolding any more. Each one builds its own
 * second-level page tables, programs the per-device structure the
 * hardware walks (VT-d root + context entries, AMD-Vi device table
 * entries), and runs its invalidation channel to a COMPLETION rather
 * than to a submit -- VT-d's invalidation queue with a wait descriptor
 * whose status DWORD is the exit, AMD-Vi's command buffer with a
 * COMPLETION_WAIT whose semaphore store is. VT-d also publishes an
 * interrupt-remap table. Mapping granularity is 4 KiB throughout:
 * neither walker emits a 2 MiB or a 1 GiB entry, so an identity map of
 * the bottom 4 GiB costs 1048576 iterations and 2054 pages of tables —
 * 2048 leaf tables of 512 eight-byte entries, four page directories, a
 * PDPT and the top level, a little over 8 MiB.
 *
 * What is NOT done is switching translation ON. iommu_init() brings the
 * units up with it off, deliberately, and nothing in this tree calls
 * vtd_enable_translation() or amdvi_enable(). Until something does,
 * every structure named above is written and never consulted, and DMA
 * reaches memory untranslated exactly as it did before boot.
 *
 * The vtable lets driver code call `iommu_map(domain, iova, phys, size,
 * perm)` against an abstract handle without caring which vendor sits
 * behind it, and get the same answer when no IOMMU sits behind it at all.
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
    /* Read this backend's fault channel, say every record out loud and
     * clear it. The two backends keep that channel in different places --
     * VT-d in the unit's own fault recording registers, AMD-Vi in a ring in
     * system memory -- so only the backend can do it, and
     * iommu_publish_touch is how the rest of the kernel asks. Never called
     * from an interrupt handler: both publish on Touch and both may wait on
     * a status bit. */
    void (*drain_faults)(void);
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

/* Boot-time audit. Says which of the four things happened: iommu_init not
 * run, no DMAR/IVRS, hardware described and none of it brought online, or a
 * backend driving with translation left off — and in the last case which
 * Touch subjects the subsystem publishes on. "No remap hardware" and "the
 * remap hardware did not come up" are different machines and it never
 * reports one as the other.
 * Idempotent and free of side effects beyond the print. Called from main.c
 * right after iommu_init, and it prints through kprintf rather than
 * debug_printf because a boot log that a shipped build compiles away tells
 * nobody on a board whether their DMA is translated. */
void            iommu_audit_dump(void);

/* Say who the backend is, then ask every unit that came online what it
 * recorded, say that too, and clear it.
 *
 * Called from kernel_main AFTER guide_init, and the ordering is not a
 * preference. TouchInit runs inside guide_init; a publish issued before it
 * is dropped at resolve_tag_pair and reaches no subscriber, now or ever --
 * the same trap main.c already documents over efi_secureboot_init. So this
 * is where `iommu:ready` is said and where the fault channel is read, and
 * `iommu:ready` goes first so a subscriber knows which backend's sixteen-byte
 * record shape is about to arrive on `iommu:fault`.
 *
 * Reading the channel is not merely informative on VT-d: whatever firmware
 * left standing in it is both the only account of what happened before this
 * kernel existed AND a gag on everything that would be recorded or signalled
 * after it (§11.4.7.1 PFO, §7.3).
 *
 * It runs ONCE. Nothing periodic follows it, and nothing needs to: with
 * GCMD.TE clear on every unit and interrupt remapping off, no new fault
 * recording entry can be produced; the invalidation queue's IQE/ICE/ITE,
 * which can move with translation off, is read by vtd_qi_submit on every
 * turn of its own wait, at the moment the error is caused. An interval here
 * would be a number standing in for an event that cannot happen.
 *
 * Silent and side-effect-free on a machine with no remap hardware, on a
 * backend that publishes no drain, and on a unit with nothing recorded. It
 * never refuses a boot. Not callable from an interrupt handler -- it
 * publishes on Touch (`iommu:fault`, payload = the sixteen bytes the hardware
 * wrote) and the AMD-Vi side waits on EventLogRun with an HourGlass. */
void            iommu_publish_touch(void);

/* ----- What each backend exports beyond the vtable -----
 *
 * These three are defined by their backends and, until now, declared
 * nowhere: any caller would have been compiling against an implicit
 * int-returning stranger, which -Werror=implicit-function-declaration turns
 * into a failed build rather than a wrong call. They are declared here so
 * that the decision to call one is the only thing still missing.
 *
 * vtd_enable_translation / amdvi_enable switch DMA translation ON for every
 * unit that came online and return how many began translating. Nothing in
 * this tree calls either, deliberately: a device only survives that switch
 * once it has a context entry (VT-d) or a device-table entry (AMD-Vi), and
 * the bus walk that would attach every function lives in pci.c and reaches
 * iommu_device_attach from nowhere.
 *
 * amdvi_poll_events drains each AMD-Vi unit's event log, which is the only
 * channel the hardware has for IO_PAGE_FAULT, ILLEGAL_DEV_TABLE_ENTRY,
 * DEV_TAB_HARDWARE_ERROR and the rest. It is reached through the vtable, as
 * this backend's drain_faults. It is NOT callable from an interrupt handler:
 * it publishes on Touch and it waits on EventLogRun with an HourGlass.
 *
 * What it will find while amdvi_enable has not been called is NOTHING, and
 * the sentence that used to stand here -- that amdvi_init arms the log so it
 * fills whether or not anybody drains it -- was wrong. 48882 Rev 3.05-PUB,
 * MMIO Offset 0018h bit 2 EventLogEn: "all events detected are written to the
 * event log WHEN IommuEn HAS ALSO BEEN SET". The same register's bit 12
 * CmdBufEn: "When CmdBufEn = 1b and IommuEn = 1b, the IOMMU starts fetching
 * commands". amdvi_init deliberately leaves IommuEn clear, so on AMD both the
 * event log and the command processor are inert, and giving that backend eyes
 * and switching the unit on are the same act.
 *
 * That is why this function is NOT this backend's drain_faults. Calling it
 * with IommuEn clear would walk a ring the hardware cannot have written --
 * amdvi_unit_init has just zeroed EvtHeadPtr and EvtTailPtr and cleared
 * EventOverflow, so head equals tail, the overflow branch is unreachable, and
 * the whole call is a provable no-op dressed as an inspection. The slot holds
 * amdvi_drain_faults instead, which reads IommuEn out of MMIO Offset 0018h
 * and SAYS that the channel is off on a board where it is, rather than
 * returning empty-handed in silence; it calls this function for the units
 * where the bit is actually set.
 *
 * They are NOT the same act as switching TRANSLATION on, which is worth
 * saying because it is the reason a first step exists at all. Translation on
 * AMD-Vi is per device, in the DTE's V and TV bits, and Table 8 on V = 0 is
 * unambiguous: "All addresses are forwarded without translation; individual
 * control fields are ignored." amdvi_unit_init leaves the whole device table
 * zeroed and nothing calls iommu_device_attach, so every entry reads V = 0.
 * IommuEn = 1 over that table starts the event log and the command engine and
 * translates nothing. Table 10 says the same of interrupts at IV = 0: LINT0,
 * LINT1, SMI, NMI, INIT and ExtInt "are passed through unmapped". */
/* vtd_drain_faults is the VT-d half of the same channel, and it is the half
 * that has something to read TODAY. Its records live in the unit's own fault
 * recording registers rather than in a ring in system memory, so what
 * firmware recorded is still there after this kernel takes the unit over --
 * and §11.4.7.1 makes reading it load-bearing rather than merely informative:
 * an inherited PFO stops the unit recording anything ever again, and §7.3
 * stops it signalling anything ever again. Same contract as its AMD-Vi
 * counterpart: not from an interrupt handler.
 *
 * Both are reached through the vtable, as each backend's drain_faults, and
 * iommu_publish_touch is the door. They are named here because the header
 * describes what each backend exports, not because anything calls them by
 * name. */
int  vtd_enable_translation(void);
int  amdvi_enable(void);
void amdvi_poll_events(void);
void vtd_drain_faults(void);

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
