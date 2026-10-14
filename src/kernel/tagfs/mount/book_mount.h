#ifndef TAGFS_MOUNT_BOOK_MOUNT_H
#define TAGFS_MOUNT_BOOK_MOUNT_H

#include "../tagfs.h"
#include "spine_read.h"
#include "tally_read.h"

/*
 * MountReadBook — a format-v2 volume's mount: its book read the way the spec
 * reads it (spec_v2 §4.5 пп.1–4, 8; §16.1 п.2–5), the Spine's slots and the
 * Tally of the newest one whose roots read, into the memory every format
 * shares — the map of live files, the registry, the tag index, the Use
 * Context — with every owner of every block claimed (§4.6) and the numbers
 * the volume gives next past everything it showed.
 *
 * ‼ THIS KERNEL READS A BOOK AND WRITES NONE: every format-v2 volume it
 * mounts stands under glass (tagfs.h, UnderGlass) — "this kernel reads
 * format-v2 books and writes none of them" — beside whatever the book itself
 * carries that no writer of this kernel could carry on. And it reads the
 * Tally, not yet the ribbon of stitches sewn past it: a book with anything
 * sewn past its Tally is turned away, in so many words, rather than mounted
 * as the Tally was (§4.5 п.5 is a newer kernel's).
 *
 * ‼ NOT A BYTE IS WRITTEN, before the door or after it: no growth (a book
 * grows only by a mount that writes it, §16.1 п.4), no Ledger, no Spare, no
 * flush. Two mounts of one volume are one state (C12).
 *
 * The two readers of a book — this one and volume-read's (tools/volumeread/
 * bookmount.py, bookcensus.py KernelBook) — name the same things in the same
 * numbers: the census scenario boots a planted book and holds these lines to
 * what the host predicts.
 *
 * Everything here runs inside tagfs_init with the door shut, on the one core
 * attending the mount; the maps it fills take their own leaf locks.
 */

/* The ground of a format-v2 volume standing on the Deed `head`
 * (volume_take_ground): its FRAME taken, block numbers counted from the
 * volume's block 0 up to its far Deed, and the verdicts the ground gives
 * before a sector of the book is read — a FRAME longer than this kernel's, a
 * ground off the medium's physical blocks (§1.2, §1.5): under glass. OK. */
error_t MountBookGround(uint8_t seat, const DeedCopy* head);

/* The book read into memory (above). OK, or the refusal with *what saying it
 * in the words the mount's refusal line carries (tagfs.c, mount_refused):
 * none of the Spine's slots holds, none names a Tally that reads, something
 * is sewn past the Tally, or there is no memory. */
error_t MountReadBook(const char** what);

/* Everything the book's mount keeps, let go — the volume let go of. */
void    MountBookLetGo(void);

/*
 * Whether file `file_id` is one the mounted book holds and cannot read — its
 * header or its Parcels lost with a rotten range of the Tally, or written
 * against the book's own rules (spec_v2 §4.5 п.3, §2.9). Such a file opens
 * for nobody (ERR_TAGFS_CORRUPTED, as the storage deck answers it), its id is
 * never given again and the blocks it may name are held. False on a volume
 * of format v1, which locks none, and when no volume is up.
 *
 * What it reads is the mount's, let go of only with the volume's door shut
 * and nobody inside (tagfs_clear_the_ground): so it is asked inside the door.
 * TagFSFileLocked takes the door for its caller; BookFileLocked is for one
 * already inside it — tagfs_open, the mount.
 */
bool    TagFSFileLocked(uint64_t file_id);
bool    BookFileLocked(uint64_t file_id);

/* What the mount of the book said, in numbers. */
typedef struct {
    uint32_t slot;          /* the Spine slot it stands on */
    uint32_t mended;        /* copies of nodes that did not hold, the next read in their place */
    uint32_t rotten;        /* ranges of keys no copy of whose node holds */
    uint64_t locked;        /* files locked by name */
    uint64_t held;          /* blocks held for what rotted */
    uint32_t shapes;        /* things written against the format's own shape */
} MountBookSaid;

