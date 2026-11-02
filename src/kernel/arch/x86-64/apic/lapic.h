#ifndef LAPIC_H
#define LAPIC_H

#include "ktypes.h"

// Local APIC register offsets (from LAPIC base address)
#define LAPIC_REG_ID            0x020   // Local APIC ID
#define LAPIC_REG_VERSION       0x030   // Local APIC Version
#define LAPIC_REG_TPR           0x080   // Task Priority Register
#define LAPIC_REG_EOI           0x0B0   // End of Interrupt
#define LAPIC_REG_SVR           0x0F0   // Spurious Interrupt Vector Register
#define LAPIC_REG_ISR_BASE      0x100   // In-Service Register (8 x 32-bit)
#define LAPIC_REG_IRR_BASE      0x200   // Interrupt Request Register (8 x 32-bit)
#define LAPIC_REG_ESR           0x280   // Error Status Register
#define LAPIC_REG_ICR_LOW       0x300   // Interrupt Command Register (low 32 bits)
#define LAPIC_REG_ICR_HIGH      0x310   // Interrupt Command Register (high 32 bits)
#define LAPIC_REG_TIMER_LVT     0x320   // Timer LVT Entry
#define LAPIC_REG_LINT0_LVT     0x350   // Local Interrupt 0 LVT
#define LAPIC_REG_LINT1_LVT     0x360   // Local Interrupt 1 LVT
#define LAPIC_REG_TIMER_ICR     0x380   // Timer Initial Count Register
#define LAPIC_REG_TIMER_CCR     0x390   // Timer Current Count Register
#define LAPIC_REG_TIMER_DCR     0x3E0   // Timer Divide Configuration Register

// SVR flags
#define LAPIC_SVR_ENABLE        (1 << 8)    // APIC Software Enable

/* The highest APIC ID an eight-bit physical destination can NAME.
 *
 * Every field that carries a destination in the compatibility formats is the
 * same eight bits: the xAPIC ICR's destination (bits 63:56), the IOAPIC
 * redirection entry's destination in physical mode (also bits 63:56), and
 * the MSI/MSI-X Message Address bits 19:12 — Intel SDM Vol 3A, "MESSAGE
 * SIGNALLED INTERRUPTS" / "Message Address Register Format": "Destination
 * ID — This field contains an 8-bit destination ID. It identifies the
 * message's target processor(s). The destination ID corresponds to bits
 * 63:56 of the I/O APIC Redirection Table Entry if the IOAPIC is used to
 * dispatch the interrupt to the processor(s)."
 *
 * Those eight bits do not hold 256 processors. Intel SDM Vol 3A, chapter
 * "ADVANCED PROGRAMMABLE INTERRUPT CONTROLLER (APIC)", section "Physical
 * Destination Mode": "For Pentium 4 and Intel Xeon processors, either a
 * single destination (local APIC IDs 00H through FEH) or a broadcast to all
 * APICs (the APIC ID is FFH) may be specified in physical destination mode."
 * FFH is not processor 255 — it is EVERY processor. Writing it as a
 * destination does not fail to reach one core, it reaches all of them: an
 * INIT so addressed resets the machine, and a device line so addressed is
 * asserted on every core at once.
 *
 * CITED BY TITLE RATHER THAN BY SECTION NUMBER, and deliberately. Intel
 * renumbers this chapter between revisions of Volume 3A: the paragraph above
 * is §10.6.2.1 in the older three-part System Programming Guide, §11.6.2.1
 * in order number 253668-082US (December 2023), and §13.6.2.1 in the merged
 * guide, order number 325384-092US (June 2026). The last two were both read
 * to write this comment and their text is identical word for word. A bare
 * section number sends the next reader to whatever happens to sit at that
 * number in the edition on their desk; the chapter and section TITLES have
 * not moved in any of them. Every citation of this chapter in this kernel is
 * written that way for that reason.
 *
 * The same paragraph's rule appears a second time in "Message Address
 * Register Format", field 3 (RH): "When RH is 1 and the physical destination
 * mode is used, the Destination ID field must not be set to FFH; it must
 * point to a processor that is present and enabled to receive the
 * interrupt." FFH is the value that does not point to a processor.
 *
 * ACPI agrees on the boundary and states it as a requirement on firmware
 * (6.5 §5.2.12.12, "Processor Local x2APIC Structure"): "Logical processors
 * with APIC ID values 255 and greater must use the Processor Local x2APIC
 * structure". 255 is already outside the nameable range, which is why the
 * spec starts the wide form there. */
