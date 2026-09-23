#include "irqchip.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "acpi_madt.h"
#include "idt.h"        /* SummonsPost — the registry keeps the line it was given */
#include "cpuid.h"
#include "klib.h"
#include "io.h"

// Current interrupt controller state
static irqchip_type_t active_type = IRQCHIP_PIC;
static irq_chip_t* active_chip = NULL;
static madt_info_t madt_info;

// --- PIC backend (wraps existing pic.c functions) ---

static void pic_chip_eoi(uint8_t gsi) {
    if (gsi < 16) pic_send_eoi(gsi);
}

static irq_chip_t pic_chip = {
    .name       = "8259A PIC",
    .send_eoi   = pic_chip_eoi,
};

// --- APIC backend (LAPIC + IO-APIC) ---

/* One LAPIC EOI, and for a LEVEL line that is not the whole of it.
 *
 * Intel SDM Vol 3A, "EOI Register": "Upon receiving an EOI, the APIC clears
 * the highest priority bit in the ISR and dispatches the next highest
 * priority interrupt to the processor. If the terminated interrupt was a
 * level-triggered interrupt, the local APIC also sends an end-of-interrupt
 * message to all I/O APICs." That broadcast is what clears the redirection
 * entry's Remote IRR — set, per the same chapter, "when the local APIC
 * accepts the interrupt for servicing and ... reset when an EOI command is
 * received" — and a pin whose Remote IRR stays set never delivers again.
 *
 * So the one write below IS the level path, on the single condition that the
 * broadcast has not been suppressed. irqchip_init makes sure of that on the
 * BSP; see the SVR bit 12 sentence there. The gsi is unused because the
 * broadcast names the vector, not the pin — the alternative, a directed EOI
 * to one IOAPIC's own EOI register, is what SDM offers to software that HAS
 * suppressed the broadcast, and this kernel has not. */
static void apic_chip_eoi(uint8_t gsi) {
    (void)gsi;
    lapic_send_eoi();
}

/* No get_isr / get_irr here any more, and no max_irqs.
 *
 * The two readers took LAPIC ISR[1] and IRR[1] — the words covering vectors 32
 * to 63 — and nothing in the kernel ever called them. They were a window into
 * exactly the range a line could occupy back when a vector was 32 plus a pin
 * number, so once lines could be delivered anywhere in the pool they would
 * have answered "nothing pending" about most of the machine. Deleting beats
 * widening: if a caller ever needs to know whether a particular vector is
 * pending, it should ask about THAT vector rather than be handed a word and
 * left to work out what it covers.
 *
 * max_irqs went with them. It answered with the width of a vector window and
 * was read where the caller meant a count of pins; the chip's own register is
 * the only honest source for that, and IoapicMaskEveryPin asks it. */
static irq_chip_t apic_chip = {
    .name       = "APIC (LAPIC + IO-APIC)",
    .send_eoi   = apic_chip_eoi,
};

// --- Detection ---

static bool cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;  // CPUID.1:EDX[9] = APIC
}

static void pic_disable_for_apic(void) {
    // Remap PIC to vectors 0x20-0x2F (same as before) then mask all
    // This prevents PIC from firing spurious interrupts on APIC vectors
    pic_init();
    pic_set_mask(0xFF, 0xFF);

    debug_printf("[IRQCHIP] Legacy PIC masked for APIC mode\n");
}

/* One MADT NMI Source, routed to the destination the caller chose. The
 * destination travels as the context because ioapic_program_nmi_source
 * takes it at full width and refuses it there if the redirection entry
 * cannot name it — this function has no opinion of its own to add. */
static void irqchip_program_nmi_source(uint32_t gsi, uint16_t mps_flags,
                                       void *ctx) {
    ioapic_program_nmi_source(gsi, mps_flags, *(const uint32_t *)ctx);
}

// --- Public API ---

