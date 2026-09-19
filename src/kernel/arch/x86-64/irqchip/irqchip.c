#include "irqchip.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "acpi_madt.h"
#include "cpuid.h"
#include "klib.h"
#include "io.h"

// Current interrupt controller state
static irqchip_type_t active_type = IRQCHIP_PIC;
static irq_chip_t* active_chip = NULL;
static madt_info_t madt_info;

// --- PIC backend (wraps existing pic.c functions) ---

static void pic_chip_enable(uint8_t gsi) {
    if (gsi < 16) pic_enable_irq(gsi);
}

static void pic_chip_disable(uint8_t gsi) {
    if (gsi < 16) pic_disable_irq(gsi);
}

static void pic_chip_eoi(uint8_t gsi) {
    if (gsi < 16) pic_send_eoi(gsi);
}

static uint32_t pic_chip_get_isr(void) {
    return (uint32_t)pic_get_isr();
}

static uint32_t pic_chip_get_irr(void) {
    return (uint32_t)pic_get_irr();
}

static irq_chip_t pic_chip = {
    .name       = "8259A PIC",
    .enable_irq = pic_chip_enable,
    .disable_irq = pic_chip_disable,
    .send_eoi   = pic_chip_eoi,
    .get_isr    = pic_chip_get_isr,
    .get_irr    = pic_chip_get_irr,
    .max_irqs   = 16,
};

// --- APIC backend (LAPIC + IO-APIC) ---

/* The BSP's APIC ID is handed down at full width. Whether it fits the
 * eight destination bits of a redirection entry is the IOAPIC's own
 * question, answered — and refused aloud, per line — in ioapic.c, where
 * the field is. Narrowing it here with a cast would put the loss at a
 * call site that the entry itself can never inspect. */
static void apic_chip_enable(uint8_t irq) {
    if (irq < IRQ_MAX_COUNT) {
        // Apply ISA-to-GSI mapping (e.g., MADT says IRQ 0 -> GSI 2 for PIT)
        uint32_t gsi = ioapic_isa_to_gsi(irq);
        // Vector is based on IRQ number, not GSI — so IRQ handler maps back correctly
        uint8_t vector = IRQ_VECTOR_BASE + irq;
        ioapic_enable_irq((uint8_t)gsi, vector, madt_info.bsp_lapic_id);
    }
}

static void apic_chip_disable(uint8_t irq) {
    if (irq < IRQ_MAX_COUNT) {
        uint32_t gsi = ioapic_isa_to_gsi(irq);
        ioapic_disable_irq((uint8_t)gsi);
    }
}

static void apic_chip_eoi(uint8_t gsi) {
    (void)gsi;
    lapic_send_eoi();
}

static uint32_t apic_chip_get_isr(void) {
    // Read first 32 bits of LAPIC ISR (covers vectors 0-31... but we want 32-63)
    // ISR register 1 covers vectors 32-63
    return lapic_read(LAPIC_REG_ISR_BASE + 0x10);  // ISR[1] = vectors 32-63
}

static uint32_t apic_chip_get_irr(void) {
    return lapic_read(LAPIC_REG_IRR_BASE + 0x10);  // IRR[1] = vectors 32-63
}

static irq_chip_t apic_chip = {
    .name       = "APIC (LAPIC + IO-APIC)",
    .enable_irq = apic_chip_enable,
    .disable_irq = apic_chip_disable,
    .send_eoi   = apic_chip_eoi,
    .get_isr    = apic_chip_get_isr,
    .get_irr    = apic_chip_get_irr,
    .max_irqs   = IRQ_MAX_COUNT,
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

    debug_printf("[IRQCHIP] %[S]Using APIC mode (LAPIC + IO-APIC, %u IRQs)%[D]\n",
                 active_chip->max_irqs);
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

void irqchip_enable_irq(uint8_t gsi) {
    if (active_chip) {
        active_chip->enable_irq(gsi);
    }
}

void irqchip_disable_irq(uint8_t gsi) {
    if (active_chip) {
        active_chip->disable_irq(gsi);
    }
}

void irqchip_send_eoi(uint8_t gsi) {
    if (active_chip) {
        active_chip->send_eoi(gsi);
    }
}

uint32_t irqchip_get_isr(void) {
    return active_chip ? active_chip->get_isr() : 0;
}

uint32_t irqchip_get_irr(void) {
    return active_chip ? active_chip->get_irr() : 0;
}

irqchip_type_t irqchip_get_type(void) {
    return active_type;
}

const char* irqchip_get_name(void) {
    return active_chip ? active_chip->name : "none";
}

uint8_t irqchip_max_irqs(void) {
    return active_chip ? active_chip->max_irqs : 0;
}