#define LAPIC_XAPIC_DEST_MAX        0xFEu
#define LAPIC_XAPIC_DEST_BROADCAST  0xFFu

/* The same word at the other width.
 *
 * x2APIC widened the destination to 32 bits and gave the all-ones value the
 * same job — Intel SDM Vol 3A, chapter "ADVANCED PROGRAMMABLE INTERRUPT
 * CONTROLLER (APIC)", section "ICR Operation in x2APIC Mode" (§11.12.9 in
 * 253668-082US, §13.12.9 in 325384-092US): "A destination ID value of
 * FFFF_FFFFH is used for broadcast of interrupts in both logical destination
 * and physical destination modes."
 *
 * So 0xFFFFFFFF does not mean "no processor here", and a MADT entry carrying
 * it is not thereby an empty seat. It is the one 32-bit ID that names ALL
 * processors: an INIT sent to it in x2APIC mode resets every core on the
 * machine, this one included. Whether a seat holds a usable processor is a
 * question the firmware answers in that entry's FLAGS — ACPI 6.5 Table 5.23:
 * "If this bit is clear and the Online Capable bit is also clear, this
 * processor is unusable, and OSPM shall ignore the contents of the Processor
 * Local APIC Structure" — and never in its ID. This constant exists so that
 * an ID which cannot name one processor is REFUSED and said out loud, the
 * way LAPIC_XAPIC_DEST_BROADCAST is at eight bits, instead of being skipped
 * as if the firmware had meant nothing by it. */
#define LAPIC_X2APIC_DEST_BROADCAST 0xFFFFFFFFu

// LVT flags (Intel SDM Vol 3A, "Local Vector Table")
#define LAPIC_LVT_MASKED            (1u << 16)   // Interrupt masked
#define LAPIC_LVT_TIMER_PERIODIC    (1u << 17)   // Periodic timer mode (bits 18:17 = 01)
#define LAPIC_LVT_TIMER_TSC_DEADLINE (2u << 17)  // TSC-deadline mode (bits 18:17 = 10)
#define LAPIC_LVT_TRIGGER_LEVEL     (1u << 15)   // 1 = level, 0 = edge
#define LAPIC_LVT_REMOTE_IRR        (1u << 14)   // RO; for level-triggered
#define LAPIC_LVT_PIN_POLARITY_LOW  (1u << 13)   // 1 = active low, 0 = active high
#define LAPIC_LVT_DELIVERY_FIXED    (0u << 8)
#define LAPIC_LVT_DELIVERY_SMI      (2u << 8)
#define LAPIC_LVT_DELIVERY_NMI      (4u << 8)
#define LAPIC_LVT_DELIVERY_EXTINT   (7u << 8)
#define LAPIC_LVT_DELIVERY_INIT     (5u << 8)

// Timer divider values for DCR
#define LAPIC_TIMER_DIV_1       0x0B
#define LAPIC_TIMER_DIV_2       0x00
#define LAPIC_TIMER_DIV_4       0x01
#define LAPIC_TIMER_DIV_8       0x02
#define LAPIC_TIMER_DIV_16      0x03
#define LAPIC_TIMER_DIV_32      0x08
#define LAPIC_TIMER_DIV_64      0x09
#define LAPIC_TIMER_DIV_128     0x0A

// IA32_APIC_BASE MSR
#define MSR_APIC_BASE           0x1B
#define MSR_APIC_BASE_ENABLE    (1 << 11)
#define MSR_APIC_BASE_EXTD      (1 << 10)   // x2APIC enable bit
#define MSR_APIC_BASE_BSP       (1 << 8)
#define MSR_APIC_BASE_ADDR_MASK 0xFFFFF000ULL