void irqchip_init(void) {
    debug_printf("[IRQCHIP] Detecting interrupt controller...\n");

    // Step 1: Check CPU APIC support
    if (!cpu_has_apic()) {
        debug_printf("[IRQCHIP] CPU does not support APIC, using legacy PIC\n");
        goto use_pic;
    }

    debug_printf("[IRQCHIP] CPU supports APIC\n");

    // Step 2: Parse MADT to find APIC addresses
    acpi_error_t err = acpi_parse_madt(&madt_info);
    if (err != ACPI_OK || !madt_info.valid) {
        debug_printf("[IRQCHIP] MADT not available, using legacy PIC\n");
        goto use_pic;
    }

    /* Step 2.5: name the processors no device interrupt can reach.
     *
     * Here, and not later, because here is the first moment the machine's
     * composition is known and nothing has been armed yet: the MADT is the
     * firmware's complete list of processors, this runs before the first AP
     * is started and long before the first MSI. The answer does not depend
     * on which backend is chosen below — an IOAPIC redirection entry and an
     * MSI message address carry the same eight destination bits whatever
     * this kernel does with them — so a fall back to the PIC after this
     * point changes nothing about who is named.
     *
     * The two exits ABOVE are a different matter and this list cannot cover
     * them: a CPU with no APIC at all and a machine with no MADT both leave
     * by `goto use_pic` before this line, and in neither case is there a
     * list of processors to read names out of. There is no processor to
     * name in the first (one CPU, one PIC, sixteen lines) and no firmware
     * statement to name one from in the second.
     *
     * Prints nothing on a machine whose APIC IDs all fit. */
    madt_name_unreachable_processors(&madt_info);

    // Step 3: Disable legacy PIC (mask all, but keep initialized for spurious handling)
    pic_disable_for_apic();

    // Step 4: Initialize Local APIC
    lapic_init(madt_info.lapic_address);
    if (!lapic_is_enabled()) {
        debug_printf("[IRQCHIP] %[E]LAPIC init failed, falling back to PIC%[D]\n");
        goto use_pic;
    }

    /* Step 4.5: Program LVT LINT0/LINT1 from MADT NMI rules on the BSP.
     * ACPI 6.5 §5.2.12.7 makes this OSPM's responsibility — without it the
     * firmware-asserted NMI source (watchdog, IPMI alert) never reaches
     * us. AP cores apply the same rules during their bring-up.
     *
     * An unresolved UID is not a reason to skip the step, as it used to be:
     * the firmware may have written one rule for every processor, and that
     * rule is this core's whether or not the table also names it
     * individually. What the unresolved case must NOT do is claim a
     * UID-addressed rule, and `uid_known` says so rather than leaving it to
     * a sentinel that could collide with a real UID. */
    lapic_apply_madt_nmi(&madt_info, madt_info.bsp_acpi_processor_uid,
                         madt_info.bsp_acpi_uid_resolved);
    if (!madt_info.bsp_acpi_uid_resolved) {
        debug_printf("[IRQCHIP] BSP ACPI processor UID unresolved; only "
                     "all-processor NMI rules were applied\n");
    }

    /* Step 4.7: let a level-triggered EOI reach the IOAPIC.
     *
     * Intel SDM Vol 3A, "EOI Register": "Software can inhibit the broadcast
     * of EOI message by setting bit 12 of the Spurious Interrupt Vector
     * Register ... If this bit is set, a broadcast EOI is not generated on an
     * EOI cycle even if the associated TMR bit indicates that the current
     * interrupt was level-triggered. The default value for the bit is 0."
     *
     * Default, not guaranteed: lapic_init composes SVR with a read-modify-
     * write, so whatever state the APIC was handed over in survives into bit
     * 12. With every line this kernel armed being edge triggered, a set bit
     * there changed nothing and could sit unnoticed for ever. From this
     * commit PCI INTx lines are level, and on a machine that arrives with the
     * bit set each of them would deliver exactly ONE interrupt and then stop:
     * the EOI would clear the local ISR and never reach the redirection
     * entry, whose Remote IRR would stay set for the rest of the session.
     * That is a disk that answers once and a keyboard that types one letter.
     *
     * The alternative SDM offers is the directed EOI — keep the suppression
     * and write each IOAPIC's own EOI register per interrupt. It buys nothing
     * here (one IOAPIC, and the broadcast costs a 14-cycle message on a bus
     * no modern part even uses serially) and it would put a second, easily
     * forgotten write on the hot path of every level line. So the broadcast
     * is what this kernel relies on, and this is where it is made true.
     *
     * The BSP only, deliberately: every redirection entry programmed below
     * carries madt_info.bsp_lapic_id as its destination, so the BSP is the
     * only core that ever acknowledges a level line. An AP that starts with
     * the bit set suppresses a broadcast it would never have sent. */
    {
        const uint32_t svr_eoi_broadcast_suppress = 1u << 12;
        uint32_t svr = lapic_read(LAPIC_REG_SVR);
        if (svr & svr_eoi_broadcast_suppress) {
            lapic_write(LAPIC_REG_SVR, svr & ~svr_eoi_broadcast_suppress);
            kprintf("[IRQCHIP] this machine handed over its local APIC with "
                    "EOI-broadcast suppression on (SVR=0x%08x); cleared, or "
                    "every level-triggered line would have delivered once and "
                    "then held its Remote IRR for ever\n", svr);
        }
    }

    // Step 5: Initialize IO-APIC
    ioapic_init(madt_info.ioapic_address, madt_info.ioapic_gsi_base);

    /* Step 5.5: Program MADT NMI Source entries (Type 3) into the IOAPIC.
     * ACPI 6.5 §5.2.12.6: any IOAPIC input that the platform wires as a
     * non-maskable interrupt source — typically a server-grade watchdog
     * or chassis intrusion alert — must be programmed by OSPM with
     * Delivery Mode = 100 (NMI). Route to the BSP; servers that need
     * cluster-wide NMI broadcast will be revisited when the SMP audit
     * adds multi-IOAPIC and cluster mode. */
    {
        uint32_t nmi_source_dest = madt_info.bsp_lapic_id;
        madt_for_each_nmi_source(&madt_info, irqchip_program_nmi_source,
                                 &nmi_source_dest);
    }

    // Step 6: Activate APIC backend
    active_type = IRQCHIP_APIC;
    active_chip = &apic_chip;

    debug_printf("[IRQCHIP] %[S]Using APIC mode (LAPIC + IO-APIC)%[D]\n");
    return;

use_pic:
    // Initialize PIC (already done in kernel_main, but ensure state is clean)
    pic_init();

    active_type = IRQCHIP_PIC;
    active_chip = &pic_chip;

    debug_printf("[IRQCHIP] %[S]Using legacy PIC mode (16 IRQs)%[D]\n");
}

