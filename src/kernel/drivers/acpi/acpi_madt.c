#include "acpi_madt.h"
#include "acpi_internal.h"
#include "ioapic.h"
#include "lapic.h"        /* IA32_APIC_BASE / EXTD, for detect_bsp_apic_id */
#include "cpuid.h"
#include "klib.h"
#include "vmm.h"

/* ACPI 6.5 §5.2.12 entry-length expectations. A real BIOS that publishes a
 * shorter entry of a known type is broken; we skip such entries rather
 * than misread garbage past their declared end. */
static uint8_t expected_entry_length(uint8_t type) {
    switch (type) {
        case MADT_TYPE_LOCAL_APIC:        return sizeof(madt_local_apic_t);
        case MADT_TYPE_IO_APIC:           return sizeof(madt_io_apic_t);
        case MADT_TYPE_ISO:               return sizeof(madt_iso_t);
        case MADT_TYPE_NMI_SOURCE:        return 8;   /* spec fixed */
        case MADT_TYPE_LOCAL_APIC_NMI:    return sizeof(madt_lapic_nmi_t);
        case MADT_TYPE_LAPIC_OVERRIDE:    return sizeof(madt_lapic_override_t);
        case MADT_TYPE_LX2APIC:           return sizeof(madt_lx2apic_t);
        case MADT_TYPE_LX2APIC_NMI:       return sizeof(madt_lx2apic_nmi_t);
        default:                          return 0;   /* type unknown to us */
    }
}

/* Which of the two Local APIC Flags a processor entry carries.
 *
 * ACPI 6.5 §5.2.12.2, Table 5.23 keeps them apart and this kernel used to
 * OR them together:
 *
 *   bit 0 Enabled       — "If this bit is set the processor is ready for
 *                          use. If this bit is clear and the Online Capable
 *                          bit is set, system hardware supports enabling
 *                          this processor during OS runtime. If this bit is
 *                          clear and the Online Capable bit is also clear,
 *                          this processor is unusable, and OSPM shall
 *                          ignore the contents of the Processor Local APIC
 *                          Structure."
 *   bit 1 Online Capable — reserved and zero while Enabled is set;
 *                          otherwise "system hardware supports enabling
 *                          this processor during OS runtime".
 *
 * So Online Capable means CAN BE ADDED LATER, never READY NOW. A Cloud
 * Hypervisor guest started `--cpus boot=2,max=8` publishes six Type 9
 * entries with Flags = 0x00000002 for sockets that hold nothing yet; a
 * server board with populated-later sockets publishes the same shape. The
 * OR handed every one of those APIC IDs to the SMP bring-up, which spent a
 * kernel stack, an INIT, two SIPIs and a full second of waiting on each
 * processor the firmware had just said is not there, and carried the
 * phantoms inside the core count that decides the K-Core split.
 *
 * Linux draws the line in the same place: acpi_is_processor_usable() may
 * register a not-Enabled processor as POSSIBLE, but what it passes as
 * `enabled` to topology_register_apic() is `lapic_flags & ACPI_MADT_ENABLED`
 * and nothing else, and only an enabled processor is started at boot.
 *
 * BoxOS has no hot-add path to hand an Online-Capable slot to, so the slot
 * is named out loud and left alone rather than booted. */
static bool lapic_flags_enabled(uint32_t flags) {
    return (flags & MADT_LAPIC_ENABLED) != 0;
}

