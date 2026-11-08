/*
 * lanes.cpp — the console's lanes and its ear. See lanes.h.
 *
 * Output arrives as ConsoleRun frames on per-strand Brook lanes. A lane is
 * the answer to DISP_CMD_LANE, asked (box/ipc.h ask): the daemon opens the
 * READER side of a fresh "console:N" Brook FIRST and only then answers with
 * the tag, so the writer arrives at a laid table. An answer that reaches
 * nobody — the asker died before it — takes the lane back at once. Lane
 * numbers are reused from a free pool — the tag registry grows to the peak
 * number of simultaneous lanes and no further.
 *
 * Input: the daemon is the cabin's ear at the keyboard. It consumes
 * "keyboard" Touch events only while some lane LISTENS, and says each key
 * again as a Touch on the tag of the lane at the top of the listening stack
 * — the program edits its own line and echoes through its own lane. A lane
 * listens for exactly as long as its program reads (DISP_CMD_LISTEN with 1
 * pushes it to the top, with 0 gives the ear back); a lane that closes is
 * dropped wherever it stands, and the one beneath hears again — and so is a
 * lane whose reader nobody can find: the kernel says how many heard each
 * key, and a key nobody heard — or one the kernel would not say on that
 * lane's tag at all — drops the ear on top and is said again to the next. Keys typed while nobody listens are NOT consumed — the kernel
 * TouchRing and the per-strand tag stash bank them for whoever reads next,
 * so type-ahead needs no third buffer.
 *
 * process:died closes what the dead leave behind: granted-but-never-
 * attached lanes are revoked, attached lanes are drained to the last frame
 * (a dying process' final words reach the screen) and closed on
 * STREAM_CLOSED.
 */

#include "lanes.h"

#include <cstdint>
#include <cstring>

#include "box/bay.h"
#include "box/brook.h"
#include "box/convert.h"
#include "box/core/cabin.h"
#include "box/debug.h"
#include "box/ipc.h"
#include "box/keyboard.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/touch.h"

