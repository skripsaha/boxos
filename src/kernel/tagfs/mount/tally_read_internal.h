#ifndef TAGFS_MOUNT_TALLY_READ_INTERNAL_H
#define TAGFS_MOUNT_TALLY_READ_INTERNAL_H

#include "tally_read.h"
#include "book_unit.h"

/*
 * The Tally's reader, between its files (tally_read.h):
 *
 *   tally_read.c   a pointer's copies held as numbers, the nodes the walk
 *                  reached for the stand, and the DIGESTS tree — kept by leaf
 *                  at the mount, read by leaf when a block is; everything the
 *                  reader holds for a mount let go (TallyDigestsLetGo)
 *   tally_walk.c   a node read from the first of its copies that holds, and a
 *                  tree walked from its root
 */

/* A copy read as a node: its head, where each record begins (the NodeHead,
 * then each item), and how much longer than known its NodeHead is. */
typedef struct {
    BookUnit  unit;
    NodeHead  head;
    uint32_t* starts;
    uint32_t  nstarts;
    uint32_t  items_at;       /* where the first item begins */
    uint32_t  head_longer;
} TallyNode;

/* What the copies of a node that did not hold said, each of the first
 * TALLY_COPIES_SAID in its own words, for the lines that name them and for
 * the refusal of a root — a pointer may name any number of copies (§16.2
 * п.17), so the ones past them are said by their count alone: a line a copy
 * would be a line for every byte a damaged or foreign pointer spends, said at
 * every mount, past the owner's bound for a list said at mount (09-27). */
#define TALLY_COPIES_SAID  4u

typedef struct {
    uint32_t failed;
    char     why[TALLY_COPIES_SAID + 1u][112];   /* the last, where each copy past them is judged */
} TallyCopiesSaid;

/* A node in hand and what its copies said, in the heap: a walk reads them on
 * the road from a program's read to the medium, which has a frame to spare
 * for neither. */
typedef struct {
    TallyNode       node;
    TallyCopiesSaid said;
    char            words[TALLY_COPIES_SAID * 120u];   /* a line about the node, made here */
} TallyReading;

/* tally_walk.c: a node read let go; the node at place `p` from the first of
 * its copies that holds — OK with n->unit.whole when one held, the copies
 * before it in *said; OK with it false when none did; ERR_NO_MEMORY, or the
 * medium's refusal; and the copies that did not hold, in the words a refusal
 * names them by. */
void    TallyNodeLetGo(TallyNode* n);
error_t TallyNodeRead(TagFSVolume* vol, const TallyPlace* p, TallyNode* n, TallyCopiesSaid* said);
void    TallyCopiesWords(const TallyCopiesSaid* said, char* out, size_t room);

/* tally_read.c: a pointer as numbers, every copy in its order, the ones past
 * the two a place holds into the mount's list — ERR_NO_MEMORY when it has no
 * room for them; a node the walk of the Tally the mount stands on reached,
 * kept for the stand. */
error_t TallyPtrOf(TagFSVolume* vol, const BookPtr* p, TallyPtr* out);
#ifdef CONFIG_VOLUME_TESTS
void    TallyStandKeep(uint64_t tree, const TallyPlace* p);
#endif

/* A leaf of DIGESTS as a Tally names it — or a range of the tree no copy of
 * whose node reads. */
typedef struct {
    TallyPlace place;
    bool       rotten;
} TallyDigestLeaf;

/*
 * ‼ TWO TABLES OF LEAVES, AND A READ HOLDS THE ONE IT STARTED ON. The mount
 * lays the leaves of the Tally it stands on down with the door shut; a Tally
 * this mount writes turns every read after it to its own DIGESTS (the Gloss
 * then lets go of what that DIGESTS names, settle/binder_done.c). Reads run on
 * every core with no lock held — and a leaf may be read off the medium — so
 * the leaves are two tables: the reads take the one `tally_read.table_at` names,
 * counted in it while they are in it, and a Tally lays its leaves into the
 * other only once no read is left in it, then names it. A read that counted
 * itself into a table no longer named goes again on the one that is.
 */
typedef struct {
    TallyDigestLeaf*  leaves;
    uint32_t          count;
    uint32_t          cap;
    volatile uint8_t* said;          /* a leaf's rot and a copy of it found wanting, each said once */
    volatile uint32_t readers;
} TallyDigestTable;

typedef struct {
    uint32_t table;
    uint32_t leaf;        /* the index of the leaf it is; UINT32_MAX: empty */
    uint8_t* stream;      /* NULL: the leaf does not read */
    uint32_t room;
    uint64_t used;
} TallyDigestSheet;

/*
 * The leaves read lately, each as its stream — four, the most recent kept:
 * a read walks a file block by block, and a file's blocks lie in a row, which
 * one leaf of up to DIGEST_ITEM_MOST digests to an element covers. Under a
 * leaf lock of its own, which holds nothing but this table: the leaf is read
 * with no lock held, and put in after. A sheet is of the table its leaf is.
 */
#define TALLY_DIGEST_SHEETS  4u

/* What one volume holds of tally_read.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /* The copies of pointers past the two a place holds (tally_read.h), in the
     * order met: written only by the mount's reads of the Tally, with the door
     * shut, and read by any reader of a leaf afterwards. */
    uint64_t* copies;
    uint32_t copies_count;
    uint32_t copies_cap;
    TallyDigestTable tables[2];
    volatile uint32_t table_at;
    TagFSLock digest_lock;
    TallyDigestSheet sheets[TALLY_DIGEST_SHEETS];
    uint64_t sheet_tick;
} TallyReadState;

void TallyReadVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_MOUNT_TALLY_READ_INTERNAL_H */
