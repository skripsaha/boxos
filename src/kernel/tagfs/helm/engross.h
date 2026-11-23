#ifndef TAGFS_ENGROSS_H
#define TAGFS_ENGROSS_H

#include "../tagfs.h"
#include "../ink/ink.h"

/*
 * Engross — a file's tail, out of the Slips over its last block, laid in that
 * block: by the holder of the file's Helm, through the
 * Ink, as a defrag is (HelmEngross) — never by the cycle, which would be a
 * second door a file changes by, racing the writer that holds the Helm.
 *
 * A Tally takes no slice while the settled state carries a Slip (LooseLeaves,
 * binder.c): the Binder asks this service, which walks the files a turn at a
 * time on a service point of the Helm and lays the tail of each one whose
 * Helm is free; the cycle that carries those Inks settles them, and the Tally
 * goes after it. A file whose Helm a writer holds is left to the writer: no
 * Slip is given while a Tally is owed (plot_slip.c), so its next write lays
 * the block, its tail folded in. And a file of a book is kept in a role with
 * its tail in its block: the loader reads such a file by its blocks (§9.1), and
 * the image builder lays one so — the stand's way of putting a role on a file
 * the volume holds is EngrossRoled.
 */

/* The service asked: listed on the Helm's service points once, the walk of
 * the files it begins answering every ask made before it — by the Settle's
 * holder at a cycle's DONE (BinderWeigh), and by the let-go of a Helm a turn
 * found held (HelmTryNoted). With no TagFS lock held.
 *
 * A file whose tail would not be laid — its last block unreadable, a file the
 * mount locked, no ground — is the file's own: the walk goes on past it, says
 * what it left once while that stays the same, and tries a file refused for
 * itself again only once its record changes. The Tally waits for each such
 * tail (BinderMayNot). */
void    EngrossAsked(TagFSVolume* vol);

/* The tails the last walk that tried every tail left refused for their files'
 * own sake — none for a refusal that may pass — that Escheat can cut. From
 * any core, one word that may be a walk old (escheat.h). The file the loader
 * reads the kernel by is never among them: a book where it carries a Slip is
 * only read (mount/book_ground.c, BookBootSlipped). */
uint32_t EngrossRefusedNow(TagFSVolume* vol);

/* Every file's tail laid in its block now, in the caller's own call — the
 * Tally a caller asks for and waits for (BinderRunWhole). OK; or, said, the
 * first file's refusal, or ERR_BUSY when every file left is another's Helm or
 * waits for the ribbon's room; ERR_DEVICE_NOT_READY when the medium left or
 * the volume takes no write. Inside the volume's door. */
error_t EngrossWhole(TagFSVolume* vol);

#ifdef CONFIG_VOLUME_TESTS
/* The stand's InkRoleLaid for file `fid` of a book, made under the file's Helm,
 * taken now, with its tail laid in its block first: two Inks the Helm holds
 * together, so no write lays a Slip between them and the file never wears its
 * role with one. OK, ERR_BUSY (said) while a writer holds the Helm, or the
 * refusal of either Ink — ERR_WOULD_BLOCK among them while the ribbon's room is
 * on its way, which the caller waits out. Inside the volume's door. */
error_t EngrossRoled(TagFSVolume* vol, uint64_t fid, uint32_t role, InkReceipt* out);
#endif

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
/* The Ink of an Engross laid at `gen` — by that Ink, under the state lock, so
 * a cycle whose snapshot takes `gen` sees it — and whether a stitch whose
 * snapshot reaches `target` names a block an Engross laid: the SETTLECUT point
 * before such a stitch (settle_cut.h, `engross`, C22). Lock-free. */
void    EngrossInked(uint64_t gen);
bool    EngrossCarriedBy(uint64_t target);
#endif

/* A walk of the files from `from` on, and what it left: the files whose tail
 * stays in Slips — their Helm another's, the ribbon's room on its way, or a
 * refusal — and the first of those refused. */
typedef struct {
    uint64_t from;        /* the next file id; UINT64_MAX once the map has no more */
    uint32_t left;
    uint32_t refused;     /* of `left`, refused by anything but a held Helm or the ribbon's room: said */
    uint32_t own;         /* of `refused`, refused for the file's own sake (EngrossActs): Escheat's to cut */
    uint64_t first_fid;
    error_t  first;
    uint64_t act;         /* the asks of Escheat this walk cuts tails for — 0 for none (EscheatTailsActing) */
    uint64_t reach;       /* the newest Ink whose record of a file the act reaches (EscheatTailsReach) */
} EngrossWalkState;

/* A refusal that is the file's own — its last block unreadable, a file the
 * mount locked — made to the record that Ink `gen` made, in mounting `epoch`:
 * tried again only once the record changes, so a rotten tail is neither read
 * nor said at every turn — or by the walk that answers an ask of Escheat. */
typedef struct {
    uint64_t fid;
    uint64_t gen;
    uint64_t epoch;
    error_t  rc;
} EngrossRefusal;

#define ENGROSS_KNOWN  16u                     /* refusals of a file's own the service keeps */

/* What one volume holds of engross.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    HelmWaiter service;
    volatile uint32_t listed;
    volatile uint64_t asked;  /* asks made: each EngrossAsked counts one */
    /* The tails the last walk that tried every tail left refused for their own
     * sake, read from any core (EngrossRefusedNow): */
    volatile uint32_t owned;
    /* The service point's alone — one listing runs at a time (engross.listed): */
    uint64_t pass;  /* the asks the walk under way answers */
    EngrossWalkState walk;
    EngrossRefusal known[ENGROSS_KNOWN];
    uint32_t known_next;
    EngrossWalkState said;  /* the last walk's leavings said */
} EngrossState;

void EngrossVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_ENGROSS_H */