namespace {

typedef struct ConsoleLane {
    Brook              *brook;
    uint32_t            owner_pid;   /* the strand the lane was granted to … */
    uint32_t            owner_gen;   /* … in this incarnation: (pid, generation) */
    uint32_t            number;      /* N of "console:N" */
    TouchTagPair        ear;         /* "console:N" as a Touch tag: where its keys are said */
    bool                closed;      /* drained to STREAM_CLOSED / revoked */
    bool                has_pending; /* `pending` holds the lane's head frame */
    ConsoleRun          pending;     /* popped but not yet rendered (merge) */
    struct ConsoleLane *next;
} ConsoleLane;

typedef struct FreeNumber {
    uint32_t           number;
    struct FreeNumber *next;
} FreeNumber;

/* ‼ THE CONSOLE'S ORDER — ONE NUMBER, OWNED BY THE DAEMON, GRANTED WITH THE LANE.
 *
 * The daemon merges the lanes by the number each frame carries. That number
 * used to be cpu_rdtsc, read on whatever core the writer happened to be on,
 * and cores agree about that counter only to within what the boot-time sync
 * left them (per_core.c tolerates four microseconds, and it drifts after
 * that) — while a program that lists a volume pushes its whole output in
 * less time than that. So two cabins' stamps were not ordered against each
 * other at all, and the shell's next prompt was rendered INSIDE the listing
 * of the command that had just finished. MEASURED on the board: four listings
 * of twelve cut open at exactly the same byte, 891 of 4235. The same byte
 * every time is a FIXED offset between two cores; a race would have
 * scattered, and an emulator, whose cores agree exactly, never showed it.
 *
 * The console does not need to know WHEN something happened. It needs to
 * know WHAT CAME BEFORE WHAT, and that is a question about order, which a
 * clock cannot answer and a counter answers exactly. So: one counter, in one
 * page every printing cabin maps, incremented with a locked add the instant
 * before a frame is pushed. Two writers on two cores get two numbers in the
 * order their two adds happened. No tolerance, nothing to drift.
 *
 * ‼ WHY THE DAEMON HOLDS IT AND NOT THE WRITERS. A Bay is refcounted: when
 * the last cabin releases its claim the pages go back to PMM, and the next
 * cabin to create the tag would start counting from zero again — putting its
 * first frame BEHIND everything still banked in the lanes. The counter must
 * therefore outlive every writer, and exactly one party does: the daemon.
 * It creates the order before it grants its first lane and holds it for as
 * long as it lives, so the counter's lifetime IS the console's lifetime.
 *
 * ‼ AND WHY IT IS GRANTED WITH THE LANE. A writer that has a lane but no
 * order would have to stamp with something else, and anything else is a
 * DIFFERENT DOMAIN of numbers — a counter near 1 against a clock near 2^40 —
 * in one sorted merge, which is worse than the defect it replaces. So there
 * is no such state to be in: the daemon refuses to grant a lane it cannot
 * order (below, in LanesGrant), and a writer that cannot map the order gives
 * its lane back and says its text into the log ring (boxlib print.c,
 * lane_ensure), which keeps its own order.
 *
 * The rejected alternative is written down so it is not invented again:
 * "drain a dead writer's lane first". It sounds causal and it is too late —
 * the daemon learns of a death on its next rotation, and the overtaking
 * happens inside a single render before that. It was written, measured
 * against the oracle, and taken out again. */
uint64_t *g_console_order;            /* the console's one order, held for life */

ConsoleLane *g_lanes;                 /* append at tail — grant order */
FreeNumber  *g_free_numbers;          /* reuse pool for lane numbers */
uint32_t     g_next_number;           /* fresh numbers when the pool is dry */

TouchTag g_kb = TOUCH_TAG_INVALID;

/* ─────────────────────────────────────────────────────────────────────────
 * The ear — which lane hears the keyboard
 *
 * A stack of listening lanes; the top hears. Listening pushes a lane to the
 * top (or moves it there), a lane that closes is dropped wherever it stands,
 * and whoever is left on top hears next. Nothing here consults a clock or a
 * pid: a lane is (pid, generation) already.
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct Ear {
    ConsoleLane *lane;
    struct Ear  *below;
} Ear;

Ear *g_ear;   /* top of the stack, NULL when nobody listens */

void EarListen(ConsoleLane *lane)
{
    Ear **pp = &g_ear;
    while (*pp) {
        if ((*pp)->lane == lane) {
            Ear *e = *pp;
            *pp = e->below;
            e->below = g_ear;
            g_ear = e;
            return;
        }
        pp = &(*pp)->below;
    }
    Ear *e = (Ear *)malloc(sizeof(Ear));
    if (!e) return;   /* nobody listens harder than memory allows; asked again, it is pushed then */
    e->lane  = lane;
    e->below = g_ear;
    g_ear    = e;
}

void EarDrop(ConsoleLane *lane)
{
    Ear **pp = &g_ear;
    while (*pp) {
        if ((*pp)->lane == lane) {
            Ear *e = *pp;
            *pp = e->below;
            free(e);
            return;
        }
        pp = &(*pp)->below;
    }
}

/* Refill a lane's pending slot from its ring. Marks the lane closed when
 * the drained writer's STREAM_CLOSED surfaces. Returns whether a pending
 * frame is available for the merge. */
bool lane_refill(ConsoleLane *ln)
{
    if (ln->has_pending) return true;
    if (ln->closed)      return false;
    int rc = brook_try_pop(ln->brook, &ln->pending);
    if (rc == 0) {
        ln->has_pending = true;
        return true;
    }
    if (rc == -ERR_STREAM_CLOSED) {
        ln->closed = true;
        EarDrop(ln);
    }
    return false;
}

/* Pending frames across ALL lanes in global push order — always the frame
 * carrying the smallest order first. A frame pushed after another one took a
 * larger number than it (g_console_order, above), so cause order is not a
 * question of when the daemon happens to look; a lane is never allowed to
 * overtake an older frame parked on another lane. The merge is bounded per
 * call so a firehose writer cannot hold the rotation away from keys and IPC
 * — and fairness needs no per-lane quota at all: an old frame outranks a
 * flooder's ever-newer ones by age alone. Keys are sipped every 32 frames so
 * echo stays live inside a long render, after what came before them is
 * drawn.
 *
 * No frame numbered past `ceiling` is met: a question waiting for its place
 * in the console's order holds everything after that place (display.cpp). */
bool lanes_render(uint32_t budget, uint64_t ceiling, FrameSink sink, FrameFlush flush)
{
    bool     did      = false;
    uint32_t rendered = 0;

    while (rendered < budget) {
        ConsoleLane *best = NULL;
        for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
            if (lane_refill(ln) &&
                (!best ||
                 (int64_t)(ln->pending.order - best->pending.order) < 0)) {
                best = ln;
            }
        }
        if (!best || best->pending.order > ceiling) break;

        sink(best->pending);
        best->has_pending = false;
        did = true;
        if ((++rendered & 31u) == 0) {
            flush();
            LanesKeys();
        }
    }
    flush();
    return did;
}

