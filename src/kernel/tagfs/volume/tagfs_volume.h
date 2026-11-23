#ifndef TAGFS_VOLUME_H
#define TAGFS_VOLUME_H

/*
 * ‼ A VOLUME IS ONE THING IN MEMORY.
 *
 * Everything the kernel holds of a mounted volume — its door and its medium,
 * its book read into memory, the map of its files and its tags, the writers
 * that sew its stitches and bind its Tallies, the work it does in the
 * background — is one TagFSVolume, and every function of TagFS that touches
 * any of it is handed the volume it works on (`vol`). Nothing of a volume is a
 * variable of its own any more: two volumes are two of these, side by side,
 * and neither can reach into the other.
 *
 * Each part is its module's, defined in its module's header beside the rest of
 * it, and begun by the module (XxxVolumeBegin) where it does not begin as nothing.
 *
 * ‼ NO WORK HOLDS TWO VOLUMES AT ONCE. A file carried from one to another is
 * read from the first, its door left, and written to the second.
 */

#include "../tagfs.h"
#include "baton.h"
#include "../block_map/block_map.h"
#include "../block_map/block_map_derived.h"
#include "../dedup/dedup_internal.h"
#include "../helm/helm.h"
#include "../helm/engross.h"
#include "../ink/ink_internal.h"
#include "../ink/commonhold.h"
#include "../ink/docket.h"
#include "../ink/fallow.h"
#include "../integrity/gloss.h"
#include "../integrity/integrity_internal.h"
#include "../keep/keep.h"
#include "../mount/away.h"
#include "../mount/book_mount.h"
#include "../mount/book_unit.h"
#include "../mount/facsimile.h"
#include "../mount/foxing.h"
#include "../mount/ground_census_internal.h"
#include "../mount/perambulation.h"
#include "../mount/ribbon.h"
#include "../mount/tally_read_internal.h"
#include "../mount/under_glass.h"
#include "../read/read.h"
#include "../retire/retire.h"
#include "../run/run.h"
#include "../scribe/scribe.h"
#include "../settle/binder.h"
#include "../settle/binder_internal.h"
#include "../settle/settle_internal.h"
#include "../settle/dog_ear.h"
#include "../settle/endpaper_fact.h"
#include "../settle/escheat.h"
#include "../settle/foliation.h"
#include "../settle/loose_leaves.h"
#include "../settle/sewing.h"
#include "../show/show.h"
#include "../use_memory/use_memory.h"

/* The volume's door, its mounting and its medium (tagfs.c): who is inside and
 * which mounting it is, a mount owed and what asks it again, and the seat, the
 * taking of it and the ground the volume stands on. */
