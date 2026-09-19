#include "ioapic.h"
#include "irqchip.h"
#include "lapic.h"      /* LAPIC_XAPIC_DEST_MAX — the one destination width */
#include "io.h"
#include "klib.h"
#include "vmm.h"

static volatile uint32_t* ioapic_base_virt = NULL;
static uint8_t ioapic_gsi_base = 0;
static uint8_t ioapic_max_entry = 0;

// Interrupt Source Override table (ISA IRQ -> GSI remapping)
#define ISO_TABLE_SIZE 16
static ioapic_iso_t iso_table[ISO_TABLE_SIZE];

/* The resolved description of a line — defined below, beside the table it
 * reads, and declared here because arming a pin is the first thing that
 * needs it. */
static bool ioapic_gsi_resolved(uint32_t gsi, uint16_t *out_flags);

/*
 * One selector, one transaction.
 *
 * The IOAPIC has no addressable registers of its own on the bus. Intel
 * 82093AA §3.0: "To reference an IOAPIC register, a byte memory write that
 * the PIIX3 decodes for the IOAPIC loads the IOREGSEL Register with an
 * 8-bit value that specifies the IOAPIC register ... to be accessed. The
 * IOWIN Register is then used to read/write the desired data from/to the
 * IOAPIC register specified by bits [7:0] of the IOREGSEL Register." The
 * index write and the data access are therefore ONE transaction against a
 * single shared piece of state, and a second core that lands between them
 * does not corrupt its own access — it silently redirects ours.
 *
 * The same section makes the read-modify-write explicit: "to modify a
 * field (e.g., bit, byte), the whole 32 bit register must be read, the
 * field modified, and the 32 bits written back." That is three accesses
 * across two transactions, and masking a pin is exactly that shape.
 *
 * So the lock is held across the WHOLE operation, not around each access:
 * both halves of a redirection entry, and the read and the write-back of a
 * mask. This is the scope of Linux's ioapic_lock (arch/x86/kernel/apic/
 * io_apic.c), which its ioapic_read_entry / ioapic_write_entry take with
 * raw_spinlock_irqsave while the low-level io_apic_read / io_apic_write
 * stay bare. spin_lock() here disables interrupts for the same reason
 * Linux's irqsave form does: an interrupt handler that touches the IOAPIC
 * between our index and our data is the single-core version of the same
 * interleaving.
 */
static spinlock_t ioapic_reg_lock;

static inline uint32_t ioapic_read_held(uint32_t reg) {
    ioapic_base_virt[IOAPIC_IOREGSEL / 4] = reg;
    return ioapic_base_virt[IOAPIC_IOWIN / 4];
}

static inline void ioapic_write_held(uint32_t reg, uint32_t value) {
    ioapic_base_virt[IOAPIC_IOREGSEL / 4] = reg;
    ioapic_base_virt[IOAPIC_IOWIN / 4] = value;
}

static uint32_t ioapic_read(uint32_t reg) {
    spin_lock(&ioapic_reg_lock);
    uint32_t value = ioapic_read_held(reg);
    spin_unlock(&ioapic_reg_lock);
    return value;
}

/*
 * The high dword goes first, and that order is the whole point.
 *
 * A redirection entry is 64 bits reached as two 32-bit registers. The mask
 * bit lives in the LOW dword (82093AA §3.2.4, IOREDTBLx bit 16: "When this
 * bit is 1, the interrupt signal is masked ... When this bit is 0, the
 * interrupt is not masked"); the destination lives in the HIGH dword (bits
 * 63:56, "If the Destination Mode of this entry is Physical Mode ... bits
 * [59:56] contain an APIC ID"). Writing the low dword first therefore arms
 * the line — new vector, mask cleared — while the destination is still
 * whatever the previous programming left there, and an assertion inside
 * that window is delivered to the wrong LAPIC under the new vector.
 *
 * Linux states it in the same words above __ioapic_write_entry: "When we
 * write a new IO APIC routing entry, we need to write the high word first!
 * If the mask bit in the low word is clear, we will enable the interrupt,
 * and we need to make sure the entry is fully populated before that
 * happens."
 */
