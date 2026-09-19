#ifndef IDT_H
#define IDT_H

#include "ktypes.h"
#include "kernel_config.h"
#include "tss.h"
#include "boxos_limits.h"
#include "irqchip.h"   /* Summons — a line arrives described, and so is answered */


#define IDT_TYPE_INTERRUPT_GATE 0x8E  // Present, Ring 0, Interrupt Gate
#define IDT_TYPE_TRAP_GATE      0x8F  // Present, Ring 0, Trap Gate
#define IDT_TYPE_USER_INTERRUPT 0xEE  // Present, Ring 3, Interrupt Gate

// System call vector (Snowball architecture: notify() syscall)
#define SYSCALL_VECTOR          CONFIG_SYSCALL_VECTOR

#define EXCEPTION_DIVIDE_ERROR      0
#define EXCEPTION_DEBUG             1
#define EXCEPTION_NMI               2
#define EXCEPTION_BREAKPOINT        3
#define EXCEPTION_OVERFLOW          4
#define EXCEPTION_BOUND_RANGE       5
#define EXCEPTION_INVALID_OPCODE    6
#define EXCEPTION_DEVICE_NOT_AVAIL  7
#define EXCEPTION_DOUBLE_FAULT      8
#define EXCEPTION_INVALID_TSS       10
#define EXCEPTION_SEGMENT_NOT_PRESENT 11
#define EXCEPTION_STACK_FAULT       12
#define EXCEPTION_GENERAL_PROTECTION 13
#define EXCEPTION_PAGE_FAULT        14
#define EXCEPTION_FPU_ERROR         16
#define EXCEPTION_ALIGNMENT_CHECK   17
#define EXCEPTION_MACHINE_CHECK     18
#define EXCEPTION_SIMD_EXCEPTION    19

#define IRQ_TIMER       32  // IRQ 0 -> INT 0x20
#define IRQ_KEYBOARD    33  // IRQ 1 -> INT 0x21
#define IRQ_CASCADE     34  // IRQ 2 (internal cascade)
#define IRQ_COM2        35  // IRQ 3
#define IRQ_COM1        36  // IRQ 4
#define IRQ_LPT2        37  // IRQ 5
#define IRQ_FLOPPY      38  // IRQ 6
#define IRQ_LPT1        39  // IRQ 7
#define IRQ_RTC         40  // IRQ 8
#define IRQ_FREE1       41  // IRQ 9
#define IRQ_FREE2       42  // IRQ 10
#define IRQ_FREE3       43  // IRQ 11
#define IRQ_MOUSE       44  // IRQ 12
#define IRQ_FPU         45  // IRQ 13
#define IRQ_ATA_PRIMARY 46  // IRQ 14
#define IRQ_ATA_SECONDARY 47 // IRQ 15

typedef struct {
    uint16_t offset_low;    // Offset bits 0-15
    uint16_t selector;      // Code segment selector
    uint8_t  ist;           // Interrupt Stack Table offset (bits 0-2)
    uint8_t  type_attr;
    uint16_t offset_middle; // Offset bits 16-31
    uint32_t offset_high;   // Offset bits 32-63
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_descriptor_t;

// Register frame passed to ISR handlers - must match isr_common push order exactly
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;  // Saved by CPU on interrupt entry
} __attribute__((packed)) interrupt_frame_t;

typedef void (*irq_callback_t)(void);

void idt_init(void);
void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type_attr, uint8_t ist);
void idt_load(void);
void idt_test(void);

