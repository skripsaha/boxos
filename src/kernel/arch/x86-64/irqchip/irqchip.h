#ifndef IRQCHIP_H
#define IRQCHIP_H

#include "ktypes.h"

// Vector layout:
//   0-31:   CPU exceptions
//   32-55:  Hardware IRQs (GSI 0-23, covers PIC + IO-APIC)
//   128:    Syscall (INT 0x80)
//   254:    LAPIC timer
//   255:    LAPIC spurious

#define IRQ_VECTOR_BASE         32
/* ‼ THIS IS A STATEMENT ABOUT THIS KERNEL, NOT ABOUT THE CHIP.
 *
 * It is the width of the window of IDT gates idt_init installs for IOAPIC
 * lines, and therefore the number of distinct hardware lines this kernel can
 * dispatch. It was written "Standard IO-APIC: 24 pins", which asserted the
 * 82093AA's input count of every part in front of it — and that assertion is
 * false on a PCH or a server IOAPIC, while the number itself stays true of
 * the vector map either way.
 *
 * So the chip is never assumed. ioapic_init reads how many inputs this one
 * actually has out of its own IOAPICVER register and, when that is more than
 * this window, says on the board which GSIs can be described but never
 * armed. The number here decides nothing about hardware; the hardware's own
 * word decides what is reachable, and the difference is spoken. */
#define IRQ_MAX_COUNT           24
#define IRQ_VECTOR_MAX          (IRQ_VECTOR_BASE + IRQ_MAX_COUNT - 1)  // 55

#define LAPIC_TIMER_VECTOR      0xFE    // 254
#define LAPIC_SPURIOUS_VECTOR   0xFF    // 255

// MSI (message-signalled interrupt) device vectors. MSI is delivered
// straight to the LAPIC (acked with LAPIC EOI, not the IOAPIC) so these sit
// outside the IOAPIC GSI range (32-55). Dispatched explicitly in irq_handler.
#define AHCI_MSI_VECTOR         0x70    // 112 — AHCI controller
/* One vector per USB controller, not one for all of them.
 *
 * MSI is a message carrying a vector, and two devices told to send the same
 * message are two devices the processor cannot tell apart. A machine has as
 * many USB controllers as it has — a chipset one and, very often, another on a
 * graphics card — and giving them all 0x71 meant every interrupt had to be
 * offered to every controller in turn, on the chance that it was the one that
 * spoke. That works and it is not right: an interrupt should name its source.
 *
 * Eight is the number of controllers this kernel will bring up, so it is the
 * number of vectors reserved for them. */
#define XHCI_MSI_VECTOR         0x71    // 113 — first USB xHCI controller
#define XHCI_MSI_VECTOR_COUNT   8
#define XHCI_MSI_VECTOR_LAST    (XHCI_MSI_VECTOR + XHCI_MSI_VECTOR_COUNT - 1)

// AMP inter-processor interrupt vectors
#define IPI_WAKE_VECTOR         0xF0    // 240 — wake idle AP or reschedule
#define IPI_SHOOTDOWN_VECTOR    0xF1    // 241 — TLB shootdown
#define IPI_PANIC_VECTOR        0xF2    // 242 — broadcast halt on panic

// Interrupt controller type
typedef enum {
    IRQCHIP_PIC,        // Legacy 8259A PIC (fallback)
    IRQCHIP_APIC        // Local APIC + IO-APIC (preferred)
} irqchip_type_t;

/* Abstract interrupt controller operations.
 *
 * Arming and disarming are NOT here. A line is armed from its Summons, and a
 * description is not something a `void (*)(uint8_t)` can carry — the two
 * wrappers that used to sit here took a bare number and each backend then
 * guessed the rest, which is the defect this file exists to close. Arming
 * lives in SummonsArm, which knows which backend is active and hands each one
 * what it can actually use. What stays here is what genuinely differs between
 * the two chips and needs no description: their name, how an interrupt is
 * acknowledged, and what they can be asked about themselves. */
typedef struct irq_chip {
    const char* name;
    void (*send_eoi)(uint8_t gsi);
    uint32_t (*get_isr)(void);
    uint32_t (*get_irr)(void);
    uint8_t max_irqs;
} irq_chip_t;

