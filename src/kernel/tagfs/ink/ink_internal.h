#ifndef TAGFS_INK_INTERNAL_H
#define TAGFS_INK_INTERNAL_H

#include "ink.h"
#include "fallow.h"
#include "docket.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_map/file_map.h"
#include "../file_table/file_table.h"
#include "../tag_registry/tag_registry.h"
#include "../settle/settle.h"
#include "../helm/helm.h"
#include "../glean/glean.h"
#include "../../../kernel/drivers/timer/rtc.h"

/*
 * The halves of an Ink, among the files that make them (ink.h):
 *
 *   ink.c        what every format shares — the guards, the counters, the
 *                first half of each change (what it asks, what it builds
 *                before the lock) — and the turn each change takes to the
 *                second half of its volume's format
 *   ink_v1.c     format v1's second halves: the record packed and laid in the
 *                pool's tail, its room planned against the reserve, a Spare
 *                made when the tail has none (meta_pool.h)
 *   ink_book.c   format v2's: the change in the mirror and a Jot in the
 *                Docket for the next stitch (docket.h) — nothing laid anywhere
 *                until a stitch carries it
 *
 * Under the volume's state lock, the counters move by these alone.
 */

/* A retry of the first half: the file changed between the halves, or a stake
 * no longer covers. Never an answer. */
#define INK_AGAIN ((error_t)0xFFFFFFFFu)

static inline TagFSState* InkState(void)
{
    return tagfs_get_state();
}

/* Under the state lock: the gen the next Ink takes; an Ink laid at `gen`; a
 * flush need its data holds. */
uint64_t InkGenNextLocked(void);
void     InkGenLaidLocked(uint64_t gen);
void     InkDataNeedRaiseLocked(uint64_t need);

/* Whether the volume takes a change at all, asked in both halves (whether it
 * takes one not begun yet, asked in the first, is ink.h's InkVolumeBegins).
 * And the guard's answer for a file's change, `boot` whether it touches the
 * kernel's blocks (ink.h). */
error_t  InkVolumeTakes(void);
error_t  InkGuardSays(uint64_t fid, bool boot, uint32_t flags, InkWarrant warrant);

/* The record of format v1, built before the lock: memory's copy and the
 * medium's packed bytes (ink_v1.c). */
typedef struct {
    FileRecord* rec;        /* memory's copy; NULL for a Quitclaim */
    uint8_t*    packed;     /* the medium's */
    uint32_t    bytes;
} InkRecord;

void     InkRecordLetGo(InkRecord* r);
error_t  InkRecordMake(const TagFSMetadata* meta, InkRecord* r);

/* What a change of a file's name and tags does to its tags (ink.c). */
typedef struct {
    uint32_t* tags;
    uint32_t  count;
    uint32_t  dropped[2];   /* the tags that come off the index */
    uint32_t  drops;
    bool      adds;         /* `add` goes onto the index */
} InkFieldsPlan;

/* The room a new file's insert needs, taken before the lock. */
typedef struct {
    FileMapStake    map;
    TagBitmapStake* index;
} InkNewStakes;

/* An answer that is not final, gone round: INK_AGAIN always; ERR_WOULD_BLOCK on
 * format v1, once a Spare is made (ink_v1.c). */
bool     InkGoesRound(error_t* rc, InkSpares spares);

/* Format v1's second halves (ink_v1.c), under the state lock. */
error_t  InkPlotRecord(const Plot* p, const FileRecord* cur, uint64_t now, InkRecord* r);
error_t  InkPlotLocked(Plot* p, const RecordWhere* was, InkRecord* r, FallowNode* fallow, InkReceipt* out);
error_t  InkFieldsLocked(uint64_t fid, const RecordWhere* was, const InkFieldsPlan* f, const TagFSMetadata* meta,
                         uint32_t add, InkRecord* r, TagBitmapStake* stake, InkReceipt* out);
error_t  InkNewLocked(const uint32_t* tags, uint32_t count, InkRecord* r, InkNewStakes* st, uint64_t* out_fid,
                      InkReceipt* out);
error_t  InkQuitclaimLocked(uint64_t fid, const RecordWhere* was, const FileRecord* cur, InkRecord* r,
                            FallowNode* fallow, InkReceipt* out);

/*
 * Format v2's (ink_book.c), each from the first half's work to the change in
 * the mirror and the Jot for the next stitch — the record it becomes built,
 * the stakes taken, the lock taken and dropped there, the cycle told: a file
 * named and tagged as `f` says (`cur` the file as it was read, sitting at
 * `was`); a new file (its id the lock's to give, past every one the book
 * names); a file given up (`fallow` its ground, taken either way). OK and the
 * receipt, or the refusal with nothing changed — INK_AGAIN when the file
 * changed between the halves.
 */
error_t  InkBookFields(uint64_t fid, FileRecord* cur, const RecordWhere* was, const InkFieldsPlan* f,
                       const char* name, uint32_t add, uint32_t flags, InkReceipt* out);
error_t  InkBookNew(const char* name, const uint32_t* tags, uint32_t count, uint64_t* out_fid, InkReceipt* out);
error_t  InkBookQuitclaim(uint64_t fid, const RecordWhere* was, const FileRecord* cur, FallowNode* fallow,
                          InkReceipt* out);

/* A Plot's change of a book's file (TagFSInkPlot): its window spliced into
 * `cur` — the record it was loaded from, sitting at `was` — the Jot made, the
 * digests of what it wrote put in the Gloss, the ribbon's room the Plot held
 * taken for its claim, and the ground the record stops naming laid to rest,
 * `small_us` the small append's time or 0. Then InkBookPlotNext: the Plot made
 * to go on from the record it inked — held afresh, its window empty — as a
 * write's next window does; a file given up since leaves it no record, and
 * its next window is refused ERR_FILE_NOT_FOUND (plot_internal.h). */
error_t  InkBookPlot(const Plot* p, const FileRecord* cur, const RecordWhere* was, uint64_t small_us,
                     InkReceipt* out);
void     InkBookPlotNext(Plot* p);

#endif /* TAGFS_INK_INTERNAL_H */
