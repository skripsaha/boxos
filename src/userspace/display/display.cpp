/*
 * display.cpp — the BoxOS DisplayServer.
 *
 * This is one process and one event loop. "Server" names its ownership of
 * console transport, keyboard routing, IPC, and process lifetime; "compositor"
 * names the retained-layer composition performed inside that same process.
 * There is no second display daemon or second renderer behind this file.
 *
 * One event loop, no timeouts: a rotation of try-steps (keys → deaths → lanes
 * → IPC). A pass that moves nothing does not go round again — the daemon turns
 * in and stops costing a core until something arrives for it. Its three
 * delivery surfaces are not alike: the kernel can see a Touch and a Result
 * land, so the sleep watches those cursors itself, but a lane frame is a store
 * into a shared page that the kernel never witnesses. That is what the bell is
 * for: before going down, the daemon hangs its pid on every lane, and a writer
 * that finds one rings. Hang the bells BEFORE the last look, or a frame that
 * arrives in between is seen by nobody.
 *
 * Output arrives as ConsoleRun frames on per-strand Brook lanes. A lane is
 * granted over DISP_CMD_LANE checkroom-style: the daemon opens the READER
 * side of a fresh "console:N" Brook FIRST and only then answers with the
 * tag, so the writer arrives at a laid table. Lane numbers are reused from
 * a free pool — the tag registry grows to the peak number of simultaneous
 * lanes and no further.
 *
 * Input: the daemon is the cabin's ear at the keyboard. It consumes
 * "keyboard" Touch events only while some lane LISTENS, and says each key
 * again as a Touch on the tag of the lane at the top of the listening stack
 * — the program edits its own line and echoes through its own lane. A lane
 * listens for exactly as long as its program reads (DISP_CMD_LISTEN with 1
 * pushes it to the top, with 0 gives the ear back); a lane that closes is
 * dropped wherever it stands, and the one beneath hears again — and so is a
 * lane whose reader nobody can find: the kernel says how many heard each
 * key, and a key nobody heard drops the ear on top and is said again to the
 * next. Keys typed while nobody listens are NOT consumed — the kernel
 * TouchRing and the per-strand tag stash bank them for whoever reads next,
 * so type-ahead needs no third buffer.
 *
 * process:died closes what the dead leave behind: granted-but-never-
 * attached lanes are revoked, attached lanes are drained to the last frame
 * (a dying process' final words reach the screen) and closed on
 * STREAM_CLOSED.
 */

#include "box/print.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/ipc.h"
#include "box/touch.h"
#include "box/brook.h"
#include "box/bay.h"
#include "box/system.h"
#include "box/memory.h"
#include "box/convert.h"
#include "box/string.h"
#include "box/debug.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/turnin.h"
#include "box/display.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "box/cxx/bay.h"
#include "box/cxx/error.h"
#include "box/display_layer_tag.h"
#include "console_model.h"

/* ─────────────────────────────────────────────────────────────────────────
 * Lanes
 * ───────────────────────────────────────────────────────────────────────── */

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
 * order (below, in grant_lane), and a writer that cannot map the order gives
 * its lane back and prints straight to the glass (boxlib print.c,
 * lane_ensure). A cabin printing straight to the glass is the only writer
 * there is, and one writer needs no order.
 *
 * The rejected alternative is written down so it is not invented again:
 * "drain a dead writer's lane first". It sounds causal and it is too late —
 * the daemon learns of a death on its next rotation, and the overtaking
 * happens inside a single render before that. It was written, measured
 * against the oracle, and taken out again. */
static uint64_t *g_order;            /* the console's one order, held for life */

static ConsoleLane *g_lanes;         /* append at tail — grant order */
static FreeNumber  *g_free_numbers;  /* reuse pool for lane numbers */
static uint32_t     g_next_number;   /* fresh numbers when the pool is dry */

struct DisplayLayerState {
    std::uint32_t Id = 0;
    std::uint32_t OwnerPid = 0;
    std::uint32_t OwnerGeneration = 0;
    DisplayLayerRole Role = DISPLAY_LAYER_OVERLAY;
    std::int32_t X = 0;
    std::int32_t Y = 0;
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::size_t CellCount = 0;
    std::uint64_t Nonce = 0;
    box::bay<std::byte> Shared;
    DisplayLayerSharedHeader *Header = nullptr;
    std::vector<DisplayLayerCell> Cells;
    std::uint64_t Sequence = 0;
    std::uint32_t CursorX = 0;
    std::uint32_t CursorY = 0;
    bool CursorVisible = false;
    bool HasFrame = false;
};

