#ifndef TAGFS_MOUNT_BOOK_MOUNT_H
#define TAGFS_MOUNT_BOOK_MOUNT_H

#include "../tagfs.h"
#include "spine_read.h"
#include "tally_read.h"
#include "ribbon.h"

/*
 * MountReadBook — a format-v2 volume's mount: its book read the way the spec
 * reads it (spec_v2 §4.5 пп.1–4, 8; §16.1 п.2–5), the Spine's slots and the
 * Tally of the newest one whose roots read, into the memory every format
 * shares — the map of live files, the registry, the tag index, the Use
 * Context — with every owner of every block claimed (§4.6) and the numbers
 * the volume gives next past everything it showed.
 *
 * It reads the Tally and the ribbon of stitches sewn past it (§4.5 п.5,
 * ribbon.h): the state it mounts is the Tally's with every stitch after it
 * replayed, the verdicts the ribbon asks for — a roll-back, a Slip to fold —
 * said and left to the writer that can lay them down (§16.1 п.2): the volume
 * stands under glass (tagfs.h, UnderGlass) for whatever the book carries that
 * no writer of this kernel could carry on. Otherwise this kernel's writer goes
 * on where the ribbon ends (settle/sewing.h): stitches of files made, named,
 * tagged and given up, of tags and of the Use Context; the data of a file it
 * does not yet lay into a book, and it writes no Tally.
 *
 * ‼ NOT A BYTE IS WRITTEN BY THE MOUNT, before the door or after it: no growth
 * (a book grows only by a mount that writes it, §16.1 п.4), no Ledger, no
 * Spare, no flush. Two mounts of one volume are one state (C12).
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
    uint32_t sewn;          /* stitches the ribbon sews on past the Tally */
    uint32_t applied;       /* of them, replayed */
    uint64_t last;          /* the seq the chain ends on: the last stitch sewn, or the Tally's */
    uint64_t flags;         /* the StitchHead flags of the last stitch sewn on; 0 when none is */
    bool     lied;          /* the medium is shown not to have kept what it confirmed */
    uint64_t fallow;        /* blocks the ribbon let go, lying fallow */
    uint32_t ribbon[RIBBON_CODES];  /* the ribbon's lines, by code */
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
    uint64_t       gen;       /* the seq of the stitch that last changed it (§2.8) */
} BookFile;

/* A tag the book names that the registry cannot hold (BookTagHeld): its id
 * and its text, kept so a record that wears it finds it known — volume-read
 * keeps every tag TAGS or a Coined names (bookmount.py). */
typedef struct {
    uint64_t id;
    TallyTag tag;        /* key and value point into `text` */
    uint8_t* text;
} BookTagAside;

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
    uint64_t       next_fid;     /* past every fid the book names; then the next one Ink gives (ink_book.c) */
    uint64_t       next_tag;     /* past every tag id TAGS and the ribbon name and a live file wears */
    uint64_t       next_bookmark;/* past every bookmark the ribbon names */
    BookFile       file;
    RibbonWalk*    ribbon;       /* the ribbon past the Tally stood on, until the census has its Quires */
    Strip*         fallow;       /* DATA ground the ribbon let go (M-1): neither a file's nor free — once the
                                  * census ran, what of it no other owner names, for the mount's Fallow */
    uint64_t       nfallow;
    uint64_t       capfallow;
    uint64_t*      born;         /* fids in a rotten FILES range the ribbon gave birth to, rising */
    uint32_t       nborn;
    uint32_t       capborn;
    BookTagAside*  aside;        /* tags the registry cannot hold, rising by id */
    uint32_t       naside;
    uint32_t       capaside;
    uint32_t       files_print;  /* the print of the live files, the mount done (BookFilesLaid) */
    uint64_t       boot_fid;     /* the file whose blocks the loader reads the kernel by; 0 for none (ink.c) */
    uint32_t       use_print;    /* the print of the Use Context memory holds, and its bytes */
    uint64_t       use_bytes;
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

/* Every live file's ground claimed for the census and the tags it wears
 * counted into the numbers given next — after the ribbon is replayed, the
 * map of live files is what the volume holds (book_ground.c). */
void    BookFilesLaid(void);

/* What of tag `tid` this kernel cannot carry on, said (§16.1 п.2, §6.4): true
 * when the registry can hold it (tally_leaves.c). */
bool    BookTagHeld(uint64_t tid, const TallyTag* t);

/* A tag the registry cannot hold, set aside — its text copied, one it replaces
 * let go — and the tag set aside as `tid`, or NULL (tally_leaves.c). OK or
 * ERR_NO_MEMORY. */
error_t         BookTagAsideNoted(uint64_t tid, const TallyTag* t);
const TallyTag* BookTagAsideOf(uint64_t tid);

/* The Use Context recalled into memory (UseMemoryRecall), its print kept for
 * the line the mount gives of what memory holds (book_mount.c). */
error_t BookUseRecalled(const uint8_t* text, uint64_t len);

/* Whether `fid` lies in a rotten range of FILES — its header lost with the
 * range — and no stitch gave birth to it anew (§4.5 п.3 (б)); and a fid a
 * stitch gave birth to, kept when such a range holds it — no other is asked
 * after (OK or ERR_NO_MEMORY). */
bool    BookRotLocks(uint64_t fid);
error_t BookBornNoted(uint64_t fid);

/* A file of the map locked: said with `why`, out of the map and its tags, its
 * id never given again, its DATA ground held (tally_leaves.c). OK or
 * ERR_NO_MEMORY. */
error_t BookFileLockLive(uint64_t fid, const char* why);

#endif /* TAGFS_MOUNT_BOOK_MOUNT_H */
