#ifndef RANDOM_H
#define RANDOM_H

#include "ktypes.h"

/*
 * Random — the kernel's one source of numbers nobody can foresee: where a
 * program's memory is laid (aslr.c), and the GUIDs a partition table names a
 * disk and its partitions by (disk_table.c).
 *
 * The processor's own generator (RDRAND) where it has one and it answers.
 * Where it has none, or stops answering — or answers every time with the one
 * value some processors give once they have lost it, all ones — a generator
 * of the kernel's own: SHA-256 over a seed the boot's timing made, a count,
 * and the TSC of the moment it is asked in. Which of the two answers is said
 * at RandomInit, and again the moment the processor's is given up.
 *
 * Any core, any lock held or none: no lock of its own, no allocation.
 */

/* Once at boot, before the first program is laid out. */
void     RandomInit(void);

uint64_t Random64(void);
void     RandomBytes(void* out, size_t bytes);

/* A number in [0, bound), each as likely as the next; 0 for a bound of 0. */
uint64_t RandomBelow(uint64_t bound);

#endif /* RANDOM_H */