void lane_free(ConsoleLane *ln)
{
    EarDrop(ln);
    /* A release that failed left the reader claim standing in the kernel,
     * and a number whose Brook is still claimed must not be granted again:
     * every open of "console:N" as READER would find it busy, and the pool
     * is a stack — the number would sit at its top and refuse every lane
     * from then on. Such a number is retired, and said so. */
    int rc = brook_release(ln->brook);
    if (rc != 0) {
        kdbg_print("[display] lane %u of pid %u: release failed (%d); number retired",
                   ln->number, ln->owner_pid, rc);
        free(ln);
        return;
    }

    FreeNumber *fn = (FreeNumber *)malloc(sizeof(FreeNumber));
    if (fn) {
        fn->number     = ln->number;
        fn->next       = g_free_numbers;
        g_free_numbers = fn;
    }
    /* malloc failure just retires the number — "console:N" stays interned
     * (tag registry entries are forever) and a fresh number replaces it. */
    free(ln);
}

/* The console's order, made on demand rather than only at boot: a Bay that
 * could not be created in the one instant at boot would otherwise leave the
 * daemon unable to grant ANY lane for the rest of its life. */
bool order_ensure(void)
{
    if (g_console_order) return true;
    g_console_order = (uint64_t *)bay_open(CONSOLE_ORDER_TAG, CONSOLE_ORDER_BYTES,
                                           BAY_CREATE);
    if (!g_console_order) {
        kdbg_print("[display] the console's order could not be made; no lane "
                   "can be granted just now and every cabin that asks says its "
                   "text into the log ring");
        return false;
    }
    return true;
}

/* [DISP_CMD_LANE][u32 this display's pid][tag NUL]: the lane, and where to
 * say what does not ride it. Anything but OK means the asker never learned of
 * the lane — it died first (ERR_PROCESS_NOT_FOUND), or the kernel could not
 * hand the answer over — and the asker holds no lane by this answer. */
int lane_answer(const Result &asked, const char *tag)
{
    uint8_t  reply[1 + sizeof(uint32_t) + 32];
    uint32_t me   = cabin_info()->pid;
    size_t   tlen = strlen(tag);
    reply[0] = DISP_CMD_LANE;
    memcpy(reply + 1, &me, sizeof(me));
    memcpy(reply + 1 + sizeof(me), tag, tlen + 1);
    return answer(&asked, reply, (uint32_t)(1 + sizeof(me) + tlen + 1));
}

}  // namespace

bool LanesStart() noexcept
{
    g_kb = touch_pair_choose(touch_intern(TOUCH_TAG_KEYBOARD));
    if (g_kb == TOUCH_TAG_INVALID || touch_claim(g_kb, TOUCH_REST, 0, 0) != 0) return false;
    /* Before the first grant can be asked for: the console's order exists,
     * or this daemon grants no lanes. BAY_CREATE of a page the kernel zeroes —
     * the first number handed out is one, and zero is therefore a number no
     * frame can carry. */
    (void)order_ensure();
    return true;
}