static void ioapic_write_redir(uint8_t entry, uint32_t low, uint32_t high) {
    uint32_t reg = IOAPIC_REG_REDTBL + entry * 2;
    spin_lock(&ioapic_reg_lock);
    ioapic_write_held(reg + 1, high);
    ioapic_write_held(reg, low);
    spin_unlock(&ioapic_reg_lock);
}

/*
 * Taking a pin out of service is the one case that goes the other way.
 *
 * High-dword-first is right while ARMING a line and wrong while disarming
 * one. The mask bit is in the low dword, so raising it first means the rest
 * of the entry only ever changes under a pin that can no longer deliver.
 * Skip that and the same window opens in reverse: ioapic_init() meets an
 * entry in whatever state firmware left it — on a warm handoff that can be
 * unmasked, with a live vector and a destination of its own — and writing
 * the high dword first drops destination 0 into an entry that is still
 * armed with the OLD vector. An assertion inside that window is formatted
 * from the old low dword and sent to APIC ID 0, which at this point in boot
 * is either the wrong core or no core at all.
 *
 * Linux splits the two for this reason and says so above ioapic_mask_entry:
 * "When we mask an IO APIC routing entry, we need to write the low word
 * first, in order to set the mask bit before we change the high bits!"
 * (arch/x86/kernel/apic/io_apic.c) — the mirror of the sentence quoted
 * above ioapic_write_redir.
 */
static void ioapic_mask_redir(uint8_t entry) {
    uint32_t reg = IOAPIC_REG_REDTBL + entry * 2;
    spin_lock(&ioapic_reg_lock);
    ioapic_write_held(reg, IOAPIC_REDIR_MASKED);
    ioapic_write_held(reg + 1, 0);
    spin_unlock(&ioapic_reg_lock);
}

void ioapic_init(uintptr_t base_addr, uint8_t gsi_base) {
    ioapic_gsi_base = gsi_base;

    debug_printf("[IOAPIC] Initializing IO-APIC at phys 0x%lx, GSI base=%u\n",
                 base_addr, gsi_base);

    // Map IO-APIC MMIO (one page, uncacheable)
    ioapic_base_virt = (volatile uint32_t*)vmm_map_mmio(
        base_addr, 4096,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_CACHE_DISABLE
    );

    if (!ioapic_base_virt) {
        kprintf("[IOAPIC] %[E]phys 0x%lx could not be mapped — this IOAPIC "
                "programs no line at all%[D]\n", base_addr);
        return;
    }

    // NOTE: Do NOT clear iso_table here — it was already populated by
    // acpi_parse_madt() -> ioapic_register_iso() BEFORE ioapic_init() runs.

    // Read version register to get max entries
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_entry = ((ver >> 16) & 0xFF);

    uint32_t id = (ioapic_read(IOAPIC_REG_ID) >> 24) & 0xFF;

    debug_printf("[IOAPIC] ID=%u, Version=0x%x, MaxEntry=%u (%u pins)\n",
                 id, ver & 0xFF, ioapic_max_entry, ioapic_max_entry + 1);

    /* Mask every input this chip HAS, counted from the chip's own VER
     * register rather than from a number written here. `uint32_t` and not
     * `uint8_t`: the Maximum Redirection Entry field is eight bits, so a part
     * reporting 255 would make `i <= ioapic_max_entry` a loop with no end. */
    for (uint32_t i = 0; i <= ioapic_max_entry; i++) {
        ioapic_mask_redir((uint8_t)i);
    }

    debug_printf("[IOAPIC] All %u pins masked\n", ioapic_max_entry + 1);

    /* ‼ A CHIP WITH MORE INPUTS THAN THIS KERNEL HAS VECTORS SAYS SO.
     *
     * IRQ_MAX_COUNT is the width of the vector window idt_init installs gates
     * for, and it is the 82093AA's twenty-four entries. A PCH or a server
     * part may present more, and the lines past the window are not armed
     * wrongly — they are not armed at all, and every refusal downstream says
     * "past the interrupt inputs this kernel dispatches" without the reader
     * being able to tell whether the chip even has them. This is the one
     * place that knows both numbers, so this is where the difference is
     * named. */
    if ((uint32_t)ioapic_max_entry + 1u > (uint32_t)IRQ_MAX_COUNT) {
        kprintf("[IOAPIC] %[E]this IOAPIC presents %u inputs (GSI %u to %u) "
                "and this kernel installs interrupt gates for %u of them — "
                "GSI %u upwards can be described but never armed%[D]\n",
                (unsigned)ioapic_max_entry + 1u, ioapic_gsi_base,
                ioapic_gsi_base + ioapic_max_entry, (unsigned)IRQ_MAX_COUNT,
                ioapic_gsi_base + (unsigned)IRQ_MAX_COUNT);
    }
    debug_printf("[IOAPIC] %[S]IO-APIC initialized successfully%[D]\n");
}

