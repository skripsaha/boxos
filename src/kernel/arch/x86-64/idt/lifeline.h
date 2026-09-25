#ifndef LIFELINE_H
#define LIFELINE_H

#include "ktypes.h"
#include "idt.h" /* interrupt_frame_t */

/* ‼ A LIFELINE IS TIED AROUND ONE CALL INTO CODE THIS KERNEL DID NOT COMPILE.
 *
 * It is NOT a general exception-recovery framework and must never become one.
 * The only faults it catches are faults whose RIP lies inside a range the
 * caller named — firmware, and nothing else. A fault in OUR code while a
 * lifeline is armed still panics, exactly as it did before, because a kernel
 * bug that hides behind a lifeline is a kernel bug nobody ever finds.
 *
 * Not Handrail. Handrail (ap_trampoline.asm) records where a core fell and
 * stops it there; a Lifeline pulls the caller back up and lets it carry on.
 *
 * Why it exists: acpi_reboot climbs a ladder — EFI ResetSystem, the ACPI reset
 * register, 0xCF9, the 8042, a triple fault. A ring-0 fault inside the
 * firmware's ResetSystem matched none of exception_handler's escapes, so it
 * reached the panic block and ended in `cli; hlt`, and rungs two through five
 * were unreachable on the one machine that needed them.
 *
 * SETJMP RULES APPLY, because this is setjmp's shape. A local of the arming
 * function that is written between the arming pass and the fault, and read
 * after the throwback, must be `volatile` — C11 §7.13.2.1 p3. Everything the
 * compiler kept in a caller-saved register across the foreign call is dead by
 * the SysV AMD64 ABI's own rule, so nothing else needs saying.
 */

typedef struct
{
    uint64_t lo, hi; /* [lo, hi) */
} LifelineRange;

/* The pad the arming function is thrown back onto. Caller-allocated, normally
 * on the arming function's own stack, and live for exactly as long as the
 * lifeline is armed.
 *
 * ‼ The field ORDER is load-bearing: LifelineArm is hand-written assembly and
 * addresses these slots by byte offset. lifeline.c carries a _Static_assert per
 * field, so a field inserted here fails the build rather than the machine. */
typedef struct LifelinePad
{
    uint64_t Rip;    /* where LifelineArm would have returned to */
    uint64_t Rsp;    /* stack pointer after that return pops                */
    uint64_t Rbp;    /* callee-saved, SysV AMD64 ABI §3.2.1 (Figure 3.4)    */
    uint64_t Rbx;
    uint64_t R12;
    uint64_t R13;
    uint64_t R14;
    uint64_t R15;
    uint64_t Rflags;
    uint64_t Cr3;
    const LifelineRange *Ranges; /* the caller's array; not copied */
    uint64_t RangeCount;
} LifelinePad;

/* Arm this core's lifeline for a call into foreign code that legitimately
 * executes inside one of `n` ranges. Returns 0 on the arming pass and 1 when
 * the exception handler has thrown control back to this point.
 *
 * ‼ Nothing that can fault may sit between LifelineArm returning 0 and the
 * foreign call itself: a fault there is in OUR code and will panic. */
int LifelineArm(LifelinePad *pad, const LifelineRange *ranges, unsigned n);

/* Disarm this core's lifeline. Idempotent. Must be called immediately after
 * the foreign call returns AND on the thrown-back path. */
void LifelineDisarm(void);

/* Asked by exception_handler. True means the frame has been rewritten to
 * resume at the pad and the handler must return at once. */
bool LifelineCatch(interrupt_frame_t *frame);

/* What the last catch on THIS core recorded, so the caller can say what
 * happened. Per-core: an AP's fall never overwrites the BSP's.
 *
 * ‼ `cr2` IS ONLY A FAULT ADDRESS WHEN `cr2_valid` SAYS SO.
 *
 * CR2 is written by page-fault delivery and by nothing else (Intel SDM Vol 3A
 * §4.7, "Page-Fault Exceptions"), so after a #GP, #UD or #NP the register
 * still holds the address of some EARLIER page fault on this core. Copying it
 * into every fall record made the log name an address that had nothing to do
 * with the fall, and the reader then resolved that address against the EFI
 * memory map and got a confident, fabricated attribution. So the catch fills
 * `cr2` for vector 14 alone; `cr2_valid` is false otherwise and `cr2` is zero.
 *
 * ‼ AND IT IS A LINEAR ADDRESS. CR2 holds the linear address the access used
 * (same section). Anything that answers about PHYSICAL memory — the EFI
 * memory map among them — has to be told which window the linear address came
 * through before it may be asked. */
typedef struct
{
    uint64_t vector, error_code, rip, cr2, cr3;
    bool valid;     /* a fall was caught on this core at all */
    bool cr2_valid; /* `cr2` is THIS fall's linear fault address */
} LifelineFall;

const LifelineFall *LifelineLastFall(void);

/* The CR2 that exception_handler read once at its top, handed over so the fall
 * record can carry it. CR2 is read exactly once per exception and nowhere else
 * (see the block at the head of exception_handler); this exists so the catch
 * path does not become the second reader. Called immediately before
 * LifelineCatch and nowhere else.
 *
 * It is handed over for EVERY vector because it is read for every vector; the
 * catch decides whether this fall is one CR2 describes. */
void LifelineNoteFaultAddress(uint64_t cr2);

#endif /* LIFELINE_H */
