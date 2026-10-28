#ifndef TAGFS_MOUNT_BOOK_MOUNT_H
#define TAGFS_MOUNT_BOOK_MOUNT_H

#include "../tagfs.h"
#include "spine_read.h"
#include "tally_read.h"
#include "ribbon.h"
#include "../settle/endpaper_fact.h"

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
 * on where the ribbon ends (settle/sewing.h): stitches of files made, written,
 * named, tagged and given up, of tags and of the Use Context; and a whole
 * Tally of it in time (settle/binder.h), named by a slot of the Spine.
 *
 * ‼ NOT A BYTE IS WRITTEN BY THE MOUNT, before the door or after it — no
 * Ledger, no Spare, no flush — but the one act a book's mount may make once
 * every verdict is in: taking the ground behind the volume, by a mount that
 * writes it (book_grow.c, §16.1 п.4). Two mounts of one volume that grows by
 * neither are one state (C12).
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
 * (volume_take_ground), of ground the partition table runs `ground_sectors`
 * long: its FRAME taken, block numbers counted from the volume's block 0 up
 * to its far Deed, and the verdicts the ground gives before a sector of the
 * book is read — a FRAME longer than this kernel's, a ground off the medium's
 * physical blocks (§1.2, §1.5): under glass. OK. */
error_t MountBookGround(uint8_t seat, const DeedCopy* head, uint64_t ground_sectors);

/*
 * The volume takes the ground behind it (spec_v2 §16.1 п.4, §1.3): after its
 * book is read and its block map derived, before the door — and only by a
 * mount that writes it, every verdict in. A volume under glass takes none, nor
 * one stood on its far copy alone, nor one whose medium refuses writes as
 * protected; nor while the census cannot say the ground past its end is no
 * one's — a rotten FILES range may name it, an owner does. Then its far Deed
 * and its head are written for the longer volume, the format-v1 way
 * (ground_grow.h), the block map widened over the new ground and the data run
 * ended at the new far Deed. Said either way; nothing written unless all of
 * it can be (book_grow.c).
 */
void    MountBookGrow(uint8_t seat);

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
    uint64_t checked;       /* blocks of fresh data the newest stitches' check read and held to their digests */
    uint64_t check_us;      /* and the microseconds that check took */
    uint32_t ribbon[RIBBON_CODES];  /* the ribbon's lines, by code */
} MountBookSaid;

#ifdef CONFIG_VOLUME_TESTS
void     MountBookStandSaid(MountBookSaid* out);
/* The print of the live files memory holds now, taken as the mount takes it
 * (BookFilesLaid) and claiming no ground: g_book's print is the last mount's,
 * and a stitch since does not move it (book_ground.c). */
uint32_t BookFilesStandPrint(void);
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
    uint64_t*      parcel_b;  /* each Parcel that read: the file block it begins at, ... */
    uint8_t**      parcel_body;/* ... its body as it lay, and the seq of the node it lay in — kept */
    uint32_t*      parcel_len;/* for the next Tally while the file may still be locked */
    uint64_t*      parcel_seq;
    uint32_t       nparcels;
    uint32_t       capparcels;
    bool           parcels_lost;/* memory would not keep them: no Tally carries it, were it locked */
} BookFile;

/*
 * A file the Tally names and the mount locked by name — what of it read, kept
 * for the next Tally to name again as it lay (settle/binder_items.c): a file
 * locked stays locked, its ground held, across a Tally this kernel writes
 * (§16.4 п.5). Its header when it read, and each Parcel that read: the file
 * block it begins at, its body as it lay and the seq of its node — its runs
 * read again from it by that seq, so each is named with the birth it had.
 */
typedef struct {
    uint64_t      fid;
    bool          head;
    TagFSMetadata meta;      /* the header: its name and tags this list's own */
    uint64_t      gen;
    uint64_t*     parcel_b;
    uint8_t**     parcel_body;
    uint32_t*     parcel_len;
    uint64_t*     parcel_seq;
    uint32_t      nparcels;
} BookKeptFile;

/* A tag the book names that the registry cannot hold (BookTagHeld): its id
 * and its text, kept so a record that wears it finds it known — volume-read
 * keeps every tag TAGS or a Coined names (bookmount.py). */
typedef struct {
    uint64_t id;
    TallyTag tag;        /* key and value point into `text` */
    uint8_t* text;
} BookTagAside;

/* A stitch the mount's verdict undoes (errata.h): its seq, why, and the files
 * it rolls back — rising, each once, when the replay is done. */
typedef struct {
    uint64_t  seq;
    uint64_t  why;
    uint64_t* fids;
    uint32_t  nfids;
    uint32_t  capfids;
} BookErratum;

typedef struct {
    VolumeFrame    frame;
    uint64_t       data_end;     /* the far Deed's block: the data run is [frame.data_block, data_end) */
    uint64_t       ground;       /* how far the partition table runs the volume's ground, sectors */
    bool           on_head;      /* stood on the Deed at its head — not on its far copy alone */
    uint64_t       reach;        /* past the farthest block an owner but the far Deed names (census) */
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
    BookKeptFile*  kept;         /* the files locked by name, by fid: what of them read */
    uint32_t       nkept;
    uint32_t       capkept;
    uint64_t       tally_blocks; /* the Tally stood on, one copy: what the next is reckoned to cost (binder.h) */
    uint64_t       ribbon_useful;/* the useful bytes of the stitches replayed past it (§4.2) */
    bool           binder_short; /* memory would not hold what the slots hold for the writer of a Tally */
    EndpaperLaid   laid;         /* what the Tally stood on laid of FILES, TAGS and DIGESTS (endpaper_fact.h) */
    uint64_t       laid_tree;    /* the tree of it being walked */
    BookErratum*   errata;       /* the mount's verdict, in order of seq and why (errata.h) */
    uint32_t       nerrata;
    uint32_t       caperrata;
    uint64_t*      wear;         /* the files it rolls back, rising, each once: settle:undone owed them */
    uint32_t       nwear;
    uint32_t       worn;         /* of them, the ones the Ink of the tag is done for */
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
 * pointer names, claimed for the census as that slot names it, and noted in
 * the Foliation (book_ground.c) — a TallySink's `node`; and a node of it read
 * past a copy that did not hold, counted and written again by the next Tally —
 * a leaf's keys into DogEar, an inner node marked in the Foliation — a
 * TallySink's `mend`. */
error_t BookGroundNode(void* ctx, const TallyPlace* at);
void    BookGroundMended(void* ctx, uint64_t tree, const TallyPlace* at, uint32_t failed);

/* A run of ground a locked file is known to name: what of it lies in the data
 * run held unless another owns it, all of it counted in the census's reach. */
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

/* A file of the map locked by the replay of the ribbon: said with `why`, out
 * of the map and its tags, its id never given again, its DATA ground held —
 * and the volume under glass: a Tally names no file it cannot name as it lay,
 * and a book no Tally is written of cannot go on (UnderGlass; volume-read's
 * bookmount.py says the same). OK or ERR_NO_MEMORY. */
error_t BookFileLockLive(uint64_t fid, const char* why);

#endif /* TAGFS_MOUNT_BOOK_MOUNT_H */
