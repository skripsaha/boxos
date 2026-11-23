#ifndef TAGFS_CARRY_H
#define TAGFS_CARRY_H

#include "../tagfs.h"
#include "../settle/settle.h"

/*
 * Carry — a file taken from one volume of the house to another whole: its
 * bytes, its name, the person's tags and the programs' own, the programs it
 * shows its tags to, the term it is kept until when it lies deleted, the
 * program that made it, the passport of the program it holds, and its times.
 * A copy leaves it where it was too; a move lets it go there once the copy is
 * the volume's for good.
 *
 * ‼ NO WORK HOLDS TWO VOLUMES AT ONCE. A carry is a run of turns of the
 * volumes' service points (HelmReady): a turn on the volume the file comes
 * from reads a piece of it into the carry's own memory, a turn on the one it
 * goes to writes it there, and each turn enters its one volume's door and
 * leaves it. A volume short of room is waited for by its next cycle; a
 * medium that leaves ends the carry, said.
 *
 * ‼ A COPY CUT SHORT IS NEVER TAKEN FOR THE FILE. The copy is born still
 * arriving (TAGFS_FILE_ARRIVING) — met by no question, no name and no open of
 * a program — and its last Ink, its marks laid, takes the bit off. One the
 * bit is still on when no carry lays a file on its volume — the machine
 * stopped in the middle of one, or its medium left — is let go by the volume
 * (CarryServe). A move lets the file go where it was only once the volume it
 * went to settled the copy (SettleWaitLink), and only if the file is still
 * the one the carry read: a file changed meanwhile stays where it was, beside
 * its copy, and the move answers ERR_BUSY.
 *
 * ‼ A PROGRAM IS WHO IT IS ON THE VOLUME IT GOES TO. The person's tags go as
 * they are. A program's own tags, the programs a file shows its tags to and
 * the program that made a file go with a program the other volume knows: a
 * signed one by its passport, which every volume knows it by, and one carried
 * earlier in the same carry by the copy it has there — so a whole volume is
 * carried with its programs first. A program the other volume cannot know is
 * nobody there: its words are left behind, counted (Carry.dropped) and said.
 * A file the system keeps (a role, TAGFS_FILE_ROLE_*) is laid by a volume's
 * maker and carried by no call: ERR_PERMISSION_DENIED.
 *
 * Holes are laid as the zeros they read as, as a span lays them.
 */

/* What one volume holds of the carries (TagFSVolume, volume/tagfs_volume.h):
 * how many lay a file on it now, and whether a file may be arriving there
 * with no carry to finish it — at a mount, or after a carry that could not
 * let its copy go. The service's own word, and the stand's hold of it. */
typedef struct {
    volatile uint32_t inbound;
    volatile uint32_t sweep_owed;
    volatile uint32_t busy;
    volatile uint32_t held;
} CarryState;

/* One file of a carry: the volume it lies on and its own id there, and its
 * own id on the volume it went to once it arrived — 0 until then. */
typedef struct {
    TagFSVolume* from;
    uint64_t     fid;
    uint64_t     arrived;
} CarryItem;

/* Who a reference to a program becomes on the volume a file goes to: the copy
 * the carry made of the program file, or its passport, or nobody (both 0) —
 * or, in a list of the programs a file shows its tags to, every program. */
typedef struct {
    uint64_t fid;
    uint64_t passport;
    bool     every;
} CarryWho;

/* A tag of the file under way, its text copied off the volume it comes from. */
typedef struct {
    CarryWho whose;      /* both 0 with `person` for the person's */
    bool     person;
    char*    text;       /* key, then value: key_len + value_len bytes */
    size_t   key_len;
    size_t   value_len;
    bool     has_value;
} CarryTag;

typedef enum {
    CARRY_SURVEY = 0,   /* on `from`: the file read whole, its handle opened */
    CARRY_BORN,         /* on `to`: its tags spoken there, the copy born arriving */
    CARRY_READ,         /* on `from`: the next piece read */
    CARRY_WRITE,        /* on `to`: the piece written, a window a turn */
    CARRY_MARKS,        /* on `to`: passport, shown, arrived — kept, when it lay deleted */
    CARRY_SETTLE,       /* a move: the copy settled on `to` */
    CARRY_LEAVE,        /* a move, on `from`: the file let go, if still the one read */
    CARRY_END,          /* what is left let go, and the answer */
} CarryStep;

typedef struct Carry Carry;
struct Carry {
    /* Set by the one who begins it. */
    TagFSVolume* to;
    CarryItem*   items;      /* in the order they go: the programs before what names them */
    uint32_t     count;
    bool         move;
    InkWarrant   warrant;
    void       (*ended)(Carry* c);   /* run once with no TagFS lock held: rc, done, dropped */

    /* The answer. */
    error_t      rc;
    uint32_t     done;       /* the items that arrived, in order */
    uint32_t     dropped;    /* program words no program of `to` could speak, left behind */

    /* The carry's own. */
    CarryStep        step;
    TagFSVolume*     at;          /* the volume the next turn runs on */
    HelmWaiter       turn;        /* listed on `at` for the next turn */
    HelmWaiter       helm;        /* the copy's Helm, asked a window at a time */
    SettleWaiter     wait;        /* a cycle of `to` (room, the copy settled) or of `from` (room) */
    volatile uint32_t holds;      /* the Helm is the carry's */
    volatile error_t came;        /* what the cycle it waited for answered */
    TagFSFileHandle* src;
    TagFSVolume*     src_vol;     /* the volume `src` is a handle of */
    TagFSFileHandle* dst;
    uint32_t*        seen_tags;   /* the record the carry read: its tags and the programs it shows them to, */
    uint64_t*        seen_shown;  /* ... by which a move knows the file is still that one (CarrySame) */
    uint32_t         seen_tag_count, seen_shown_count;
    uint64_t         copy;        /* the copy's id on `to`, 0 before it is born */
    uint64_t         copy_gen;    /* the copy's last Ink */
    bool             inbound;     /* counted in to->carry.inbound */
    char*            name;
    uint32_t         flags;
    uint64_t         size, created_ns, modified_ns, keep_until, passport;
    CarryWho         made_by;
    CarryWho*        shown;
    uint32_t         shown_count;
    CarryTag*        tags;
    uint32_t         tag_count;
    uint8_t*         piece;
    uint64_t         piece_len, piece_at, offset;
};

/* The carry begun: OK, and `ended` runs once it is over — every item that
 * arrived counted in `done`, the answer in `rc`; or the refusal before
 * anything moved, and `ended` never runs: ERR_INVALID_ARGUMENT for no items,
 * an item already on `to`, or `to` not mounted; ERR_NO_MEMORY. The carry is
 * the caller's memory until `ended`. */
error_t CarryBegin(Carry* c);

/* The service, at every service point: the files left arriving with no carry
 * to finish them let go, once none is under way to the volume. */
void CarryServe(TagFSVolume* vol);
/* A mount's start: whatever arrives there is looked at first. */
void CarryMountReset(TagFSVolume* vol);
/* The stand's hold on the service, as KeepHold: whether it was held. */
bool CarryHold(TagFSVolume* vol, bool held);

#endif /* TAGFS_CARRY_H */
