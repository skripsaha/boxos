#include "iommu.h"
#include "acpi.h"
#include "klib.h"
#include "touch.h"
#include "memtag.h"  /* Phase 2G — auto-tag DMA-mapped phys with iommu:domain:N */
#include "pmm.h"     /* PMM_PAGE_SIZE for page-count derivation */
#include "tme.h"     /* TME-MK aware DMA — KeyID encoding */
#include "pci.h"     /* the config-space walk that seats every present device */

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
 * units come up with it off, because at this point in the boot PCI has not
 * been enumerated and a unit switched on over an empty context table stops
 * the DMA of every live controller on the machine. iommu_take_the_wheel,
 * further down this file, is the second step -- it seats every device PCI
 * reports and then throws the switch, or refuses and throws nothing.
 *
 * Their fault channel is read once, at the handover from firmware, by
 * iommu_publish_touch -- which kernel_main calls after guide_init, because
 * before guide_init there is no Touch to say it on. On VT-d that channel is
 * live and holds whatever firmware left in it; on AMD-Vi it is inert until
 * IommuEn, and the AMD side says so on the board rather than reading an
 * empty ring in silence.
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
    /* Nothing is said on Touch from here, and the two things this function
     * has to say are both said later, from iommu_publish_touch.
     *
     * TouchInit runs inside guide_init, which is a long way further down
     * kernel_main than this call. A TouchPublish issued here finds an empty
     * tag registry, is dropped at resolve_tag_pair, and reaches nobody --
     * not now and not retroactively. main.c already carries that lesson in
     * full over efi_secureboot_init, which defers its publishes for exactly
     * this reason; the `iommu:ready` publish that used to stand on the next
     * line had been going nowhere all along. */
    return 0;
}

/* Everything this subsystem has to SAY, said at the first moment anyone can
 * hear it. Called from kernel_main once guide_init has brought Touch up.
 *
 * Order inside matters and is the reason these two are one call: the sixteen
 * bytes a fault record carries have a different shape per backend, so a
 * subscriber has to know WHICH backend is on the machine before the first
 * record arrives. `iommu:ready` names it, and goes first.
 *
 * Then the units are asked, once, what they recorded while they were still
 * firmware's -- and it is cleared, which on VT-d is the difference between a
 * unit that can report a fault and one that will never report another
 * (§11.4.7.1: while PFO stands "hardware does not record any new faults";
 * §7.3: while any status bit stands "the Fault Event interrupt is not
 * generated").
 *
 * This is not a poll and it is not on a clock. The handover from firmware is
 * the fact, and it happens exactly once. Nothing is scheduled to look again,
 * because at THIS point in the boot translation is still off (GCMD.TE clear
 * on every VT-d unit) and interrupt remapping is off, so no new fault
 * recording entry can be produced for anyone to miss -- and the one channel
 * that CAN move while translation is off, the invalidation queue's
 * IQE/ICE/ITE, already has its own event: vtd_qi_submit reads FSTS on every
 * turn of its own wait and reports there, at the moment the error is caused
 * rather than some interval later.
 *
 * Switching translation on IS an event, and iommu_take_the_wheel answers to
 * it: it reads this same channel once more the instant the switch is thrown,
 * which is the first instant a fault can exist at all. What still has no
 * reader is a fault raised later, under a running driver -- the unit's own
 * Fault Event interrupt is the answer to that one, not a timer, and no
 * vector is armed for it in this tree. */
void iommu_publish_touch(void) {
    if (!g_ops) return;
    TouchPublish("iommu:ready", g_ops->name, 0);
    if (g_ops->drain_faults) g_ops->drain_faults();
}