static ConsoleModel *g_console_model;
static std::vector<TextCell> g_composed_cells;
static std::vector<DisplayLayerState> g_layers;
static std::uint32_t g_next_layer_id = 1;
static bool g_present_dirty = false;

static void render_took(int rc);
static bool LayersDropProcess(std::uint32_t Pid, std::uint32_t Generation);

static std::size_t LayerCellCount(const DisplayLayerState &Layer)
{
    return Layer.CellCount;
}

static DisplayLayerCell *LayerSlot(DisplayLayerState &Layer, std::uint32_t Slot)
{
    auto *Base = reinterpret_cast<DisplayLayerCell *>(
        reinterpret_cast<std::byte *>(Layer.Header) + sizeof(DisplayLayerSharedHeader));
    return Base + static_cast<std::size_t>(Slot) * LayerCellCount(Layer);
}

static bool LayerConsume(DisplayLayerState &Layer)
{
    bool Changed = false;
    std::uint64_t NewestSequence = Layer.Sequence;
    for (std::uint32_t Slot = 0; Slot < DISPLAY_LAYER_SLOTS; Slot++) {
        std::uint32_t Expected = DISPLAY_LAYER_READY;
        if (!__atomic_compare_exchange_n(&Layer.Header->slot_state[Slot], &Expected,
                                          DISPLAY_LAYER_READING, false,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            continue;

        const std::uint64_t Sequence = Layer.Header->sequence[Slot];
        if (!Layer.HasFrame || Sequence > NewestSequence) {
            std::memcpy(Layer.Cells.data(), LayerSlot(Layer, Slot),
                        LayerCellCount(Layer) * sizeof(DisplayLayerCell));
            NewestSequence = Sequence;
            Layer.CursorX = Layer.Header->cursor_x[Slot];
            Layer.CursorY = Layer.Header->cursor_y[Slot];
            Layer.CursorVisible = Layer.Header->cursor_visible[Slot] != 0;
            Layer.HasFrame = true;
            Changed = true;
        }
        __atomic_store_n(&Layer.Header->slot_state[Slot], DISPLAY_LAYER_FREE, __ATOMIC_RELEASE);
    }
    Layer.Sequence = NewestSequence;
    if (Changed) g_present_dirty = true;
    return Changed;
}

static bool LayerAbove(const DisplayLayerState &Left, const DisplayLayerState &Right)
{
    if (Left.Role != Right.Role) return Left.Role < Right.Role;
    return Left.Id < Right.Id;
}

static bool PresentDisplay()
{
    if (!g_present_dirty || !g_console_model) return false;
    g_present_dirty = false;

    const std::vector<TextCell> &BaseCells = g_console_model->Cells();
    if (g_composed_cells.size() != BaseCells.size()) {
        g_present_dirty = true;
        return false;
    }
    std::copy(BaseCells.begin(), BaseCells.end(), g_composed_cells.begin());
    std::vector<std::size_t> Order;
    try {
        Order.reserve(g_layers.size());
        for (std::size_t Index = 0; Index < g_layers.size(); Index++) {
            if (g_layers[Index].HasFrame) Order.push_back(Index);
        }
        std::sort(Order.begin(), Order.end(), [](std::size_t A, std::size_t B) {
            return LayerAbove(g_layers[A], g_layers[B]);
        });
    } catch (...) {
        g_present_dirty = true;
        return false;
    }

    std::uint32_t CursorX = g_console_model->CursorX();
    std::uint32_t CursorY = g_console_model->CursorY();
    for (std::size_t Ordered : Order) {
        DisplayLayerState &Layer = g_layers[Ordered];
        for (std::uint32_t Row = 0; Row < Layer.Height; Row++) {
            for (std::uint32_t Col = 0; Col < Layer.Width; Col++) {
                const DisplayLayerCell &Cell =
                    Layer.Cells[static_cast<std::size_t>(Row) * Layer.Width + Col];
                if (!Cell.Opaque) continue;
                const std::int32_t ScreenX = Layer.X + static_cast<std::int32_t>(Col);
                const std::int32_t ScreenY = Layer.Y + static_cast<std::int32_t>(Row);
                if (ScreenX < 0 || ScreenY < 0 ||
                    ScreenX >= static_cast<std::int32_t>(g_console_model->Width()) ||
                    ScreenY >= static_cast<std::int32_t>(g_console_model->Height()))
                    continue;
                g_composed_cells[static_cast<std::size_t>(ScreenY) * g_console_model->Width() + ScreenX] =
                    Cell.Cell;
            }
        }
        if (Layer.CursorVisible) {
            CursorX = std::min<std::uint32_t>(Layer.CursorX,
                                              g_console_model->Width() - 1u);
            CursorY = std::min<std::uint32_t>(Layer.CursorY,
                                              g_console_model->Height() - 1u);
        }
    }

    const int PaintCode = vga_paint(0, 0,
                                    static_cast<std::uint8_t>(g_console_model->Height()),
                                    static_cast<std::uint8_t>(g_console_model->Width()),
                                    g_composed_cells.data());
    if (PaintCode != 0) {
        g_present_dirty = true;
        render_took(PaintCode);
        return false;
    }
    (void)vga_setcursor(static_cast<std::uint8_t>(CursorY), static_cast<std::uint8_t>(CursorX));
    return true;
}

static bool LayersStep()
{
    bool Changed = false;
    for (DisplayLayerState &Layer : g_layers) Changed |= LayerConsume(Layer);
    return Changed;
}

/* Touch tags, interned once at startup. */
static TouchTag g_kb    = TOUCH_TAG_INVALID;
static TouchTag g_pdied = TOUCH_TAG_INVALID;

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

static Ear *g_ear;   /* top of the stack, NULL when nobody listens */

static void EarListen(ConsoleLane *lane)
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

static void EarDrop(ConsoleLane *lane)
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

static bool kb_step(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Frame rendering
 * ───────────────────────────────────────────────────────────────────────── */

static void render_run(const ConsoleRun *f)
{
    if (!g_console_model) return;
    g_console_model->Apply(*f);
    g_present_dirty = true;
}

/* ‼ A RENDER THAT WAS REFUSED IS NEWS, AND IT USED TO BE SWALLOWED.
 *
 * vga_commit fires one Manifest carrying a whole burst of output and returns
 * the kernel's answer. Nothing here read it. So a batch the kernel would not
 * finish — an op it refused, a snapshot it had no memory for — went by in
 * silence, and the screen simply had less on it than the machine had said.
 * The ops behind the refusal used to be dropped along with it; they are marked
 * OPTIONAL now (boxlib vga.c) so a refused cell costs its own cell. What is
 * left is the fact of the refusal, and a console that quietly draws less than
 * it was told to is exactly the kind of thing nobody finds by looking.
 *
 * Said once, with the reason and with how many frames were in flight when it
 * happened: a screen that is behind does not need a line per cell, and the
 * first one carries everything the next reader needs. */
static void render_took(int rc)
{
    static bool said = false;
    if (rc == 0 || said) return;
    said = true;
    kdbg_print("[display] the console refused a render (%d) — what it carried "
               "did not reach the glass", rc);
}

/* Refill a lane's pending slot from its ring. Marks the lane closed when
 * the drained writer's STREAM_CLOSED surfaces. Returns whether a pending
 * frame is available for the merge. */
static bool lane_refill(ConsoleLane *ln)
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

/* Render pending frames across ALL lanes in global push order — always the
 * frame carrying the smallest order first. A frame pushed after another one
 * took a larger number than it (g_order, above), so cause order is not a
 * question of when the daemon happens to look; a lane is never allowed to
 * overtake an older frame parked on another lane. The
 * merge is bounded per call so a firehose writer cannot hold the rotation
 * away from keys and IPC — and fairness needs no per-lane quota at all:
 * an old frame outranks a flooder's ever-newer ones by age alone. Keys
 * are sipped every 32 frames so echo stays live inside a long render. */
static bool lanes_render(uint32_t budget)
{
    bool     did      = false;
    uint32_t rendered = 0;

    vga_begin();
    while (rendered < budget) {
        ConsoleLane *best = NULL;
        for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
            /* Ordered by the number every writer shares (boxlib print.c:
             * console_order_next), so a frame pushed after another cannot be
             * rendered before it — whatever the two cores' clocks say. */
            if (lane_refill(ln) &&
                (!best ||
                 (int64_t)(ln->pending.order - best->pending.order) < 0)) {
                best = ln;
            }
        }
        if (!best) break;

        render_run(&best->pending);
        best->has_pending = false;
        did = true;
        if ((++rendered & 31u) == 0) {
            render_took(vga_commit());
            kb_step();
            vga_begin();
        }
    }
    render_took(vga_commit());
    if (did) (void)PresentDisplay();
    return did;
}

static void lane_free(ConsoleLane *ln)
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

/* ─────────────────────────────────────────────────────────────────────────
 * Deaths
 * ───────────────────────────────────────────────────────────────────────── */

/* Consume banked process:died events. Never unlinks lane nodes (the lane
 * walk owns the list) — it marks never-attached grants closed; attached
 * lanes need nothing here, because the merge already renders a dead writer's
 * banked tail before anything a survivor pushes in reaction to the death:
 * every frame of that tail took its number before the death happened, and
 * the survivor's reaction takes its number after.
 *
 * A death names (pid, generation), and so does a lane: the match is exact.
 * A recycled pid is a different generation and matches nothing here, so the
 * CURRENT incarnation keeps the lane it was just granted, and no guess about
 * liveness (proc_info) stands between an event and its lane any more. */
static bool death_step(void)
{
    bool  did = false;
    Touch t;

    while (touch_try_pop_tag(g_pdied, &t)) {
        did = true;
        if (t.payload_len < sizeof(TouchProcessDied)) continue;

        TouchProcessDied d;
        memcpy(&d, t.payload, sizeof(d));
        did |= LayersDropProcess(d.pid, d.generation);

        for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
            if (ln->owner_pid != d.pid || ln->owner_gen != d.generation ||
                ln->closed)
                continue;
            if (!brook_writer_ever_attached(ln->brook)) {
                ln->closed = true;
                EarDrop(ln);
            }
        }

    }
    return did;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Lane walk — the only place lane nodes are unlinked.
 * ───────────────────────────────────────────────────────────────────────── */

/* Hang this daemon's pid on every open lane, and take them all in again.
 *
 * Only lanes that still carry frames matter, but hanging on all of them is one
 * store each and spares the walk a liveness argument: a lane the daemon is
 * about to free has its bell taken back before lanes_step ever reaches it,
 * because taking them in happens the instant the sleep ends. A closed lane's
 * writer is gone and will never ring; the store is harmless.
 *
 * New lanes cannot appear behind the daemon's back — grant_lane runs inside
 * ipc_step, which only runs while it is awake. */
static void lanes_bell_hang(uint32_t me)
{
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next)
        brook_bell_hang(ln->brook, me);
}

static void lanes_bell_take(void)
{
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next)
        brook_bell_take(ln->brook);
}