/* ONE LINE, AS MANY ANSWERERS AS THE BUS ALLOWS.
 *
 * PCI 3.0 §6.5: "PCI interrupts are shareable. PCI device drivers are
 * required to support shared interrupts, since it is very likely that system
 * implementations will connect more than one device to a single interrupt
 * line." Until now this kernel kept ONE `irq_callback_t` per line and the
 * second driver to arrive overwrote the first — silently, because the warning
 * it printed went through debug_printf, which compiles to nothing. Two
 * functions of one chipset device on one INTx line is the ordinary case, not
 * an exotic one.
 *
 * ‼ EVERY ANSWERER IS CALLED, AND EACH DECIDES FOR ITSELF WHETHER THE
 * INTERRUPT WAS ITS OWN. The signature stays `void (*)(void)`. The other
 * shape — every handler returning "mine" or "not mine" — was considered and
 * rejected on three counts. It changes the signature of every registering
 * driver in the kernel, including four that a shared PCI line can never
 * reach (the ACPI SCI, COM1, and both legacy IDE channels), for a fact none
 * of them has any use for. Every handler here ALREADY begins by reading its
 * own device's status register — AHCI reads IS, xHCI reads IMAN, the UART
 * reads IIR, the SCI reads PM1_STS — so the answer would be a second reading
 * of a register just read, or a duplicate of a decision just made. And the
 * only thing a kernel does with "nobody claimed it" is count unclaimed
 * interrupts and shut the line down after some number of them, which is a
 * number standing in for a mechanism; what this kernel does instead is at the
 * bottom of irq_handler, where a line that fires with NO answerer at all —
 * a fact, not a tally — is masked at its pin and named.
 *
 * The node belongs to the ANSWERER, as a Baton node belongs to the work that
 * passes it. Nothing here allocates: what bounds the number of answerers is
 * the number of devices the machine has, which is not a number this file
 * could invent, and a driver that has a controller record already has
 * somewhere to keep one link.
 *
 * `who` is printed in refusals and in the boot log, so "this line is already
 * spoken for" says by whom. */
typedef struct SummonsResponder {
    irq_callback_t            answer;
    const char               *who;
    struct SummonsResponder  *next;
} SummonsResponder;

/* ‼ THE PIN OWNS THE VECTOR, AND THE FIRST LINE ON IT OWNS THE WIRE.
 *
 * A redirection entry carries one vector, so a pin delivers under one
 * vector, so this registry — which irq_handler can reach only through the
 * vector it was handed — holds one chain per pin. But a Summons carries BOTH
 * numbers and they are not always gsi + IRQ_VECTOR_BASE: an ISA line keeps
 * the vector of its pre-override IRQ number while its pin moves. So two
 * descriptions can name one pin under two vectors, and before this existed
 * they were invisible to one another — the second arming rewrote the first's
 * redirection entry and the first line's answerers were never called again.
 *
 * Settling asks the registry who already holds `line->gsi`. If somebody
 * does, the newcomer ADOPTS that vector — it is joining a wire, not claiming
 * one — and, if that line already has answerers, it also takes the wiring
 * already programmed into the pin: an entry other drivers depend on is not
 * re-cut because a second device arrived, and a disagreement about it is
 * said aloud rather than resolved by whoever ran last. Shareability is the
 * one field that is merged rather than inherited, and it is merged by AND:
 * a line is shareable only if everyone describing it says so.
 *
 * False, with the reason printed, when the line cannot be settled at all:
 * a vector outside the dispatched window, or a vector already delivering a
 * DIFFERENT pin, which no amount of joining can reconcile. */
bool SummonsSettleOnPin(Summons *line);

/* Record what a line IS, without answering it. Called by SummonsArm so that
 * an armed line irq_handler has to deal with is always one it can describe.
 * Takes an already-settled description: the invariant that the registry and
 * the redirection entry never disagree is enforced in SummonsSettleOnPin,
 * and this refuses aloud rather than storing one that was not. */
void SummonsPost(const Summons *line);

/* Answer a line. Refused — aloud — when the line is not shareable and
 * somebody already answers it, when two answerers disagree about which pin
 * the line is, or when the vector is outside the dispatched range. Does NOT
 * arm the line: acpi.c deliberately registers the SCI handler long before it
 * opens that door, and that order is worth keeping available. */
bool SummonsAnswer(const Summons *line, SummonsResponder *node,
                   irq_callback_t answer, const char *who);

/* The ISA-era form: one exclusive edge-triggered line, one answerer, and a
 * node the registry keeps on the line's behalf because a legacy caller has
 * nowhere to put one. Equivalent to SummonsAnswer with SummonsForIsaLine. */
void irq_register_handler(uint8_t irq, irq_callback_t callback);

void exception_handler(interrupt_frame_t* frame);
void irq_handler(interrupt_frame_t* frame);
void syscall_handler(interrupt_frame_t* frame);

// Switch syscall dispatch between sync (single-core) and async (multi-core).
// Must be called after kcore_init() and before any user processes start.
void idt_set_syscall_mode(bool multicore);

extern void* isr_table[IDT_ENTRIES];

#endif // IDT_H