/* ───── Taking the wheel: seat every device, then switch translation on ─────
 *
 * WHAT FIRMWARE'S PRE-BOOT DMA PROTECTION MEANS NOW, and the reconciliation
 * the previous wave's decision needs.
 *
 * A firmware that sets DMAR flags bit 2 (and the AMD equivalent) hands the OS
 * a unit already translating, through firmware's own tables. The backends
 * take that apart deliberately and say so out loud -- vtd_adopt_from_firmware
 * clears GCMD.TE before it touches RTADDR, amdvi_unit_init clears IommuEn
 * before it moves a base register -- because those tables are not ours: we do
 * not know what they map, we cannot invalidate them, and replacing a root
 * table under a running unit is the one move that cannot be undone.
 *
 * The two decisions do not conflict; they are the two halves of one handover.
 * Firmware's protection ends at iommu_init and this kernel's begins here, and
 * between those two points -- ACPI, interrupts, timers, the AP bring-up, the
 * PCI walk -- DMA reaches memory untranslated. That gap is REAL and it is
 * named rather than hidden. It is also as short as this kernel can make it
 * without adopting tables it cannot read: this call sits at the first line of
 * kernel_main at which every device is enumerable, and no driver has started
 * a transfer yet.
 *
 * ORDER, and it is the whole job:
 *   1. every present PCI function gets an entry pointing at the identity map;
 *   2. only then does any unit begin translating;
 *   3. and if step 1 fails anywhere, step 2 does not happen anywhere.
 *
 * "Anywhere" is per SEGMENT, not in total. A segment that has a remap unit
 * and produced no seats is a segment this kernel is reading wrongly, and its
 * unit going on over a table with no present entries is the dead-controller
 * case — so the sum being non-zero is not the test; every segment answering
 * is.
 */

#ifndef CONFIG_IOMMU_TRANSLATION_OFF
/* Config space for the segment it is given.
 *
 * Segment 0 goes through pci_config_read_dword, which uses ECAM where the bus
 * falls inside a window and 0xCF8/0xCFC where it does not -- so every bus of
 * segment 0 is reachable on every machine, MCFG or no MCFG. A segment above 0
 * exists only because MCFG says so, and only ECAM can reach it. */
static uint32_t iommu_cfg_dword(uint16_t segment, uint8_t bus, uint8_t dev,
                                uint8_t fn, uint8_t offset) {
    if (segment == 0) return pci_config_read_dword(bus, dev, fn, offset);
    return pci_ecam_read_dword(segment, bus, dev, fn, offset);
}

/* Which buses of this segment can be read, and how.
 *
 * NOT a walk down from bus 0 through bridges, which is what stood here. That
 * misses two whole classes of device and both of them are ordinary:
 *   - a host bridge that is multi-function publishes one PCI ROOT BUS per
 *     function, and the buses behind functions 1..7 hang off nothing on bus
 *     0 (pci_find_nth_by_class in this tree already handles that case);
 *   - a segment whose MCFG entry starts above bus 0 has no bus 0 at all.
 * Either way the missed function gets no context entry, and on VT-d its DMA
 * is blocked the instant GCMD.TE is set -- the dead-controller failure this
 * ordering exists to prevent.
 *
 * So the sweep is the bus range itself, which is what pci_enumerate_ecam does
 * for the same reason. The range comes from the MCFG entry for this segment
 * (PCI Firmware Specification 3.0 §4.1.2 gives one base and one start/end bus
 * pair per segment group); segment 0 with no MCFG entry is the legacy
 * mechanism's whole range, because a 0xCF8 address carries eight bits of bus
 * and reaches all of them. Returns false when this segment cannot be read at
 * all, which is a reason to switch nothing on rather than to guess. */
typedef struct {
    uint16_t segment;
    uint8_t  first_bus;
    uint8_t  last_bus;
} IommuSegmentWindow;

static bool iommu_segment_window(uint16_t segment, IommuSegmentWindow* w) {
    const acpi_mcfg_info_t* mcfg = acpi_get_mcfg();
    if (mcfg) {
        for (uint8_t i = 0; i < mcfg->count; i++) {
            if (mcfg->segments[i].segment_group != segment) continue;
            if (segment != 0 && !pci_has_ecam()) return false;
            w->segment   = segment;
            w->first_bus = mcfg->segments[i].start_bus;
            w->last_bus  = mcfg->segments[i].end_bus;
            return w->last_bus >= w->first_bus;
        }
    }
    /* No window of its own. Segment 0 is still fully reachable through
     * 0xCF8/0xCFC; anything else is not reachable by any mechanism this
     * kernel has. */
    if (segment != 0) return false;
    w->segment   = 0;
    w->first_bus = 0;
    w->last_bus  = 0xFF;
    return true;
}