static bool lanes_step(void)
{
    bool did = lanes_render(2u * CONSOLE_LANE_FRAMES);

    /* Unlink lanes that are fully over: STREAM_CLOSED surfaced (or the
     * grant was revoked) AND the last pending frame has rendered. */
    ConsoleLane **pp = &g_lanes;
    while (*pp) {
        ConsoleLane *ln = *pp;
        if (ln->closed && !ln->has_pending) {
            *pp = ln->next;
            lane_free(ln);
            continue;
        }
        pp = &ln->next;
    }
    return did;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Keys — said again on the lane that listens
 * ───────────────────────────────────────────────────────────────────────── */

/* Consume keys ONLY while someone listens — otherwise they stay banked on
 * the TouchRing/stash as type-ahead for whoever listens next. (The stash
 * matters: death_step's tag-selective pop parks any keys it runs into
 * there, so this must always pull by tag, never gate on ring emptiness.)
 * Each key is the daemon's own Touch on the lane's tag, payload as heard.
 *
 * The kernel says how many heard it. Nobody means the lane on top has no
 * reader any more — it died in the middle of its reading, or gave its claim
 * up without giving the ear back — and its ear is dropped right here, before
 * the daemon has heard of the death by any other road. The key is not lost
 * with it: it stays in hand and is said again to whoever is beneath, or, when
 * nobody is, to the next lane that listens — ahead of everything typed after
 * it, which is still banked in the ring. One key in hand at most: the next
 * is not taken until this one has been heard. */
static Touch g_key;           /* taken from the keyboard, not yet heard by anyone */
static bool  g_key_in_hand;

static bool kb_step(void)
{
    bool did = false;
    while (g_ear) {
        if (!g_key_in_hand) {
            if (!touch_try_pop_tag(g_kb, &g_key)) break;
            did = true;
            if (g_key.payload_len < sizeof(kb_event_t)) continue;
            g_key_in_hand = true;
        }
        int heard = touch_send(g_ear->lane->ear, g_key.payload, g_key.payload_len, 0);
        if (heard > 0) {
            g_key_in_hand = false;
            continue;
        }
        /* Nobody wears the tag of the lane on top: its reader is gone. The
         * lane itself closes by its own road (STREAM_CLOSED, process:died);
         * only the ear goes now, so the key finds the next listener. */
        EarDrop(g_ear->lane);
        did = true;
    }
    return did;
}

/* ─────────────────────────────────────────────────────────────────────────
 * IPC — grants, input requests, ping.
 * ───────────────────────────────────────────────────────────────────────── */

/* Say something to one cabin — ONCE, and never waiting for it.
 *
 * ‼ THIS WAITED FOR ROOM, AND THAT IS A DEADLOCK IN THIS PARTICULAR CABIN.
 * `send` used to be refused for a reason that said nothing about whether the
 * requester existed — its reply ring was momentarily full — and the writer's
 * side of this (boxlib print.c, daemon_say) waits such a refusal out. So the
 * same shape was put here, and it was wrong here for a reason that has nothing
 * to do with `send`: THE DAEMON IS ITS OWN ROTATION. While it stands in a
 * retry loop it is not stepping keys, not draining lanes, not answering IPC —
 * the whole console stops, and the machine goes silent with nothing in the log
 * to say why. A writer that stalls stalls one cabin; the daemon that stalls
 * stalls every cabin there is.
 *
 * MEASURED, and it is why this note survives the fix: with the wait in place
 * the six-config matrix failed twice, in two different configurations both
 * times (a prompt that never came back after a program ended, and a parked
 * subscriber that stopped being drained fast enough for its publisher) — while
 * the same matrix on the tree without it passed all six.
 *
 * ‼ THE DEBT NAMED HERE IS PAID, AND NOT THE WAY THIS NOTE GUESSED. It asked
 * for a pending reply the rotation carries and retries, and said the missing
 * piece was a bell on somebody else's ring. There is no bell and none was
 * needed: the KERNEL now takes an answer its target's ring cannot hold and
 * keeps it as a debt, which the target settles itself at its own next syscall
 * gate (kring.c, Owed). A full ring is therefore no longer a refusal at all —
 * ERR_ROUTE_TARGET_FULL is unreachable for a live target — and nothing is left
 * for this daemon to carry, retry or be woken for. One call, one answer, and
 * the rotation never stops.
 *
 * What can still come back is ERR_NO_MEMORY: no room to copy the payload into
 * the requester's cabin, or no room for the kernel to hold the promise. That
 * is a machine out of memory, not a busy consumer, and there is nothing to
 * wait for — the requester's own re-ask finds the same lane, because grant_lane
 * is idempotent per (pid, generation). */
static int say_to(uint32_t pid, const uint8_t *msg, uint16_t len)
{
    int rc = send(pid, msg, len);
    if (rc == 0) return 0;
    if (box_errno_of(rc) == ERR_NO_MEMORY) {
        /* Said once per daemon: until this was written down nobody knew it
         * happened at all, and a writer whose grant went missing waits for it
         * without a deadline, which is correct and therefore silent. */
        static bool refused_before = false;
        if (!refused_before) {
            refused_before = true;
            kdbg_print("[display] pid %u could not be given its lane answer "
                       "just now (no room for the record); it was not re-sent, "
                       "and that cabin waits until it asks again", pid);
        }
    }
    return rc;
}

/* The console's order, made on demand rather than only at boot.
 *
 * It used to be made exactly once, in main, and a Bay that could not be
 * created in that one instant left the daemon unable to grant ANY lane for
 * the rest of its life — the whole machine on the direct road, permanently,
 * because of one transient shortage at boot. Making it here costs one
 * Manifest call on a path that is already a round trip, only ever runs while
 * there is no order, and turns a permanent verdict into a momentary one. */
static bool order_ensure(void)
{
    if (g_order) return true;
    g_order = (uint64_t *)bay_open(CONSOLE_ORDER_TAG, CONSOLE_ORDER_BYTES,
                                   BAY_CREATE);
    if (!g_order) {
        kdbg_print("[display] the console's order could not be made; no lane "
                   "can be granted just now and every cabin that asks prints "
                   "straight to the glass");
        return false;
    }
    return true;
}

static void grant_lane(uint32_t requester, uint32_t generation)
{
    /* No order, no lane. A granted lane whose frames cannot be numbered would
     * force its writer to stamp with something else, and two domains of
     * numbers in one merge order NOTHING. The requester takes the honest
     * one-byte refusal and prints straight to the glass instead. */
    if (!order_ensure()) {
        uint8_t refusal = DISP_CMD_LANE;
        (void)say_to(requester, &refusal, 1);
        return;
    }

    /* Idempotent per requester. A strand IS a process here, so one lane per
     * (pid, generation) is the whole rule — and a writer whose grant reply
     * was lost (or who simply asked again while the answer was in flight)
     * must get the SAME lane back, never a second one. Granting twice would
     * strand the first Brook: the daemon would hold a reader nobody writes
     * to and the writer would push into whichever tag it learned last,
     * leaking a lane per re-ask. A recycled pid carries a new generation and
     * matches no lane of its predecessor. */
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
        if (ln->owner_pid != requester || ln->owner_gen != generation ||
            ln->closed)
            continue;
        char     again[32];
        uint8_t  reply[2 + sizeof(again)];
        memcpy(again, "console:", 8);
        uint64_to_str(ln->number, again + 8, sizeof(again) - 8);
        size_t alen = strlen(again);
        reply[0] = DISP_CMD_LANE;
        memcpy(reply + 1, again, alen + 1);
        (void)say_to(requester, reply, (uint16_t)(2 + alen));
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
        /* The number is NOT returned to the pool. An open that failed says
         * the Brook behind this number is not usable, and a pool that is a
         * stack would hand the same failure to every requester after this
         * one. A fresh number replaces it; "console:N" stays interned. */
        uint8_t refusal = DISP_CMD_LANE;      /* 1-byte reply = honest no */
        (void)say_to(requester, &refusal, 1);
        kdbg_print("[display] lane grant failed for pid %u (number %u retired)",
                   requester, number);
        return;
    }

    ln->brook     = b;
    ln->owner_pid = requester;
    ln->owner_gen = generation;
    ln->number    = number;
    ln->ear       = touch_intern(tag);
    ln->closed    = false;
    ln->next      = NULL;
    if (g_lanes) {
        ConsoleLane *last = g_lanes;
        while (last->next) last = last->next;
        last->next = ln;
    } else {
        g_lanes = ln;
    }

    uint8_t reply[2 + sizeof(tag)];
    size_t  tlen = strlen(tag);
    reply[0] = DISP_CMD_LANE;
    memcpy(reply + 1, tag, tlen + 1);
    (void)say_to(requester, reply, (uint16_t)(2 + tlen));
}