const struct madt_info *irqchip_get_madt(void) {
    if (active_type != IRQCHIP_APIC) return NULL;
    return &madt_info;
}

void irqchip_apply_lapic_nmi_self(void) {
    if (active_type != IRQCHIP_APIC) return;

    /* Which processor this core is, asked of the MADT itself. The answer
     * used to come from a 64-entry copy of the table made during the parse,
     * which meant that on a machine with more processors than that the
     * cores past the sixty-fourth could not find themselves and silently
     * took only the all-processor rules. The table has no such limit, and
     * reading it here is safe on an AP: madt_uid_for_apic_id only walks
     * memory the BSP already mapped.
     *
     * WHAT IT COSTS, said plainly because it is not free. ACPI tables are
     * mapped uncacheable (acpi_map_physical -> vmm_map_mmio, PCD+PWT), so
     * these are uncached reads, and there are two walks per core: this one
     * for the UID, then another inside lapic_apply_madt_nmi for the rules.
     * On a 300-processor machine with a MADT of several hundred entries
     * that is two full table passes per core during bring-up — the one
     * class of machine where the price is highest is the same class this
     * walk exists for. It is paid once per core, on a path that already
     * sends an INIT and two SIPIs and waits for the core to answer, and the
     * copy it replaced bought its speed by dropping every processor past
     * the sixty-fourth. A cache of the whole table would be the honest
     * alternative if this ever shows up in bring-up time; a ceiling on how
     * much of it to cache would not. */
    uint32_t uid = 0;
    bool uid_known = madt_uid_for_apic_id(&madt_info, lapic_get_id(), &uid);

    lapic_apply_madt_nmi(&madt_info, uid, uid_known);
}

void irqchip_enable_irq(uint8_t irq) {
    Summons line = SummonsForIsaLine(irq);
    SummonsArm(&line);
}

void irqchip_disable_irq(uint8_t irq) {
    Summons line = SummonsForIsaLine(irq);
    SummonsDisarm(&line);
}

void irqchip_send_eoi(uint8_t gsi) {
    if (active_chip) {
        active_chip->send_eoi(gsi);
    }
}

irqchip_type_t irqchip_get_type(void) {
    return active_type;
}