/* Bit 1 carries "Online Capable" only on a FADT that knows the bit exists.
 *
 * ACPI 6.3 is the revision that defined it; before that the bit was RESERVED
 * in Table 5.23, and a reserved bit is one the firmware was never asked to
 * clear. Reading a stray 1 there as a hot-add seat describes a processor the
 * firmware said nothing about — and by the PRE-6.3 rule that entry is simply
 * unusable: "If this bit is clear ... this processor is unusable, and OSPM
 * shall ignore the contents of the Processor Local APIC Structure."
 *
 * Which revision the table was written to is FADT Major.Minor, and the minor
 * half is the byte at offset 131 that this kernel used to carry as padding
 * (acpi.h, ACPI_FADT_MINOR_VERSION). Linux draws the same line in
 * acpi_parse_madt: `acpi_support_online_capable = (revision > 6 || (revision
 * == 6 && minor_revision >= 3))`.
 *
 * Linux then adds a second rule this kernel does NOT copy — under a
 * hypervisor it treats every not-Enabled entry as usable anyway, "QEMU
 * expects legacy Enabled=0 LAPIC entries to be counted as usable in order to
 * support CPU hotplug in guests". BoxOS starts no processor the firmware did
 * not mark Enabled on any machine, guest or not, so there is nothing for that
 * exception to buy.
 *
 * No FADT at all (a MADT parsed before acpi_init finished, which cannot
 * happen on the boot path but is not this function's to assume) is the
 * conservative answer: the bit means nothing. */
static bool madt_online_capable_is_meaningful(void) {
    const acpi_fadt_t *f = g_acpi.fadt;
    if (!f) return false;
    if (f->header.length < offsetof(acpi_fadt_t, minor_revision) + 1)
        return false;
    if (f->header.revision > 6) return true;
    return f->header.revision == 6 && ACPI_FADT_MINOR_VERSION(f) >= 3;
}

static bool lapic_flags_online_capable(uint32_t flags) {
    return !lapic_flags_enabled(flags) &&
           (flags & MADT_LAPIC_ONLINE_CAP) != 0 &&
           madt_online_capable_is_meaningful();
}

static const char* lapic_flags_text(uint32_t flags) {
    if (lapic_flags_enabled(flags))        return "enabled";
    if (lapic_flags_online_capable(flags)) return "not enabled, online-capable "
                                                  "(hot-add slot)";
    return "unusable";
}

/*
 * ACPI 6.5 §5.2.12.2: the OSPM determines the BSP at runtime — it is the
 * processor that is currently executing OS init code. Asking "which
 * processor am I" instead of taking the first enabled MADT entry avoids
 * the trap of MADTs whose Local APIC list is sorted by ACPI processor ID
 * rather than by which core booted first.
 *
 * The question must be asked at FULL WIDTH. CPUID.01H:EBX[31:24] is the
 * 8-bit *initial* APIC ID and cannot name a processor whose APIC ID has
 * any bit set above bit 7 — which is precisely the machine class that
 * publishes its processors as MADT Type 9 (Processor Local x2APIC, 32-bit
 * X2APIC ID, ACPI 6.5 §5.2.12.12) and whose firmware hands the OS over
 * already in x2APIC mode. On such a machine the 8-bit answer matches the
 * wrong Type 0 entry, or no entry at all, and the BSP's ACPI processor ID
 * is never resolved — so its MADT-declared NMI (LINT0/LINT1) is never
 * programmed and the firmware's watchdog line never reaches the OS.
 *
 * CPUID.0BH:EDX is the full 32-bit x2APIC ID of the executing logical
 * processor and is valid whether or not x2APIC mode is currently enabled.
 * EDK2's GetInitialApicId() (UefiCpuPkg BaseXApicX2ApicLib) reads it in
 * two cases, and so do we:
 *
 *   - the APIC is already in x2APIC mode. Then leaf 0BH is the ONLY
 *     honest answer, and the fallback is actively wrong: CPUID.01H:EBX
 *     [31:24] is eight bits wide, so a processor whose initial APIC ID is
 *     255 or more reports 0xFF there — the very machine class that gets
 *     handed over in x2APIC mode would name itself 0xFF and match either
 *     nothing or the wrong entry.
 *   - the APIC is not in x2APIC mode but leaf 0BH reports a non-zero
 *     level in CPUID.0BH:EBX[15:0], i.e. the leaf is meaningfully
 *     populated. Otherwise the 8-bit form is all there is.
 *
 * The mode is read straight from IA32_APIC_BASE.EXTD rather than asked of
 * lapic_is_x2apic_active(), because that cache is filled by lapic_init()
 * and lapic_init() has not run: acpi_parse_madt() is what TELLS it where
 * the APIC lives. For the same reason lapic_get_id() cannot be used here
 * either, though it would answer correctly.
 */
