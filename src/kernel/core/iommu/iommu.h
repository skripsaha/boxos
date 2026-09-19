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
 * interrupt-remap table.
 *
 * The identity map each backend builds reaches pmm_get_mem_end() — the top of
 * this machine's memory map — and never less than the whole of what a Single
 * Address Cycle can address, because a device without Dual Address Cycle can
 * name any address below that line and none above it (PCI 3.0 §3.9). On top
 * of that it maps, by name, every reserved region the firmware's own table
 * publishes: the DMAR's RMRR entries on Intel, the IVRS's IVMD blocks on AMD.
 * Those fields are 64-bit and the regions are exactly the ones a device stops
 * without, so they are read rather than assumed covered. The leaf is 2 MiB
 * where the hardware can walk one (AMD-Vi always, per Table 15; VT-d where
 * every unit reports CAP.SSLPS), 4 KiB otherwise.
 *
 * Translation is ON by default
 * ----------------------------
 * iommu_init() still brings the units up with it OFF: at that point in the
 * boot ACPI has been read but PCI has not been enumerated, and switching
 * translation on before every present function has a context entry (VT-d) or
 * a device-table entry (AMD-Vi) is how a live disk controller loses its DMA
 * mid-boot.
 *
 * iommu_take_the_wheel() is the second step and the one that throws the
 * switch. kernel_main calls it immediately after pci_init(), which is the
 * first moment every device is enumerable and still the last moment before
 * any driver starts a transfer. It seats every function it finds and then
 * enables every unit, or it refuses and enables none.
 *
 * `make IOMMU=off` builds a kernel where that second step declines out loud
 * and DMA reaches memory untranslated exactly as it did before boot. That key
 * exists for one reason: the first board to run this is the first machine on
 * which all DMA has ever gone through these tables, and a way back that does
 * not require editing sources is part of shipping it.
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

    /* ----- The three the wheel is turned with -----
     *
     * These exist beside domain_attach rather than on top of it because
     * seating a whole machine is a different operation from attaching one
     * device: it happens once, before anything is translating, and the
     * invalidation it needs is one global one at the end rather than a round
     * trip per function. On AMD-Vi that difference is not an optimisation —
     * with IommuEn still clear the command engine does not run at all, so a
     * per-device COMPLETION_WAIT could only ever run its whole span out.
     *
     * unit_segment: the PCI segment of the index-th ONLINE unit; false past
     * the end. iommu.c walks config space once per distinct segment a unit
     * lives in, because a segment with no unit has nothing to seat into.
     *
     * seat_device: write this function's translation-table entry into every
     * unit of its segment, pointed at the backend's identity map, and
     * invalidate nothing. Returns 0 only when the entry now stands in every
     * such unit.
     *
     * take_the_wheel: invalidate, then switch translation on, on every online
     * unit or on none. Writes the number of online units through
     * `units_total` and returns the number now translating, or negative with
     * nothing left switched on. */
    bool (*unit_segment)(uint8_t index, uint16_t* segment_out);
    int  (*seat_device)(uint16_t segment, uint8_t bus, uint8_t devfn);
    int  (*take_the_wheel)(uint8_t* units_total);
} iommu_ops_t;

/* Probe ACPI; pick backend; call ops->init. Returns 0 if any IOMMU
 * came online, -1 if none detected, -2 if init failed. */
int  iommu_init(void);
bool iommu_present(void);