// LAPIC TSC-deadline timer MSR (Intel SDM Vol 3A, "TSC-Deadline Mode")
#define MSR_IA32_TSC_DEADLINE   0x6E0

// x2APIC MSR layout (Intel SDM Vol 3A, "x2APIC Register Address Space")
#define MSR_X2APIC_BASE         0x800       // x2APIC MSR window starts here
#define MSR_X2APIC_APICID       0x802       // APIC ID: 32-bit RO MSR (replaces MMIO 0x20)
#define MSR_X2APIC_ICR          0x830       // ICR: single 64-bit MSR write

void lapic_init(uintptr_t base_addr);
void lapic_send_eoi(void);
uint32_t lapic_get_id(void);
void lapic_enable(void);
void lapic_disable(void);
bool lapic_is_enabled(void);
uintptr_t lapic_get_base(void);
bool lapic_is_mapped(void);

// LAPIC timer
void lapic_timer_init(uint8_t vector, uint32_t frequency_hz);
void lapic_timer_stop(void);

/* Re-arm the LAPIC timer for the next tick. No-op unless the timer is running
 * in TSC-deadline mode (one-shot per deadline), in which case it programs the
 * next absolute TSC deadline. Called from the LAPIC timer IRQ handler. */
void lapic_timer_rearm(void);

/* Register access, mode-correct on both xAPIC and x2APIC.
 *
 * The argument is always the legacy xAPIC MMIO offset. In xAPIC mode it is
 * used as such; in x2APIC mode it is translated to the matching MSR,
 * 0x800 + (offset >> 4), because the MMIO window is not decoded at all once
 * IA32_APIC_BASE.EXTD is set (Intel SDM Vol 3A, "x2APIC Register
 * Availability"). Callers never need to know which mode is live.
 *
 * The two ICR offsets are the exception: in x2APIC mode the ICR is a single
 * 64-bit MSR and cannot be written a half at a time. Use lapic_icr_write. */
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

// IPI (Inter-Processor Interrupt) command values for ICR.
// No INIT-deassert command: required only for the 82489DX discrete APIC
// (Intel SDM Vol 3A, "MP Initialization Protocol Requirements and
// Expectations") — modern integrated APICs treat INIT as
// edge-triggered and ignore the deassert. See amp.c::send_init_ipi.
#define LAPIC_IPI_INIT          0x00004500  // INIT assert
#define LAPIC_IPI_SIPI          0x00004600  // Startup IPI (OR with page number in low byte)

// ICR delivery status bit
#define LAPIC_ICR_SEND_PENDING  (1 << 12)

// ICR Delivery Mode 100b — NMI; the vector field is ignored for it
#define LAPIC_ICR_DELIVERY_NMI  (4u << 8)

// IPI delivery
void lapic_send_ipi(uint32_t dest_lapic_id, uint8_t vector);
void lapic_send_ipi_all_excluding_self(uint8_t vector);

/* An NMI to one core: taken whatever its RFLAGS.IF says, so it reaches a core
 * that keeps its interrupts off — the stop of a panic (klib_panic.c). The same
 * no-refusal rule as the directed send above. */
void LapicSendNmi(uint32_t dest_lapic_id);

/* Returns true if the local APIC is currently running in x2APIC mode
 * (IA32_APIC_BASE bit 10 = EXTD). Sampled once at lapic_init and cached;
 * BoxOS never disables x2APIC at runtime so the cached value is stable.
 * Read by lapic_read/lapic_write to pick the register access path, and by
 * amp_boot_aps to refuse to INIT a core whose APIC ID is wider than the
 * 8-bit destination field the xAPIC ICR has to offer. */
bool lapic_is_x2apic_active(void);