#define CPUID_LEAF_EXTENDED_TOPOLOGY 0x0000000Bu

static inline uint64_t madt_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t detect_bsp_apic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    bool x2apic_live = (madt_rdmsr(MSR_APIC_BASE) & MSR_APIC_BASE_EXTD) != 0;

    cpuid(CPUID_LEAF_VENDOR, &eax, &ebx, &ecx, &edx);
    if (eax >= CPUID_LEAF_EXTENDED_TOPOLOGY) {
        cpuid_count(CPUID_LEAF_EXTENDED_TOPOLOGY, 0, &eax, &ebx, &ecx, &edx);
        if (x2apic_live || (ebx & 0xFFFFu) != 0)
            return edx;                 /* full 32-bit x2APIC ID */
    }

    cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFFu;         /* 8-bit initial APIC ID */
}

/* The MADT is parsed more than once per boot — irqchip_init takes the
 * interrupt half of it and amp_init takes the processor half, and each calls
 * acpi_parse_madt with its own madt_info_t. The parse is pure and may repeat;
 * the SENTENCE about Online-Capable seats may not. Printed per call, an
 * operator counting cores would read every APIC ID twice and have no way to
 * tell that from two real seats. Said on the first parse, once. */
static bool g_seats_named = false;

acpi_error_t acpi_parse_madt(madt_info_t* info) {
    if (!info) return ACPI_ERR_INVALID_TABLE;

    memset(info, 0, sizeof(madt_info_t));

    if (!g_acpi.initialized || !g_acpi.rsdp) {
        debug_printf("[MADT] ACPI not initialized, cannot parse MADT\n");
        return ACPI_ERR_INVALID_TABLE;
    }

    acpi_madt_t* madt = (acpi_madt_t*)acpi_find_table("APIC");
    if (!madt) {
        debug_printf("[MADT] MADT table not found\n");
        return ACPI_ERR_INVALID_TABLE;
    }

    debug_printf("[MADT] Found MADT: length=%u\n", madt->header.length);
    debug_printf("[MADT] Local APIC address: 0x%x\n", madt->local_apic_address);
    debug_printf("[MADT] Flags: 0x%x (PIC present: %s)\n",
                 madt->flags,
                 (madt->flags & MADT_FLAG_PCAT_COMPAT) ? "yes" : "no");

    info->lapic_address = madt->local_apic_address;
    info->has_pic = (madt->flags & MADT_FLAG_PCAT_COMPAT) != 0;
    /* The full-width ID is what the MADT entries below are matched against,
     * and it is what is handed on. Narrowing it here — as this did, to the
     * eight bits an IOAPIC redirection-table physical destination carries —
     * loses the only information a consumer needs in order to know it
     * cannot route to this CPU. The field is 8 bits by format; the ANSWER
     * to "which processor am I" is not, and a consumer that cannot carry
     * the answer has to refuse, not round it off. */
    uint32_t bsp_apic_id = detect_bsp_apic_id();
    info->bsp_lapic_id = bsp_apic_id;
    info->bsp_lapic_found = true;
    if (bsp_apic_id > 0xFFu) {
        kprintf("[MADT] BSP APIC ID %u does not fit the 8-bit IOAPIC "
                "destination field — legacy IRQs cannot be routed to this "
                "CPU without interrupt remapping\n", bsp_apic_id);
    }
    debug_printf("[MADT] BSP APIC ID: %u\n", bsp_apic_id);

    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr + sizeof(madt_entry_header_t) <= end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;

        if (entry->length < sizeof(madt_entry_header_t)) {
            debug_printf("[MADT] Entry length %u below header, stopping\n",
                         entry->length);
            break;
        }
        if (ptr + entry->length > end) {
            debug_printf("[MADT] Entry extends past table end, stopping\n");
            break;
        }

        uint8_t expected = expected_entry_length(entry->type);
        if (expected != 0 && entry->length < expected) {
            debug_printf("[MADT] Type %u length %u < spec %u, skipping\n",
                         entry->type, entry->length, expected);
            ptr += entry->length;
            continue;
        }

        switch (entry->type) {
            case MADT_TYPE_LOCAL_APIC: {
                madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
                bool enabled = lapic_flags_enabled(lapic->flags);
                debug_printf("[MADT]   LAPIC: ACPI ID=%u, APIC ID=%u, %s\n",
                             lapic->acpi_processor_id, lapic->apic_id,
                             lapic_flags_text(lapic->flags));
                /* Resolve BSP ACPI processor ID by matching the APIC ID
                 * that CPUID reported for the running CPU. */
                if (info->bsp_lapic_found &&
                    !info->bsp_acpi_id_resolved &&
                    (uint32_t)lapic->apic_id == bsp_apic_id) {
                    info->bsp_acpi_processor_id = lapic->acpi_processor_id;
                    info->bsp_acpi_id_resolved  = true;
                }
                if (info->cpu_map_count < MADT_MAX_CPU_MAP) {
                    madt_cpu_map_t* m = &info->cpu_map[info->cpu_map_count++];
                    m->apic_id           = lapic->apic_id;
                    m->acpi_processor_id = lapic->acpi_processor_id;
                    m->enabled           = enabled;
                    m->online_capable    =
                        lapic_flags_online_capable(lapic->flags);
                }
                break;
            }

            case MADT_TYPE_IO_APIC: {
                madt_io_apic_t* ioapic = (madt_io_apic_t*)entry;
                debug_printf("[MADT]   IO-APIC: ID=%u, addr=0x%x, GSI base=%u\n",
                             ioapic->io_apic_id,
                             ioapic->io_apic_address,
                             ioapic->gsi_base);
                /* Use the first IO-APIC found. Multi-IOAPIC support is a
                 * future IOAPIC-subsystem audit. */
                if (info->ioapic_address == 0) {
                    info->ioapic_address = ioapic->io_apic_address;
                    info->ioapic_id = ioapic->io_apic_id;
                    info->ioapic_gsi_base = ioapic->gsi_base;
                }
                break;
            }

            case MADT_TYPE_ISO: {
                madt_iso_t* iso = (madt_iso_t*)entry;
                debug_printf("[MADT]   ISO: bus=%u, source IRQ %u -> GSI %u, flags=0x%04x\n",
                             iso->bus, iso->source, iso->gsi, iso->flags);
                ioapic_register_iso(iso->source, iso->gsi, iso->flags);
                break;
            }

            case MADT_TYPE_LOCAL_APIC_NMI: {
                madt_lapic_nmi_t* nmi = (madt_lapic_nmi_t*)entry;
                if (nmi->lint > 1) {
                    debug_printf("[MADT]   LAPIC NMI: invalid LINT%u, dropping\n",
                                 nmi->lint);
                    break;
                }
                if (info->nmi_count >= MADT_MAX_NMI_ENTRIES) {
                    debug_printf("[MADT]   LAPIC NMI: storage full, dropping\n");
                    break;
                }
                madt_nmi_entry_t* slot = &info->nmi[info->nmi_count++];
                slot->acpi_processor_id = nmi->acpi_processor_id;
                slot->lint              = nmi->lint;
                slot->mps_flags         = nmi->flags;
                slot->valid             = true;
                debug_printf("[MADT]   LAPIC NMI: proc=%u LINT%u flags=0x%04x stored\n",
                             nmi->acpi_processor_id, nmi->lint, nmi->flags);
                break;
            }

            case MADT_TYPE_LAPIC_OVERRIDE: {
                madt_lapic_override_t* ovr = (madt_lapic_override_t*)entry;
                debug_printf("[MADT]   LAPIC Override: addr=0x%lx\n",
                             ovr->local_apic_address);
                info->lapic_address = (uintptr_t)ovr->local_apic_address;
                break;
            }

            case MADT_TYPE_LX2APIC: {
                madt_lx2apic_t* x = (madt_lx2apic_t*)entry;
                bool enabled = lapic_flags_enabled(x->flags);
                debug_printf("[MADT]   x2APIC: UID=%u APIC ID=%u %s\n",
                             x->acpi_processor_uid, x->x2apic_id,
                             lapic_flags_text(x->flags));
                if (info->bsp_lapic_found &&
                    !info->bsp_acpi_id_resolved &&
                    x->x2apic_id == bsp_apic_id) {
                    info->bsp_acpi_processor_id =
                        (uint8_t)(x->acpi_processor_uid & 0xFF);
                    info->bsp_acpi_id_resolved = true;
                }
                if (info->cpu_map_count < MADT_MAX_CPU_MAP) {
                    madt_cpu_map_t* m = &info->cpu_map[info->cpu_map_count++];
                    m->apic_id           = x->x2apic_id;
                    m->acpi_processor_id = x->acpi_processor_uid;
                    m->enabled           = enabled;
                    m->online_capable    = lapic_flags_online_capable(x->flags);
                }
                break;
            }

            case MADT_TYPE_LX2APIC_NMI: {
                madt_lx2apic_nmi_t* n = (madt_lx2apic_nmi_t*)entry;
                if (n->lint > 1) break;
                if (info->nmi_count >= MADT_MAX_NMI_ENTRIES) break;
                madt_nmi_entry_t* slot = &info->nmi[info->nmi_count++];
                /* Truncate UID to one byte for our processor-ID match
                 * — adequate until BoxOS supports >255 CPUs. */
                slot->acpi_processor_id = (n->acpi_processor_uid == 0xFFFFFFFFu)
                                          ? MADT_NMI_PROCESSOR_ALL
                                          : (uint8_t)(n->acpi_processor_uid & 0xFF);
                slot->lint      = n->lint;
                slot->mps_flags = n->flags;
                slot->valid     = true;
                debug_printf("[MADT]   x2APIC NMI: UID=%u LINT%u flags=0x%04x stored\n",
                             n->acpi_processor_uid, n->lint, n->flags);
                break;
            }

            case MADT_TYPE_NMI_SOURCE: {
                madt_nmi_source_t* src = (madt_nmi_source_t*)entry;
                if (info->nmi_source_count >= MADT_MAX_NMI_SOURCES) {
                    debug_printf("[MADT]   NMI Source: storage full, dropping\n");
                    break;
                }
                madt_nmi_source_info_t* slot =
                    &info->nmi_sources[info->nmi_source_count++];
                slot->gsi       = src->gsi;
                slot->mps_flags = src->flags;
                slot->valid     = true;
                debug_printf("[MADT]   NMI Source: GSI %u flags=0x%04x stored\n",
                             src->gsi, src->flags);
                break;
            }

            default:
                debug_printf("[MADT]   Type %u entry (len=%u) — not consumed here\n",
                             entry->type, entry->length);
                break;
        }

        ptr += entry->length;
    }

    /* The seats, said out loud with the APIC IDs the firmware reserved for
     * them. kprintf, not debug_printf: the number of processors this kernel
     * starts is smaller than the number the MADT lists, and an operator
     * counting cores in front of the machine needs the reason and the IDs —
     * a hot-add path, when there is one, starts from exactly this list.
     * Once per boot, not once per caller of this parser. */
    if (!g_seats_named) {
        g_seats_named = true;
        for (uint16_t i = 0; i < info->cpu_map_count; i++) {
            if (!info->cpu_map[i].online_capable) continue;
            kprintf("[MADT] APIC ID %u (ACPI processor %u) is an "
                    "Online-Capable seat: the firmware says hardware can "
                    "enable it during OS runtime, not that a processor is "
                    "there now — not started\n",
                    info->cpu_map[i].apic_id,
                    info->cpu_map[i].acpi_processor_id);
        }
    }

    if (info->lapic_address && info->ioapic_address) {
        info->valid = true;
        debug_printf("[MADT] %[S]MADT parsed: LAPIC=0x%lx IOAPIC=0x%lx BSP=%u NMI=%u%[D]\n",
                     info->lapic_address, info->ioapic_address,
                     info->bsp_lapic_id, info->nmi_count);
    } else {
        debug_printf("[MADT] %[E]MADT incomplete: LAPIC=0x%lx IOAPIC=0x%lx%[D]\n",
                     info->lapic_address, info->ioapic_address);
    }

    return info->valid ? ACPI_OK : ACPI_ERR_INVALID_TABLE;
}

