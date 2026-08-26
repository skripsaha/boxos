#ifndef BOARDROOM_H
#define BOARDROOM_H

#include "ktypes.h"
#include "error.h"

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

/*
 * Something arrived on a bus that can gain media while the machine runs.
 *
 * Noted from wherever it was noticed — an interrupt handler, usually — and
 * acted on by the pass below, which runs where waiting is allowed. It has to
 * be two steps: seating a medium means asking it how large it is and whether
 * it is ready, and both of those are transfers.
 *
 * Without this the room was called to order exactly once, at boot, and a stick
 * pushed in afterwards was a device the USB driver knew all about and no
 * filesystem could ever reach: it had no seat, and a seat is the only way in.
 */
void BoardroomNoteArrival(void);

/*
 * A medium left, and this is said at the moment it does.
 *
 * Not deferred, because a seat holds a controller-specific index and those are
 * handed out again as soon as they are free: by the time a deferred pass ran,
 * the seat could be pointing at a different medium with the same number, and
 * nothing would ever have noticed the first one leaving. Called from the
 * service pass that takes the device down, which is ordinary kernel context.
 */
void BoardroomNoteDeparture(void);

/* Cheap enough for the idle loop: one atomic load when nothing has arrived,
 * which is almost always. */
void BoardroomAttendIfPending(void);

/*
 * The room says what changed in it, and does not decide what that means.
 *
 * It used to call the filesystem directly — the comment at the call site
 * apologised for it, which is usually a sign that the arrangement is wrong
 * rather than that the apology was needed. A room full of media has no
 * business knowing that anybody keeps a filesystem on one; it announces, and
 * whoever cares is listening.
 *
 *   seat:taken     one or more media are in the room that were not before
 *   seat:emptied   a medium left
 *
 * Announced AFTER the room has settled, never part-way through seating: a
 * listener that mounts on this event would otherwise be looking at a room
 * that is still filling up, and would choose from the seats that happened to
 * be added first.
 *
 * A departure does not name a seat. The medium is already gone by the time
 * anyone is told, and seat indices are handed out again as soon as they are
 * free — so a number here would name a socket that may already hold something
 * else. A listener re-checks the seat it cares about instead.
 */
typedef struct __attribute__((packed)) {
    uint8_t seated;      /* media in the room now */
    uint8_t arrived;     /* how many of them are new (0 for a departure) */
    uint8_t first_new;   /* lowest new seat number, or BOARDROOM_NO_SEAT */
    uint8_t reserved;
} BoardroomSeatEvent;

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

/*
 * A read for a caller that is not going to stand and watch.
 *
 * The room is where "which controller is this" is answered, and this is the
 * same question in the one place it is asked. Before it, the storage deck asked
 * "is there an AHCI disk under the volume?" — because when it was written a
 * volume could only live on one, and there was nothing else to ask. A machine
 * that boots from a flash drive answers no, and every block of every file it
 * reads is read by a core standing over the transfer, while the same machine
 * booting from SATA parks the caller and gets on with something else.
 *
 * `cb` runs on a K-Core once the medium has answered, and is shaped the way the
 * SATA path already shaped it — `index` names the port or the unit, `slot` is
 * the AHCI command slot and is zero for a seat that has no such thing — so the
 * same function can be handed to either kind of seat with nothing standing in
 * between to translate.
 *
 * Sectors land straight in `dma_phys`. Returns OK when the read is on its way
 * and the callback WILL run; on any other return it will not, and the caller
 * still owns everything it passed in.
 */
typedef void (*BoardroomAsyncCb)(uint8_t index, uint8_t slot,
                                 error_t status, void* ctx);

error_t BoardroomReadAsync(uint8_t seat, uint64_t lba, uint32_t count,
                           void* dma_phys, BoardroomAsyncCb cb, void* ctx);

/* Whether this seat can take the call above at all. A seat that cannot is not
 * a broken seat — it is one whose caller has to stay and wait. */
bool BoardroomSeatCanReadAsync(uint8_t seat);

/*
 * Prove, on this machine, that a read nobody stands over brings back the same
 * bytes as one somebody does.
 *
 * A boot self-test rather than a unit test, because the thing being tested is a
 * conversation with the hardware in front of it: whether the completion reaches
 * a K-Core, whether the sectors land where they were asked to, and whether the
 * bytes are the right ones. None of that can be established anywhere but on the
 * machine, and the machine this matters most on is read by photographing its
 * screen — so it says PASS or FAIL in one line and says why.
 *
 * No-op, silently, for a seat that has no asynchronous read to test.
 */
void BoardroomAsyncSelfTest(uint8_t seat);

/* Push the medium's own write cache out. A write that has completed is a write
 * the device has accepted, not necessarily one it has kept. */
int BoardroomFlush(uint8_t seat);

/*
 * Find the seat carrying a volume, by asking each one in turn.
 *
 * The caller supplies the recognition — the Boardroom knows about media, not
 * about what anybody keeps on them. `probe` is handed a seat and returns true
 * when it recognises its own volume there, filling `out_uuid` with whatever
 * that volume calls itself.
 *
 * When more than one seat answers, the identity decides. The loader read this
 * kernel out of one particular volume and wrote down which one, so the seat
 * whose volume matches the Boarding Pass is the seat this machine booted from
 * — not a resemblance, the same sixteen bytes.
 *
 * Only when there is no pass, or nothing on the bus matches it, does the old
 * rule apply: the removable medium wins, because a machine that boots from a
 * stick while carrying an old volume on an internal disk should not quietly
 * mount the wrong decade. That rule is a guess, and it is printed as one.
 *
 * Returns the seat, or 0xFF when nothing was recognised.
 */
typedef bool (*BoardroomProbe)(void* ctx, uint8_t seat, uint8_t out_uuid[16]);
uint8_t BoardroomFindVolume(BoardroomProbe probe, void* ctx);

#define BOARDROOM_NO_SEAT 0xFF

#endif