/* Consume keys ONLY while someone listens — otherwise they stay banked on
 * the TouchRing/stash as type-ahead for whoever listens next. Each key is
 * the daemon's own Touch on the lane's tag, payload as heard.
 *
 * The kernel says how many heard it. Nobody means the lane on top has no
 * reader any more — it died in the middle of its reading, or gave its claim
 * up without giving the ear back — and its ear is dropped right here, before
 * the daemon has heard of the death by any other road. The key is not lost
 * with it: it stays in hand and is said again to whoever is beneath, or, when
 * nobody is, to the next lane that listens — ahead of everything typed after
 * it, which is still banked in the ring. One key in hand at most. */
bool LanesKeys() noexcept
{
    static Touch key;           /* taken from the keyboard, not yet heard by anyone */
    static bool  key_in_hand;

    bool did = false;
    while (g_ear) {
        if (!key_in_hand) {
            if (!touch_try_pop_tag(g_kb, &key)) break;
            did = true;
            if (key.payload_len < sizeof(kb_event_t)) continue;
            key_in_hand = true;
        }
        int heard = touch_send(g_ear->lane->ear, key.payload, key.payload_len, 0);
        if (heard > 0) {
            key_in_hand = false;
            continue;
        }
        /* Nobody wears the tag of the lane on top: its reader is gone. Or the
         * kernel would not say anything on that tag at all — and that is no
         * passing state to wait out: a full submission ring is waited for
         * inside the call, so what comes back as a refusal is the tag's own
         * (who may speak on it), and the next turn would get the same. Either
         * way that lane hears nothing more. The lane itself closes by its own
         * road (STREAM_CLOSED, process:died); only the ear goes now, so the
         * key finds the next listener. */
        if (heard < 0) {
            static bool said = false;
            if (!said) {
                said = true;
                kdbg_print("[display] a key could not be said on lane %u (%d); "
                           "its ear is dropped and the key goes to the next "
                           "listener", g_ear->lane->number, heard);
            }
        }
        EarDrop(g_ear->lane);
        did = true;
    }
    return did;
}

/* A death names (pid, generation), and so does a lane: the match is exact.
 * Never unlinks lane nodes (the lane walk owns the list) — a never-attached
 * grant is marked closed; attached lanes need nothing here, because the merge
 * already renders a dead writer's banked tail before anything a survivor
 * pushes in reaction to the death. */
void LanesDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept
{
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
        if (ln->owner_pid != Pid || ln->owner_gen != Generation || ln->closed)
            continue;
        if (!brook_writer_ever_attached(ln->brook)) {
            ln->closed = true;
            EarDrop(ln);
        }
    }
}

bool LanesStep(FrameSink Sink, FrameFlush Flush, std::uint64_t Ceiling) noexcept
{
    bool did = lanes_render(2u * CONSOLE_LANE_FRAMES, Ceiling, Sink, Flush);

    /* Unlink lanes that are fully over: STREAM_CLOSED surfaced (or the
     * grant was revoked) AND the last pending frame has rendered. Freeing
     * one calls into the kernel, so it counts as moving. */
    ConsoleLane **pp = &g_lanes;
    while (*pp) {
        ConsoleLane *ln = *pp;
        if (ln->closed && !ln->has_pending) {
            *pp = ln->next;
            lane_free(ln);
            did = true;
            continue;
        }
        pp = &ln->next;
    }
    return did;
}

/* A strand numbers its own frames in the order it pushes them, so the head of
 * its lane is the least number it still has there. */
bool LanesDrawnTo(std::uint32_t Pid, std::uint32_t Generation, std::uint64_t Mark) noexcept
{
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
        if (ln->owner_pid != Pid || ln->owner_gen != Generation) continue;
        if (lane_refill(ln) && ln->pending.order <= Mark) return false;
    }
    return true;
}