// Global API - all drivers use these instead of pic_* directly
void irqchip_init(void);
void irqchip_enable_irq(uint8_t gsi);
void irqchip_disable_irq(uint8_t gsi);
void irqchip_send_eoi(uint8_t gsi);
uint32_t irqchip_get_isr(void);
uint32_t irqchip_get_irr(void);

irqchip_type_t irqchip_get_type(void);
const char* irqchip_get_name(void);
uint8_t irqchip_max_irqs(void);

/* Access to the MADT info captured by irqchip_init(). Returns NULL when
 * running on the PIC fallback path (no MADT). Used by AP bring-up to
 * apply Local APIC NMI assignments per-CPU. */
struct madt_info; /* forward decl from acpi_madt.h */
const struct madt_info *irqchip_get_madt(void);

/* Apply MADT LAPIC NMI entries to the *currently executing* CPU.
 * Looks up the running CPU's APIC ID via lapic_get_id(), finds the
 * matching ACPI processor ID in the MADT CPU map, and writes the LVT
 * NMI bits. Safe to call from the AP entry path after lapic_enable. */
void irqchip_apply_lapic_nmi_self(void);

/* ===========================================================================
 * Summons — an interrupt line that arrives DESCRIBED.
 *
 * A `uint8_t gsi` says which pin. It does not say how the pin is driven, and
 * this kernel used to fill that in by omission: ioapic_enable_irq asked the
 * MADT Interrupt Source Override table and, finding nothing, programmed the
 * redirection entry with neither POLARITY_LOW nor TRIGGER_LEVEL — edge
 * triggered, active high. That default is the ISA bus's, and the override
 * table it was asked is by construction about ISA sources. No PCI line has an
 * ISO entry, so every PCI INTx line this kernel ever armed was armed wrong.
 *
 * PCI Local Bus 3.0 §2.2.6, "Interrupt Pins (Optional)": "Interrupts on PCI
 * are optional and defined as 'level sensitive,' asserted low (negative true),
 * using open drain output drivers. ... Once the INTx# signal is asserted, it
 * remains asserted until the device driver clears the pending request." An
 * open-drain line held low until a driver clears the device is the definition
 * of level triggered and active low, and it is not a property of one board —
 * it is what INTx IS.
 *
 * The same section, one page on: "This means the device driver may not make
 * any assumptions about interrupt sharing. All PCI device drivers must be able
 * to share an interrupt (chaining) with any other logical device including
 * devices in the same multi-function package." §6.5 repeats it as a
 * requirement: "PCI interrupts are shareable. PCI device drivers are required
 * to support shared interrupts, since it is very likely that system
 * implementations will connect more than one device to a single interrupt
 * line."
 *
 * ‼ THOSE TWO FACTS ARE ONE DEFECT, not two. A shared line MUST be level
 * triggered, because a second device pulling INTx# low while the first is
 * still holding it low produces NO NEW EDGE. Under edge programming that
 * second device's interrupt is lost for ever. Fixing the sharing without
 * fixing the trigger, or the other way round, fixes nothing.
 *
 * So a line is described before it is armed, and the description says where
 * each of its parts came from. Sources are ordered by how much they are worth
 * believing, and a better one overrides a worse one IN THE OPEN:
 *
 *   BUS RULE          the weakest. PCI INTx = level, active low, shareable;
 *                     ISA = edge, active high, exclusive. Until now this was
 *                     implied, and implied wrong. It is now stated.
 *   INTERRUPT LINE    PCI config 0x3C. §6.2.4 footnote 43: "For x86 based PCs,
 *                     the values in this register correspond to IRQ numbers
 *                     (0-15) of the standard dual 8259 configuration. The
 *                     value 255 is defined as meaning 'unknown' or 'no
 *                     connection' to the interrupt controller. Values between
 *                     15 and 254 are reserved." A byte about the 8259, read on
 *                     a machine running its IOAPIC. It names the GSI here only
 *                     because nothing better has been built yet, and it is
 *                     MARKED so that when _PRT arrives it is seen to beat it.
 *   FIRMWARE OVERRIDE MADT Interrupt Source Override (ACPI 6.5 §5.2.12.5).
 *                     The board's own word about one line, and it wins over
 *                     the bus rule — except where it says 00, which per
 *                     Table 5.26 means "Conforms to the specifications of the
 *                     bus", i.e. defers BACK to the bus rule. That deferral is
 *                     the case the old code got wrong: it read 00 as "not
 *                     level, not low".
 *   ROUTING TABLE     ACPI _PRT, and a PCI Interrupt Link's _CRS behind it.
 *                     The real answer to "which GSI", because it is the only
 *                     source written about THIS machine in THIS interrupt
 *                     mode — see AmlRouteForPciPin. It outranks the byte at
 *                     0x3C for the pin, and a link's _CRS outranks the bus
 *                     rule for the wiring.
 *
 * A line for which there is no description at all is a REFUSAL SAID ALOUD,
 * never a quiet "as usual". See ioapic_enable_irq.
 *
 * ‼ AND A PIN IS OWNED BY ONE LINE. A redirection entry carries one vector,
 * so a pin delivers under one vector, so the responder registry — which
 * irq_handler can only reach through the vector it was given — holds one
 * chain per pin. Two descriptions that name the same pin under different
 * vectors are not two lines; they are one line described twice, and the
 * second must JOIN the first rather than rewrite its entry. That is
 * SummonsSettleOnPin, and without it the ACPI SCI on ISA IRQ 9 remapped to
 * INTIN 11 (vector 41, pin 11) and a PCI function _PRT puts on GSI 11
 * (vector 43, pin 11) were invisible to each other: the second arming
 * rewrote the first's entry and the SCI was never delivered again.
 * ========================================================================= */

