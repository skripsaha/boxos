#ifndef KLIB_PANIC_H
#define KLIB_PANIC_H

#include "ktypes.h"
#include "kstdarg.h" /* va_list — panic()'s message */
#include "idt.h"     /* interrupt_frame_t — the exception's */

/* The one claim of a panic, taken before the first line of either account of
 * one — the exception dump (idt.c), the record of a fatal #MC said ahead of
 * the dump among it (mce.c), and panic()'s banner (PanicClaimForMessage).
 *
 * `already_claimed_here` is the caller's own record that this account holds
 * the claim: false at the start of each account, set here, so a second claim
 * further down the same account returns at once.
 *
 * The core that takes the claim first returns, every other online core
 * stopped by an NMI. A core that finds another core holding it stands where
 * it is and says nothing. The core that holds it and comes here again — a
 * fault or a panic inside its own account — ends that account at the line it
 * broke off in, says in one line more what broke it off — on the wire before
 * the glass is touched (klib_panic.c, PanicSayBreak) — and stands; once the
 * account is said whole (PanicEndAccount) the same one line says what came
 * after it instead, for nothing broke it off. For an
 * exception that line is the numbers of `frame` — vector, error code, RIP —
 * and, for a #PF, `cr2`, the address the handler read on its way in. A core
 * this stops, or that loses the claim, stands with no machine check left in
 * progress on it (MCG_STATUS.MCIP clear). */
void PanicClaimForException(bool *already_claimed_here, const interrupt_frame_t *frame,
                            uint64_t cr2);

/* The same claim, for panic(): at the owner's second coming the line that says
 * what broke the account off — or, the account said whole, what came after it
 * — is "panic: " and `message` formatted with `args`, which may itself fault,
 * and then the core stands at once, the words before the fault on the wire.
 * `args` is read only there, where nothing returns, so the caller formats its
 * own account with it after. */
void PanicClaimForMessage(bool *already_claimed_here, const char *message, va_list args);

/* The end of either account — panic()'s, after its last line, and the dump's
 * (idt.c): the account marked said whole, what it said out to the memory the
 * next boot reads and onto the wire, and its owner standing. What brings the
 * owner to the claim after the mark — a #MC, which nothing masks, taken in the
 * drain or waking it from its hlt — broke nothing off, and its one line says
 * so (klib_panic.c, PanicClaim). Called by the owner alone, with its
 * interrupts off. */
__attribute__((noreturn)) void PanicEndAccount(void);

/* Stands for good, saying nothing and taking nothing, when a core other than
 * this one holds the claim; returns at once otherwise. The NMI a panic stops a
 * core by lands here before it is counted or answered (idt.c,
 * exception_handler), and so does a kprintf begun after the claim
 * (klib_print.c). */
void PanicStandIfClaimedElsewhere(void);

/* True on the core that holds the claim — once a panic is claimed, the one core
 * still running, and running nothing but its account (klib_panic.c says why
 * it never leaves it). spin_lock asks it before it waits: no other core will
 * ever give a lock back to it (klib_lock.c). kprintf asks it to take the
 * glass back from a Canvas batch another core left open (klib_print.c). */
bool PanicOwnedHere(void);

#endif /* KLIB_PANIC_H */