/* Write the LAPIC Interrupt Command Register in a mode-correct way.
 * This is the BRING-UP entry point: it is allowed to refuse.
 *
 *   xAPIC:  wait for the delivery-status bit of the PREVIOUS IPI to clear,
 *           write ICR_HIGH (dest<<24), write ICR_LOW (cmd) — the LOW write
 *           triggers the send. All three happen with interrupts disabled
 *           and the caller's flags restored afterwards: ICR_HIGH holds the
 *           destination the LOW write sends to, so an interrupt handler
 *           that sends its own IPI in between would redirect ours. Linux
 *           brackets the same pair in local_irq_save
 *           (default_send_IPI_single_phys).
 *   x2APIC: a memory fence, then one wrmsr(0x830, (dest << 32) | cmd).
 *           Nothing is waited for, and not because the write completes the
 *           delivery: the delivery-status bit was REMOVED in x2APIC mode,
 *           and the Intel 64 Architecture x2APIC Specification states that
 *           completion of the WRMSR does not guarantee the interrupt has
 *           been received by the targeted processors. There is simply
 *           nothing left to poll, and nothing that can be overwritten.
 *           The fence is the caller's ordering, which WRMSR to an APIC
 *           register does not carry on its own.
 *
 * Returns false when the xAPIC wait ran out of real time with the previous
 * IPI still undelivered. Nothing is written in that case: overwriting the
 * ICR while delivery is pending destroys the pending IPI, and a lost IPI
 * that nobody was told about is how a core ends up waiting forever for a
 * wake that was never sent. The caller decides what a lost IPI means —
 * which is why only the INIT/SIPI path uses this entry point, where a
 * destination that never accepts may simply not exist. The runtime senders
 * below have void callers that could not act on a refusal, so they wait
 * for the ICR instead of dropping the interrupt.
 *
 * The one refusal NO caller can opt out of is a destination wider than the
 * eight bits the xAPIC ICR carries. There the choice is not "send or lose
 * it" but "send it to somebody else": the masked ID names a different,
 * running processor. Every xAPIC send refuses such an ID and says so,
 * including the void senders below, because delivering an INIT or a wake
 * to the wrong core is worse than not delivering it. Only x2APIC mode can
 * name those processors, with the full 32-bit destination of MSR 0x830.
 *
 * Writing the legacy MMIO offsets (ICR_HIGH=0x310, ICR_LOW=0x300) on an
 * x2APIC-enabled CPU does NOT fault: Intel SDM Vol 3A, table "MSR/MMIO
 * Interface of a Local x2APIC in Different Modes of Operation" — MMIO in
 * x2APIC mode "behaves identically to xAPIC in globally disabled state",
 * i.e. the store is silently dropped. The #GP runs the other way, on
 * RDMSR/WRMSR in the 0800H-0BFFH window while NOT in x2APIC mode. Every
 * IPI path must go through this helper precisely BECAUSE the wrong access
 * would not announce itself. */
bool lapic_icr_write(uint32_t dest_id, uint32_t cmd);

/* Apply MADT Local APIC NMI entries to the currently-running CPU's LVT.
 *
 * ACPI 6.5 §5.2.12.7: each Local APIC NMI Structure specifies which LINT
 * pin (LINT0 or LINT1) is wired to NMI on a given processor (acpi_proc_id),
 * or on all processors (0xFFu). OSPM is responsible for programming the
 * LVT to deliver NMI for those pins. Without this step the firmware-
 * provided NMI source (watchdog, IPMI alert, server PSU fault) never
 * reaches the OS.
 *
 * `acpi_processor_uid` is the ACPI Processor UID of THIS CPU, at the full
 * 32 bits Table 5.34 gives it — the Type 9 machine class is the one that
 * needs these entries most (ACPI 6.5 §5.2.12.13: on a machine with any
 * x2APIC ID of 255 or greater "the Local X2APIC NMI structure must be used
 * to specify global LINTx for ALL logical processors"), and it is exactly
 * the class whose UIDs do not fit a byte.
 *
 * `uid_known` false means the caller could not find itself in the MADT at
 * all. Then only the rules the firmware marked as applying to every
 * processor are programmed: a rule addressed to one UID cannot be claimed by
 * a processor that does not know whether it is the one addressed.
 */
struct madt_info; /* forward decl — full definition in acpi_madt.h */
void lapic_apply_madt_nmi(const struct madt_info *info,
                          uint32_t acpi_processor_uid, bool uid_known);

#endif // LAPIC_H