static void LayerReply(std::uint32_t Requester, error_t Error,
                       std::uint32_t LayerId, std::uint64_t Nonce)
{
    DisplayLayerOpenReply Reply{};
    Reply.command = DISP_CMD_LAYER_OPEN;
    Reply.error = static_cast<std::int32_t>(Error);
    Reply.layer_id = LayerId;
    Reply.display_pid = cabin_info()->pid;
    Reply.nonce = Nonce;
    const int Code = send(Requester, &Reply, sizeof(Reply));
    if (Code != 0) {
        kdbg_print("[display] layer reply to pid %u failed (%d)\n",
                   Requester, Code);
    }
}

static void LayerOpen(std::uint32_t Requester, const DisplayLayerOpenRequest &Request)
{
    if (!g_console_model || Request.width == 0 || Request.height == 0 ||
        Request.width > g_console_model->Width() || Request.height > g_console_model->Height() ||
        Request.x < 0 || Request.y < 0 ||
        static_cast<std::uint32_t>(Request.x) + Request.width > g_console_model->Width() ||
        static_cast<std::uint32_t>(Request.y) + Request.height > g_console_model->Height()) {
        LayerReply(Requester, ERR_OUT_OF_RANGE, 0, Request.nonce);
        return;
    }
    if (Request.role < DISPLAY_LAYER_SCENE || Request.role > DISPLAY_LAYER_NOTICE ||
        Request.flags != 0) {
        LayerReply(Requester, ERR_INVALID_ARGUMENT, 0, Request.nonce);
        return;
    }
    const std::size_t CellCount = static_cast<std::size_t>(Request.width) * Request.height;
    const std::size_t ExpectedBytes = sizeof(DisplayLayerSharedHeader) +
                                      DISPLAY_LAYER_SLOTS * CellCount * sizeof(DisplayLayerCell);
    std::expected<std::string, box::error> Tag =
        box::display::LayerBayTag(Requester, Request.generation, Request.nonce);
    if (!Tag) {
        LayerReply(Requester, Tag.error().raw(), 0, Request.nonce);
        return;
    }
    for (const DisplayLayerState &Existing : g_layers) {
        if (Existing.OwnerPid != Requester ||
            Existing.OwnerGeneration != Request.generation ||
            Existing.Nonce != Request.nonce)
            continue;
        if (Existing.Role != static_cast<DisplayLayerRole>(Request.role) ||
            Existing.X != Request.x || Existing.Y != Request.y ||
            Existing.Width != Request.width || Existing.Height != Request.height) {
            LayerReply(Requester, ERR_INVALID_ARGUMENT, 0, Request.nonce);
        } else {
            LayerReply(Requester, OK, Existing.Id, Request.nonce);
        }
        return;
    }
    box::bay<std::byte> Shared = box::bay<std::byte>::open(Tag->c_str());
    if (!Shared || Shared.size_bytes() != ExpectedBytes) {
        LayerReply(Requester, ERR_OBJECT_NOT_FOUND, 0, Request.nonce);
        return;
    }
    auto *Header = reinterpret_cast<DisplayLayerSharedHeader *>(Shared.data());
    if (Header->magic != DISPLAY_LAYER_MAGIC || Header->version != DISPLAY_LAYER_VERSION ||
        Header->width != Request.width || Header->height != Request.height ||
        Header->cell_count != CellCount || Header->slot_count != DISPLAY_LAYER_SLOTS) {
        LayerReply(Requester, ERR_VERSION_MISMATCH, 0, Request.nonce);
        return;
    }

    DisplayLayerState Layer;
    Layer.Id = g_next_layer_id++;
    if (Layer.Id == 0) Layer.Id = g_next_layer_id++;
    Layer.OwnerPid = Requester;
    Layer.OwnerGeneration = Request.generation;
    Layer.Nonce = Request.nonce;
    Layer.Role = static_cast<DisplayLayerRole>(Request.role);
    Layer.X = Request.x;
    Layer.Y = Request.y;
    Layer.Width = Request.width;
    Layer.Height = Request.height;
    Layer.CellCount = CellCount;
    Layer.Shared = std::move(Shared);
    Layer.Header = Header;
    try {
        Layer.Cells.resize(CellCount);
        g_layers.push_back(std::move(Layer));
    } catch (...) {
        LayerReply(Requester, ERR_NO_MEMORY, 0, Request.nonce);
        return;
    }
    g_present_dirty = true;
    LayerReply(Requester, OK, g_layers.back().Id, Request.nonce);
}

