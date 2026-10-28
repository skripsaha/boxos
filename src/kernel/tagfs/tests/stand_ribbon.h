#ifndef TAGFS_STAND_RIBBON_H
#define TAGFS_STAND_RIBBON_H

#include "stand_tests.h"
#include "volume_ribbon.h"
#include "../file_map/file_map.h"
#include "../settle/sewing.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The hand the stand sews a book's ribbon with (stand_ribbon_tests.c): where
 * the ribbon goes on past its last stitch and two free blocks of the data run,
 * stitches laid there with the ribbon's own codec past TagFS, by the
 * Boardroom's doors, the volume mounted on them — and everything laid put back
 * and the volume mounted again as it was. Shared by the suites that sew
 * (stand_ribbon, stand_errata).
 */

#define STAND_RIBBON_MOUNT_MS  10000u
#define STAND_RIBBON_LAID      8u
#define STAND_RIBBON_RECORDS   8u

/* Where the ribbon goes on past the Tally the mount stood on, and what the
 * test laid there to put back. */
typedef struct {
    uint8_t  seat;
    uint64_t base;
    uint8_t  uuid[16];
    uint64_t quire;          /* the ribbon's Quire, its first block and blocks */
    uint64_t quire_blocks;
    uint64_t next;           /* the next_quire the slot names */
    uint64_t next_blocks;
    uint64_t at;             /* the block the ribbon goes on at */
    uint64_t seq;            /* the last stitch the Tally took in, and its Clasp */
    uint32_t clasp;
    uint64_t top;            /* the largest seq the mount saw: a stitch laid takes one past it */
    uint64_t fid;            /* the next file id the volume gives */
    uint64_t free;           /* two blocks in a row no owner names */
    uint64_t laid[STAND_RIBBON_LAID];
    uint32_t nlaid;
    bool     data;           /* the free blocks were written: their bytes are kept below */
} StandRibbon;

/* A stitch's records as the test lays them, and where each begins. */
typedef struct {
    uint8_t  bytes[2048];
    uint32_t len;
    uint32_t starts[STAND_RIBBON_RECORDS];
    uint32_t n;
    uint64_t fresh;
} StandRibbonRecords;

bool                StandRibbonLay(StandRibbon* w, uint64_t vlba, uint32_t count, const uint8_t* sectors);
StandRibbonRecords* StandRibbonFresh(uint32_t k);
void                StandRibbonPut(StandRibbonRecords* r, uint64_t kind, const uint8_t* body, uint32_t len);
void                StandRibbonBorn(StandRibbonRecords* r, uint64_t fid, uint64_t size, uint64_t start, uint32_t count,
                                    const uint64_t* digests, uint32_t slip_off, uint32_t slip_len, uint8_t slip_byte);
void                StandRibbonEmpty(StandRibbonRecords* r, uint64_t fid);
uint32_t            StandRibbonSew(StandRibbon* w, uint64_t block, uint64_t seq, const StitchHead* h,
                                   const StandRibbonRecords* r, uint32_t write);
StitchHead          StandRibbonHead(const StandRibbon* w, uint64_t seq, uint64_t prev_seq, uint32_t prev_clasp);
bool                StandRibbonData(StandRibbon* w, uint64_t digests[2]);
bool                StandRibbonPutBack(StandRibbon* w);
const char*         StandRibbonAside(StandRibbon* w);
bool                StandRibbonLive(uint64_t fid, FileRecord** out);

#endif /* CONFIG_VOLUME_TESTS */

#endif /* TAGFS_STAND_RIBBON_H */
