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
 *
 * Plain integer literals, no type include — valid in both C and C++, kernel
 * and userspace. Defined ONCE here so the two sides can never drift.
 */
#define PROC_EXIT_KILLED   (-1)
#define PROC_EXIT_CRASHED  (-2)

#endif /* BOXOS_PROC_EXIT_H */