void LanesGrant(const Result &Asked, std::uint32_t Generation) noexcept
{
    const std::uint32_t Requester = Asked.sender_pid;
    const uint8_t       refusal   = DISP_CMD_LANE;   /* the answer alone = an honest no */

    /* No order, no lane. A granted lane whose frames cannot be numbered would
     * force its writer to stamp with something else, and two domains of
     * numbers in one merge order NOTHING. The requester takes the honest
     * refusal and says its text into the log ring instead. */
    if (!order_ensure()) {
        (void)answer(&Asked, &refusal, sizeof(refusal));
        return;
    }

    /* Idempotent per requester: a strand that asks again in the same life
     * gets the SAME lane back, never a second one. A recycled pid carries a
     * new generation — the kernel's word, in the question — and matches no
     * lane of its predecessor. */
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
        if (ln->owner_pid != Requester || ln->owner_gen != Generation || ln->closed)
            continue;
        char again[32];
        memcpy(again, "console:", 8);
        uint64_to_str(ln->number, again + 8, sizeof(again) - 8);
        (void)lane_answer(Asked, again);
        return;
    }

    uint32_t number;
    if (g_free_numbers) {
        FreeNumber *fn = g_free_numbers;
        g_free_numbers = fn->next;
        number = fn->number;
        free(fn);
    } else {
        number = g_next_number++;
    }

    char tag[32];
    memcpy(tag, "console:", 8);
    uint64_to_str(number, tag + 8, sizeof(tag) - 8);

    Brook *b = brook_open(tag, sizeof(ConsoleRun), CONSOLE_LANE_FRAMES,
                          BROOK_READER | BROOK_CREATE);
    ConsoleLane *ln = b ? (ConsoleLane *)malloc(sizeof(ConsoleLane)) : NULL;
    if (!b || !ln) {
        if (b) brook_release(b);
        /* The number is NOT returned to the pool: an open that failed says
         * the Brook behind it is not usable, and a pool that is a stack would
         * hand the same failure to every requester after this one. */
        (void)answer(&Asked, &refusal, sizeof(refusal));
        kdbg_print("[display] lane grant failed for pid %u (number %u retired)",
                   Requester, number);
        return;
    }

    ln->brook       = b;
    ln->owner_pid   = Requester;
    ln->owner_gen   = Generation;
    ln->number      = number;
    ln->ear         = touch_intern(tag);
    ln->closed      = false;
    ln->has_pending = false;
    ln->next        = NULL;
    if (g_lanes) {
        ConsoleLane *last = g_lanes;
        while (last->next) last = last->next;
        last->next = ln;
    } else {
        g_lanes = ln;
    }

    /* A lane nobody learned of is nobody's: closed now, freed by the next
     * step, its number back in the pool. */
    if (lane_answer(Asked, tag) != 0) ln->closed = true;
}

void LanesListen(std::uint32_t Requester, std::uint32_t Generation, bool Listening) noexcept
{
    /* Listening is a property of a lane, and a closed one cannot hear. */
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
        if (ln->owner_pid == Requester && ln->owner_gen == Generation && !ln->closed) {
            if (Listening) EarListen(ln);
            else           EarDrop(ln);
            return;
        }
    }
}

/* Only lanes that still carry frames matter, but hanging on all of them is
 * one store each and spares the walk a liveness argument. New lanes cannot
 * appear behind the daemon's back — LanesGrant runs only while it is awake. */
void LanesBellsHang(std::uint32_t Me) noexcept
{
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next)
        brook_bell_hang(ln->brook, Me);
}

void LanesBellsTake() noexcept
{
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next)
        brook_bell_take(ln->brook);
}

void LanesSaid(std::uint64_t Order) noexcept
{
    // Only ever forward: a late frame numbered below one already said must not
    // take back what a leaving writer was told (box/display.h).
    std::uint64_t *Said = &g_console_order[CONSOLE_ORDER_SAID];
    if (Order > __atomic_load_n(Said, __ATOMIC_RELAXED))
        __atomic_store_n(Said, Order, __ATOMIC_RELEASE);
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next)
        brook_ring_peer(ln->brook);
}