/* Switch DMA translation on. The second half of iommu_init, and the only
 * thing in this kernel that ever sets GCMD.TE or IOMMU_CTRL.IommuEn.
 *
 * WHERE IT IS CALLED FROM IS PART OF WHAT IT IS. kernel_main calls it
 * immediately after pci_init(), and neither side of that is arbitrary:
 *   - not before, because it walks PCI config space to find every function
 *     that needs an entry, and on a multi-segment machine that walk needs the
 *     ECAM windows pci_init maps;
 *   - not later, because every driver initialised after that line — AHCI,
 *     ATA, xHCI — starts DMA, and a controller whose transfer is in flight
 *     when the tables change underneath it is the one thing this ordering
 *     exists to prevent.
 *
 * ALL OR NOTHING. A unit that is not found, a table that is not populated, a
 * device whose entry the backend would not take, a hardware capability that
 * is not what this kernel expected — any one of them and translation is
 * switched on NOWHERE, the machine is left exactly as untranslated as it was,
 * and kprintf says which of them it was. Half a machine translating is worse
 * than none of it: the failures look like broken devices rather than like a
 * kernel that got something wrong.
 *
 * Says one line on success naming the backend, how many units translate and
 * how many functions were seated, and then reads the fault channel once more
 * — the act of switching on is the first moment an I/O page fault can be
 * produced at all.
 *
 * Returns 0 when translation is on, 0 when there is no remap hardware to
 * switch on (a machine with no DMAR and no IVRS is not a failure), and -1
 * when something did not add up. Never refuses a boot. */
int  iommu_take_the_wheel(void);

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
 * backend driving with translation not yet switched on — and in the last case
 * which Touch subjects the subsystem publishes on, and where the switch is
 * thrown. "No remap hardware" and "the
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
 * It runs ONCE here, and nothing periodic follows it. At THIS point in the
 * boot nothing needs to: GCMD.TE is clear on every unit and interrupt
 * remapping is off, so no new fault recording entry can be produced, and the
 * one channel that can move with translation off -- the invalidation queue's
 * IQE/ICE/ITE -- is read by vtd_qi_submit on every turn of its own wait, at
 * the moment the error is caused. An interval here would be a number
 * standing in for an event that cannot happen.
 *
 * Translation goes on LATER, in iommu_take_the_wheel, and that IS an event:
 * it is the first instant a fault can be recorded, so that function reads
 * this channel once more the moment the switch is thrown. What no event
 * covers yet is a fault raised later, while a driver is running -- the
 * unit's own Fault Event interrupt is what answers that, and no vector is
 * armed for it in this tree.
 *
 * Silent and side-effect-free on a machine with no remap hardware, on a
 * backend that publishes no drain, and on a unit with nothing recorded. It
 * never refuses a boot. Not callable from an interrupt handler -- it
 * publishes on Touch (`iommu:fault`, payload = the sixteen bytes the hardware
 * wrote) and the AMD-Vi side waits on EventLogRun with an HourGlass. */
void            iommu_publish_touch(void);

/* ----- What each backend exports beyond the vtable -----
 *
 * These two are defined by their backends and declared here so a caller is
 * not compiling against an implicit int-returning stranger, which
 * -Werror=implicit-function-declaration turns into a failed build rather
 * than a wrong call.
 *
 * (The enables that used to be named here are gone from this list: switching
 * translation on is now reached through the vtable's take_the_wheel, with
 * iommu_take_the_wheel as the one door, because the step that must come
 * first — seating every device the machine has — is not something a caller
 * can be trusted to remember.)
 *
 * amdvi_poll_events drains each AMD-Vi unit's event log, which is the only
 * channel the hardware has for IO_PAGE_FAULT, ILLEGAL_DEV_TABLE_ENTRY,
 * DEV_TAB_HARDWARE_ERROR and the rest. It is reached through the vtable, as
 * this backend's drain_faults. It is NOT callable from an interrupt handler:
 * it publishes on Touch and it waits on EventLogRun with an HourGlass.
 *
 * What it will find while iommu_take_the_wheel has not run is NOTHING, and
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
 * zeroed, so until amdvi_seat_device has written the entries every one of
 * them reads V = 0, and IommuEn = 1 over that table would start the event log
 * and the command engine and translate nothing. That is precisely why
 * iommu_take_the_wheel seats every present function BEFORE it sets IommuEn,
 * and why the AMD failure mode of a missed device is a device silently
 * outside the protection rather than a device that stops. Table 10 says the
 * same of interrupts at IV = 0: LINT0, LINT1, SMI, NMI, INIT and ExtInt "are
 * passed through unmapped", and no DTE this driver writes sets IV. */
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
