#ifndef TAGFS_STAND_BINDER_H
#define TAGFS_STAND_BINDER_H

#include "stand_stitch.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The kit a test that writes Tallies is run with (stand_binder_hand.c),
 * shared by every suite whose tests write a Tally or wait on one — a Tally
 * over the ring the stitch kit (stand_stitch.h) cannot put back: stand_binder,
 * whose kit it was, stand_endpaper (stand_endpaper_tests.c), the measure of
 * the next Tally the reserve keeps, and each suite that includes it — the
 * tests of stand_docket whose wait for room a Tally answers among them.
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
 *   StandBinderGleaned    the Tallies Glean asks for laid, none owed after
 *   StandBinderRot        a leaf of FILES rotten in both its copies, every slot
 *                         naming it in the one node, and the volume mounted
 *                         again on it writing nothing (the stand_binder and
 *                         stand_escheat suites)
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
bool        StandBinderGleaned(void);
TestResult  StandBinderRot(StandBinder* b, uint64_t* files);

/* stand_tip_in_cost_tests.c: what the next Tally is reckoned to cost, held to
 * the rule counted by the test (§16.6 п.6), and a Tally under way when the
 * volume is let go of — cases of the stand_tip_in suite. */
TestResult  TestStandTipInCostLives(void);
TestResult  TestStandTipInCostFiles(void);
TestResult  TestStandTipInTornDown(void);

/* stand_tally_range_tests.c: a FILES leaf neither copy of which reads, a
 * rotten range (C19) — its Tallies written first where no leaf is one every
 * slot names as it lies — a case of the stand_tally suite. And
 * stand_book_room_quire_tests.c: a next_quire given clear of the files the
 * newest Tally names (§3.7) — a case of the stand_book_room suite. */
TestResult  TestStandTallyRottenLeafLocked(void);
TestResult  TestStandBookRoomNextQuireClear(void);

#define STAND_BINDER_PUT_BACK(b)                                                                           \
    do {                                                                                                   \
        if (!StandBinderPutBack(b))                                                                        \
            verdict = TestFailedAt(__FILE__, __LINE__, "the Spine and the ribbon would not be put back "  \
                                   "as they were found (last, 0)", (int64_t)(b)->s.at.last, 0);            \
    } while (0)

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_BINDER_H */
