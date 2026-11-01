#ifndef TAGFS_STAND_STITCH_H
#define TAGFS_STAND_STITCH_H

#include "stand_tests.h"
#include "../settle/sewing.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The kit a book's writer is tested with (stand_stitch_tests.c), shared by the
 * suites that sew on a format-v2 volume's ribbon: stand_stitch,
 * stand_stitch_edge (stand_stitch_edge_tests.c) — the edges of a Quire, a
 * refusal there, and the measure of the Tally's reserve — and stand_docket
 * (stand_docket_tests.c), the files a book's Ink changes.
 *
 *   StandStitchAside    why a test written for a book's writer stands aside
 *                       here, or NULL — and where the ribbon stood, kept in *s,
 *                       the machine's Use Context set aside for the test
 *   StandStitchContextBack the machine given back the Use Context set aside, if
 *                       one still is: false when it would not be set again — by
 *                       the put-back, and by the runner after every test
 *   StandStitchRemount  the volume let go of, writing nothing, and mounted
 *                       again on what its medium holds: true when it came up
 *   StandStitchPutBack  the ribbon put back as the test found it, the machine's
 *                       Use Context given back, the volume mounted again on it
 *                       (STAND_STITCH_PUT_BACK at `out`)
 *   StandStitchSay      the Use Context told to the volume as the machine's
 *                       `k`-th word past any it said, settled by the cycle the
 *                       word demands: its answer, *kept whether the volume holds it
 *   StandStitchSayLong  the same, a word of `bytes` bytes of one letter
 *   StandStitchSayFilling the same, a word whose stitch takes `sectors` sectors
 *   StandStitchLayEmpty a stitch of no records laid by hand at the ribbon's end,
 *                       its `flags`, naming the last: its Clasp (0 refused)
 *   StandStitchLayAt    the same, `of` sectors long — whole blocks, at most
 *                       STAND_STITCH_LAID_MOST — naming `next` its next_quire
 *   StandStitchAnswered a waiter of the stand's own answered, as an anchor's is
 *   StandStitchRead     the last stitch this mount sewed, read back whole with
 *                       the mount's own reader (*out the caller's to let go)
 */

/* Where the ribbon stood when the test began, and on what seat and base. */
typedef struct {
    RibbonCursor at;
    uint8_t      seat;
    uint64_t     base;
    uint8_t      uuid[16];
} StandStitch;

/* A waiter of the stand's own, as an anchor's is. */
typedef struct {
    SettleWaiter w;
    bool         answered;
    error_t      rc;
} StandStitchWaiter;

void        StandStitchAnswered(SettleWaiter* w, error_t rc);
const char* StandStitchAside(StandStitch* s);
bool        StandStitchContextBack(void);
bool        StandStitchRemount(void);
bool        StandStitchPutBack(const StandStitch* s);
error_t     StandStitchSay(const char* text, size_t bytes, uint64_t k, bool* kept);
error_t     StandStitchSayLong(uint64_t bytes, char letter, uint64_t k, bool* kept);
error_t     StandStitchSayFilling(uint64_t sectors, char letter, uint64_t k, bool* kept);
uint32_t    StandStitchLayEmpty(const StandStitch* s, uint64_t flags);
uint32_t    StandStitchLayAt(const StandStitch* s, uint64_t flags, uint32_t of, const RibbonQuire* next);

/* The longest stitch the stand lays by hand: a flash page of 16 KiB, the
 * atom of a removable medium's ribbon (§16.6 п.2), in sectors. A test that
 * lays one an atom of the ribbon long stands down, saying so, where the atom
 * is longer — the medium's own block, past a page (BookRibbonAtomOf). */
#define STAND_STITCH_LAID_MOST 32u
#define STAND_STITCH_ATOM_LONG "the ribbon's atom is longer than a stitch the stand lays by hand"

/* The bytes a stitch's head, its UseContext's tag and length and its Clasp
 * take, and more. */
#define STAND_STITCH_SLACK  200u
bool        StandStitchRead(RibbonUnit** out);

#define STAND_STITCH_PUT_BACK(s)                                                                       \
    do {                                                                                               \
        if (!StandStitchPutBack(s))                                                                    \
            verdict = TestFailedAt(__FILE__, __LINE__, "the ribbon would not be put back as it was "  \
                                   "found (last, 0)", (int64_t)(s)->at.last, 0);                       \
    } while (0)

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_STITCH_H */
