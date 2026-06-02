#ifndef BOX_TIMEOUTS_H
#define BOX_TIMEOUTS_H

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

#define BOX_TIMEOUT_FAST_MS       5000u
#define BOX_TIMEOUT_STORAGE_MS    5000u
#define BOX_TIMEOUT_IPC_MS        5000u
#define BOX_TIMEOUT_INPUT_MS      30000u
#define BOX_TIMEOUT_KDBG_MS       60000u

#endif /* BOX_TIMEOUTS_H */