/*
 * Can this APIC ID be named in a redirection entry at all?
 *
 * TWO DOCUMENTS DESCRIBE THIS FIELD AND THEY DO NOT AGREE ON ITS WIDTH, so
 * say which one rules before using either. Intel 82093AA §3.2.4, IOREDTBLx
 * Destination Field, describes the DISCRETE chip: "If the Destination Mode
 * of this entry is Physical Mode ... bits [59:56] contain an APIC ID" —
 * four bits, because the discrete IOAPIC was paired with discrete local
 * APICs whose IDs were four bits wide, and no ID above 0x0F existed on such
 * a board to be lost. The width that governs the IOAPIC integrated into
 * every chipset this kernel runs on is xAPIC's, and that is Intel SDM Vol
 * 3A's to state: "The destination ID corresponds to bits 63:56 of the I/O
 * APIC Redirection Table Entry if the IOAPIC is used to dispatch the
 * interrupt to the processor(s)" — the same eight bits the MSI Message
 * Address carries, the whole high byte of the entry rather than half of it.
 *
 * SDM is therefore the authority here, and 82093AA is quoted in this file
 * only for the parts the integrated IOAPIC inherited unchanged: the
 * IOREGSEL/IOWIN transaction and the position of the mask bit. Taking the
 * narrower reading would refuse MORE IDs, never fewer, so the test below is
 * correct under either document; what would be wrong is leaving both widths
 * standing side by side with no word about which one the field has.
 *
 * Those eight bits stop one short of 256. SDM Vol 3A, chapter "ADVANCED
 * PROGRAMMABLE INTERRUPT CONTROLLER (APIC)", section "Physical Destination
 * Mode": "either a single destination (local APIC IDs 00H through FEH) or a
 * broadcast to all APICs (the APIC ID is FFH) may be specified in physical
 * destination mode." (Cited by title; the section number differs between
 * SDM revisions — see LAPIC_XAPIC_DEST_MAX in lapic.h.) So there are two
 * ways to get this wrong, and they fail differently. An APIC ID above 255 has no
 * representation here at all, and truncating it routes the line to a
 * DIFFERENT, running processor, which takes an interrupt under a vector
 * whose handler expects other hardware — a machine whose BSP is x2APIC ID
 * 0x100 would send every legacy IRQ to the core with ID 0. An APIC ID of
 * exactly 255 survives the field intact and still does not mean that core:
 * it is the broadcast, and the line would be asserted on EVERY processor,
 * each of which would run the handler and, on a level-triggered line, race
 * to EOI it. LAPIC_XAPIC_DEST_MAX is where both of those begin.
 *
 * The refusal lives here, at the field, and not in the caller. A
 * caller-side test is a test this file cannot see, and an eight-bit
 * parameter would put the narrowing at the call where nothing here could
 * ever notice it — the same shape that let MSI arm a truncated
 * destination for as long as it did.
 *
 * So the pin is left masked and the refusal is named. Naming it per line
 * rather than once is deliberate: each refused line is one device that
 * will not raise an interrupt, and the operator needs to know WHICH.
 *
 * What would lift the refusal is interrupt remapping (VT-d IR / AMD-Vi),
 * whose IRTE carries a 32-bit destination and turns the redirection entry
 * into an index. Until that is armed there is no honest 8-bit answer, and
 * inventing one is exactly the failure this refuses.
 */