typedef enum {
    SUMMONS_FROM_NOBODY            = 0,
    SUMMONS_FROM_BUS_RULE          = 1,
    SUMMONS_FROM_INTERRUPT_LINE    = 2,
    SUMMONS_FROM_FIRMWARE_OVERRIDE = 3,
    SUMMONS_FROM_ROUTING_TABLE     = 4
} SummonsSource;

typedef enum {
    SUMMONS_ACTIVE_HIGH = 0,
    SUMMONS_ACTIVE_LOW  = 1
} SummonsPolarity;

typedef enum {
    SUMMONS_EDGE  = 0,
    SUMMONS_LEVEL = 1
} SummonsTrigger;

typedef struct Summons {
    /* Which IOAPIC input the line lands on. */
    uint32_t        gsi;

    /* Which IDT vector it is delivered under, and — because irq_handler
     * derives one from the other — the key the responder registry uses.
     *
     * It is carried rather than computed because the two numbers are NOT
     * always gsi + IRQ_VECTOR_BASE. An ISA line keeps the vector of its
     * PRE-override IRQ number while its pin moves: with the usual MADT
     * override of IRQ 0 onto GSI 2, the timer is pin 2 carrying vector 32.
     * A PCI line has no ISA number and no override, so there vector really
     * is gsi + IRQ_VECTOR_BASE. Computing one from the other at a call site
     * is how a PCI line ends up armed on the pin some unrelated ISA
     * override happened to name. */
    uint8_t         vector;

    /* 1..4 = INTA#..INTD#, the value of the PCI Interrupt Pin register; 0 for
     * a line that is not PCI INTx. Kept because (device, pin) is exactly the
     * key _PRT is indexed by — this is the field the next stage looks up. */
    uint8_t         intx_pin;

    SummonsPolarity polarity;
    SummonsTrigger  trigger;

    /* May more than one device answer this line? A PCI INTx line always may.
     * An ISA line never does, and a second answerer on one is a mistake worth
     * refusing rather than a configuration worth supporting. */
    bool            shared;

    SummonsSource   gsi_from;
    SummonsSource   wiring_from;

    /* "PCI" or "ISA" — the bus whose rule was applied, named in every line
     * this prints so a refusal says which rule it was judging by. */
    const char     *bus;
} Summons;

