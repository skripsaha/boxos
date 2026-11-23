#ifndef TAGFS_VOLUME_LAY_H
#define TAGFS_VOLUME_LAY_H

#include "../tagfs.h"
#include "../settle/settle.h"
#include "carry.h"
#include "volume_tally.h"
#include "ground.h"

/*
 * VolumeLay — a new volume laid on a BoxOS partition of a disk, by the kernel
 * as its maker: the volume a person makes by hand (`volumes new`) and the one
 * install lays a system on.
 *
 * ‼ AS EVERY MAKER LAYS ONE. The Deed by the one pen (deed_pen.h), its
 * GEOMETRY and FRAME by the format's own rules (VolumeGeometryLaid,
 * VolumeFrameLaid), its book by the one binder (book_bind.h) — the reserved
 * words, the name a person gave it, no file — written as the host's maker
 * writes one: the Deed's blocks at both ends blanked and flushed, the front
 * blanked, the Tally and the Spine's slot 0, a flush, and only then the Deed
 * at both ends and a flush. Cut anywhere before the last, the partition holds
 * no Deed at all, never one that names a book half written.
 *
 * ‼ THEN THE HOUSE TAKES IT IN, AND IT IS GIVEN THE SYSTEM. The house looks at
 * every seat again and takes the new volume in as one met on a medium
 * arriving (TagFSHouseLook); once it is mounted, a carry of its maker gives it
 * every file the home volume keeps in a role — the kernel first, gathered into
 * one run — each in the role it had (Carry.maker), and the Deed's BOOT is
 * amended to name the kernel's run, far copy then head (DeedsAmendedWritten),
 * so a loader finds it. The answer comes once a cycle of the new volume has
 * settled all of it.
 *
 * ‼ NOTHING A RUNNING SYSTEM STANDS ON. The partition is a BoxOS one of the
 * disk the person looked at — its seat and seating — and no volume of the
 * house stands on it.
 *
 * One laying at a time.
 */

typedef enum {
    VOLUME_LAY_TAKEN = 1,   /* the house is to take it in */
    VOLUME_LAY_ROLES,       /* the maker's carry gives it the files the system keeps */
    VOLUME_LAY_BOOT,        /* the Deed is to name the kernel's run */
    VOLUME_LAY_SETTLING,    /* a cycle of it settles all of it */
    VOLUME_LAY_SETTLED,     /* the cycle answered: the laying's answer */
} VolumeLayStep;

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
    VolumeLayStep     step;
    uint8_t           uuid[16];
    MediumGround      ground;                     /* the partition's, in the room's sectors */
    uint64_t          made_at;
    TagFSVolume*      vol;
    Carry             carry;
    CarryItem*        items;
    SettleWaiter      wait;
    volatile error_t  came;
};

/*
 * The volume laid — its book and Deed on the medium — and the rest begun:
 * OK, and `ended` runs once it is all done or refused on the way; or the
 * refusal before anything was taken in, `ended` never run:
 *   ERR_WOULD_BLOCK       another laying is under way;
 *   ERR_OBJECT_NOT_FOUND  the seat is empty, or holds another disk since;
 *   ERR_OUT_OF_RANGE      no partition in that entry;
 *   ERR_INVALID_ARGUMENT  the partition is not BoxOS's, or the name is no name;
 *   ERR_BUSY              a volume of the house stands on it;
 *   ERR_TAGFS_NO_SPACE    the partition is too small for a book;
 *   ERR_PERMISSION_DENIED the medium refuses writes;
 *   ERR_DEVICE_NOT_READY  the medium refused the reads or left;
 *   ERR_WRITE_FAILED, ERR_NO_MEMORY.
 * The laying is the caller's memory until `ended`.
 */
error_t VolumeLayBegin(VolumeLaying* l);

/* The service, at every service point: a laying's next step. */
void    VolumeLayServe(void);

#endif /* TAGFS_VOLUME_LAY_H */