static bool LayerClose(std::uint32_t Requester, std::uint32_t Generation,
                       std::uint32_t LayerId)
{
    for (auto It = g_layers.begin(); It != g_layers.end(); ++It) {
        if (It->Id != LayerId) continue;
        if (It->OwnerPid != Requester || It->OwnerGeneration != Generation) return false;
        g_layers.erase(It);
        g_present_dirty = true;
        return true;
    }
    return false;
}

static bool LayerFrame(std::uint32_t Requester, std::uint32_t Generation,
                       std::uint32_t LayerId)
{
    for (DisplayLayerState &Layer : g_layers) {
        if (Layer.Id != LayerId || Layer.OwnerPid != Requester ||
            Layer.OwnerGeneration != Generation) continue;
        return LayerConsume(Layer);
    }
    return false;
}

static bool LayersDropProcess(std::uint32_t Pid, std::uint32_t Generation)
{
    bool Changed = false;
    for (auto It = g_layers.begin(); It != g_layers.end();) {
        if (It->OwnerPid == Pid && It->OwnerGeneration == Generation) {
            It = g_layers.erase(It);
            Changed = true;
        } else {
            ++It;
        }
    }
    if (Changed) g_present_dirty = true;
    return Changed;
}

static bool ipc_step(void)
{
    bool   did = false;
    Result entry;

    while (receive(&entry)) {
        did = true;
        if (entry.data_addr == 0 || entry.data_length == 0) continue;
        if (entry.sender_pid == 0) continue;

        const uint8_t *data = (const uint8_t *)(uintptr_t)entry.data_addr;
        uint16_t       len  = (uint16_t)entry.data_length;

        switch (data[0]) {
        case DISP_CMD_LAYER_OPEN:
            if (len >= sizeof(DisplayLayerOpenRequest)) {
                DisplayLayerOpenRequest Request{};
                memcpy(&Request, data, sizeof(Request));
                LayerOpen(entry.sender_pid, Request);
            } else {
                LayerReply(entry.sender_pid, ERR_INVALID_ARGUMENT, 0, 0);
            }
            break;
        case DISP_CMD_LAYER_FRAME:
            if (len >= sizeof(DisplayLayerFrameNotice)) {
                DisplayLayerFrameNotice Notice{};
                memcpy(&Notice, data, sizeof(Notice));
                (void)LayerFrame(entry.sender_pid, Notice.generation, Notice.layer_id);
            }
            break;
        case DISP_CMD_LAYER_CLOSE:
            if (len >= sizeof(DisplayLayerCloseRequest)) {
                DisplayLayerCloseRequest Request{};
                memcpy(&Request, data, sizeof(Request));
                (void)LayerClose(entry.sender_pid, Request.generation, Request.layer_id);
            }
            break;
        case DISP_CMD_LANE:
            /* [cmd][u32 generation]: a request without its generation names
             * nobody and gets nothing. */
            if (len >= 5) {
                uint32_t gen;
                memcpy(&gen, data + 1, sizeof(gen));
                grant_lane(entry.sender_pid, gen);
            }
            break;
        case DISP_CMD_LISTEN:
            /* [cmd][u32 generation][u8 listening]: the lane of (sender,
             * generation) takes the ear or gives it back. A lane must exist
             * and be open: listening is a property of a lane, and a closed
             * one cannot hear. */
            if (len >= 6) {
                uint32_t gen;
                memcpy(&gen, data + 1, sizeof(gen));
                for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
                    if (ln->owner_pid == entry.sender_pid &&
                        ln->owner_gen == gen && !ln->closed) {
                        if (data[5]) EarListen(ln);
                        else         EarDrop(ln);
                        break;
                    }
                }
            }
            break;
        case DISP_CMD_PING: {
            uint32_t my_pid = cabin_info()->pid;
            send(entry.sender_pid, &my_pid, 4);
            break;
        }
        default:
            /* The render-over-IPC wire is gone; nothing legitimate sends
             * anything else. Say so once rather than swallow it forever. */
            {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    kdbg_print("[display] unknown wire byte 0x%x from pid %u",
                               data[0], entry.sender_pid);
                }
            }
            break;
        }
    }
    return did;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Main — claim tags, announce, rotate.
 * ───────────────────────────────────────────────────────────────────────── */