uint8_t amp_collect_lapics(uint32_t* ids_out, uint8_t max_count) {
    if (!ids_out || max_count == 0) return 0;
    if (!g_acpi.initialized || !g_acpi.rsdp) return 0;

    acpi_madt_t* madt = (acpi_madt_t*)acpi_find_table("APIC");
    if (!madt) return 0;

    uint8_t count = 0;
    /* MADT ENTRIES, not processors: a firmware that double-lists one
     * not-Enabled processor as both Type 0 and Type 9 contributes two of
     * them, and unlike the accepted ids below there is nothing to compare a
     * rejected entry against — its id is exactly what this kernel refuses to
     * take. Counted in 32 bits because the MADT's own length is, and a table
     * with more than 255 such entries would otherwise wrap the count to
     * silence. */
    uint32_t deferred = 0;
    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    /* Collect BOTH MADT Type 0 (Processor Local APIC, 8-bit id) and Type 9
     * (Processor Local x2APIC, 32-bit id). ACPI 6.5 §5.2.12.2 / §5.2.12.12:
     * a logical processor with id < 255 is listed via Type 0 (even in x2APIC
     * mode), and any processor with id >= 255 MUST be listed via Type 9. A
     * machine with >254 logical processors therefore interleaves both.
     *
     * Only Enabled processors, per Table 5.23 — see lapic_flags_enabled
     * above. This list is what amp_boot_aps() sends INIT and SIPI to and
     * what amp_init() derives the K-Core split from, so an Online-Capable
     * slot in it is a processor the firmware said is not there being given a
     * kernel stack, two IPIs and a second of waiting, and a core count with
     * nothing behind it. */
    while (ptr + sizeof(madt_entry_header_t) <= end && count < max_count) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;
        if (entry->length < sizeof(madt_entry_header_t)) break;
        if (ptr + entry->length > end) break;

        uint32_t id = 0xFFFFFFFFu;   /* sentinel: "no usable id from this entry" */

        if (entry->type == MADT_TYPE_LOCAL_APIC &&
            entry->length >= sizeof(madt_local_apic_t)) {
            madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
            if (lapic_flags_enabled(lapic->flags))
                id = lapic->apic_id;                 /* 8-bit xAPIC id */
            else if (lapic_flags_online_capable(lapic->flags))
                deferred++;
        } else if (entry->type == MADT_TYPE_LX2APIC &&
                   entry->length >= sizeof(madt_lx2apic_t)) {
            madt_lx2apic_t* x2 = (madt_lx2apic_t*)entry;
            /* x2apic_id == 0xFFFFFFFF marks an unusable slot — skip it. */
            if (x2->x2apic_id != 0xFFFFFFFFu) {
                if (lapic_flags_enabled(x2->flags))
                    id = x2->x2apic_id;              /* 32-bit x2APIC id */
                else if (lapic_flags_online_capable(x2->flags))
                    deferred++;
            }
        }

        if (id != 0xFFFFFFFFu) {
            /* A processor is listed in exactly one of Type 0 / Type 9, but be
             * defensive against firmware that double-lists it. */
            bool dup = false;
            for (uint8_t i = 0; i < count; i++)
                if (ids_out[i] == id) { dup = true; break; }
            if (!dup)
                ids_out[count++] = id;
        }
        ptr += entry->length;
    }

    /* kprintf, not debug_printf: on a board with hot-add sockets the number
     * of processors this kernel starts is smaller than the number the MADT
     * lists, and the operator counting cores in front of the machine is the
     * one person who needs to read why. */
    if (deferred) {
        kprintf("[MADT] %u of this MADT's processor entries are "
                "Online-Capable but not Enabled — firmware says those "
                "processors can be added during OS runtime, not that they "
                "are here now; the %u Enabled processor(s) are what this "
                "kernel starts\n",
                (unsigned)deferred, count);
    }

    return count;
}
