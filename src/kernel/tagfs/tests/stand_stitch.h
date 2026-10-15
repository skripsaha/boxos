#ifndef TAGFS_STAND_STITCH_H
#define TAGFS_STAND_STITCH_H

#include "stand_tests.h"
#include "../settle/sewing.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The kit a book's writer is tested with (stand_stitch_tests.c), shared by the
 * suites that sew on a format-v2 volume's ribbon: stand_stitch, and
 * stand_stitch_edge (stand_stitch_edge_tests.c) — the edges of a Quire, a
 * refusal there, and the measure of the Tally's reserve.
 *
 *   StandStitchAside    why a test written for a book's writer stands aside
 *                       here, or NULL — and where the ribbon stood, kept in *s
 *   StandStitchRemount  the volume let go of, writing nothing, and mounted
 *                       again on what its medium holds: true when it came up
 *   StandStitchPutBack  the ribbon put back as the test found it, the volume
 *                       mounted again on it (STAND_STITCH_PUT_BACK at `out`)
 *   StandStitchSay      the Use Context told to the volume as the machine's
 *                       `k`-th word past any it said, settled by the cycle the
 *                       word demands: its answer, *kept whether the volume holds it
 *   StandStitchSayLong  the same, a word of `bytes` bytes of one letter
 *   StandStitchAnswered a waiter of the stand's own answered, as an anchor's is
 */

/* Where the ribbon stood when the test began, on what seat and base, and
 * whether the machine held a Use Context of its own then. */
typedef struct {
    RibbonCursor at;
    uint8_t      seat;
    uint64_t     base;
    uint8_t      uuid[16];
    bool         context;
} StandStitch;

/* A waiter of the stand's own, as an anchor's is. */
typedef struct {
    SettleWaiter w;
    bool         answered;
    error_t      rc;
} StandStitchWaiter;

void        StandStitchAnswered(SettleWaiter* w, error_t rc);
const char* StandStitchAside(StandStitch* s);
bool        StandStitchRemount(void);
bool        StandStitchPutBack(const StandStitch* s);
error_t     StandStitchSay(const char* text, size_t bytes, uint64_t k, bool* kept);
error_t     StandStitchSayLong(uint64_t bytes, char letter, uint64_t k, bool* kept);

#define STAND_STITCH_PUT_BACK(s)                                                                       \
    do {                                                                                               \
        if (!StandStitchPutBack(s))                                                                    \
            verdict = TestFailedAt(__FILE__, __LINE__, "the ribbon would not be put back as it was "  \
                                   "found (last, 0)", (int64_t)(s)->at.last, 0);                       \
    } while (0)

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_STITCH_H */