typedef struct {
    volatile uint8_t door;
    volatile uint32_t inside;
    volatile uint32_t mount_epoch;
    /* A volume that was shut but not yet let go of, because somebody was inside.
     * Raised where the departure is noticed, lowered by the pass that finishes it
     * (TagFSServiceIfPending) — the same arrangement the retiring xHCI slots use,
     * and for the same reason: the place that notices cannot wait. */
    volatile uint32_t abandon_pending;
    bool leaving_said;
    /*
     * ‼ A MOUNT THAT DID NOT FINISH, AND WHAT MAKES IT ASK AGAIN.
     *
     * Mounting is driven by ARRIVALS: a medium is seated, `seat:taken` is
     * published, and this file takes the volume up. So a mount that begins and
     * fails leaves the machine with no filesystem until the NEXT arrival — and if
     * the medium is sitting in its socket, answering, there will not be one.
     * Measured on the owner's board, 2026-08-30: three returns out of fifty-five
     * failed their mount, and all three recovered ONLY because the hand at the
     * machine kept replugging.
     *
     * Two different failures, and only one of them is worth asking about again:
     *
     *   the medium REFUSED a read while the volume was being read — the room
     *   says when it answers again: `seat:ready` (boardroom.h), rung by the seat
     *   itself on the first command it answers after a refusal, and by its
     *   driver's recovery. The mount is owed until a seat says it, and is asked
     *   again then — not on a clock, not a count of tries: a clock guessed how
     *   long the hardware needs, and a count gave up on a medium that only needed
     *   longer, or went on asking one that would never do;
     *
     *   the medium ANSWERED every read and what it holds was refused — a chain
     *   that leaves the volume, a bitmap too small for the run, no memory to hold
     *   it. Asking again reads the same bytes and refuses them again, so the cause
     *   is said once and nothing asks until a medium arrives.
     *
     * And a medium that LEFT while it was being read is neither: its return is an
     * arrival, and the arrival asks.
     *
     * ‼ ANY seat's seat:ready, counted from where the attempt ENDED. A mount reads
     * the whole room to find its volume, so a seat that answers again anywhere may
     * change what it finds. But a bell rung while the attempt was reading is the
     * attempt's own — its next read answered after the refused one, or a driver's
     * recovery of the very command it refused, which says seat:ready in the same
     * breath as it answers that command ERR_IO — and the attempt that heard it
     * has already read the room it rang about. The count was taken from BEFORE the
     * first read, so that bell asked the mount again; a sector the medium could
     * never read refused that attempt too, and its recovery rang again: an AHCI
     * volume with a medium error under its metadata was mounted on every service
     * pass, for as long as the machine ran. So the room's count is taken when an
     * attempt ends (mount.mount_rang), and only a bell after it — a command of
     * somebody else's answered after the refusal, or the seat's own reset (the
     * note over mount.reset_seating) — asks again; a medium that arrives asks on its
     * own. A refusal whose recovery rang in the same breath costs one reset of the
     * seat, never a mount that waits for good.
     *
     * The retry this replaced was 750 ms × 8, and the count was reset by the very
     * attempt that exhausted it: a volume refused for good was mounted again
     * forever.
     */
    volatile uint32_t mount_owed;
    volatile uint32_t mount_rang;  /* the room's seat:ready count as the attempt ended */
    /*
     * ‼ A SEAT THAT REFUSED, AND THAT NOTHING WILL EVER ASK AGAIN, IS RESET ONCE.
     *
     * The bell rings on the first command a seat answers after its refusal — and
     * while a mount is owed nobody sends that seat a command. A medium error no
     * driver recovered from (an IDE drive whose retries ran out, a USB disk that
     * answered MEDIUM ERROR and needed no transport reset) left the mount waiting
     * for a bell no command could ring, until a hand replugged the medium. So a
     * refusal no bell answered asks the seat's driver for its own recovery
     * (BoardroomSeatReset): a medium that comes back from it rings, and the mount
     * goes again; one that does not is a seat the room cannot reach, and the
     * mount waits for a medium to arrive.
     *
     * ONCE per occupant of a seat, for as long as the mount is owed: the seating
     * reset is kept here and let go of when the mount settles, either way. A
     * medium that refuses again after its reset is asked again only by a bell
     * after that attempt, or an arrival — never a second reset, never a loop.
     */
    uint32_t reset_seating;
    /*
     * ‼ AND A MOUNT THE MACHINE HAD NO MEMORY FOR WAITS FOR THE MEMORY.
     *
     * The heap running short while the volume is read is no verdict on the volume,
     * and the attempt that met it was taken for one refused for what it holds —
     * said once, never asked again. Owed instead, on the event a Settle cycle
     * refused for want of memory waits for (settle/settle_wait.c): the machine's
     * free pages, counted when the attempt was refused, and the mount goes again
     * once there are more of them — looked at by every service pass, which a
     * cabin's memory coming back knocks awake (TagFSMemoryCameBack).
     */
    volatile uint32_t mount_heap_owed;
    volatile uint64_t mount_heap_level;  /* free pages when it was refused */
    /* Refused reads of the volume, counted where the mount's readers meet them
     * (TagFSNoteReadRefused): a mount that saw one failed on a medium that did not
     * answer. The ground's own reads — the partition table and the Deed — are the
     * survey's, and a mount refused there is one the medium did not answer only
     * when the medium refused them (volume_take_ground): a Deed that says what
     * does not fit is an answer. */
    volatile uint32_t reads_refused;
    bool ground_refused;
    /*
     * ‼ ONE CORE AT A TIME MOUNTS OR UNMOUNTS, AND THE REST GO AWAY.
     *
     * Attending to a medium used to reach this file down one road only — the
     * Boardroom being called to order, which is already one-at-a-time — so nothing
     * here needed a flag of its own. The service pass is a SECOND road, taken by
     * the idle loop and by every K-Core, and without this two of them can be
     * inside tagfs_init at the same moment: two mounts of one volume, each
     * allocating what the other is about to overwrite.
     *
     * Whoever gets here first does the work; the others leave rather than queue
     * up behind it — the same arrangement the hubs and the slot service use.
     *
     * ‼ BUT AN ARRIVAL IS NEVER LEFT BEHIND. A medium that arrives while somebody
     * is mounting is not "nothing to add": the mount under way may have read the
     * room before it came, and fail on the medium that left it. So an arrival is
     * a debt (mount.arrival_owed), not a call: the room's seat:taken only notes it,
     * every service pass looks for it, and whoever holds the flag looks again
     * before letting it go. The retry that used to pick such an arrival up was a
     * clock, and it is gone.
     */
    volatile uint32_t attending;
    volatile uint32_t arrival_owed;  /* seat:taken heard, not yet attended */
    Knock mount_knock;
    /*
     * Which medium the volume is on.
     *
     * This used to be two numbers and a question asked at every call site: an ATA
     * drive index, an AHCI port, and "is AHCI initialised?" repeated eight times
     * over. It is now one seat number in the Boardroom, which knows what kind of
     * medium sits in it. The filesystem's business is sectors; whose sectors they
     * are is somebody else's.
     */
    uint8_t seat;
    /*
     * WHICH occupant of that seat this volume is.
     *
     * A seat can be taken again, which is what lets a stick that was pulled out
     * come home to the chair it left. The cost of that is exactly this: "there is
     * a medium in seat 3" stopped being enough to know it is the SAME medium, so
     * the taking is stamped here at the moment the volume is mounted and compared
     * afterwards. A chair that changed hands while nobody was reading from it is a
     * medium that left, whatever is sitting in it now.
     *
     * Before seats could be reused this was covered by accident — a returning
     * medium always got a NEW seat number, so the number itself carried the
     * answer. It also meant the room grew by one chair per unplug for ever, and a
     * medium that got its old unit number back was never announced at all.
     */
    uint32_t seating;
    /* The first seat the last search of the room could not read the answer out
     * of, BOARDROOM_NO_SEAT when every seat answered — "no volume here", said by a
     * medium that would not be read, is no answer at all. */
    uint8_t search_unread;
    /*
     * Which volume this is, as opposed to which seat it happens to be in.
     *
     * A seat is a socket and a volume is a filesystem, and on a machine you can
     * unplug things from, the second moves between the first. The identity is the
     * one the superblock carries and the loader writes on its boarding pass, so it
     * survives the medium being pulled out and put back in a different socket —
     * which is exactly the case where a seat number proves nothing.
     */
    uint8_t uuid[16];
    bool uuid_known;
    /*
     * Where this volume's ground begins on the medium it is sitting on.
     *
     * The one absolute address TagFS holds, and it is not written down anywhere
     * inside the volume: it is discovered from the medium's partition table every
     * time the volume is met. Zero until a volume has been taken up, which is
     * safe — nothing reads sectors before then, and reading sector 0 of the medium
     * is not something a filesystem should be able to do by accident.
     */
    uint64_t base;
    /*
     * Has the medium the volume lives on left the machine?
     *
     * A removable disk is removable while the kernel is using it, and once it has
     * gone the volume on it has gone with it. Saying so once, here, is what makes
     * everything above deterministic — without it, whether a file could still be
     * read depended on whether its blocks happened to be in a four-entry
     * read-ahead cache, and a program image half of which is real and half of
     * which is missing is one the loader will happily run. Measured: it did.
     *
     * The cached bytes are not wrong. They are simply not the medium any more, and
     * a filesystem that cannot tell the difference is one that answers questions
     * about a disk that is in somebody's pocket.
     *
     * Nothing is taken down: the volume's book is held open until an event
     * decides it (mount/away.h).
     */
    bool medium_left;
    /*
     * ‼ AND HOW MANY TAKINGS THE ROOM HAD MADE WHEN THE BOOK OPENED.
     *
     * Every taking of a chair anywhere in the room gets the next number
     * (BoardroomSeatSeating), so a medium seated after the book opened may be the
     * one that left, and one seated before it never left — the internal disk a
     * stick was copied from, a second stick carrying a copy of the same volume.
     * The return used to judge the first seat carrying the volume's identity, and
     * measured so (P0, replug): the stick came back in seat 1, and seat 0 — the
     * internal disk, which never moved — was judged in its place, found older
     * than what a cycle left on the stick, and the medium accused of lying. Noted before the latch is set, so whoever reads the latch set reads
     * what it opened on; 0 while no book is open.
     */
    volatile uint32_t book_seatings;
    /*
     * Until the machine has tried to mount its volume once, an arriving medium is
     * not news — it is part of the same boot, and the mount that is about to
     * happen will see it. Without this gate a K-Core running the guide loop and
     * the boot core both call tagfs_init, and the volume is mounted twice.
     * Measured: two "volume on seat 0" lines, one boot.
     */
    volatile uint32_t boot_mount_settled;
} MountState;

