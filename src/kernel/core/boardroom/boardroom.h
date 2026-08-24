#ifndef BOARDROOM_H
#define BOARDROOM_H

#include "ktypes.h"

/*
 * The Boardroom — where every medium this machine can keep a filesystem on has
 * a seat, and where anything that wants sectors comes to ask for them.
 *
 * Before it, a filesystem asked "AHCI or ATA?" at every call site that touched
 * a disk. There were eight of them. Adding a third medium would have made
 * twenty-four, and each one would have been a place where the answer could be
 * given differently from the others — which is how a volume ends up half read
 * from one device and half written to another.
 *
 * So the question is asked once, here. A seat is a medium that answered when
 * it was called on; what kind of controller is under it is the Boardroom's
 * business and nobody else's.
 */

typedef enum {
    BOARD_NONE = 0,
    BOARD_AHCI,             /* a SATA disk behind an AHCI controller */
    BOARD_ATA,              /* a disk on the legacy IDE channels */
    BOARD_USB               /* a mass storage device on the USB bus */
} BoardKind;

/* Sectors are 512 bytes here, everywhere, on every kind of seat. A medium that
 * addresses itself differently is refused by whoever knows about it rather
 * than translated silently. */
#define BOARDROOM_SECTOR_BYTES 512

/* Call the room to order: find every medium, and let the USB bus finish saying
 * what is on it first. Safe to call more than once; later calls pick up media
 * that arrived since. */
void BoardroomInit(void);

/* How many media answered, and what each of them is. */
uint8_t     BoardroomSeatCount(void);
BoardKind   BoardroomSeatKind(uint8_t seat);
const char* BoardroomSeatName(uint8_t seat);

/* True for a medium that can be unplugged — which is also the medium this
 * machine was most likely booted from, and the tie-breaker when a filesystem
 * turns out to live on more than one seat. */
bool        BoardroomSeatIsRemovable(uint8_t seat);

/*
 * Is there still a medium in this seat?
 *
 * A seat outlives the medium that sat in it — seat numbers never move under
 * anybody, so a disk that leaves leaves an empty chair rather than renumbering
 * the room. Anything holding a seat number has to be able to ask whether there
 * is still something in it, because the answer changes while it is holding it:
 * that is what removable means.
 */
bool        BoardroomSeatOccupied(uint8_t seat);

/* The controller-specific index behind a seat — an AHCI port, an ATA drive, a
 * USB unit. This is a deliberate way out of the abstraction, for the one thing
 * the abstraction cannot express: a fast path that exists on one kind of
 * controller and nowhere else. Anything using it must check the kind first,
 * because the number means something different for each. */
uint8_t BoardroomSeatIndex(uint8_t seat);

/* Sector I/O. Counts are in 512-byte sectors and are split as needed, so no
 * caller has to know what any particular controller's limit is. */
int BoardroomRead (uint8_t seat, uint64_t lba, uint32_t count, void* buffer);
int BoardroomWrite(uint8_t seat, uint64_t lba, uint32_t count, const void* buffer);

/* Push the medium's own write cache out. A write that has completed is a write
 * the device has accepted, not necessarily one it has kept. */
int BoardroomFlush(uint8_t seat);

/* Find the seat carrying a volume, by asking each one in turn.
 *
 * The caller supplies the recognition — the Boardroom knows about media, not
 * about what anybody keeps on them. `probe` is handed a seat and returns true
 * when it recognises its own volume there.
 *
 * When more than one seat answers, the removable one wins and every candidate
 * is named. This kernel cannot ask the firmware which device it was booted
 * from once it is in long mode, so "the medium we booted from" is approximated
 * by "the medium somebody plugged in" — and the approximation is printed
 * rather than assumed, so a wrong guess is visible instead of silent.
 *
 * Returns the seat, or 0xFF when nothing was recognised. */
typedef bool (*BoardroomProbe)(void* ctx, uint8_t seat);
uint8_t BoardroomFindVolume(BoardroomProbe probe, void* ctx);

#define BOARDROOM_NO_SEAT 0xFF

#endif