int main(void)
{
    io_set_mode(IO_MODE_VGA);

    vga_dimensions_t Dimensions{};
    if (vga_getdimensions(&Dimensions) != 0 || Dimensions.cols == 0 || Dimensions.rows == 0)
        return 1;
    try {
        g_console_model = new ConsoleModel(Dimensions.cols, Dimensions.rows);
        g_composed_cells.resize(static_cast<std::size_t>(Dimensions.cols) * Dimensions.rows);
    } catch (...) {
        delete g_console_model;
        g_console_model = NULL;
        return 1;
    }
    g_present_dirty = true;
    (void)PresentDisplay();

    g_kb    = touch_pair_choose(touch_intern(TOUCH_TAG_KEYBOARD));
    g_pdied = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    if (g_kb    != TOUCH_TAG_INVALID) touch_claim(g_kb,    TOUCH_REST, 0, 0);
    if (g_pdied != TOUCH_TAG_INVALID) touch_claim(g_pdied, TOUCH_REST, 0, 0);

    /* Before the first grant can be asked for, and before this cabin says it
     * is ready: the console's order exists, or this daemon grants no lanes.
     * BAY_CREATE of a page the kernel zeroes — the first number handed out is
     * one, and zero is therefore a number no frame can carry. */
    (void)order_ensure();

    CabinInfo *ci = cabin_info();
    if (ci->spawner_pid != 0) {
        uint8_t ready = 0xFF;
        send(ci->spawner_pid, &ready, 1);
    }

    uint32_t me = ci->pid;

    for (;;) {
        bool progressed = false;
        progressed |= kb_step();
        progressed |= death_step();
        progressed |= lanes_step();
        progressed |= LayersStep();
        progressed |= ipc_step();
        if (g_present_dirty) (void)PresentDisplay();
        if (progressed) continue;

        /* Nothing moved. Hang the bells FIRST, then take the mark, then look
         * one last time — in that order, and the order is the whole proof.
         * Anything arriving from here on either finds a bell out (and rings,
         * which is itself an arrival on the Result ring), or moves a cursor
         * past the mark, or turns up in the look below. There is no fourth
         * way for it to arrive and no gap between the three. */
        lanes_bell_hang(me);
        TurnInMark mark = box_mark();

        if (kb_step() | death_step() | lanes_step() | LayersStep() | ipc_step()) {
            if (g_present_dirty) (void)PresentDisplay();
            lanes_bell_take();
            continue;
        }

        box_turn_in(mark);
        lanes_bell_take();
    }

    return 0;
}
