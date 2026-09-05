#ifndef BOX_TIMEOUTS_H
#define BOX_TIMEOUTS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * box/timeouts.h — what a synchronous submit needs to know about deadlines:
 * it has none.
 *
 * A Manifest submitted to the kernel is ANSWERED. The reply carries a
 * cloakroom token, and the kernel keeps ring room in reserve for exactly that
 * (kring.c: a token-carrying Result may use the whole reply ring, unsolicited
 * traffic may not take the last pocket-ring's worth of it), so an answer can
 * never be refused for want of space. An operation that leaves the kernel's
 * own hands — a read off a medium, a write to it — is bounded by the driver's
 * own patience: AHCI, legacy ATA and USB storage each name a command that will
 * not finish, reset the transport, and answer with an error. So the deck
 * answers those too, and a strand parked on one is woken by that answer.
 *
 * A deadline on such an answer can only do harm. On a loaded machine it turns
 * a completed operation into a refusal the caller reports upward as failure —
 * and it abandons a Manifest and Crates that live on the CALLER'S STACK while
 * the K-Core is still going to read them and write results back into them,
 * after that frame is gone. Waiting is the correct behaviour; a number here is
 * a guess that can only ever be wrong in one of two directions.
 *
 * This file used to hold a family of such guesses — FAST 5 s, STORAGE 5 s,
 * IPC 5 s, INPUT 30 s, KDBG 60 s, and a 30 s WATCHDOG kept over a lost answer
 * whose path was afterwards found (the reply ring read tail before head and
 * declared itself full) and closed. Each of them, when it expired, was
 * believed: a write to a slow stick that took six seconds came back as "write
 * failed" while the medium went on taking the bytes.
 *
 * A deadline belongs where SILENCE IS POSSIBLE, and only there — an event that
 * may never happen. Those are named by the caller who chose to wait a bounded
 * time (brook_pop_timeout, kb_getchar_timeout, a timed addr_park), never by
 * this file on behalf of an answer the kernel owes. An answer that never comes
 * is a kernel defect, and Nightwatch names it on facts (ANSWER OWED); a clock
 * in front of it is how it stayed unnamed.
 */
#define BOX_ANSWER_GUARANTEED     0u

#ifdef __cplusplus
}
#endif

#endif /* BOX_TIMEOUTS_H */
