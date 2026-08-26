#ifndef DEED_READER_H
#define DEED_READER_H

#include "ktypes.h"
#include "error.h"
#include "volume_deed.h"
#include "ground.h"

/*
 * Reading a Deed off a medium.
 *
 * The format is in volume_deed.h. This is the part that goes and gets one, and
 * everything it does is a refusal waiting to happen: a Deed is the one thing a
 * kernel believes before it believes anything else, so every reason to doubt
 * it is checked here and said out loud rather than carried forward.
 */

/*
 * The other half of an agreement with two assemblers' worth of code.
 *
 * src/boot/stage2/stage2.asm reads a Deed by offsets it spells for itself —
 * an assembler cannot ask a C structure where a field is. The old superblock
 * had exactly this arrangement for one field, and it held only because
 * somebody remembered. These are the whole prologue, checked by the compiler:
 * move a field and the build stops, instead of the boarding pass quietly
 * naming sixteen bytes of something else.
 */
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, magic)          == 0,  "stage2 DEED_OFF_MAGIC");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, prologue_bytes) == 8,  "stage2 DEED_OFF_PROLOGUE_BYTES");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, stamp_bytes)    == 10, "stage2 DEED_OFF_STAMP_BYTES");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, crc32)          == 12, "stage2 DEED_OFF_CRC32");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, uuid)           == 16, "stage2 DEED_OFF_UUID");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, sectors)        == 32, "stage2 DEED_OFF_SECTORS");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, tail_sector)    == 40, "stage2 DEED_OFF_TAIL_SECTOR");
STATIC_ASSERT(__builtin_offsetof(VolumeDeed, role)           == 48, "stage2 DEED_OFF_ROLE");
STATIC_ASSERT(sizeof(VolumeDeed)                             == 52, "stage2 DEED_PROLOGUE_BYTES");

/* And the one stamp stage2 walks to, whose three numbers it reads by offset. */
STATIC_ASSERT(__builtin_offsetof(VolumeBoot, kernel_block)  == 0, "stage2 BOOT_KERNEL_BLOCK");
STATIC_ASSERT(__builtin_offsetof(VolumeBoot, kernel_blocks) == 4, "stage2 BOOT_KERNEL_BLOCKS");
STATIC_ASSERT(__builtin_offsetof(VolumeBoot, kernel_bytes)  == 8, "stage2 BOOT_KERNEL_BYTES");

/* What a read produced. The head and tail are both full Deeds; `stamps` points
 * into `raw` and is walked by DeedStamp. */
typedef struct {
    VolumeDeed     head;
    const uint8_t *stamps;      /* into raw[], stamp_bytes long */
    uint8_t       *raw;         /* the sectors as they were read */
    uint32_t       raw_bytes;
} DeedCopy;

/*
 * Read the Deed at the head of this ground, and check it.
 *
 * Checked, in order: the magic; that the prologue and stamps fit what was
 * read; the checksum over both; that it says it is a head and not a tail; and
 * that the ground it was handed is at least as long as the Deed claims to be.
 * That last one is what catches an image copied short — today a truncated
 * volume mounts happily and reads into ground that is not there.
 *
 * Returns OK and fills `out` (whose `raw` the caller frees with DeedRelease),
 * or an error, having already said which check failed and on which seat.
 */
error_t DeedReadHead(uint8_t seat, const MediumGround *ground, DeedCopy *out);

/*
 * And the copy at the far end, which the head says where to find.
 *
 * A volume with both is a volume whose whole extent is present and whose
 * identity agrees with itself at both ends. One with only a head still mounts
 * — a medium may have lost its tail — but the machine says so, because that is
 * a volume with no second opinion left.
 */
error_t DeedReadTail(uint8_t seat, const MediumGround *ground,
                     const DeedCopy *head, DeedCopy *out);

/*
 * The far copy when there is no head to ask.
 *
 * The head states where its own tail is, so the ordinary path reads it from
 * there. When the head is the thing that is damaged, that number is gone too —
 * and the ground itself still says how long it is, which is enough: the tail
 * sits in the last whole block of it, because that is where it is put.
 *
 * This is what makes two copies worth having rather than merely reassuring. A
 * volume whose head was lost to one bad erase block still mounts, off the copy
 * at the other end of the medium, and says that is what happened.
 */
error_t DeedReadTailAlone(uint8_t seat, const MediumGround *ground,
                          DeedCopy *out);

void DeedRelease(DeedCopy *copy);

/*
 * Find one stamp. Returns a pointer to its payload and writes its length, or
 * NULL when this Deed does not carry that kind — which is a fact and not a
 * failure: a Deed written by an older mkfs simply has fewer stamps, and asking
 * for one it never had is how a kernel finds that out.
 */
const void *DeedStamp(const DeedCopy *copy, uint16_t kind, uint16_t *out_bytes);

/* Say what a Deed holds, in the log. Used when a volume is first met. */
void DeedDescribe(uint8_t seat, const DeedCopy *copy);

/*
 * Walk every seated medium, survey its ground, and read whatever Deeds are on
 * it — saying what was found and what was not.
 *
 * A boot self-test rather than a unit test, and for the reason the Boardroom's
 * own already gives: the thing being tested is a conversation with the medium
 * in front of it. Whether a partition table parses, whether a Deed checksums,
 * and whether the copy at the far end agrees with the one at the near end are
 * all facts about THIS machine and cannot be established anywhere else.
 */
void DeedSurveyAll(void);

#endif /* DEED_READER_H */