struct TagFSVolume {
    TagFSState            state;            /* what it is and its maps: registry, tag index, files (tagfs.h) */
    MountState            mount;            /* its door, its mounting, its medium (above) */
    BookMount             book;             /* its book as the mount read it (mount/book_mount.h) */
    BinderBook            binder;           /* the slots and what the next Tally is written into (settle/binder.h) */
    UseMemoryState        use_memory;       /* its memory of the Use Context (use_memory/use_memory.h) */

    /* the mount's parts (mount/) */
    BookMountState        book_mount;
    BookUnitState         book_unit;
    TallyReadState        tally_read;
    TallyLeavesState      tally_leaves;
    FacsimileState        facsimile;
    FoxingState           foxing;
    GroundCensusState     ground_census;
    RibbonCollateState    ribbon_collate;
    UnderGlassState       under_glass;
    AwayState             away;
    PerambulationState    perambulation;

    /* what is written, and when (settle/) */
    SettleState           settle;
    SettleWaitState       settle_wait;
    SettleHaltState       settle_halt;
    CadenceState          cadence;
    SewingState           sewing;
    RibbonState           ribbon;
    RibbonLeftState       ribbon_left;
    BinderState           binding;          /* the Tally being bound (settle/binder.c) */
    BinderGleanState      binder_glean;
    BinderProofState      binder_proof;
#if BINDER_CHECKS
    BinderCheck           binder_check;     /* the Binder's self-check, where a DEFECT is looked for */
#endif
    BirthsState           births;
    BirthsSharedState     births_shared;
    DogEarState           dog_ear;
    EndpaperState         endpaper;
    EndpaperFactState     endpaper_fact;
    EscheatState          escheat;
    FoliationState        foliation;
    LooseLeavesState      loose_leaves;

    /* changes and the ground they take (ink/, block_map/) */
    InkState              ink;
    DocketState           docket;
    FallowState           fallow;
    CommonholdState       commonhold;
    AllotmentState        allotment;
    BlockMapDerivedState  block_map_derived;

    /* its files' writers, readers and keepers */
    HelmState             helm;
    EngrossState          engross;
    ReadAheadState        read_ahead;
    RunState              run;
    ScribeState           scribe;
    KeepState             keep;
    RetireState           retire;
    ShowState             show;

    /* what it checks of itself (integrity/) and what it finds alike (dedup/) */
    IntegrityState        integrity;
    IntegrityRotState     integrity_rot;
    GlossState            gloss;
    DedupGleanState       dedup_glean;
    DedupPassState        dedup_pass;
    CommonplaceState      commonplace;
};

/* Every part of `vol` as it begins: a volume taken up is begun once, before
 * anything reads or writes it (volume/tagfs_volume.c). */
void TagFSVolumeBegin(TagFSVolume* vol);

void MountVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_VOLUME_H */
