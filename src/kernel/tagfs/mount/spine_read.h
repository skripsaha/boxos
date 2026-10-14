#ifndef TAGFS_MOUNT_SPINE_READ_H
#define TAGFS_MOUNT_SPINE_READ_H

#include "../tagfs.h"
#include "volume_tally.h"

/*
 * SpineRead — the Spine's slots of a format-v2 volume, read and judged
 * (spec_v2 §1.4): each one block of eight sectors at spine_block + i *
 * spine_stride. A slot is VALID when all eight hold as one SLOT unit and its
 * stream parses — SpineHead first, a root per tree at most once, SpineRibbon
 * and SpineCounts once each; BLANK when every byte of it is zero, a slot no
 * Tally has been written to; TORN otherwise, with why.
 *
 * A record of a kind this reader does not know is stepped over when its tag
 * says it may be, and otherwise, like a body longer than this reader knows,
 * is a reason the volume may only be read — kept with the slot and said only
 * if the mount stands on it (§2.3, M-8). So is a sector whose lead its records
 * do not give it (§16.3 п.4): a shape the format does not allow, said, and
 * the slot read all the same — the two readers of a book, this and
 * volume-read's (tally.py SlotRead), judge a slot by the one rule.
 *
 * The mount's alone, with the door shut.
 */

#define SPINE_SLOT_BLANK  0u
#define SPINE_SLOT_TORN   1u
#define SPINE_SLOT_VALID  2u

typedef struct {
    uint32_t    index;
    uint64_t    block;
    uint32_t    state;           /* SPINE_SLOT_* */
    char        why[160];        /* TORN: why */
    uint64_t    seq;             /* VALID: the seq its eight sectors carry */
    SpineHead   head;
    SpineRibbon ribbon;
    SpineCounts counts;
    uint64_t    flags;           /* SpineFlags; 0 with none */
    uint8_t*    stream;          /* VALID: the slot's stream — its roots' pointers lie in it */
    SpineRoot*  root;            /* VALID: `roots` of them, in the order the slot names them */
    uint32_t    roots;
    char**      glass;           /* reasons the slot gives that the volume may only be read */
    uint32_t    nglass;
    char        shape[160];      /* a lead its records do not give a sector; "" for none */
} SpineSlot;

/* Slot `slot->index` at `slot->block` read and judged into *slot. OK, or
 * ERR_NO_MEMORY, or the medium's refusal (book_unit.h). */
error_t SpineSlotJudge(SpineSlot* slot);

/* Every slot of the ring `frame` names: the VALID ones into a new array,
 * newest first (by seq; a tie by index) — *count of them — and every TORN one
 * said as it is met. `what` (room bytes) gets the slots' states in a line, for
 * a mount that finds none valid to say. OK — none valid is *count 0 —, or
 * ERR_NO_MEMORY, or the medium's refusal. */
error_t SpineRead(const VolumeFrame* frame, SpineSlot** out, uint32_t* count, char* what, size_t room);

/* A slot's memory, and a run of them, let go. */
void    SpineSlotLetGo(SpineSlot* slot);
void    SpineLetGo(SpineSlot* slots, uint32_t count);

#endif /* TAGFS_MOUNT_SPINE_READ_H */
