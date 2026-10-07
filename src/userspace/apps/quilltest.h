/*
 * quilltest.h — what the three parts of quilltest share.
 *
 * quilltest.c holds the red tests of the Quill write core, the table of
 * cases and the one way in; quilltest_throwaway.c the cases only a throwaway
 * image may be asked (VOLUME_TESTS=on builds); quilltest_powercut.c the modes
 * the power-cut stand drives (tools/powercut.sh); quilltest_locks.c abba, the
 * guard on the two lock orders a growing chain crosses, a throwaway image's
 * case too; quilltest_turns.c beside, a write and its anchor beside cabins that
 * would keep the kernel's time to themselves (logcheck turns). All say their
 * verdict in the same four words and ask the volume for files by name.
 */
#ifndef QUILLTEST_H
#define QUILLTEST_H

#include "box/types.h"

typedef enum { QUILL_PASS, QUILL_DEFECT, QUILL_PENDING, QUILL_FAIL } QuillVerdict;

#define QUILL_CHUNK          4096u       /* one write of the cases that write a block at a time */

/* `len` bytes (a multiple of 8) of the stream seeded `seed`, from file offset
 * `base`: every 8 bytes a function of where they sit. */
void     QuillFill(uint64_t *dst, uint32_t seed, uint64_t base, uint64_t len);
/* [0, size) of `fid` read back against that stream: the offset of the first
 * byte that is not it (`size` when all are), the read that stopped it in *err. */
uint64_t QuillReadBack(uint32_t fid, uint32_t seed, uint64_t size, int64_t *err);
/* The name up to its last dot: the tag TagFS stamps on every file. */
void     QuillStem(const char *name, char *out, size_t cap);
/* Every file called `name`, whatever Use Context is set: how many there are
 * (the first `max` in ids), or a negative -error_t. */
int      QuillFindEverywhere(const char *name, uint32_t *ids, int max);
/* A fresh, empty `name` on the whole volume — every earlier one deleted —
 * and its file id, or <= 0. */
int      QuillCreate(const char *name);
/* The size file_info reports, 0 when it will not answer. */
uint64_t QuillSize(uint32_t fid);
/* A count or a number as the shell gave it — digits only, at most ten, no
 * more than a uint32_t holds — in *out; false for anything else. */
bool     QuillNumber(const char *word, uint32_t *out);

#ifdef CONFIG_VOLUME_TESTS
/* A throwaway image's cases, quilltest_throwaway.c. */
QuillVerdict QuillSequential(const char *const *args);
QuillVerdict QuillInterleave(const char *const *args);
QuillVerdict QuillFidHistory(const char *const *args);
QuillVerdict QuillSystemTag(const char *const *args);
/* abba and its workers, quilltest_locks.c — a throwaway image's too. */
QuillVerdict QuillAbba(const char *const *args);
QuillVerdict QuillAbbaWorker(const char *const *args);
#endif

/* The power-cut modes, quilltest_powercut.c. */
__attribute__((noreturn)) QuillVerdict QuillHog(const char *const *args);
QuillVerdict QuillSettle(const char *const *args);
QuillVerdict QuillHogged(const char *const *args);
QuillVerdict QuillVerify(const char *const *args);
QuillVerdict QuillChurn(const char *const *args);
QuillVerdict QuillRace(const char *const *args);
QuillVerdict QuillCrowd(const char *const *args);

/* The machine's turns, quilltest_turns.c. */
QuillVerdict QuillFlood(const char *const *args);
QuillVerdict QuillBeside(const char *const *args);

#endif /* QUILLTEST_H */