#ifdef CONFIG_VOLUME_TESTS
void    MountBookStandSaid(MountBookSaid* out);
#endif

// ----------------------------------------------------------------------------
// The mount's own parts: book_mount.c (the slots, the walk, the numbers),
// tally_leaves.c (the leaves into memory), book_ground.c (the census)
// ----------------------------------------------------------------------------

/* A range of FILES keys no copy of whose node holds — [lo, hi), to the
 * end of the tree without hi — and the files its parent counts under it. */
typedef struct {
    TallyKey lo;
    TallyKey hi;
    bool     has_hi;
    uint64_t count;
} BookRotten;

/* A file's items as the walk hands them over, gathered until the next file's. */
typedef struct {
    uint64_t       fid;
    bool           open;      /* items of `fid` are being gathered */
    bool           head;      /* its header read */
    bool           rotten;    /* a rotten range of FILES touches it */
    bool           ext;       /* a run of it carries an extension */
    bool           wide;      /* its header's flags reach past the 32 bits memory holds */
    char           why[160];  /* why its items do not read; "" when they do */
    TagFSMetadata  meta;      /* the header: `filename` and `tag_ids` are the gathering's */
    Strip*         strips;
    uint32_t       nstrips;
    uint32_t       cap;
    uint64_t       covered;   /* the blocks its Parcels covered, in a row from block 0 */
} BookFile;

typedef struct {
    VolumeFrame    frame;
    uint64_t       data_end;     /* the far Deed's block: the data run is [frame.data_block, data_end) */
    uint32_t       atom;         /* the medium's atom of a write, bytes (§1.5) */
    SpineSlot*     slots;        /* the slots that hold, newest first */
    uint32_t       nslots;
    SpineSlot*     stood;        /* the one the mount stands on */
    MountBookSaid  said;
    BookRotten*    rot;          /* FILES ranges that rot, in key order */
    uint32_t       nrot;
    uint32_t       caprot;
    uint64_t*      locked;       /* fids locked by name, rising */
    uint32_t       nlocked;
    uint32_t       caplocked;
    bool           files_rot;    /* a FILES range rots: every digested block no name owns is held */
    bool           bookmarked;   /* a Tally of a slot that holds names a bookmark: no map, free not counted */
    uint64_t       files;        /* live files */
    uint64_t       next_fid;     /* past every fid the Tally names */
    uint64_t       next_tag;     /* past every tag id TAGS names and a live file wears */
    BookFile       file;
} BookMount;

extern BookMount g_book;

/* The leaves of `tree` of the Tally stood on into memory (tally_leaves.c): OK,
 * or ERR_NO_MEMORY. */
error_t TallyLeaves(uint64_t tree, const SpineRoot* root);

/* The last file the FILES walk gathered, laid into memory or locked. */
error_t BookFileClose(void);

/* A rotten FILES range, said and kept (tally_leaves.c). OK or ERR_NO_MEMORY. */
error_t BookRangeRots(const TallyPlace* at, const char* why);

/* The census of the book (book_ground.c): every owner's claim the walk did not
 * make as it went, what rotted held, two owners of one block said, and the
 * volume's block map derived from the claims (block_map_derived.h) — none
 * where a Tally names a bookmark, whose ground it does not see. OK, or
 * the refusal — memory's, the map's, or the medium's for a Tally it must see
 * — with what the mount's line says in *what. */
error_t BookGroundCensus(const char** what);

/* Every node of the Tally the mount stands on a walk reaches, every copy its
 * pointer names, claimed for the census as that slot names it
 * (book_ground.c) — a TallySink's `node`. */
error_t BookGroundNode(void* ctx, const TallyPlace* at);

/* A run of ground a locked file is known to name: held unless another owns it. */
void    BookGroundHoldRun(uint64_t start, uint64_t count);

#endif /* TAGFS_MOUNT_BOOK_MOUNT_H */