static bool ioapic_dest_is_nameable(uint32_t dest_lapic_id, uint32_t gsi,
                                    const char *what) {
    if (dest_lapic_id <= LAPIC_XAPIC_DEST_MAX) return true;

    if (dest_lapic_id == LAPIC_XAPIC_DEST_BROADCAST) {
        kprintf("[IOAPIC] %[E]%s GSI %u not programmed: APIC ID 255 is the "
                "broadcast destination of the redirection entry's 8-bit "
                "physical destination, so this line would be asserted on "
                "EVERY processor instead of on one%[D]\n", what, gsi);
    } else {
        kprintf("[IOAPIC] %[E]%s GSI %u not programmed: APIC ID %u does not "
                "fit the 8-bit destination of the redirection entry, and the "
                "low eight bits name the core with ID %u instead%[D]\n",
                what, gsi, dest_lapic_id, dest_lapic_id & 0xFFu);
    }
    return false;
}

bool SummonsPinIsReachable(uint32_t gsi, uint32_t dest_lapic_id) {
    if (!ioapic_dest_is_nameable(dest_lapic_id, gsi, "IRQ")) return false;

    /* ioapic_init() returns on a failed mapping and irqchip_init() selects
     * the APIC backend regardless, so this is reachable with no window to
     * write through. Without the test ioapic_write_redir indexes off a null
     * base and the machine takes a page fault at IOREGSEL instead of saying
     * which line it could not arm. */
    if (!ioapic_base_virt) {
        kprintf("[IOAPIC] %[E]IRQ GSI %u not programmed: no IOAPIC is "
                "mapped%[D]\n", gsi);
        return false;
    }

    /* Below this IOAPIC's first GSI there is no pin to subtract towards.
     * Without this test `gsi - ioapic_gsi_base` wraps and the line is still
     * refused — by the pin test below, naming a pin number that was never
     * asked for and a reason that is not the real one. A refusal that
     * misnames its own cause sends the next reader to look for a pin count
     * problem on a machine whose IOAPICs are simply numbered from elsewhere. */
    if (gsi < ioapic_gsi_base) {
        kprintf("[IOAPIC] %[E]IRQ GSI %u not programmed: it is below this "
                "IOAPIC's GSI base %u, so this line belongs to another "
                "IOAPIC%[D]\n", gsi, ioapic_gsi_base);
        return false;
    }

    uint32_t pin = gsi - ioapic_gsi_base;
    if (pin > ioapic_max_entry) {
        kprintf("[IOAPIC] %[E]IRQ GSI %u not programmed: pin %u is past this "
                "IOAPIC's last pin %u (GSI base %u)%[D]\n",
                gsi, pin, ioapic_max_entry, ioapic_gsi_base);
        return false;
    }

    return true;
}

void ioapic_enable_irq(uint8_t gsi, uint8_t vector, uint32_t dest_lapic_id) {
    /* SummonsArm asks this same question before it gets here, and returns
     * false to its caller when the answer is no — so on that path this is a
     * second, cheap confirmation rather than the place the refusal is
     * printed. It stays because the pin count and the destination width are
     * this chip's facts and an entry must never be written without them
     * having been checked, whoever the caller turns out to be. */
    if (!SummonsPinIsReachable(gsi, dest_lapic_id)) return;

    uint8_t pin = (uint8_t)(gsi - ioapic_gsi_base);

    /* ‼ THE ENTRY IS WRITTEN FROM A DESCRIPTION, OR IT IS NOT WRITTEN.
     *
     * This used to ask ioapic_gsi_flags and, on a miss, arm the pin anyway
     * with neither POLARITY_LOW nor TRIGGER_LEVEL — edge triggered, active
     * high. That is the ISA bus's default, and the table it had just asked
     * answers only about ISA sources, so every PCI INTx line missed and every
     * PCI INTx line was armed with the wrong electrical contract: a
     * level-sensitive open-drain wire (PCI 3.0 §2.2.6) programmed as an edge.
     * Nothing said so, and the device was simply quiet.
     *
     * A pin with no description is now a refusal spoken aloud. The caller
     * that arms anything is SummonsArm, which describes the line immediately
     * before this call, so reaching the refusal means a route into the IOAPIC
     * that bypassed the description — and a device that does not interrupt is
     * worth more than a device that interrupts wrongly. */
    uint16_t flags = 0;
    if (!ioapic_gsi_resolved(gsi, &flags)) {
        kprintf("[IOAPIC] %[E]IRQ GSI %u not programmed: nothing describes "
                "this line's polarity or trigger mode, and the bus default "
                "this used to fall back to is the ISA one — on a PCI line it "
                "programs a level-sensitive wire as an edge and the device "
                "goes silent%[D]\n", gsi);
        return;
    }

    uint32_t redir_flags = IOAPIC_REDIR_DELMOD_FIXED | IOAPIC_REDIR_DESTMOD_PHYS;

    /* ACPI 6.5 §5.2.12.5 Table 5.26: polarity is bits 1:0 (11 = active low),
     * trigger is bits 3:2 (11 = level-triggered). A resolved description
     * never carries 00 in either field, so there is nothing here to defer. */
    if ((flags & 0x03u) == 0x03u) redir_flags |= IOAPIC_REDIR_POLARITY_LOW;
    if ((flags & 0x0Cu) == 0x0Cu) redir_flags |= IOAPIC_REDIR_TRIGGER_LEVEL;

    uint32_t low = (vector & IOAPIC_REDIR_VECTOR_MASK) | redir_flags;
    uint32_t high = dest_lapic_id << 24;

    ioapic_write_redir(pin, low, high);

    debug_printf("[IOAPIC] GSI %u -> vector %u, dest LAPIC %u (pin %u)\n",
                 gsi, vector, dest_lapic_id, pin);
}

