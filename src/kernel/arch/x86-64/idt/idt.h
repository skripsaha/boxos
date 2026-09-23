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

/* The sixteen ISA IRQ numbers used to be spelled out here as vectors —
 * IRQ_TIMER 32 through IRQ_ATA_SECONDARY 47 — which was the formula
 * "32 + number" written by hand a third time. A grep of the tree matched none
 * of them anywhere but their own definitions. Deleted rather than updated: a
 * line's vector is given to it by the pool now, and a name that claims to know
 * it in advance would be wrong on the first boot. */

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

/* The kernel's own two lines, answered like any other.
 *
 * The tick and the 8042 used to be dispatched by a switch on the ISA number
 * at the bottom of irq_handler, which is to say by arithmetic on the vector,
 * which is to say by the one assumption the vector pool removes. They are
 * ordinary answerers now; whoever brings each line up hands the matching one
 * of these to irq_register_handler. */
/* The frame the interrupt currently being served arrived on, or NULL outside
 * a handler. The tick asks for it because preemption means writing a return
 * frame; nothing else in this kernel needs it. ‼ Valid only for the duration
 * of the answerer — it points into the interrupted core's stack. */
interrupt_frame_t *IrqFrameInFlight(void);

void IrqTheTickAnswers(void);
void IrqTheKeyboardAnswers(void);
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
/* Register an answerer for a legacy ISA line.
 *
 * `row` is not optional and has no default on purpose: it is what this line
 * costs when it is late, and the driver is the only one who knows. It decides
 * the priority class the line is delivered in -- see right_of_way.h. */
void irq_register_handler(uint8_t irq, irq_callback_t callback, RightOfWay row);

/* Answer a message-signalled interrupt: a registry entry keyed by vector with
 * no pin behind it. The vector comes from the pool like any other line's, and
 * the device was told it by whoever programmed its MSI capability. */
bool IrqAnswerMessage(uint8_t vector, SummonsResponder *node,
                      irq_callback_t answer, const char *who);

/* Stop answering a line, and give its vector back when the last answerer on it
 * leaves. The caller must already have stopped its device from raising the
 * line; this masks the pin, unlinks the node and returns the number, in that
 * order and for that reason. */
bool SummonsUnanswer(const Summons *line, SummonsResponder *node,
                     const char *who);

/* ‼ There is deliberately no message-signalled counterpart to this. One was
 * written and then removed, because nothing in this kernel calls it: every
 * path that takes an MSI vector either keeps it for the life of the boot or
 * fails before registering an answerer, and a function with no caller is a
 * function nobody has ever run. When a controller that has taken a message
 * can actually leave, the counterpart is ten lines — unlink the node, and
 * give the number back — and the source is stopped by clearing the device's
 * own MSI enable, which is the caller's doing either way. */

void exception_handler(interrupt_frame_t* frame);
void irq_handler(interrupt_frame_t* frame);
void syscall_handler(interrupt_frame_t* frame);

// Switch syscall dispatch between sync (single-core) and async (multi-core).
// Must be called after kcore_init() and before any user processes start.
void idt_set_syscall_mode(bool multicore);

extern void* isr_table[IDT_ENTRIES];

#endif // IDT_H