/* Seat every function of one segment. Returns the number seated, or a
 * negative number when the backend refused one -- in which case the refused
 * address is left in *bus_out / *devfn_out for the caller to name. */
static int iommu_seat_segment(const IommuSegmentWindow* w, uint8_t* bus_out,
                              uint8_t* devfn_out) {
    uint32_t seated = 0;
    for (unsigned bus = w->first_bus; bus <= w->last_bus; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t ident = iommu_cfg_dword(w->segment, (uint8_t)bus, dev, 0,
                                             PCI_VENDOR_ID);
            if ((ident & 0xFFFFu) == PCI_INVALID_VENDOR) continue;
            uint32_t hdr = iommu_cfg_dword(w->segment, (uint8_t)bus, dev, 0,
                                           PCI_CACHE_LINE_SIZE);
            uint8_t  fns = ((hdr >> 16) & PCI_HEADER_TYPE_MF) ? 8u : 1u;
            for (uint8_t fn = 0; fn < fns; fn++) {
                if (fn != 0) {
                    ident = iommu_cfg_dword(w->segment, (uint8_t)bus, dev, fn,
                                            PCI_VENDOR_ID);
                    if ((ident & 0xFFFFu) == PCI_INVALID_VENDOR) continue;
                }
                uint8_t devfn = (uint8_t)((dev << 3) | fn);
                if (g_ops->seat_device(w->segment, (uint8_t)bus, devfn) != 0) {
                    *bus_out   = (uint8_t)bus;
                    *devfn_out = devfn;
                    return -1;
                }
                seated++;
            }
        }
    }
    return (int)seated;
}
#endif /* !CONFIG_IOMMU_TRANSLATION_OFF */