void ioapic_disable_irq(uint8_t gsi) {
    /* Silent, unlike the arming path: an unmapped IOAPIC never armed the
     * line either, so there is no work being dropped — only a null base to
     * keep out of ioapic_read_held(). */
    if (!ioapic_base_virt) return;

    /* Same underflow as the arming path, and here it is worse than a wrong
     * message: a wrapped pin that happens to land within ioapic_max_entry
     * masks a DIFFERENT line on this IOAPIC, and the device that owned it
     * goes quiet with nothing said. Silent, like the rest of this function:
     * a line this IOAPIC never armed has nothing to disarm. */
    if (gsi < ioapic_gsi_base) return;

    uint8_t pin = gsi - ioapic_gsi_base;

    if (pin > ioapic_max_entry) {
        return;
    }

    /* Read-modify-write of one 32-bit register: read and write-back are a
     * single operation, not two transactions that may be interleaved. */
    uint32_t reg = IOAPIC_REG_REDTBL + pin * 2;
    spin_lock(&ioapic_reg_lock);
    uint32_t low = ioapic_read_held(reg);
    ioapic_write_held(reg, low | IOAPIC_REDIR_MASKED);
    spin_unlock(&ioapic_reg_lock);
}

/*
 * WHAT A LINE IS, once somebody has worked it out.
 *
 * The override table above is FIRMWARE'S WORD, indexed by ISA IRQ because
 * that is what a MADT Interrupt Source Override is about. This one is the
 * RESOLVED answer for a line — bus rule, then whatever better source spoke —
 * and it is what a redirection entry is programmed from. The two are
 * different things and must stay apart: an override may legitimately say 00
 * in either field, which ACPI 6.5 Table 5.26 defines as "Conforms to the
 * specifications of the bus", i.e. an explicit deferral. Programming a pin
 * from firmware's word directly is how that deferral became "not level, not
 * low" and every PCI INTx line came up edge-triggered and active high.
 *
 * Whoever resolves a line calls ioapic_describe_gsi with the answer — see
 * SummonsArm, which is the one caller that arms anything. Flags written here
 * are therefore always 01 or 11 in both fields; nothing stored here defers.
 *
 * ‼ ONE SLOT PER PIN, ADDRESSED BY THE PIN. There is no search and no
 * allocation: a redirection entry belongs to exactly one input, so its
 * description belongs in that input's slot, and the question "how many
 * descriptions may exist" stops being asked.
 *
 * How many slots THIS chip uses is ioapic_max_entry + 1, read from its own
 * IOAPICVER register in ioapic_init, and that is what every bound and every
 * refusal below is written against. The array's declared length is not a
 * choice either: the Maximum Redirection Entry field is IOAPICVER bits
 * 23:16 — eight bits, as ioapic.h records and ioapic_init reads — so no
 * IOAPIC can state a size above 256 inputs however many it has. The
 * encoding sets the ceiling; the chip sets the limit.
 *
 * This replaces eight slots that nothing chose, and then twenty-four that
 * were one part's input count asserted of every part — including in the
 * refusal message, which told the operator "this IOAPIC has 24 inputs" on
 * chips where that was untrue. Past the last slot the original code armed
 * the line by bus default: the exact outcome this table exists to prevent.
 */
