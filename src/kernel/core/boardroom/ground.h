#ifndef GROUND_H
#define GROUND_H

#include "ktypes.h"

/*
 * Ground — the run of a medium a volume may stand on.
 *
 * A Deed (volume_deed.h) says which volume it is, how far it runs, and where
 * its parts are. What it deliberately does not say is where it BEGINS on the
 * medium, because that is not a volume's business: it is the medium that is
 * divided, and the division is written in the medium's own partition table.
 *
 * This is what reads that table. It answers one question — "where on this
 * medium is ground claimed for BoxOS?" — and answers it the same way whether
 * the table is an MBR or a GPT, because the answer is the same kind of fact.
 *
 * Before it, nothing in the kernel read a partition table at all. The volume
 * was assumed to begin at sector 1034 of the whole device, so BoxOS could
 * exist in exactly one place on any medium and could not be given a partition
 * on a disk that belonged to somebody else without writing over them.
 */

/* Where the ground came from. A machine that will not mount says which table
 * it read, which is the difference between "no BoxOS partition here" and "this
 * disk has no partition table I could read". */
typedef enum {
    GROUND_FROM_MBR = 0,
    GROUND_FROM_GPT,
} GroundOrigin;

typedef struct {
    uint64_t     start_sector;  /* first 512-byte sector of the ground */
    uint64_t     sectors;       /* how far it runs */
    GroundOrigin origin;
    uint8_t      entry;         /* which entry of the table it came from */
} MediumGround;

/* A disk with more BoxOS partitions than this on it is a disk somebody is
 * doing something unusual with; the survey reports what it found and says the
 * rest were not looked at, rather than silently stopping. */
#define GROUND_MAX_PER_MEDIUM 8

/*
 * Read this seat's partition table and report every run of it claimed for
 * BoxOS. Returns how many were written to `out`.
 *
 * Zero is a complete answer, not a failure: a medium with no table, a table
 * that does not check out, or a table with nothing of ours in it are all
 * "there is no BoxOS ground here", and each says so on its own line.
 */
uint8_t GroundSurvey(uint8_t seat, MediumGround *out, uint8_t max);

const char *GroundOriginName(GroundOrigin origin);

#endif /* GROUND_H */