/* The ISA rule for one legacy IRQ number, with the firmware's GSI remap
 * already applied. Edge triggered, active high, exclusive — ACPI 6.5 §5.2.12.5
 * Table 5.26 names this bus by name for the trigger default: "00 Conforms to
 * specifications of the bus (For example, ISA is edge-triggered)". */
Summons SummonsForIsaLine(uint8_t isa_irq);

/* Let the MADT Interrupt Source Override have its say over the bus rule,
 * field by field, leaving anything the firmware marked 00 ("conforms to the
 * bus") as the bus rule left it. Idempotent. */
void SummonsResolve(Summons *line);

/* The MPS INTI flag word (ACPI 6.5 §5.2.12.5) this description amounts to.
 * Never 00 in either field: a resolved description has an answer, so it states
 * 01 or 11 and defers to nothing. */
uint16_t SummonsMpsFlags(const Summons *line);

/* Is there a pin for this GSI on a mapped IOAPIC, and can its redirection
 * entry name this destination? Implemented in ioapic.c, because the GSI base,
 * the pin count and the width of the destination field are that chip's own
 * facts and copying them anywhere else is how two places come to disagree
 * about what the hardware is.
 *
 * Asked by two callers for two purposes and answered once, so that the tests
 * and the wording of their refusals exist in a single place: the arming path
 * must not write a redirection entry it cannot reach, and SummonsArm must be
 * able to tell its caller "this line will not work" while the driver still
 * has a choice to make about polling. False prints why.
 *
 * Meaningless on the PIC backend and not consulted there — an 8259 has no
 * destination field and sixteen fixed inputs, and SummonsArm asks its own
 * question of it. */
bool SummonsPinIsReachable(uint32_t gsi, uint32_t dest_lapic_id);

/* What the FIRMWARE said about this GSI, and only the firmware: the MADT
 * Interrupt Source Override table, never the resolved description this
 * kernel wrote there itself. Implemented in ioapic.c, which owns that table.
 * See the long note above its definition for why asking the wider
 * ioapic_gsi_flags made this kernel report its own conclusion as the
 * board's. */
bool SummonsFirmwareSaid(uint32_t gsi, uint16_t *out_flags);

/* The ACPI System Control Interrupt, described by the specification rather
 * than by the ISA bus rule its number comes from.
 *
 * ACPI 6.5 §5.2.9, Table 5.9, SCI_INT: "System vector the SCI interrupt is
 * wired to in 8259 mode. On systems that do not contain the 8259, this field
 * contains the Global System interrupt number of the SCI interrupt. OSPM is
 * required to treat the ACPI SCI interrupt as a shareable, level, active low
 * interrupt." All three words are requirements on OSPM, and SHAREABLE is the
 * one SummonsForIsaLine cannot give it: the SCI carries an ISA number, so it
 * was born exclusive, and a PCI function that _PRT puts on the same pin —
 * the ordinary PIRQ outcome on IRQ 9, 10 or 11 — was refused the line and
 * dropped to polling. */
Summons SummonsForAcpiSci(uint8_t sci_int);

/* Program the interrupt controller FROM THE DESCRIPTION and unmask the line.
 * Resolves, settles onto its pin and posts it first, so the registry and the
 * redirection entry can never disagree about what the line is. False when
 * the line could not be armed, and the reason is always printed.
 *
 * NOT const: on success the caller's description is replaced by the one that
 * was actually programmed. Settling can move a line onto the vector its pin
 * already delivers under, and onto the wiring that pin already carries, and
 * a driver that then prints or records its own copy would be describing a
 * line that does not exist. */
bool SummonsArm(Summons *line);

/* Mask the line at its own pin. Takes the description rather than a number so
 * that it masks `line->gsi` and not whatever pin an unrelated ISA override
 * maps that number to. */
void SummonsDisarm(const Summons *line);

const char *SummonsSourceName(SummonsSource from);

// Convert between GSI and IDT vector
static inline uint8_t irq_gsi_to_vector(uint8_t gsi) {
    return IRQ_VECTOR_BASE + gsi;
}
static inline uint8_t irq_vector_to_gsi(uint8_t vector) {
    return vector - IRQ_VECTOR_BASE;
}

#endif // IRQCHIP_H
