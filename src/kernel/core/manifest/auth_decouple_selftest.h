#ifndef AUTH_DECOUPLE_SELFTEST_H
#define AUTH_DECOUPLE_SELFTEST_H

#include "error.h"

/*
 * Boot self-test proving process op-authority is decoupled from the TagFS
 * registry id. In a scratch registry it interns the "god" key past id 63 — the
 * range where the old (1ULL<<id) auth mask collapsed to 0 — and shows the gate
 * predicate (auth_level_permits) still grants god-override and still denies a
 * stopped process. No mkfs / global state: it drives the real predicate over a
 * private registry. Prints "[AUTHDEC] PASS" on success or
 * "[AUTHDEC] FAIL: <reason>" on the first failing assertion.
 * Returns OK / ERR_INTERNAL.
 */
error_t AuthDecoupleSelfTest(void);

#endif /* AUTH_DECOUPLE_SELFTEST_H */