#define IOAPIC_REDIR_ENTRY_CEILING 256

typedef struct {
    uint16_t flags;
    bool     active;
} ioapic_gsi_desc_t;

static ioapic_gsi_desc_t gsi_desc_table[IOAPIC_REDIR_ENTRY_CEILING];

/* Which slot of this chip's table describes that GSI, or false when the GSI
 * is not one of this chip's inputs at all. The two tests are the same two
 * SummonsPinIsReachable makes, and they are here rather than borrowed from
 * it because this path must answer without printing: a GSI belonging to an
 * IOAPIC this kernel has not brought up is a question, not yet a refusal. */
static bool ioapic_desc_slot(uint32_t gsi, uint32_t *out_pin) {
    if (gsi < ioapic_gsi_base) return false;
    uint32_t pin = gsi - ioapic_gsi_base;
    if (pin > ioapic_max_entry) return false;
    if (out_pin) *out_pin = pin;
    return true;
}

/* The resolved description alone, which is what a redirection entry needs.
 * Deliberately NOT firmware's word: an override may say 00 in either field,
 * which ACPI 6.5 Table 5.26 defines as a deferral to the bus, and a
 * redirection entry cannot be programmed from a deferral. */
static bool ioapic_gsi_resolved(uint32_t gsi, uint16_t *out_flags) {
    uint32_t pin;
    if (!ioapic_desc_slot(gsi, &pin)) return false;
    if (!gsi_desc_table[pin].active) return false;
    if (out_flags) *out_flags = gsi_desc_table[pin].flags;
    return true;
}

/* ‼ FIRMWARE'S WORD AND NOTHING ELSE.
 *
 * SummonsResolve asks this, and asking ioapic_gsi_flags instead — which it
 * did — meant reading back THIS KERNEL'S OWN CONCLUSION and stamping it as
 * the firmware's. ioapic_gsi_flags falls through to the resolved table that
 * SummonsArm writes, so from the second time any GSI was armed or answered
 * the "override" it found was the flag word this kernel had stored moments
 * before. Two things came of it, and neither is cosmetic. The provenance
 * line printed on the board said "wiring from the firmware's own override"
 * on machines that published no override — a fabrication, printed as
 * evidence, by the very field the Summons exists to keep honest. And once
 * _PRT could describe a wire, a line first described from a link's _CRS as
 * active HIGH left 0x0D in the resolved table, which the NEXT line on that
 * same GSI then read back as firmware's word and was programmed by, against
 * its own bus rule.
 *
 * So the question "what did firmware say" is answered from firmware's table
 * alone — here, and by every caller that ever asks it.
 *
 * ‼ ioapic_gsi_flags below, which answers the wider "whatever is known about
 * this line", now has NO CALLER ANYWHERE IN THE TREE: acpi.c's SCI path was
 * the last one and it asks this instead, for the same reason. It is left
 * standing only because deleting it means deleting its declaration in
 * ioapic.h, and that file is outside this change. It should go with it.
 */
bool SummonsFirmwareSaid(uint32_t gsi, uint16_t *out_flags) {
    for (int i = 0; i < ISO_TABLE_SIZE; i++) {
        if (iso_table[i].active && iso_table[i].gsi == gsi) {
            if (out_flags) *out_flags = iso_table[i].flags;
            return true;
        }
    }
    return false;
}

bool ioapic_gsi_flags(uint32_t gsi, uint16_t *out_flags) {
    if (SummonsFirmwareSaid(gsi, out_flags)) return true;
    return ioapic_gsi_resolved(gsi, out_flags);
}

