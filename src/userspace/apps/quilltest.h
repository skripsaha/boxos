/*
 * quilltest.h — what the three parts of quilltest share.
 *
 * quilltest.c holds the red tests of the Quill write core, the table of
 * cases and the one way in; quilltest_throwaway.c the cases only a throwaway
 * image may be asked (VOLUME_TESTS=on builds); quilltest_powercut.c the modes
 * the power-cut stand drives (tools/powercut.sh); quilltest_locks.c abba, the
 * guard on the two lock orders a growing chain crosses, a throwaway image's
 * case too; quilltest_turns.c beside, a write and its anchor beside cabins that
 * would keep the kernel's time to themselves (logcheck turns); quilltest_cores.c
 * and quilltest_ground.c the volume asked from many cabins at once — writers,
 * appenders beside a spanner, a read beside a delete, defrag beside delete
 * (logcheck cores); quilltest_away.c away, writers queued behind a write when
 * its medium leaves (logcheck replugwrite); quilltest_room.c roomback and
 * emptyfull, a full volume (logcheck fullvolume). All say their verdict in the
 * same four words and ask the volume for files by name.
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
QuillVerdict QuillKeeper(const char *const *args);
QuillVerdict QuillRace(const char *const *args);
QuillVerdict QuillCrowd(const char *const *args);

/* The machine's turns, quilltest_turns.c. */
QuillVerdict QuillFlood(const char *const *args);
QuillVerdict QuillBeside(const char *const *args);

/* A worker in a cabin of its own (quilltest_cores.c): started from its line,
 * and its end waited for as a state — process_gone's answer, its exit in
 * *code. */
#define QUILL_LINE_CAP 48u               /* "quilltest", a case's name and two numbers */
#define QUILL_NAME_CAP 16u               /* a stem of up to 9 bytes, two digits, ".bin" */
typedef struct {
    uint32_t pid;
    uint32_t gen;
} QuillCabin;
int  QuillCabinStart(QuillCabin *c, const char *line);
int  QuillCabinEnd(const QuillCabin *c, int32_t *code);
/* "quilltest WORD A" and, with_b, " B". */
void QuillCabinLine(char out[QUILL_LINE_CAP], const char *word, uint32_t a, uint32_t b,
                    bool with_b);
/* STEM, two digits of `index` and ".bin". */
void QuillIndexedName(char out[QUILL_NAME_CAP], const char *stem, uint32_t index);

/* The volume asked from many cabins at once, quilltest_cores.c and
 * quilltest_ground.c. */
QuillVerdict QuillWriters(const char *const *args);
QuillVerdict QuillWriter(const char *const *args);
QuillVerdict QuillWritersBack(const char *const *args);
QuillVerdict QuillAppend(const char *const *args);
QuillVerdict QuillAppender(const char *const *args);
QuillVerdict QuillSpanner(const char *const *args);
QuillVerdict QuillReadDelete(const char *const *args);
QuillVerdict QuillDeleter(const char *const *args);
QuillVerdict QuillDefragDelete(const char *const *args);
QuillVerdict QuillDefragger(const char *const *args);
QuillVerdict QuillDefragBack(const char *const *args);

/* A medium that leaves under a write and its waiters, quilltest_away.c. */
QuillVerdict QuillAway(const char *const *args);
QuillVerdict QuillAwayWriter(const char *const *args);
QuillVerdict QuillAwayCheck(const char *const *args);

/* A full volume, quilltest_room.c. */
QuillVerdict QuillRoomBack(const char *const *args);
QuillVerdict QuillEmptyFull(const char *const *args);

#endif /* QUILLTEST_H */
