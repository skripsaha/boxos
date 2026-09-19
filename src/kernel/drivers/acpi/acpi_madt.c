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

/* One Online-Capable seat, said out loud with the APIC ID the firmware
 * reserved for it. kprintf, not debug_printf: the number of processors this
 * kernel starts is smaller than the number the MADT lists, and an operator
 * counting cores in front of the machine needs the reason and the IDs — a
 * hot-add path, when there is one, starts from exactly this list. */
static void name_online_capable_seat(uint32_t apic_id, uint32_t acpi_uid) {
    kprintf("[MADT] APIC ID %u (ACPI processor %u) is an Online-Capable "
            "seat: the firmware says hardware can enable it during OS "
            "runtime, not that a processor is there now — not started\n",
            apic_id, acpi_uid);
}

acpi_error_t acpi_parse_madt(madt_info_t* info) {
    if (!info) return ACPI_ERR_INVALID_TABLE;

    /* Sampled before the walk and acted on during it, so the seats are
     * named in table order by whichever caller parses first and by nobody
     * after. g_seats_named is raised when this walk ends. */
    const bool name_seats = !g_seats_named;

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

    /* Where the table is, kept for every question this parse does not
     * answer on the spot — the NMI rules, the UID of a processor an AP finds
     * itself to be, the roll call of processors no destination field can
     * name. Copying those into arrays needed a ceiling; asking the table
     * needs none. */
    info->table = madt;
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
    debug_printf("[MADT] BSP APIC ID: %u\n", bsp_apic_id);
    /* The BSP being unreachable is not said here. It is said once, in
     * madt_name_unreachable_processors, beside every other processor in the
     * same condition — this function runs twice per boot and would say it
     * twice, and a machine that cannot route to its BSP almost certainly
     * cannot route to its other processors either. One list, or none. */

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
                debug_printf("[MADT]   LAPIC: ACPI ID=%u, APIC ID=%u, %s\n",
                             lapic->acpi_processor_id, lapic->apic_id,
                             lapic_flags_text(lapic->flags));
                /* Resolve the BSP's ACPI Processor UID by matching the APIC
                 * ID CPUID reported for the running CPU. */
                if (info->bsp_lapic_found &&
                    !info->bsp_acpi_uid_resolved &&
                    (uint32_t)lapic->apic_id == bsp_apic_id) {
                    info->bsp_acpi_processor_uid = lapic->acpi_processor_id;
                    info->bsp_acpi_uid_resolved  = true;
                }
                if (name_seats && lapic_flags_online_capable(lapic->flags))
                    name_online_capable_seat(lapic->apic_id,
                                             lapic->acpi_processor_id);
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
                /* Not copied out: madt_for_each_nmi_rule reads this entry
                 * from the table when a processor asks which rules are its
                 * own. Nothing here can be dropped for want of room. */
                madt_lapic_nmi_t* nmi = (madt_lapic_nmi_t*)entry;
                debug_printf("[MADT]   LAPIC NMI: proc=%u LINT%u flags=0x%04x\n",
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
                debug_printf("[MADT]   x2APIC: UID=%u APIC ID=%u %s\n",
                             x->acpi_processor_uid, x->x2apic_id,
                             lapic_flags_text(x->flags));
                /* The UID is taken whole. This is the entry type a machine
                 * with 255-or-more processors MUST use (ACPI 6.5 §5.2.12.12),
                 * so it is exactly the entry whose UID does not fit a byte. */
                if (info->bsp_lapic_found &&
                    !info->bsp_acpi_uid_resolved &&
                    x->x2apic_id == bsp_apic_id) {
                    info->bsp_acpi_processor_uid = x->acpi_processor_uid;
                    info->bsp_acpi_uid_resolved = true;
                }
                if (name_seats && lapic_flags_online_capable(x->flags))
                    name_online_capable_seat(x->x2apic_id,
                                             x->acpi_processor_uid);
                break;
            }

            case MADT_TYPE_LX2APIC_NMI: {
                /* Likewise read from the table, at the full 32 bits Table
                 * 5.35 gives the UID. This used to be truncated to a byte
                 * on the way into a cache, which on the one machine class
                 * that publishes these entries — any with an x2APIC ID of
                 * 255 or greater — handed one processor's NMI rule to
                 * another whose UID happened to share the low eight bits. */
                madt_lx2apic_nmi_t* n = (madt_lx2apic_nmi_t*)entry;
                debug_printf("[MADT]   x2APIC NMI: UID=%u LINT%u flags=0x%04x\n",
                             n->acpi_processor_uid, n->lint, n->flags);
                break;
            }

            case MADT_TYPE_NMI_SOURCE: {
                /* Read from the table by madt_for_each_nmi_source. */
                madt_nmi_source_t* src = (madt_nmi_source_t*)entry;
                debug_printf("[MADT]   NMI Source: GSI %u flags=0x%04x\n",
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

    g_seats_named = true;

    if (info->lapic_address && info->ioapic_address) {
        info->valid = true;
        debug_printf("[MADT] %[S]MADT parsed: LAPIC=0x%lx IOAPIC=0x%lx BSP=%u%[D]\n",
                     info->lapic_address, info->ioapic_address,
                     info->bsp_lapic_id);
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

        /* "This entry yielded a processor to start" is a fact of its own,
         * not a reserved value of the id: every 32-bit pattern is a legal
         * APIC ID somewhere, and the one that is not — all-ones, the x2APIC
         * broadcast — is refused below on its own terms rather than doubling
         * as the marker for "nothing here". */
        uint32_t id = 0;
        bool have_id = false;

        if (entry->type == MADT_TYPE_LOCAL_APIC &&
            entry->length >= sizeof(madt_local_apic_t)) {
            madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
            if (lapic_flags_enabled(lapic->flags)) {
                id = lapic->apic_id;                 /* 8-bit xAPIC id */
                have_id = true;
            } else if (lapic_flags_online_capable(lapic->flags)) {
                deferred++;
            }
        } else if (entry->type == MADT_TYPE_LX2APIC &&
                   entry->length >= sizeof(madt_lx2apic_t)) {
            madt_lx2apic_t* x2 = (madt_lx2apic_t*)entry;
            if (lapic_flags_enabled(x2->flags)) {
                /* An all-ones ID used to be skipped here as "an unusable
                 * slot", which is a reading of the ID that no specification
                 * supports: ACPI 6.5 Table 5.34 reserves no such value, and
                 * whether a seat holds a processor is what the FLAGS above
                 * just answered. What all-ones really is, is the x2APIC
                 * broadcast destination (LAPIC_X2APIC_DEST_BROADCAST) — an
                 * ID that names every processor and therefore none. Starting
                 * it would send INIT to the whole machine, so it is refused;
                 * refused ALOUD, because a processor the firmware marked
                 * Enabled and this kernel will not start is exactly what
                 * this wave exists to stop hiding. */
                if (x2->x2apic_id == LAPIC_X2APIC_DEST_BROADCAST) {
                    kprintf("[MADT] %[E]ACPI processor %u is listed as "
                            "Enabled with APIC ID 0xFFFFFFFF, which is the "
                            "x2APIC broadcast destination and names every "
                            "processor at once — not started, because an "
                            "INIT sent there would reset this machine%[D]\n",
                            x2->acpi_processor_uid);
                } else {
                    id = x2->x2apic_id;              /* 32-bit x2APIC id */
                    have_id = true;
                }
            } else if (lapic_flags_online_capable(x2->flags)) {
                deferred++;
            }
        }

        if (have_id) {
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

/* ======================================================================
 * Questions put to the table, not to a copy of it.
 *
 * Everything below walks the MADT that acpi_parse_madt() recorded in
 * madt_info_t::table. The walk is bounded by the length the table declares
 * in its own ACPI header and by the end pointer derived from it, so there is
 * no entry ceiling to exceed and no tail to drop: the firmware said how much
 * it wrote, and that is the only number involved.
 *
 * All four are read-only over memory that is already mapped, take no lock
 * and call nothing in the ACPI table subsystem. That is what makes them safe
 * on an AP in the middle of bring-up, where acpi_find_table() — which fills
 * a global cache and may map — is not.
 * ====================================================================== */

static bool madt_walk_begin(const madt_info_t *info,
                            const uint8_t **cursor, const uint8_t **end) {
    if (!info || !info->table) return false;
    const acpi_madt_t *m = info->table;
    if (m->header.length < sizeof(acpi_madt_t)) return false;
    *cursor = (const uint8_t *)m + sizeof(acpi_madt_t);
    *end    = (const uint8_t *)m + m->header.length;
    return true;
}

/* The next entry, or NULL at the end of the table — and NULL also for an
 * entry whose declared length is impossible or reaches past the table's own
 * end, because past that point the bytes are not entries and walking them is
 * reading whatever follows the table in memory. */
static const madt_entry_header_t *madt_walk_next(const uint8_t **cursor,
                                                 const uint8_t *end) {
    if (*cursor + sizeof(madt_entry_header_t) > end) return NULL;
    const madt_entry_header_t *e = (const madt_entry_header_t *)*cursor;
    if (e->length < sizeof(madt_entry_header_t)) return NULL;
    if (*cursor + e->length > end) return NULL;
    *cursor += e->length;
    return e;
}

void madt_for_each_nmi_rule(const madt_info_t *info,
                            uint32_t acpi_processor_uid, bool uid_known,
                            MadtNmiRuleFn fn, void *ctx) {
    const uint8_t *cursor, *end;
    if (!fn || !madt_walk_begin(info, &cursor, &end)) return;

    const madt_entry_header_t *e;
    while ((e = madt_walk_next(&cursor, end)) != NULL) {
        MadtNmiRule rule;

        if (e->type == MADT_TYPE_LOCAL_APIC_NMI &&
            e->length >= sizeof(madt_lapic_nmi_t)) {
            const madt_lapic_nmi_t *n = (const madt_lapic_nmi_t *)e;
            rule.acpi_processor_uid = n->acpi_processor_id;
            rule.all_processors     = (n->acpi_processor_id == 0xFFu);
            rule.lint               = n->lint;
            rule.mps_flags          = n->flags;
        } else if (e->type == MADT_TYPE_LX2APIC_NMI &&
                   e->length >= sizeof(madt_lx2apic_nmi_t)) {
            const madt_lx2apic_nmi_t *n = (const madt_lx2apic_nmi_t *)e;
            rule.acpi_processor_uid = n->acpi_processor_uid;
            rule.all_processors     = (n->acpi_processor_uid == 0xFFFFFFFFu);
            rule.lint               = n->lint;
            rule.mps_flags          = n->flags;
        } else {
            continue;
        }

        /* A LAPIC has LINT0 and LINT1 and nothing else; an entry naming any
         * other pin describes hardware that is not there. */
        if (rule.lint > 1) continue;

        if (!rule.all_processors &&
            (!uid_known || rule.acpi_processor_uid != acpi_processor_uid))
            continue;

        fn(&rule, ctx);
    }
}

void madt_for_each_nmi_source(const madt_info_t *info,
                              MadtNmiSourceFn fn, void *ctx) {
    const uint8_t *cursor, *end;
    if (!fn || !madt_walk_begin(info, &cursor, &end)) return;

    const madt_entry_header_t *e;
    while ((e = madt_walk_next(&cursor, end)) != NULL) {
        if (e->type != MADT_TYPE_NMI_SOURCE) continue;
        if (e->length < sizeof(madt_nmi_source_t)) continue;
        const madt_nmi_source_t *src = (const madt_nmi_source_t *)e;
        fn(src->gsi, src->flags, ctx);
    }
}

bool madt_uid_for_apic_id(const madt_info_t *info, uint32_t apic_id,
                          uint32_t *out_uid) {
    const uint8_t *cursor, *end;
    if (!out_uid || !madt_walk_begin(info, &cursor, &end)) return false;

    const madt_entry_header_t *e;
    while ((e = madt_walk_next(&cursor, end)) != NULL) {
        if (e->type == MADT_TYPE_LOCAL_APIC &&
            e->length >= sizeof(madt_local_apic_t)) {
            const madt_local_apic_t *l = (const madt_local_apic_t *)e;
            if ((uint32_t)l->apic_id == apic_id) {
                *out_uid = l->acpi_processor_id;
                return true;
            }
        } else if (e->type == MADT_TYPE_LX2APIC &&
                   e->length >= sizeof(madt_lx2apic_t)) {
            const madt_lx2apic_t *x = (const madt_lx2apic_t *)e;
            if (x->x2apic_id == apic_id) {
                *out_uid = x->acpi_processor_uid;
                return true;
            }
        }
    }
    return false;
}

/*
 * The processors this machine cannot hand a device interrupt to, named.
 *
 * Two fields decide it, and they are the same eight bits twice. Intel SDM
 * Vol 3A, "Message Address Register Format": "Destination ID — This field
 * contains an 8-bit destination ID. It identifies the message's target
 * processor(s). The destination ID corresponds to bits 63:56 of the I/O APIC
 * Redirection Table Entry if the IOAPIC is used to dispatch the interrupt to
 * the processor(s)." So a legacy IRQ line and an MSI message reach a
 * processor by exactly the same eight-bit name, and the section "Physical
 * Destination Mode" says what those eight bits can spell: "either a single
 * destination (local APIC IDs 00H through FEH) or a broadcast to all APICs
 * (the APIC ID is FFH)". 255 processors, and one word for everybody. (Both
 * cited by title; the section numbers move between SDM revisions — see
 * LAPIC_XAPIC_DEST_MAX in lapic.h, where the full quotes and the editions
 * they were read from are kept.)
 *
 * ACPI draws the line in the same place and for the same reason — §5.2.12.12:
 * "Logical processors with APIC ID values 255 and greater must use the
 * Processor Local x2APIC structure" — so this list is, exactly, the Type 9
 * entries whose X2APIC ID reached that far.
 *
 * Said ONCE, here, where the machine's composition is already known: the
 * MADT is the complete list of the processors the firmware published, and it
 * is known before the first AP is started and long before the first MSI is
 * armed. The alternative — a line each time a destination is refused — is
 * one line per device per boot for the same handful of processors, and it
 * arrives too late to be a description of the machine.
 *
 * What the named processors DO get: everything that does not go through an
 * eight-bit destination. In x2APIC mode the ICR carries a 32-bit destination
 * (MSR 0x830), so IPIs, the scheduler wake, TLB shootdown and the panic
 * broadcast all reach them, and their own LAPIC timer is their own. They run
 * — they just cannot be the target of a device's interrupt.
 *
 * What would lift it is interrupt remapping (VT-d IR / AMD-Vi), whose IRTE
 * carries the destination at full width and turns both of those fields into
 * an index. It is not armed in this kernel, so the refusal stands and is
 * named rather than guessed around.
 *
 * HOT-ADD SEATS ARE IN THIS LIST TOO, and that is the point of naming it at
 * boot rather than at the first refused line. An Online-Capable entry is a
 * socket the firmware says hardware can fill during OS runtime (ACPI 6.5
 * Table 5.23); if the APIC ID it reserved for that socket is one the
 * destination fields cannot spell, the operator wants to know BEFORE a
 * processor goes into it, not after the machine is running and the new core
 * takes no device interrupt. An entry the firmware marked neither Enabled
 * nor Online-Capable is skipped, because the same table tells OSPM to
 * "ignore the contents of the Processor Local APIC Structure" — with one
 * exception, the processor executing this code, which is there whatever the
 * table says about it.
 */
void madt_name_unreachable_processors(const madt_info_t *info) {
    static bool named = false;

    const uint8_t *cursor, *end;
    if (named || !madt_walk_begin(info, &cursor, &end)) return;
    named = true;

    uint32_t count = 0;
    bool bsp_listed = false;

    const madt_entry_header_t *e;
    while ((e = madt_walk_next(&cursor, end)) != NULL) {
        uint32_t apic_id, uid, flags;

        if (e->type == MADT_TYPE_LOCAL_APIC &&
            e->length >= sizeof(madt_local_apic_t)) {
            /* A Type 0 APIC ID is one byte by format (Table 5.23), so the
             * width test below can only catch it at one value: 0xFF, which
             * is the broadcast and not processor 255. Firmware that puts a
             * processor there is already outside ACPI 6.5 §5.2.12.12 ("255
             * and greater must use the Processor Local x2APIC structure"),
             * and it is named like any other processor we cannot address.
             * Walked rather than assumed for that reason: the test belongs
             * to the field, not to the entry type — and this is also where
             * the BSP of every machine we have is found and confirmed
             * reachable. */
            const madt_local_apic_t *l = (const madt_local_apic_t *)e;
            apic_id = l->apic_id;
            uid     = l->acpi_processor_id;
            flags   = l->flags;
        } else if (e->type == MADT_TYPE_LX2APIC &&
                   e->length >= sizeof(madt_lx2apic_t)) {
            const madt_lx2apic_t *x = (const madt_lx2apic_t *)e;
            apic_id = x->x2apic_id;
            uid     = x->acpi_processor_uid;
            flags   = x->flags;
        } else {
            continue;
        }

        /* Whether this entry is the processor running the kernel is asked
         * of the ID alone, before anything is decided about the flags. It
         * used to be asked only of Enabled entries, so a firmware that
         * lists its BSP with Enabled clear — broken, but real — made the
         * fallback below announce that the MADT "does not list it at all".
         * A roll call that exists to name things truthfully cannot carry a
         * sentence that the table in front of it contradicts. */
        const bool is_bsp = info->bsp_lapic_found &&
                            apic_id == info->bsp_lapic_id;
        if (is_bsp) bsp_listed = true;

        const bool enabled = lapic_flags_enabled(flags);
        const bool seat    = lapic_flags_online_capable(flags);

        /* ACPI 6.5 Table 5.23, with neither bit set: "this processor is
         * unusable, and OSPM shall ignore the contents of the Processor
         * Local APIC Structure". There is nobody to name. The exception is
         * the processor executing this code, which is demonstrably here
         * whatever its entry claims. */
        if (!enabled && !seat && !is_bsp) continue;

        const char *who = is_bsp ? " — the BSP, this CPU —" : "";

        /* All-ones is not an empty socket; it is the x2APIC broadcast
         * destination, so this entry names every processor and therefore
         * none of them. Named here rather than skipped, because a seat the
         * firmware published and this kernel can never address is exactly
         * what this list is for. */
        if (apic_id == LAPIC_X2APIC_DEST_BROADCAST) {
            count++;
            kprintf("[MADT] %[E]ACPI processor %u%s is listed with APIC ID "
                    "0xFFFFFFFF, the x2APIC broadcast destination: that ID "
                    "names every processor at once, so nothing — no device "
                    "interrupt and no IPI — can be addressed to this one "
                    "alone%[D]\n", uid, who);
            continue;
        }

        if (apic_id <= LAPIC_XAPIC_DEST_MAX) continue;

        count++;
        if (enabled || is_bsp) {
            kprintf("[MADT] %[E]APIC ID %u (ACPI processor %u)%s receives no "
                    "legacy IRQ and no MSI: both name their target with the "
                    "same eight bits, which reach 254 and then mean "
                    "broadcast%[D]\n", apic_id, uid, who);
        } else {
            kprintf("[MADT] %[E]APIC ID %u (ACPI processor %u) is an "
                    "Online-Capable seat that no device interrupt could ever "
                    "reach: a processor put there would run and take IPIs, "
                    "but the eight bits a redirection entry and an MSI "
                    "message address carry reach 254 and then mean "
                    "broadcast%[D]\n", apic_id, uid);
        }
    }

    /* The running processor, when the firmware's list does not contain it.
     * detect_bsp_apic_id() asked the CPU itself, so this ID is real whatever
     * the table says; a machine whose MADT omits the processor executing the
     * kernel is broken, and if that ID is also too wide to name, it is
     * broken in the way that costs it every device interrupt. */
    if (!bsp_listed && info->bsp_lapic_found &&
        info->bsp_lapic_id > LAPIC_XAPIC_DEST_MAX) {
        count++;
        kprintf("[MADT] %[E]APIC ID %u — the BSP, this CPU — receives no "
                "legacy IRQ and no MSI, and the MADT does not list it at "
                "all, so it has no ACPI processor UID to name it by%[D]\n",
                info->bsp_lapic_id);
    }

    if (count) {
        kprintf("[MADT] %[E]%u processor(s) and hot-add seat(s) above cannot "
                "be the target of a device interrupt on this machine. The "
                "ones that are running still run and still take IPIs in "
                "x2APIC mode; only the IOAPIC redirection entry and the MSI "
                "message address are too narrow to name them, and only "
                "interrupt remapping (VT-d IR / AMD-Vi), which this kernel "
                "does not arm, would widen them%[D]\n", count);
    }
}
