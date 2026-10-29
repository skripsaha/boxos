#ifndef TAGFS_STAND_BINDER_H
#define TAGFS_STAND_BINDER_H

#include "stand_stitch.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The kit a test that writes Tallies is run with (stand_binder_tests.c),
 * shared by the suites that write them: stand_binder, and stand_endpaper
 * (stand_endpaper_tests.c), the measure of the next Tally the reserve keeps.
 *
 *   StandBinderRead/Write a block of the volume, as the medium holds it
 *   StandBinderAside      why the test stands aside here, or NULL — the ring
 *                         and the ground its slots hold kept as found
 *   StandBinderKeep       a block the test changes by hand, kept as found
 *   StandBinderPutBack    the volume let go of, the ring, the kept block and
 *                         the slots' ground written back as found, the ribbon
 *                         put back and the volume mounted on it
 *                         (STAND_BINDER_PUT_BACK at `out`)
 *   StandBinderTallies    `n` Tallies in a row, the lag's stitches settled
 *                         past each: OK, or the first refusal
 */

#define STAND_BINDER_SLOTS_MOST  8u

/* The ring as the test found it, raw, and the ribbon (stand_stitch.h); and
 * the ground its slots hold, which the Tallies a test writes may give back
 * and hand on — kept as found too. */
typedef struct {
    StandStitch s;
    uint8_t*    ring;
    uint32_t    slots;
    uint64_t    spine;
    uint64_t    stride;
    uint64_t    kept_block;    /* one more block the test changed by hand, kept as found; 0 for none */
    uint8_t*    kept;
    Strip*      ground;        /* what the slots held as found — as many runs as they hold — and its bytes */
    uint8_t**   held;
    uint32_t    nground;
} StandBinder;

bool        StandBinderRead(const StandBinder* b, uint64_t block, uint8_t* out);
bool        StandBinderWrite(const StandBinder* b, uint64_t block, const uint8_t* in);
const char* StandBinderAside(StandBinder* b);
bool        StandBinderKeep(StandBinder* b, uint64_t block);
bool        StandBinderPutBack(StandBinder* b);
error_t     StandBinderTallies(uint32_t n);

#define STAND_BINDER_PUT_BACK(b)                                                                           \
    do {                                                                                                   \
        if (!StandBinderPutBack(b))                                                                        \
            verdict = TestFailedAt(__FILE__, __LINE__, "the Spine and the ribbon would not be put back "  \
                                   "as they were found (last, 0)", (int64_t)(b)->s.at.last, 0);            \
    } while (0)

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_BINDER_H */
