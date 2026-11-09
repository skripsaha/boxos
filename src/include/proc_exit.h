#ifndef BOXOS_PROC_EXIT_H
#define BOXOS_PROC_EXIT_H

/*
 * process:died exit-code disposition — the SINGLE source of truth, shared by
 * the kernel (touch.c / system_ops.c / process.c) and userspace observers
 * (apps/touch_test.c). The value rides in TouchProcessDied.exit_code
 * (box/touch.h), an int32_t each death-site computes and passes to
 * TouchCleanupProcess:
 *
 *   >= 0   clean exit — the value the process passed to exit(), masked to
 *          [0, INT32_MAX] (sign bit cleared) so a clean code can never collide
 *          with the negative dispositions below.
 *   -1     PROC_EXIT_KILLED  — terminated by another process via PROC_KILL.
 *   -2     PROC_EXIT_CRASHED — abnormal teardown (fault / kernel-forced) via
 *          process_destroy.
 *   -3     PROC_EXIT_STACK_OVERFLOW — a crash whose cause is known: the
 *          strand's stack used its whole reserve and touched the gap under
 *          it. An observer that does not tell it from a crash reads it as
 *          one (any negative but -1 is a crash).
 *
 * Plain integer literals, no type include — valid in both C and C++, kernel
 * and userspace. Defined ONCE here so the two sides can never drift.
 */
#define PROC_EXIT_KILLED   (-1)
#define PROC_EXIT_CRASHED  (-2)
#define PROC_EXIT_STACK_OVERFLOW (-3)

/* How a program that crashed fell, said with each of its deaths
 * (TouchProcessDied, after the three fields above): the vector of the first
 * exception any of its strands took, and the function the faulting
 * instruction lies in, named from the program's own Nameplate — at most
 * PROC_FELL_IN_MAX - 1 bytes and a NUL, "" when it carries no names. */
#define PROC_FELL_IN_MAX 64

#endif /* BOXOS_PROC_EXIT_H */
