#ifndef TAGFS_STAND_FRESH_H
#define TAGFS_STAND_FRESH_H

#include "stand_tests.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The kit the data of a file on a book is tested with (stand_fresh_tests.c),
 * shared by stand_fresh and stand_fresh_ground (stand_fresh_ground_tests.c):
 *
 *   StandFreshModel        the bytes a file must read, as the test writes them
 *   StandFreshWrite        `bytes` of the pattern of `stream` written into the
 *                          file at `at` and into the model: what the write said
 *   StandFreshSpan         the file spanned to `size`, the model with it
 *   StandFreshHolds        whether the file is the model's size and reads its
 *                          bytes, every one
 *   StandFreshPrint        the file's print — its size, and its runs as ground,
 *                          the Strips that go on from one another one run — and
 *                          in *runs (NULL: not asked) how many runs of ground
 *   StandFreshSettledAndBack the cycle settled, the volume mounted again on what
 *                          its medium holds: true when the file has the print it
 *                          had and reads the model's bytes
 *   StandFreshLastStitch   the last stitch this mount sewed: its flags and its
 *                          Clasp's fresh blocks
 */

typedef struct {
    uint8_t* bytes;
    uint64_t room;
    uint64_t size;
} StandFreshModel;

/* How long a job of the port is waited for. */
#define STAND_FRESH_PORT_MS  10000u

bool     StandFreshModelMake(StandFreshModel* m, uint64_t room);
void     StandFreshModelLetGo(StandFreshModel* m);
int      StandFreshWrite(StandFreshModel* m, uint64_t fid, uint64_t at, uint32_t bytes, uint64_t stream);
error_t  StandFreshSpan(StandFreshModel* m, uint64_t fid, uint64_t size);
bool     StandFreshHolds(const StandFreshModel* m, uint64_t fid);
uint32_t StandFreshPrint(uint64_t fid, uint64_t* runs);
bool     StandFreshSettledAndBack(const StandFreshModel* m, uint64_t fid);
bool     StandFreshLastStitch(uint64_t* flags, uint64_t* fresh);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_FRESH_H */