const char* irqchip_get_name(void) {
    return active_chip ? active_chip->name : "none";
}

/* ===========================================================================
 * Summons
 * ========================================================================= */

const char *SummonsSourceName(SummonsSource from) {
    switch (from) {
    case SUMMONS_FROM_BUS_RULE:          return "the rule of its bus";
    case SUMMONS_FROM_INTERRUPT_LINE:    return "the byte the firmware wrote "
                                                "for the 8259";
    case SUMMONS_FROM_FIRMWARE_OVERRIDE: return "the firmware's own override";
    case SUMMONS_FROM_ROUTING_TABLE:     return "the ACPI routing table";
    case SUMMONS_FROM_NOBODY:            break;
    }
    return "nobody";
}

Summons SummonsForIsaLine(uint8_t isa_irq) {
    /* The ISA rule, said out loud instead of arrived at by omission. ACPI 6.5
     * §5.2.12.5 Table 5.26 names this bus when it defines the trigger default:
     * "00 Conforms to specifications of the bus (For example, ISA is
     * edge-triggered)", and the polarity default in the same table: "00
     * Conforms to the specifications of the bus (for example, EISA is
     * active-low for level-triggered interrupts)" — active high for the
     * edge-triggered ISA line this is. Exclusive, because an ISA interrupt is
     * one wire from one device: there is no open-drain wire-OR to share. */
    uint32_t gsi = ioapic_isa_to_gsi(isa_irq);

    Summons line = {
        .gsi         = gsi,
        /* No vector yet. A line is given one when it settles on a pin that
         * nobody else holds, and not before: SummonsForIsaLine is called more
         * than once per line in a boot -- irq_register_handler builds one and
         * irqchip_enable_irq builds another -- and a vector taken here would
         * be taken again on every call, leaking one each time. Settling is
         * also the only place that knows whether this pin already delivers
         * under somebody else's vector, which it must then join. */
        .vector      = 0,
        .row         = RIGHT_OF_WAY_WORK_IS_REDONE,
        .intx_pin    = 0,
        .polarity    = SUMMONS_ACTIVE_HIGH,
        .trigger     = SUMMONS_EDGE,
        .shared      = false,
        /* An override that maps a line onto its own number is
         * indistinguishable here from no override at all, and reads as the
         * bus rule. That is harmless in a way the wiring case is not: both
         * name the same pin, so nothing is programmed differently — only the
         * sentence in the log is less specific than it could be. */
        .gsi_from    = (gsi == isa_irq) ? SUMMONS_FROM_BUS_RULE
                                        : SUMMONS_FROM_FIRMWARE_OVERRIDE,
        .wiring_from = SUMMONS_FROM_BUS_RULE,
        .bus         = "ISA",
    };
    return line;
}

Summons SummonsForAcpiSci(uint16_t sci_int) {
    /* ACPI 6.5 §5.2.9, Table 5.9, SCI_INT: "System vector the SCI interrupt
     * is wired to in 8259 mode. On systems that do not contain the 8259,
     * this field contains the Global System interrupt number of the SCI
     * interrupt. OSPM is required to treat the ACPI SCI interrupt as a
     * shareable, level, active low interrupt."
     *
     * ‼ SHAREABLE IS THE WORD THAT WAS BEING DROPPED. The SCI reached the
     * registry through irq_register_handler, i.e. through SummonsForIsaLine,
     * which states the ISA rule — edge, active high, EXCLUSIVE — and
     * acpi_sci_register then corrected the first two by planting an override
     * in the table SummonsResolve reads. Nothing corrected the third,
     * because sharing is not in the MPS INTI word at all. So on a board
     * where _PRT routes a PCI function onto the SCI's pin, which is the
     * ordinary PIRQ outcome on IRQ 9, 10 or 11, that function was refused
     * the line and dropped to polling — over a default belonging to a bus
     * whose rule the specification here overrides by name.
     *
     * Trigger and polarity are stated here too, for the same reason and
     * from the same sentence, rather than being left to the override
     * acpi_sci_register writes: the requirement is unconditional, and a
     * description that depends on another function having run first is not
     * a description.
     *
     * The number in SCI_INT is an 8259 IRQ on every machine that has an
     * 8259, so the pin is ioapic_isa_to_gsi(SCI_INT) — the canonical PCAT
     * case being the SCI on IRQ 9 remapped to INTIN 11. Above 15 there is no
     * 8259 input for it to be and the field is already a GSI. */
    uint32_t gsi = (sci_int < 16) ? ioapic_isa_to_gsi(sci_int) : sci_int;

    Summons line = {
        .gsi         = gsi,
        .vector      = 0,
        /* The SCI reports: a power button, a thermal event, a GPE. Nothing is
         * in flight on it and nothing is redone if it waits a moment. */
        .row         = RIGHT_OF_WAY_A_PERSON_WAITS,
        .intx_pin    = 0,
        .polarity    = SUMMONS_ACTIVE_LOW,
        .trigger     = SUMMONS_LEVEL,
        .shared      = true,
        .gsi_from    = SUMMONS_FROM_FIRMWARE_OVERRIDE,
        .wiring_from = SUMMONS_FROM_FIRMWARE_OVERRIDE,
        .bus         = "ACPI",
    };
    return line;
}

