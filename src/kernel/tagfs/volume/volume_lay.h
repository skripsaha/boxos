#ifndef TAGFS_VOLUME_LAY_H
#define TAGFS_VOLUME_LAY_H

#include "../tagfs.h"
#include "../helm/helm.h"
#include "../box_hash/box_hash.h"
#include "volume_tally.h"
#include "book_bind.h"
#include "ground.h"

/*
 * VolumeLay — a new volume laid on a BoxOS partition of a disk, by the kernel
 * as its maker, with the system on it: the volume a person makes by hand
 * (`volumes new`) and the one install lays a system on.
 *
 * ‼ AS EVERY MAKER LAYS ONE — WHOLE, OR NOT AT ALL. The volume is born with
 * every file the home volume keeps in a role — the kernel, the hub, the
 * programs the system starts — each in that role, as the host's volume maker
 * (tools/create_tagfs_book.c) lays them: placed in a row from the data run's
 * first block, the kernel first and so in one run; the digest of every block
 * taken and the book laid out in memory, and the ground refused before a byte
 * of it is written when they do not fit; then the Deed's blocks at both ends
 * blanked and flushed, the front blanked, every block written and held to its
 * digest, the first Tally by the one binder (book_bind.h) and the Spine's slot
 * 0, a flush, and only then the Deed — GEOMETRY · FRAME · BORN · BOOT naming
 * the kernel's run · PACKED — at both ends, and a flush. Cut anywhere before
 * the last, the partition holds no Deed, never one that names a book or a
 * system half written; a laying cut short is laid again.
 *
 * ‼ THE FILES AS THEY ARE. Each keeps its name, its bytes, its tags, the
 * programs it shows them to, who made it and when, and its passport. A word
 * of a program is the program's on the new volume too — its file there, or
 * its passport when a file there carries it — and a word of a program the new
 * volume will not hold is left behind, as a carry leaves it (carry.h).
 *
 * ‼ A TURN AT A TIME. The pieces are read off the home volume and written to
 * the disk at its service points (HelmReady), a piece a turn, so a laying of
 * megabytes holds no core for long. The house then takes the volume in as one
 * met on a medium arriving (TagFSHouseLook), and the answer comes once it is
 * mounted.
 *
 * ‼ NOTHING A RUNNING SYSTEM STANDS ON. The partition is a BoxOS one of the
 * disk the person looked at — its seat and seating — and no volume of the
 * house stands on it.
 *
 * One laying at a time.
 */

typedef enum {
    VOLUME_LAY_DIGEST = 1,  /* every file read a piece a turn, each block's digest taken */
    VOLUME_LAY_BLANK,       /* the Deed's blocks blanked, then the front a run a turn */
    VOLUME_LAY_DATA,        /* every file read again and written a piece a turn, held to its digests */
    VOLUME_LAY_BOOK,        /* the book bound, then the Deed at both ends */
    VOLUME_LAY_TAKEN,       /* the house is to take it in */
} VolumeLayStep;

/* A file the system keeps, as the new volume is to hold it. */
typedef struct {
    TagFSFileHandle* src;                         /* its read handle on the home volume */
    uint64_t         home;                        /* its id there */
    char*            name;
    uint32_t         name_len;
    uint64_t         flags;                       /* TALLY_FILE_*: ACTIVE, its role, and what it carries */
    uint64_t         size, created_ns, modified_ns, passport, made_by;
    uint32_t*        tags;                        /* its tags' ids on the new volume, rising */
    uint32_t         ntags;
    uint64_t*        shown;                       /* the programs it shows them to there, rising */
    uint32_t         nshown;
    uint8_t          head[TALLY_HEAD_MOST];
    uint32_t         head_len;
    uint64_t         start, blocks;               /* its run on the new volume */
} VolumeLaidFile;

/* A tag the new volume's book names past the reserved words, at
 * TAGFS_RESERVED_COUNT + its place among them. */
typedef struct {
    uint64_t owner_id;                            /* the program's id there; 0 for the person's */
    char*    text;                                /* key, then value */
    uint32_t key_len, value_len;
    bool     has_value;
} VolumeLaidTag;

typedef struct VolumeLaying VolumeLaying;
struct VolumeLaying {
    /* Set by the one who asks it. */
    uint8_t   seat;
    uint32_t  seating;
    uint16_t  entry;                              /* the BoxOS partition's place in its table */
    uint8_t   name[TALLY_VOLUME_NAME_MOST];       /* TallyNameSpelled; name_len 0 for none */
    uint32_t  name_len;
    void    (*ended)(VolumeLaying* l);            /* once, with no TagFS lock held: rc, number, roles */

    /* The answer. */
    error_t   rc;
    uint32_t  number;                             /* the volume's number in the house */
    uint32_t  roles;                              /* the files the system keeps laid on it */

    /* The laying's own. */
    VolumeLayStep   step;
    uint8_t         uuid[16];
    uint64_t        made_at;
    MediumGround    ground;                       /* the partition's, in the room's sectors */
    uint64_t        blocks;                       /* the ground in the volume's blocks */
    VolumeFrame     frame;
    VolumeLaidFile* files;                        /* the kernel first */
    uint32_t        count;
    VolumeLaidTag*  tags;
    uint32_t        tag_count, tag_room;
    uint64_t        end;                          /* the first block past the files */
    uint64_t*       digests;                      /* a block's, from frame.data_block to `end` */
    BoxHashContext  hash;                         /* the digests' seed: the uuid */
    BindLeaves      leaves;
    BindPlan        plan;
    VolumePacked    packed;                       /* the shipment folded as it is read, the kernel's plate */
    uint32_t        plates;                       /* how many the kernel showed */
    uint32_t        at;                           /* the file a pass is on */
    uint64_t        offset;                       /* how far into it */
    uint64_t        front;                        /* the first block of the front not yet blank */
    uint8_t*        piece;                        /* a piece, behind the kernel's last bytes still to be looked at */
    uint32_t        kept;                         /* those bytes */
    HelmWaiter      turn;                         /* listed on the home volume for the next turn */
};

/*
 * The laying begun — the partition held to what it may stand on, the files
 * the system keeps opened on the home volume and placed, and the book laid
 * out to see it fits — before a byte is written: OK, and `ended` runs once it
 * is done or refused on the way; or the refusal, `ended` never run:
 *   ERR_WOULD_BLOCK       another laying is under way;
 *   ERR_OBJECT_NOT_FOUND  the seat is empty, or holds another disk since;
 *   ERR_OUT_OF_RANGE      no partition in that entry;
 *   ERR_INVALID_ARGUMENT  the partition is not BoxOS's, or the name is no name;
 *   ERR_BUSY              a volume of the house stands on it;
 *   ERR_TAGFS_NO_SPACE    the partition is too small for the system and its book;
 *   ERR_FILE_NOT_FOUND    the home volume keeps no kernel;
 *   ERR_DEVICE_NOT_READY  the medium refused the reads or left;
 *   ERR_NO_MEMORY.
 * On the way: ERR_PERMISSION_DENIED for a medium that refuses writes,
 * ERR_WRITE_FAILED, ERR_DEVICE_NOT_READY for a disk or a home volume that
 * left, a refusal of reading a file, ERR_CHECKSUM for one whose bytes were not
 * the bytes their digests were taken of. The laying is the caller's memory
 * until `ended`.
 */
error_t VolumeLayBegin(VolumeLaying* l);

/* The service, at every service point: a laying the house is to take in. */
void    VolumeLayServe(void);

#endif /* TAGFS_VOLUME_LAY_H */