void ioapic_describe_gsi(uint32_t gsi, uint16_t flags) {
    uint32_t pin;
    if (!ioapic_desc_slot(gsi, &pin)) {
        /* Reachable only on a machine with more than one IOAPIC, where the
         * GSIs of a second chip land outside this one's range. That kernel
         * does not exist yet — irqchip_init programs a single IOAPIC — and
         * when it does, each chip brings its own table. Until then this
         * refusal is the honest end of the road rather than a silent fall
         * back to the bus default. */
        kprintf("[IOAPIC] %[E]GSI %u cannot be described: this IOAPIC answers "
                "for GSI %u to %u, so GSI %u belongs to an IOAPIC this kernel "
                "has not brought up%[D]\n",
                gsi, ioapic_gsi_base, ioapic_gsi_base + ioapic_max_entry, gsi);
        return;
    }
    gsi_desc_table[pin].flags  = flags;
    gsi_desc_table[pin].active = true;
}

void ioapic_register_iso(uint8_t isa_irq, uint32_t gsi, uint16_t flags) {
    if (isa_irq >= ISO_TABLE_SIZE) {
        kprintf("[IOAPIC] %[E]override for ISA IRQ %u dropped: there are only "
                "%d ISA interrupts, so the firmware named one that cannot "
                "exist%[D]\n", isa_irq, ISO_TABLE_SIZE);
        return;
    }

    iso_table[isa_irq].isa_irq = isa_irq;
    iso_table[isa_irq].gsi = gsi;
    iso_table[isa_irq].flags = flags;
    iso_table[isa_irq].active = true;

    debug_printf("[IOAPIC] ISO: ISA IRQ %u -> GSI %u (flags=0x%04x)\n",
                 isa_irq, gsi, flags);
}

uint32_t ioapic_isa_to_gsi(uint8_t isa_irq) {
    if (isa_irq < ISO_TABLE_SIZE && iso_table[isa_irq].active) {
        return iso_table[isa_irq].gsi;
    }
    // Default identity mapping: ISA IRQ N = GSI N
    return isa_irq;
}

uint16_t ioapic_get_iso_flags(uint8_t isa_irq) {
    if (isa_irq < ISO_TABLE_SIZE && iso_table[isa_irq].active) {
        return iso_table[isa_irq].flags;
    }
    return 0;
}

void ioapic_program_nmi_source(uint32_t gsi, uint16_t mps_flags,
                                uint32_t dest_lapic_id) {
    if (!ioapic_dest_is_nameable(dest_lapic_id, gsi, "NMI Source")) return;

    if (!ioapic_base_virt) {
        kprintf("[IOAPIC] %[E]NMI Source GSI %u not programmed: this IOAPIC "
                "is not mapped%[D]\n", gsi);
        return;
    }

    if (gsi < ioapic_gsi_base) {
        kprintf("[IOAPIC] %[E]NMI Source GSI %u not programmed: it is below "
                "this IOAPIC's GSI base %u%[D]\n", gsi, ioapic_gsi_base);
        return;
    }
    uint32_t pin32 = gsi - ioapic_gsi_base;
    if (pin32 > ioapic_max_entry) {
        kprintf("[IOAPIC] %[E]NMI Source GSI %u not programmed: it is past "
                "this IOAPIC's last pin %u%[D]\n", gsi, ioapic_max_entry);
        return;
    }
    uint8_t pin = (uint8_t)pin32;

    /* Translate MPS INTI flags (ACPI 6.5 §5.2.12.5) into redirection bits.
     * Bus default for the LPC/ISA bus is edge-triggered, active high; on
     * the system bus the default is level-triggered, active low. NMI
     * sources are almost always edge active high — but honour whatever
     * the firmware explicitly states. */
    uint32_t redir = IOAPIC_REDIR_DELMOD_NMI | IOAPIC_REDIR_DESTMOD_PHYS;
    uint16_t polarity = mps_flags & 0x3;
    uint16_t trigger  = (mps_flags >> 2) & 0x3;
    if (polarity == 0x3) redir |= IOAPIC_REDIR_POLARITY_LOW;
    if (trigger  == 0x3) redir |= IOAPIC_REDIR_TRIGGER_LEVEL;

    /* Vector is don't-care for NMI delivery, but Intel SDM advises leaving
     * a sentinel for analysis; 0x00 is fine. */
    uint32_t high = dest_lapic_id << 24;
    ioapic_write_redir(pin, redir, high);

    debug_printf("[IOAPIC] NMI Source: GSI %u pin %u mps=0x%04x dest=%u low=0x%08x\n",
                 gsi, pin, mps_flags, dest_lapic_id, redir);
}