void SummonsResolve(Summons *line) {
    if (!line) return;

    /* ‼ FIRMWARE'S TABLE, NOT THIS KERNEL'S OWN CONCLUSION. ioapic_gsi_flags
     * falls through to the resolved description SummonsArm writes, so asking
     * it here read back what this kernel had stored moments before and
     * stamped it SUMMONS_FROM_FIRMWARE_OVERRIDE. See the note above
     * SummonsFirmwareSaid in ioapic.c. */
    uint16_t said = 0;
    if (!SummonsFirmwareSaid(line->gsi, &said)) return;

    /* ‼ THE ORDER OF SOURCES IS ENFORCED HERE, not merely written down.
     *
     * A MADT Interrupt Source Override is the FIRMWARE OVERRIDE rung, and the
     * ACPI routing table stands above it: _PRT names the line for the mode
     * this kernel is actually running in, and a link's _CRS describes the
     * very wire the line arrives on. Applying an override on top of that
     * would let the weaker statement win by running later, which is how a
     * source order becomes decoration.
     *
     * It should never happen — an ISO describes an ISA source (acpi_madt.c
     * now refuses one that names another bus) and a PCI line has no ISA
     * number — so when it does, that is worth a sentence rather than a silent
     * skip: it means the board has described the same pin twice, in two
     * tables, and this kernel is telling the operator which one it believed. */
    if (line->wiring_from > SUMMONS_FROM_FIRMWARE_OVERRIDE) {
        static bool outranked_said = false;
        if (!outranked_said) {
            outranked_said = true;
            kprintf("[IRQCHIP] GSI %u is described both by a MADT override "
                    "and by %s; the routing table is the later and more "
                    "specific statement and it stands\n",
                    line->gsi, SummonsSourceName(line->wiring_from));
        }
        return;
    }

    /* ACPI 6.5 §5.2.12.5, Table 5.26. Polarity is bits 1:0 and trigger is
     * bits 3:2, and in BOTH fields 00 means "Conforms to the specifications
     * of the bus" — the firmware declining to answer, not an answer of
     * "high" and "edge". Reading 00 as an answer is what programmed every
     * PCI line edge/active-high; here 00 leaves the bus rule standing.
     *
     * 10 is Reserved in both fields. A reserved encoding is not a
     * description, so it is named and the bus rule stands — the alternative,
     * silently treating it as "not low" and "not level", is the same mistake
     * with a different input. */
    uint16_t polarity = said & 0x3u;
    uint16_t trigger  = (uint16_t)((said >> 2) & 0x3u);

    if (polarity == 0x1u || polarity == 0x3u) {
        line->polarity    = (polarity == 0x3u) ? SUMMONS_ACTIVE_LOW
                                               : SUMMONS_ACTIVE_HIGH;
        line->wiring_from = SUMMONS_FROM_FIRMWARE_OVERRIDE;
    } else if (polarity == 0x2u) {
        /* Once. This runs again every time the same line is resolved — at
         * registration and again at arming — and a board that writes one
         * malformed field writes it in one table it will not rewrite. */
        static bool polarity_said = false;
        if (!polarity_said) {
            polarity_said = true;
            kprintf("[IRQCHIP] %[E]GSI %u: the firmware states polarity 10, "
                    "which ACPI 6.5 Table 5.26 reserves — the %s rule stands; "
                    "further lines like it go unremarked%[D]\n",
                    line->gsi, line->bus);
        }
    }

    if (trigger == 0x1u || trigger == 0x3u) {
        line->trigger     = (trigger == 0x3u) ? SUMMONS_LEVEL : SUMMONS_EDGE;
        line->wiring_from = SUMMONS_FROM_FIRMWARE_OVERRIDE;
    } else if (trigger == 0x2u) {
        static bool trigger_said = false;
        if (!trigger_said) {
            trigger_said = true;
            kprintf("[IRQCHIP] %[E]GSI %u: the firmware states trigger mode "
                    "10, which ACPI 6.5 Table 5.26 reserves — the %s rule "
                    "stands; further lines like it go unremarked%[D]\n",
                    line->gsi, line->bus);
        }
    }

    /* Sharing is not in the MPS INTI word at all, so firmware never overrides
     * it. It follows the bus rule and nothing else — which is why an ISA line
     * that firmware has re-described as level is still not a line two drivers
     * may answer. */
}