int iommu_take_the_wheel(void) {
#ifdef CONFIG_IOMMU_TRANSLATION_OFF
    /* `make IOMMU=off`. Said through kprintf and not debug_printf, because
     * the whole point of the key is that somebody standing at a board can
     * tell which kernel they are looking at. */
    kprintf("[IOMMU] built with IOMMU=off — DMA translation is NOT switched "
            "on, every device reaches memory untranslated, and whatever the "
            "units were brought up with stays as it is\n");
    return 0;
#else
    if (!g_ops) {
        /* Two different machines end up here and only one of them is
         * uneventful. With no DMAR and no IVRS there is nothing to switch on
         * and iommu_audit_dump has already said so. With hardware that was
         * described and did not come up, the bring-up ALREADY took whatever
         * firmware was running off its own tables before it failed, so this
         * machine is running with no DMA protection of any kind — and that
         * is said here, at the line where it becomes final, rather than left
         * to be inferred from a call that printed nothing. */
        if (g_outcome == IommuOutcomeBackendFailed)
            kprintf("[IOMMU] %s hardware was described and none of it came "
                    "online, so there is no wheel to take. This machine runs "
                    "every DMA untranslated, and firmware's pre-boot "
                    "protection is not standing either: bringing a unit up is "
                    "what took it off firmware's tables\n",
                    g_described ? g_described : "remap");
        return 0;
    }
    if (!g_ops->seat_device || !g_ops->unit_segment || !g_ops->take_the_wheel) {
        kprintf("[IOMMU] backend=%s cannot seat devices or switch translation "
                "on — DMA stays untranslated\n", g_ops->name);
        return -1;
    }

    uint32_t seated = 0;
    uint16_t segment = 0;
    for (uint8_t i = 0; g_ops->unit_segment(i, &segment); i++) {
        /* One walk per DISTINCT segment. Units are counted, not collected:
         * asking the backend again for the segments already visited costs a
         * handful of comparisons and spares this file an array whose size
         * would be a number nobody can source. */
        bool already = false;
        for (uint8_t j = 0; j < i; j++) {
            uint16_t earlier = 0;
            if (g_ops->unit_segment(j, &earlier) && earlier == segment) {
                already = true;
                break;
            }
        }
        if (already) continue;

        IommuSegmentWindow window;
        if (!iommu_segment_window(segment, &window)) {
            kprintf("[IOMMU] a remap unit lives in PCI segment %u and nothing "
                    "here can read that segment's config space — it has no "
                    "MCFG window of its own and it is not segment 0, so the "
                    "legacy mechanism cannot reach it either. Translation is "
                    "switched on NOWHERE rather than over a segment whose "
                    "devices were never seated\n", (unsigned)segment);
            return -1;
        }

        uint8_t bad_bus = 0, bad_devfn = 0;
        int here = iommu_seat_segment(&window, &bad_bus, &bad_devfn);
        if (here < 0) {
            kprintf("[IOMMU] backend=%s would not seat %04x:%02x:%02x.%x in "
                    "the identity domain — translation is switched on NOWHERE, "
                    "and DMA reaches memory untranslated exactly as it did "
                    "before this call\n",
                    g_ops->name, (unsigned)segment, bad_bus,
                    (bad_devfn >> 3) & 0x1Fu, bad_devfn & 0x7u);
            return -1;
        }
        if (here == 0) {
            /* PER SEGMENT, not in total. A segment that has a remap unit and
             * answered with nothing is a segment whose window this kernel is
             * reading wrongly — and switching that unit on over a table with
             * no present entries blocks the DMA of everything behind it. The
             * global count hid exactly this: thirty seats on segment 0 made
             * the sum non-zero while a second segment contributed none. */
            kprintf("[IOMMU] backend=%s: segment %u has a remap unit and not "
                    "one PCI function answered anywhere in its bus range "
                    "%02x-%02x. Its unit would translate for a table with no "
                    "present entries, so translation is switched on NOWHERE\n",
                    g_ops->name, (unsigned)segment, window.first_bus,
                    window.last_bus);
            return -1;
        }
        seated += (uint32_t)here;
    }

    uint8_t units = 0;
    int translating = g_ops->take_the_wheel(&units);
    if (translating < 0 || (uint8_t)translating != units) {
        kprintf("[IOMMU] backend=%s left translation OFF — the lines above "
                "say what did not add up. %u device(s) have an entry that "
                "nothing is walking, and DMA reaches memory untranslated\n",
                g_ops->name, seated);
        return -1;
    }

    kprintf("[IOMMU] translation ON: backend=%s, %u of %u unit(s) translating, "
            "%u PCI function(s) seated in the identity domain. Every DMA on "
            "this machine now goes through tables this kernel wrote; firmware's "
            "pre-boot protection ended at iommu_init and this is where ours "
            "begins\n",
            g_ops->name, (unsigned)translating, (unsigned)units, seated);

    /* The first instant an I/O page fault can exist is the instant above.
     * Reading the channel here is not a poll on an interval — it answers to
     * that event and to nothing else, the same way the read in
     * iommu_publish_touch answers to the handover from firmware. A fault
     * raised later, while a driver is running, still has no reader: the
     * unit's own Fault Event interrupt is what answers that one, and no
     * vector is armed for it in this tree. */
    if (g_ops->drain_faults) g_ops->drain_faults();
    return 0;
#endif
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
        kprintf("[IOMMU] backend=%s, units up, translation still OFF at this "
                "line — PCI has not been enumerated yet, and a unit switched "
                "on over a context table with no present entries stops the "
                "DMA of every live controller. iommu_take_the_wheel does "
                "that, further down, straight after pci_init: it seats every "
                "function PCI reports and then switches every unit on, or it "
                "refuses and switches none. Between here and there DMA "
                "reaches memory untranslated, and so does firmware's, "
                "because bringing a unit up took it off firmware's tables. "
                "Their fault channel has NOT been read at this point and is "
                "read by iommu_publish_touch below -- Touch does not exist "
                "until guide_init, and a fault said before it would be said "
                "to nobody. Every line further down beginning [VT-d] fault or "
                "[AMD-Vi] event is what firmware left in these units. "
                "Touch subjects: iommu:ready, iommu:fault, "
                "iommu:domain:attached, iommu:dma:mapped, iommu:dma:unmapped; "
                "MemTag labels every mapped page iommu:domain:N\n",
                g_ops->name);
        return;
    }
}
