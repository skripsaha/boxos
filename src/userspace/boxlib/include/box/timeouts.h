#ifndef BOX_TIMEOUTS_H
#define BOX_TIMEOUTS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * box/timeouts.h — single source of truth for boxlib synchronous-syscall
 * timeouts (milliseconds).
 *
 * Goal: every MfCall1 / ManifestSubmit timeout should be a named constant
 * with a justification, not a magic 100000-vs-50000 literal scattered across
 * boxlib. Pre-audit history had `send/broadcast/listen` passing 500000 ms
 * (almost 9 minutes), `vga_*` passing 100000 ms (100 s) and `time_*` passing
 * 50000 ms (50 s) — operations that nominally complete in microseconds.
 * When a kernel-side bug or a peer crash made those calls hang, the userspace
 * waiter would burn 100s of seconds before noticing; the same hang now
 * surfaces inside 5 s.
 *
 * Rule of thumb:
 *   - Sub-µs in-kernel work (VGA, time, RTC, sysinfo, defrag, storage ops):
 *     TIMEOUT_FAST (5 s) is plenty; if the kernel can't answer in 5 s
 *     something is genuinely broken and we want to fail loudly.
 *   - Hardware operations with retry semantics (storage write, AHCI flush):
 *     TIMEOUT_STORAGE (5 s).
 *   - IPC blocking calls (receive_wait, broadcast acks) — bounded but a peer
 *     thread may need a scheduling slice: TIMEOUT_IPC (5 s) is enough.
 *   - Blocking input where the user genuinely may not press a key for a
 *     while (kb_readline, touch_await): TIMEOUT_INPUT (30 s) per iteration,
 *     then the caller re-arms.
 *   - Diagnostic kdbg writes pass TIMEOUT_KDBG (60 s) — the long ceiling is
 *     intentional, see debug.c rationale.
 *
 * Adjusting any value here changes EVERY caller, by design.
 */

/* The absence of a deadline, said out loud.
 *
 * A Manifest submitted to the kernel is ANSWERED. The reply carries a
 * cloakroom token, and the kernel keeps ring room in reserve for exactly that
 * (kring.c: a token-carrying Result may use the whole reply ring, unsolicited
 * traffic may not take the last pocket-ring's worth of it), so an answer can
 * never be refused for want of space.
 *
 * A deadline on such an answer can only do harm. On a loaded machine it turns
 * a completed operation into a refusal the caller reports upward as failure —
 * and it abandons a Manifest and Crates that live on the CALLER'S STACK while
 * the K-Core is still going to read them and write results back into them,
 * after that frame is gone. Waiting is the correct behaviour; a number here is
 * a guess that can only ever be wrong in one of two directions.
 *
 * A deadline belongs where SILENCE IS POSSIBLE — a device that may never
 * answer (STORAGE_TIMEOUT_MS), or an event that may never happen. There it is
 * a watchdog with something real to watch, not a guess about how fast a
 * machine ought to be. */
#define BOX_ANSWER_GUARANTEED     0u

/* What the call sites use TODAY, and why it is not the zero above.
 *
 * Removing the deadline was measured, on this tree, with the full matrix:
 *
 *   HEAD                                        BIOS 16c PASS
 *   this tree, deadline = 0                     BIOS 16c WEDGE (twice)
 *   this tree, deadline restored                BIOS 16c PASS
 *
 * and the wedge was x-rayed on the frozen machine: the waiting strand sat at
 * `result_wait+0x182` — an answer that never arrived — while 14 of 16 cores
 * were idle and no ring had refused anything. So an answer IS lost, rarely,
 * under 16-core pressure, by some path not yet found. A deadline does not fix
 * that. What it does is turn the hang into a 30-second stall the machine walks
 * out of, which is why nobody had seen the defect: the guess was hiding it.
 *
 * Both things are true at once, so both are said here rather than one of them
 * being quietly chosen: the number below is NOT a guess about how fast a
 * machine ought to be — it is a watchdog over a NAMED, EVIDENCED defect, and
 * it goes away in one edit the day that defect is closed. Until then, removing
 * it trades a rare stall for a permanent wedge, which is a worse machine.
 *
 * See project memory `rings-claim-and-deadlines` for the receipts. */
#define BOX_ANSWER_WATCHDOG_MS    30000u

#define BOX_TIMEOUT_FAST_MS       5000u
#define BOX_TIMEOUT_STORAGE_MS    5000u
#define BOX_TIMEOUT_IPC_MS        5000u
#define BOX_TIMEOUT_INPUT_MS      30000u
#define BOX_TIMEOUT_KDBG_MS       60000u

#ifdef __cplusplus
}
#endif

#endif /* BOX_TIMEOUTS_H */