uint16_t SummonsMpsFlags(const Summons *line) {
    /* 01 and 11, never 00: a resolved description states its answer rather
     * than deferring to a bus whose rule the reader would then have to know.
     * This is the word handed to ioapic_describe_gsi, and it is the reason
     * ioapic_enable_irq can program a redirection entry without guessing. */
    uint16_t flags = 0;
    flags |= (line->polarity == SUMMONS_ACTIVE_LOW) ? 0x3u : 0x1u;
    flags |= (uint16_t)(((line->trigger == SUMMONS_LEVEL) ? 0x3u : 0x1u) << 2);
    return flags;
}

bool SummonsArm(Summons *line) {
    if (!line || !active_chip) return false;

    /* There is no vector test here, and its absence is the point.
     *
     * A barrier used to stand on this line, judging `line->vector` against the
     * width of a twenty-four-vector window before anything had been resolved
     * or settled. It was checking a number the caller had computed for
     * itself, and once lines stopped arriving with a vector of their own it
     * would have rejected every one of them: a line carries zero until it
     * settles, and zero minus the vector base underflows into a number well
     * past any window. What it was really guarding — "this line must end up
     * somewhere this kernel can dispatch" — is the settling's job now, and
     * the settling either gives the line right of way or refuses aloud. */
    /* Remembered before the settling, because the settling is what hands a
     * vector out and afterwards there is no way to tell a number this line
     * was just given from one it walked in with. */
    bool line_had_no_vector = (line->vector < IRQ_VECTOR_BASE);

    Summons resolved = *line;
    SummonsResolve(&resolved);

    /* ‼ SETTLE BEFORE ANYTHING IS WRITTEN. The pin is the resource, and the
     * registry is the only record of who holds it. Without this, a second
     * line whose vector differs but whose pin coincides rewrote the first
     * line's redirection entry — new vector, new flags — and the first
     * line's answerers were never called again, on a wire its device goes on
     * holding. Settling makes the newcomer join that pin's vector and accept
     * the wire already cut for it, or be refused aloud. */
    if (!SummonsSettleOnPin(&resolved)) return false;

    /* The caller is told what was ACTUALLY programmed, not what it asked
     * for: a driver that prints its line's wiring must print the wire, and
     * one that records a vector must record the one its interrupts arrive
     * on. */
    *line = resolved;

    /* From here on there are two ways out that are not "armed", and both of
     * them owe the pool a vector.
     *
     * The settling above is where a line that nobody held is GIVEN one, so a
     * refusal after this point walks away holding a number. Before the pool
     * existed that was harmless — the number was the pin plus thirty-two and
     * came back by arithmetic whether anybody returned it or not. Now it is a
     * leak, and an invisible one: nothing is wrong until some later device
     * finds its row full. */
    bool line_was_given_a_vector = (line_had_no_vector && resolved.vector >= IRQ_VECTOR_BASE);

    if (active_type == IRQCHIP_APIC) {
        /* Asked BEFORE anything is written or recorded, because the answer
         * decides what the caller does next: a driver that is told its line
         * is armed stops polling, and a driver told the truth keeps its
         * fallback. The refusal itself is printed inside. */
        if (!SummonsPinIsReachable(resolved.gsi, madt_info.bsp_lapic_id)) {
            if (line_was_given_a_vector)
                VectorPoolGiveBack(resolved.vector, resolved.bus);
            return false;
        }

        /* The registry and the redirection entry are told the same thing, in
         * that order, so that a line which is armed is always a line
         * irq_handler can describe — including one nobody has answered yet. */
        SummonsPost(&resolved);
        ioapic_describe_gsi(resolved.gsi, SummonsMpsFlags(&resolved));
        ioapic_enable_irq(resolved.gsi, resolved.vector,
                          madt_info.bsp_lapic_id);
    } else {
        if (resolved.gsi >= 16) {
            kprintf("[IRQCHIP] %[E]%s GSI %u not armed: the 8259 has sixteen "
                    "inputs and this machine offered no MADT, so there is no "
                    "controller input with that number%[D]\n",
                    resolved.bus, resolved.gsi);
            if (line_was_given_a_vector)
                VectorPoolGiveBack(resolved.vector, resolved.bus);
            return false;
        }
        /* The 8259 takes polarity and trigger from the chipset's Edge/Level
         * Control Register, not from anything this kernel writes to the PIC
         * itself, so a level line here is left as the firmware wired it.
         * Said once rather than per line: on a machine with no MADT there is
         * no PCI INTx line to arm in the first place, and the sentence exists
         * so that if one ever is, the reader is not left to deduce why it
         * behaves as an edge. */
        if (resolved.trigger == SUMMONS_LEVEL) {
            static bool elcr_said = false;
            if (!elcr_said) {
                elcr_said = true;
                kprintf("[IRQCHIP] GSI %u is level-triggered and this machine "
                        "is on the 8259 fallback — its trigger mode is the "
                        "chipset ELCR's, which this kernel does not "
                        "restate\n", resolved.gsi);
            }
        }
        SummonsPost(&resolved);
        pic_enable_irq((uint8_t)resolved.gsi);
    }

    /* ‼ A LINE THAT SOMETHING OTHER THAN ITS BUS RULE DECIDED SAYS SO ON THE
     * BOARD, not only in a debug build.
     *
     * This is what the provenance fields are FOR, and it is the reason they
     * are not decoration. When _PRT lands, a PCI line's pin stops coming from
     * "the byte the firmware wrote for the 8259" and starts coming from "the
     * ACPI routing table", and that change has to be VISIBLE on the machine
     * where it matters rather than inferred from the fact that a disk started
     * working. The same sentence is what tells an operator today that a GSI
     * came from a byte the specification reserves.
     *
     * A plain ISA line whose bus rule nobody touched stays in the debug log:
     * it is the default case, it was silent before, and six identical lines
     * of "edge, active high, from the rule of its bus" would bury the two
     * that carry information. */
    if (resolved.wiring_from != SUMMONS_FROM_BUS_RULE ||
        resolved.gsi_from    != SUMMONS_FROM_BUS_RULE) {
        kprintf("[IRQCHIP] %s GSI %u armed on vector %u: %s, %s, %s; wiring "
                "from %s, pin from %s\n",
                resolved.bus, resolved.gsi, resolved.vector,
                resolved.trigger == SUMMONS_LEVEL ? "level" : "edge",
                resolved.polarity == SUMMONS_ACTIVE_LOW ? "active low"
                                                        : "active high",
                resolved.shared ? "shareable" : "exclusive",
                SummonsSourceName(resolved.wiring_from),
                SummonsSourceName(resolved.gsi_from));
    } else {
        debug_printf("[IRQCHIP] %s GSI %u armed on vector %u: %s, %s, %s "
                     "(bus rule throughout)\n",
                     resolved.bus, resolved.gsi, resolved.vector,
                     resolved.trigger == SUMMONS_LEVEL ? "level" : "edge",
                     resolved.polarity == SUMMONS_ACTIVE_LOW ? "active low"
                                                             : "active high",
                     resolved.shared ? "shareable" : "exclusive");
    }
    return true;
}

void SummonsDisarm(const Summons *line) {
    if (!line || !active_chip) return;

    if (active_type == IRQCHIP_APIC) {
        ioapic_disable_irq(line->gsi);
    } else if (line->gsi < 16) {
        pic_disable_irq((uint8_t)line->gsi);
    }
}
