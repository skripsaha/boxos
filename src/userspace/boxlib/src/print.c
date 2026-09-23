#include "box/print.h"
#include "box/color.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/debug.h"
#include "box/brook.h"
#include "box/bay.h"
#include "box/system.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/core/strand_self.h"
#include "box/memory.h"
#include "box/display.h"
#include "box/touch.h"
#include "box/turnin.h"

/* ===========================================================================
 * Per-strand print state — thread-confined, NO lock.
 *
 * Every strand (main + spawned) owns its own console lane, frame under
 * construction, VGA-attr cache and current colours. Concurrent strands
 * calling print/printf never touch each other's state, so the print path
 * itself never takes a lock.
 *
 * The console is a stream: output rides a per-strand Brook lane the display
 * daemon reads ("console:N", granted over DISP_CMD_LANE). The lane opens
 * lazily on the strand's FIRST print — opening costs a daemon round-trip,
 * a brook_open (kmalloc + PMM + VMM in both cabins) and one malloc for the
 * handle, so only strands that actually print ever pay it (the
 * touch_stash_ptr pattern). After that the steady-state path is memcpy into
 * the frame plus a lock-free brook_push — no malloc, no syscall until the
 * daemon side needs waking. That preserves the real safety contract of
 * printf-under-heap_lock ("no malloc on the path"): memory.c's
 * heap_dump_tags caller prints long after its strand's first print.
 *
 * A full lane SLOWS the writer (lane_push waits politely) — printing is
 * never dropped. A dead daemon is an honest refusal: one kdbg trace, then
 * the process falls back to direct VGA (the shell's own no-daemon
 * precedent) so the machine keeps talking.
 *
 * Storage: the main strand uses a static instance (g_main_print_state); a
 * spawned strand's instance lives inline in its StrandInfo (print_state[]).
 * The kernel zero-inits that block, so `initialized` starts false and
 * print_state_self() corrects fg/bg to the process defaults on first touch.
 * Per owner decision, a spawned strand's colours always start FRESH
 * (COLOR_DEFAULT / COLOR_BLACK) — no inheritance from whoever spawned it.
 * =========================================================================== */

enum {
    LANE_UNOPENED = 0,   /* kernel zero-init — no lane yet */
    LANE_OPEN     = 1,
    LANE_DEAD     = 2,   /* daemon refused/left; strand emits direct VGA */
};

typedef struct StrandPrintState {
    void      *lane;        /* Brook* — this strand's console lane */
    uint64_t  *order;       /* the console's order, mapped by THIS strand */
    ConsoleRun run;         /* frame under construction; run.len = fill */
    uint32_t   last_fg;     /* VGA-direct colour cache (resolved pair) */
    uint32_t   last_bg;
    uint32_t   color_fg;    /* current colours (may hold sentinels) */
    uint32_t   color_bg;
    uint16_t   ear;         /* the lane's tag as a Touch tag (valid with EAR_CLAIMED) */
    uint16_t   kb;          /* "keyboard" tag (valid with KB_CLAIMED) */
    uint8_t    lane_state;  /* LANE_* */
    uint8_t    last_set;    /* last_fg/last_bg carry a sent pair */
    uint8_t    initialized;
    uint8_t    ear_flags;   /* EAR_* */
} StrandPrintState;

enum { EAR_CLAIMED = 1u, KB_CLAIMED = 2u };
_Static_assert(sizeof(StrandPrintState) == STRAND_PRINT_BYTES,
              "StrandPrintState must match strand_info.h STRAND_PRINT_BYTES");

static StrandPrintState g_main_print_state = {
    .color_fg = COLOR_DEFAULT, .color_bg = COLOR_BLACK, .initialized = 1
};

static StrandPrintState *print_state_self(void)
{
    StrandInfo *si = strand_info_or_null();
    if (!si) return &g_main_print_state;

    StrandPrintState *ps = (StrandPrintState *)(void *)si->print_state;
    if (!ps->initialized) {
        ps->color_fg    = COLOR_DEFAULT;
        ps->color_bg    = COLOR_BLACK;
        ps->initialized = 1;
    }
    return ps;
}

/* ===========================================================================
 * Cabin I/O state
 *
 *   1. g_io_mode      — VGA direct vs. display-daemon IPC routing.
 *   2. g_display_pid  — discovered display daemon (on first IPC operation).
 *
 * Both stay PROCESS-GLOBAL (not per-strand): every strand shares one
 * backend and one discovered daemon. g_display_pid's write is hardened
 * below (first-writer-wins CAS) since any strand's lane grant / readline /
 * getchar can race to discover it; g_io_mode is a single-writer invariant
 * (see io_set_mode) with one sanctioned exception — the daemon-death
 * fallback flips it to IO_MODE_VGA so the machine keeps talking.
 * =========================================================================== */
static uint8_t  g_io_mode      = IO_MODE_IPC;
static uint32_t g_display_pid  = 0;

void     io_set_mode(uint8_t mode)
{
    /* Switching between VGA and IPC paths invalidates the kernel colour
     * state we cached locally; force the next emit to re-send.
     * Single-writer invariant: call before any strand_spawn — no other
     * strand's cache exists yet, so invalidating only the caller's own is
     * sufficient (every strand spawned afterwards starts uninitialised
     * anyway and re-sends its colour on its first emit). A strand that
     * already opened a console lane and is then switched to VGA simply
     * stops pushing; the daemon closes the idle lane when the strand
     * exits (writer-leave drains to STREAM_CLOSED). */
    if (mode != g_io_mode) print_state_self()->last_set = false;
    g_io_mode = mode;
}
uint8_t  io_get_mode(void)                { return g_io_mode; }
void     io_set_display_pid(uint32_t pid) { g_display_pid = pid; }
uint32_t io_get_display_pid(void)         { return g_display_pid; }

/* ===========================================================================
 * Console lane — the strand's output stream to the display daemon.
 *
 * Grant protocol ("checkroom"): send DISP_CMD_LANE, the daemon opens the
 * reader side of a fresh "console:N" Brook FIRST and only then replies
 * [DISP_CMD_LANE][tag NUL] — the writer arrives at a laid table. A 1-byte
 * reply is an honest refusal.
 * =========================================================================== */

static void emit_run(StrandPrintState *ps, const char *bytes, int len,
                     Color fg, Color bg);

/* This STRAND's lane is over: it talks straight to the glass from here on.
 *
 * ‼ WHAT THIS USED TO DO AND WHY IT WAS WRONG. It also flipped the CABIN onto
 * the direct road (io_set_mode(IO_MODE_VGA)), and that is a decision about
 * everybody taken on one strand's evidence. A sibling with a living lane and
 * frames already banked in it would, on its very next emit, see IO_MODE_VGA
 * and write straight to the glass — past the order, past the merge — while the
 * daemon was still rendering what that same strand had pushed a moment
 * earlier. One strand's broken lane reordered another strand's output.
 *
 * A strand's lane failing says nothing about anyone else's. What DOES say
 * something about everyone is "there is no display at all", and that fact has
 * its own name and its own door (g_no_daemon, set where it is learned rather
 * than guessed at from a symptom). So this is now strictly local: LANE_DEAD
 * makes lane_ensure return false for this strand forever, which is the whole
 * of what it needs. A cabin with no daemon still converges — every strand
 * learns it from its own ask, and the ask is answered at once. */
/* Is this strand about to write straight to the glass? Two ways it can be:
 * the whole cabin is on the direct road (no daemon was ever found), or this
 * one strand's lane is over. Both take the same road and both want the same
 * thing from the caller — one batch around the whole call, so a colour and
 * the text it applies to cannot be split by another core's kprintf landing
 * between two operations that each take the console lock on their own. */
static bool io_direct(const StrandPrintState *ps)
{
    return g_io_mode != IO_MODE_IPC || ps->lane_state == LANE_DEAD;
}

static void lane_fail(StrandPrintState *ps)
{
    ps->lane_state = LANE_DEAD;
    /* Everything this strand knew about the kernel's current colour was true
     * of the DAEMON's road, where the daemon has been setting colours of its
     * own. On the direct road the first run must state its colour again. */
    ps->last_set   = false;
    /* The order was mapped for the lane's sake; with no lane there is nothing
     * left to number, and holding the claim would pin a page for a strand that
     * will never read through it again. */
    if (ps->order) { bay_release(ps->order); ps->order = NULL; }
    kdbg_print("[print] this strand's console lane is unavailable; it prints "
               "straight to the glass from here on");
}

/* Learned, never guessed: the kernel answers a broadcast to "display" with
 * ERR_ROUTE_NO_SUBSCRIBERS the instant nobody wears the tag. THAT is a fact
 * about the whole cabin, and it is the only thing that may move the whole
 * cabin onto the direct road. */
static bool g_no_daemon;

/* Wait for the daemon's DISP_CMD_LANE reply. Messages that are not the
 * grant are HELD aside and restashed after the wait — restashing inside
 * the loop would hand the same message straight back to us (receive
 * consults the ipc stash first), and dropping would eat a payload the
 * application is owed (spawn args arrive before a utility's first print).
 * The hold grows on demand so no flood can force a drop.
 *
 * There is no clock in this wait and no asking on a schedule. Two facts
 * end it, and nothing else:
 *
 *   - the ANSWER: a grant, or the daemon's honest one-byte refusal. The
 *     kernel says at once whether anyone wears the display tag at all
 *     (ERR_ROUTE_NO_SUBSCRIBERS), and a request that was delivered sits in
 *     a living daemon's ring until it gets there — a daemon that has not
 *     reached its loop yet on a crowded boot is worth waiting for
 *     (measured: 16 vCPUs on one host thread stretched a grant past five
 *     seconds, which is what a guessed deadline once called "no display");
 *   - the DEATH of whoever owes it: process:died is claimed before the
 *     ask. Once the daemon is known by pid, its own death ends the wait.
 *     Before it is known, any death makes the kernel the question again —
 *     an EMPTY broadcast, which a daemon ignores without replying (nothing
 *     to misread later) and which the kernel refuses when nobody wears the
 *     tag.
 *
 * Between the two the strand turns in — the same mark, look, sleep the
 * daemon itself lives by — so a writer waiting for its lane costs nothing. */
/* Say something to the display daemon, and tell a REFUSAL from a DEATH.
 *
 * ‼ A FULL RING IS NOT A FUNERAL.
 *
 * `send` is a Manifest op, and the kernel refuses it for reasons that have
 * nothing to do with whether the daemon exists: its result ring is momentarily
 * full (ERR_ROUTE_TARGET_FULL, system_ops.c SysRoute), or there was no room to
 * copy the payload into it (ERR_NO_MEMORY). Both of those mean "not now". Only
 * ERR_PROCESS_NOT_FOUND means "nobody is there".
 *
 * Reading the first as the second is what this closes, and the place it hurt
 * was the prompt. console_listen runs the instant after a shell prints one —
 * the busiest moment the daemon has, directly behind a screenful of output —
 * and on ANY refusal the caller used to conclude the daemon had died: it tore
 * this strand's lane down, released the Brook with the prompt still sitting
 * unread inside it, and moved the strand onto the direct-VGA road. So the
 * prompt printed after a long command sometimes never appeared and the one
 * after a short command always did. Seen on the board, where a frame takes
 * long enough for the daemon to fall behind; never once under an emulator,
 * where it never falls behind at all.
 *
 * A refusal is WAITED OUT, not counted: the daemon is running and draining, so
 * the room comes back, and yield hands it the core to do that with instead of
 * spinning on the very thing being waited for. The wait ends by itself if the
 * daemon really goes — the next answer is then ERR_PROCESS_NOT_FOUND, which is
 * a death and is returned as one. No clock is consulted and no attempt is
 * counted, because neither of those would be the truth about when the room
 * comes back. */
static int daemon_say(const uint8_t *req, uint16_t len)
{
    /* Said once per cabin, the first time a word to the daemon has to be
     * waited for. Not a threshold and not a clock: the FACT that the daemon's
     * ring was full at all is the news, because until this was found nobody
     * knew it happened — the refusal was swallowed as a death and the machine
     * quietly changed how it prints. One line, and the next board run says
     * whether this road is the one being taken. */
    static bool waited_before = false;

    for (;;) {
        int rc = send(g_display_pid, req, len);
        if (rc == 0) return 0;
        error_t why = box_errno_of(rc);
        if (why != ERR_ROUTE_TARGET_FULL && why != ERR_NO_MEMORY) return rc;
        if (!waited_before) {
            waited_before = true;
            kdbg_print("[print] the display daemon would not take a word just "
                       "now (%s); waiting for room rather than burying it",
                       why == ERR_ROUTE_TARGET_FULL ? "its ring is full"
                                                    : "no room for the record");
        }
        yield();
    }
}

static bool lane_await_grant(char *tag, size_t tag_cap, bool *no_daemon)
{
    Result  *held     = NULL;
    uint32_t held_n   = 0;
    uint32_t held_cap = 0;
    bool     got      = false;
    bool     done     = false;

    TouchTag pdied    = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    bool     watching = (pdied != TOUCH_TAG_INVALID) &&
                        touch_claim(pdied, TOUCH_REST, 0, 0) == 0;

    /* [cmd][u32 generation]: the lane is granted to (pid, generation),
     * so a recycled pid can never be handed its predecessor's lane. */
    uint8_t  req[5];
    uint32_t gen = strand_self_generation();
    req[0] = DISP_CMD_LANE;
    memcpy(req + 1, &gen, sizeof(gen));

    if (g_display_pid != 0) {
        if (daemon_say(req, sizeof(req)) < 0) done = true;
    } else {
        int rc = broadcast("display", req, sizeof(req));
        if (rc < 0 && box_errno_of(rc) == ERR_ROUTE_NO_SUBSCRIBERS) {
            *no_daemon = true;
            done = true;
        }
    }

    while (!done) {
        TurnInMark mark  = box_mark();
        bool       moved = false;

        Result r;
        while (!done && receive(&r)) {
            moved = true;
            const uint8_t *d = (const uint8_t *)(uintptr_t)r.data_addr;
            if (r.sender_pid != 0 && r.data_addr != 0 &&
                r.data_length >= 1 && d[0] == DISP_CMD_LANE) {
                if (r.data_length >= 2) {
                    uint32_t n = r.data_length - 1;
                    if (n >= tag_cap) n = (uint32_t)tag_cap - 1;
                    memcpy(tag, d + 1, n);
                    tag[n] = '\0';
                    if (tag[0] != '\0') {
                        __sync_bool_compare_and_swap(&g_display_pid, 0,
                                                     r.sender_pid);
                        got = true;
                    }
                }
                done = true;   /* grant, or the daemon's explicit refusal */
                break;
            }

            if (held_n == held_cap) {
                uint32_t cap = held_cap ? held_cap * 2 : 8;
                Result *grown = (Result *)malloc(cap * sizeof(Result));
                if (!grown) {
                    /* ‼ THE HOLD COULD NOT GROW, AND THERE IS NO WAY ROUND IT.
                     *
                     * What stood here restashed the message and broke the
                     * receive loop — and `moved` was already true, so the
                     * outer loop did NOT turn in, came round, and `receive`
                     * handed back the very message just restashed. A livelock
                     * at full speed, burning a core until the daemon died.
                     * Nor can it be cured by parking instead: the grant sits
                     * BEHIND this message in the ring, and the stash is
                     * consulted first, so a strand that stops reading past
                     * the hold can never reach its own grant. The hold is not
                     * an optimisation, it is the only way to see past.
                     *
                     * So this is an honest refusal. A cabin that cannot find
                     * room for eight Results has troubles far larger than its
                     * console, and the caller's answer to no grant is the
                     * direct road, which needs no memory at all. */
                    result_restash(&r);
                    kdbg_print("[print] no room to hold what is not the lane "
                               "grant; this strand gives up the lane and "
                               "prints straight to the glass");
                    for (uint32_t i = 0; i < held_n; i++) result_restash(&held[i]);
                    if (held) free(held);
                    return false;
                }
                if (held) {
                    memcpy(grown, held, held_n * sizeof(Result));
                    free(held);
                }
                held = grown;
                held_cap = cap;
            }
            held[held_n++] = r;
        }
        if (done) break;

        bool  ask_again = false;
        Touch t;
        while (watching && touch_try_pop_tag(pdied, &t)) {
            moved = true;
            if (t.payload_len < sizeof(TouchProcessDied)) continue;
            TouchProcessDied dd;
            memcpy(&dd, t.payload, sizeof(dd));
            if (g_display_pid != 0) {
                if (dd.pid == g_display_pid) { done = true; break; }
            } else {
                ask_again = true;
            }
        }
        if (done) break;
        if (ask_again) {
            int rc = broadcast("display", NULL, 0);
            if (rc < 0 && box_errno_of(rc) == ERR_ROUTE_NO_SUBSCRIBERS) break;
        }

        if (!moved) box_turn_in(mark);
    }

    if (watching) touch_release(pdied);

    /* Give every held message back in arrival order — the stash is
     * consulted before the ring, so later consumers see them first. */
    for (uint32_t i = 0; i < held_n; i++) result_restash(&held[i]);
    if (held) free(held);
    return got;
}

/* ‼ THE CONSOLE'S ORDER IS A NUMBER EVERY WRITER SHARES, NOT A CLOCK.
 *
 * Each frame used to carry cpu_rdtsc, taken on whatever core the writer was
 * running on, and the daemon merged the lanes by it. Cores agree about that
 * counter only to within what the boot-time sync left them — per_core.c
 * tolerates four microseconds, and it drifts after that — while a program
 * that lists a volume pushes its whole output in less time than that. So two
 * cabins' stamps were not reliably ordered against each other, and the shell's
 * next prompt was rendered INSIDE the listing of the command that had just
 * finished.
 *
 * MEASURED on the board: four listings out of twelve cut open at exactly the
 * same byte, 891 of 4235. The same byte every time is what a FIXED offset
 * between two cores looks like — a race would have scattered — and it never
 * happened once in an emulator, where the cores agree exactly.
 *
 * ‼ A CLOCK CANNOT BE MADE TO WORK HERE, AND TIGHTENING IT WOULD ONLY MOVE
 * THE FAILURE. What the console needs is not "when did this happen" but "what
 * came before what", and that is a question about ORDER. One counter, in a
 * page every printing cabin maps, incremented with a locked add the instant
 * before the frame is pushed: two writers on two cores get two numbers in the
 * order their adds happened, and there is no tolerance and nothing to drift.
 * A prompt printed after a program has ended took its number after that
 * program took its last one, so it cannot be put in front of it.
 *
 * The tried-and-rejected alternative is worth writing down: draining a dead
 * writer's lane first. It sounds causal and it is too late — the daemon learns
 * of a death on its next rotation, and the overtaking happens inside a single
 * render before that. It was written, measured, and taken out again.
 *
 * ‼ THERE IS NO FALLBACK HERE, AND THAT IS THE POINT. An earlier version of
 * this stamped with cpu_rdtsc when the order could not be mapped, and that is
 * strictly worse than the defect it replaced: a counter near 1 and a clock
 * near 2^40 are two DOMAINS of numbers, and a merge that sorts across two
 * domains orders nothing — every clock-stamped frame sinks to the end of the
 * console. So the order is not something a writer asks for separately and
 * might not get. It is GRANTED WITH THE LANE (lane_ensure, below): the daemon
 * refuses to grant a lane it cannot order, and a writer that cannot map the
 * order gives the lane back and prints straight to the glass — where it is the
 * only writer, and one writer needs no order. LANE_OPEN therefore implies the
 * order is mapped, and console_order_next has no branch to take.
 *
 * ‼ AND EACH STRAND MAPS ITS OWN, WHICH IS WHY THIS IS A FIELD AND NOT A
 * FILE-SCOPE POINTER. It was a file-scope pointer first, and that was a page
 * which VANISHED from under every sibling the moment the strand that opened it
 * exited — a fault in the print path of a program whose printing was working
 * perfectly. A Bay claim used to belong to the STRAND that opened it, while
 * the mapping it carries lives in the CABIN's address space, shared by every
 * strand in it. The shape that hides that is the common one (the main strand
 * prints its banner before it spawns anybody, and outlives them all); the
 * shape that shows it is a spawned strand printing first and finishing early.
 *
 * ‼ THE KERNEL'S RULE HAS SINCE BEEN CHANGED AT THE ROOT — a Bay claim is the
 * CABIN's now, released only by an explicit bay_release or by the cabin's own
 * teardown, and a strand exiting unmaps nothing (kernel bay.c, BayCleanupCabin,
 * called from cabin_destroy). So a shared pointer would be safe today. This
 * stays per-strand anyway, and deliberately: what the console needs to know is
 * that LANE_OPEN implies a mapped order, and as a field that is true because
 * the same strand did both. As a shared pointer it would be true only because
 * of a rule in another subsystem — the rule that had just been found to be the
 * other way round. The cost of not depending on it is one Manifest call on a
 * strand's first print, beside the daemon round trip it is already paying for
 * the lane, and one page table entry onto a page that is the same physical
 * page for everyone. */

/* Map the console's order for THIS strand. Called only from lane_ensure, and
 * a false return costs the caller its lane. */
static bool console_order_ensure(StrandPrintState *ps)
{
    if (ps->order) return true;

    /* BAY_OPEN, never BAY_CREATE: the daemon made it before it granted the
     * lane this call belongs to, so a missing order means the daemon is not
     * the one that answered — and a writer must not conjure a second console
     * order into existence. */
    ps->order = (uint64_t *)bay_open(CONSOLE_ORDER_TAG, 0, BAY_OPEN);
    if (!ps->order) {
        kdbg_print("[print] the console granted a lane but no order; this "
                   "strand gives the lane back and prints straight to the glass");
        return false;
    }
    return true;
}

/* The next number in the console's one order. LANE_OPEN is the caller's
 * proof that it is mapped — see console_order_ensure. */
static uint64_t console_order_next(StrandPrintState *ps)
{
    return __atomic_add_fetch(ps->order, 1u, __ATOMIC_SEQ_CST);
}

static bool lane_ensure(StrandPrintState *ps)
{
    if (ps->lane_state == LANE_OPEN) return true;
    if (ps->lane_state == LANE_DEAD) return false;

    /* Learned once: with nobody wearing the display tag there is nothing to
     * ask, and asking again per strand would only repeat the same answer. */
    if (g_no_daemon) { lane_fail(ps); return false; }

    char tag[64];
    bool no_daemon = false;
    if (!lane_await_grant(tag, sizeof(tag), &no_daemon)) {
        if (no_daemon) {
            /* This one IS about the whole cabin, and it is a fact rather than
             * an inference: the kernel says nobody wears the tag. */
            g_no_daemon = true;
            io_set_mode(IO_MODE_VGA);
        }
        lane_fail(ps);
        return false;
    }

    /* Shape is part of tag identity; passing the canonical constants makes
     * a header drift between writer and daemon fail loudly at open. */
    Brook *b = brook_open(tag, sizeof(ConsoleRun), CONSOLE_LANE_FRAMES,
                          BROOK_WRITER);
    if (!b) {
        lane_fail(ps);
        return false;
    }
    /* The order comes with the lane or the lane does not come at all. */
    if (!console_order_ensure(ps)) {
        brook_release(b);
        lane_fail(ps);
        return false;
    }

    ps->lane       = b;
    ps->lane_state = LANE_OPEN;
    /* The lane's tag is also where this strand's keys will be said. */
    ps->ear        = touch_pair_choose(touch_intern(tag));
    ps->ear_flags &= (uint8_t)~EAR_CLAIMED;
    return true;
}

/* Push one built frame. A full lane SLOWS the writer — output is never dropped
 * — and until this had a bell, the wait had to be a guess.
 *
 * WHAT STOOD HERE, and why the guessing was not the writer's fault. Both naive
 * waits fail a fan-in at scale, measured with sixteen printing strands:
 *   - brook_push's old UMWAIT path held the core while it watched the reader's
 *     cursor; sixteen watchers starved the very reader they were waiting for,
 *     and 24k lines sat banked until the writers died;
 *   - a yield per attempt flooded the Guide with a syscall storm from every
 *     blocked writer, and the daemon's render ops drowned in that queue — 299
 *     frames rendered in 160 s.
 * So what stood here was the shape that survives both: pause-spin with
 * exponential backoff between yields, LANE_PUSH_SPIN_MIN doubling toward
 * LANE_PUSH_SPIN_MAX. It worked, and every number in it was a guess about a
 * machine nobody could measure — "always shorter than the ring-drain it is
 * waiting out" was a hope, not a fact. Beside it stood a second guess: after
 * sixty seconds of silence, announce that the lane is jammed, because a
 * deadline-free wait with no voice is how a machine stands still without a
 * word.
 *
 * Both guesses are gone, and neither is replaced by a better number. The
 * writer now hangs its own bell — in the cacheline the daemon already reads
 * for `tail` on every pop — and the daemon rings it the moment it frees a
 * slot. brook_push does that and then turns in, so a blocked writer costs
 * NOTHING rather than a core: strictly better than the UMWAIT it replaces, and
 * its syscall count is bounded by how many times it SLEEPS instead of by how
 * often it retries.
 *
 * The sixty-second voice is not replaced either, because the jam now has a
 * better witness than a stopwatch. A daemon that stops draining sleeps with
 * frames unread and its own bell hung out, and Nightwatch convicts exactly
 * that (BrookBellUnrung) — naming the reader that is not draining, on facts,
 * with no clock anywhere in it, and without the writer having to narrate its
 * own wait.
 *
 * On daemon death this flips to the VGA fallback and returns false so the
 * caller re-delivers its content by the direct path. */
static bool lane_push(StrandPrintState *ps, const ConsoleRun *f)
{
    int rc = brook_push((Brook *)ps->lane, f);
    if (rc == 0) return true;

    Brook *dead = (Brook *)ps->lane;
    ps->lane = NULL;
    lane_fail(ps);
    if (dead) brook_release(dead);
    return false;
}

static void lane_flush_run(StrandPrintState *ps)
{
    if (ps->run.len == 0) return;
    ps->run.kind      = CONSOLE_RUN_TEXT;
    ps->run._reserved = 0;
    ps->run.order     = console_order_next(ps);

    bool    ok  = lane_push(ps, &ps->run);
    uint8_t len = ps->run.len;
    ps->run.len = 0;
    if (!ok) {
        /* The frame never reached the daemon; its text still must reach
         * the screen. lane_fail switched us to VGA, so this re-emit takes
         * the direct path (run.fg/bg are already resolved — resolving
         * again is the identity). */
        emit_run(ps, ps->run.text, len, ps->run.fg, ps->run.bg);
    }
}

/* Flush WHOLE LINES, and keep the unfinished one for the next frame.
 *
 * A frame is the unit the daemon interleaves. It merges every lane in global
 * push order, so two strands printing at once alternate at frame boundaries —
 * and a boundary that falls in the middle of a line puts half of one strand's
 * line inside another's. MEASURED, sixteen strands on sixteen cores: roughly
 * three hundred of thirty-two thousand lines came out spliced, e.g.
 *
 *     [PS-11] 00000[PS-04] 00000000
 *     [PS-04787
 *
 * Nothing is LOST — the character count is exact, every time — but a spliced
 * line is unreadable, and worse, unmatchable: every marker this system is
 * checked by is a grep, and a torn "[STRAND] PASS" is a green run reported as
 * a hang. That is what made a sixteen-core matrix a lottery.
 *
 * The cut is therefore taken at the last newline in the run rather than at the
 * end of the buffer, and the remainder — an unfinished line — stays behind to
 * be completed by the writer that owns it. A line then occupies whole frames
 * and cannot be split by anybody else's.
 *
 * Two cases still end a frame mid-line, and both are honest: a single line
 * longer than a whole frame has nowhere else to break, and a colour change
 * inside a line genuinely ends a run (the run IS the colour). Neither is a
 * splice between two writers waiting to happen the way an arbitrary
 * 108th-byte boundary was.
 *
 * Frame count is unchanged in the case that matters — the cut moves by at most
 * one line's worth, it does not add frames. */
static void lane_flush_lines(StrandPrintState *ps)
{
    uint8_t len = ps->run.len;
    if (len == 0) return;

    int cut = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (ps->run.text[i] == '\n') { cut = i + 1; break; }
    }
    /* No line ends inside this run, or the run already ends on one: it goes
     * whole either way. */
    if (cut <= 0 || (uint8_t)cut == len) { lane_flush_run(ps); return; }

    char    tail[CONSOLE_RUN_TEXT_MAX];
    uint8_t tlen = (uint8_t)(len - cut);
    memcpy(tail, ps->run.text + cut, tlen);
    Color fg = ps->run.fg, bg = ps->run.bg;

    ps->run.len = (uint8_t)cut;
    lane_flush_run(ps);

    if (ps->lane_state == LANE_OPEN) {
        ps->run.fg  = fg;
        ps->run.bg  = bg;
        memcpy(ps->run.text, tail, tlen);
        ps->run.len = tlen;
    } else {
        /* The lane died inside the flush and it has already re-delivered its
         * own bytes the direct way; the carried remainder must follow them. */
        emit_run(ps, tail, tlen, fg, bg);
    }
}

static void io_flush_state(StrandPrintState *ps)
{
    if (g_io_mode == IO_MODE_IPC && ps->lane_state == LANE_OPEN)
        lane_flush_run(ps);
}

void io_flush(void)
{
    io_flush_state(print_state_self());
}

/* ===========================================================================
 * Color state.
 *
 * On the lane, colour is frame METADATA — every ConsoleRun carries its
 * resolved (fg, bg) pair, so set_color is pure state and the pair rides
 * the next emitted run. Only the direct VGA path pushes colour eagerly
 * (the kernel keeps a current pair for kprintf and cursor-relative ops).
 * =========================================================================== */
static void push_color(StrandPrintState *ps)
{
    if (g_io_mode == IO_MODE_IPC) return;

    uint32_t fg = BoxColorResolveFg(ps->color_fg);
    uint32_t bg = BoxColorResolveBg(ps->color_bg);
    if (ps->last_set && ps->last_fg == fg && ps->last_bg == bg) return;

    vga_setcolor_rgb(fg, bg);
    ps->last_fg  = fg;
    ps->last_bg  = bg;
    ps->last_set = true;
}

void  set_color(Color fg)
{
    StrandPrintState *ps = print_state_self();
    ps->color_fg = fg;
    push_color(ps);
}
Color get_color(void) { return print_state_self()->color_fg; }

void  set_color_bg(Color bg)
{
    StrandPrintState *ps = print_state_self();
    ps->color_bg = bg;
    push_color(ps);
}
Color get_color_bg(void) { return print_state_self()->color_bg; }

/* ===========================================================================
 * Low-level emit — push a chunk of ASCII bytes with a given colour pair.
 * Honours g_io_mode: the IPC path packs ConsoleRun frames onto the lane
 * ('\n' travels inside the text — it is content, not a control record);
 * the VGA direct path uses vga_puts.
 * =========================================================================== */
static void emit_run(StrandPrintState *ps, const char *bytes, int len, Color fg, Color bg)
{
    if (len <= 0) return;

    fg = BoxColorResolveFg(fg);
    bg = BoxColorResolveBg(bg);

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        if (ps->run.len > 0 && (ps->run.fg != fg || ps->run.bg != bg))
            lane_flush_run(ps);

        int off = 0;
        while (off < len && ps->lane_state == LANE_OPEN) {
            if (ps->run.len == CONSOLE_RUN_TEXT_MAX) {
                /* Always makes room: the cut leaves either nothing or the
                 * unfinished tail, both shorter than a full run. */
                lane_flush_lines(ps);
                continue;
            }
            if (ps->run.len == 0) {
                ps->run.fg = fg;
                ps->run.bg = bg;
            }
            uint32_t space = CONSOLE_RUN_TEXT_MAX - ps->run.len;
            uint32_t chunk = (uint32_t)(len - off);
            if (chunk > space) chunk = space;
            memcpy(ps->run.text + ps->run.len, bytes + off, chunk);
            ps->run.len = (uint8_t)(ps->run.len + chunk);
            off += chunk;
        }
        /* A finished line leaves NOW; only an unfinished tail stays staged.
         * A strand's completed lines must never sit in its frame waiting for
         * the frame to fill: measured, a worker's last two lines stayed
         * behind while the main strand exited the cabin, the push then found
         * the lane closed and the lines fell through to direct VGA — after
         * the shell's next prompt, glued to it. */
        if (ps->lane_state == LANE_OPEN && memchr(ps->run.text, '\n', ps->run.len))
            lane_flush_lines(ps);
        if (off >= len) return;

        /* The daemon died mid-run (lane_flush_run above fell back); the
         * pending frame was already re-emitted, deliver the remainder the
         * direct way too. */
        bytes += off;
        len   -= off;
    }

    /* VGA direct: set colour only when it actually changed, then the text in
     * as few PUTSTRING operations as it takes, newlines INSIDE them. The
     * console lock is held per operation, so a line's text in one operation
     * and its newline in the next left a gap for another core's kprintf to
     * land in (the daemon's render had the same shape; measured there). A
     * chunk is cut at the last newline that fits, so only a line longer than
     * a chunk is ever split, and then by nobody else's. */
    bool changed = !ps->last_set || ps->last_fg != fg || ps->last_bg != bg;
    if (changed) {
        vga_setcolor_rgb(fg, bg);
        ps->last_fg  = fg;
        ps->last_bg  = bg;
        ps->last_set = true;
    }
    char tmp[256];
    int  off = 0;
    while (off < len) {
        int take = len - off;
        if (take > (int)sizeof(tmp) - 1) {
            take = (int)sizeof(tmp) - 1;
            for (int k = off + take - 1; k >= off; k--) {
                if (bytes[k] == '\n') { take = k - off + 1; break; }
            }
        }
        memcpy(tmp, bytes + off, (size_t)take);
        tmp[take] = '\0';
        vga_puts(tmp);
        off += take;
    }
}

/* ===========================================================================
 * Public output API
 *
 * print() streams the input in fixed-size chunks: each chunk is UTF-8
 * filtered into a stack scratch buffer, then handed to emit_run. There is no
 * upper bound on input length — the loop keeps consuming until the source
 * NUL. The chunk size is purely an internal staging optimisation.
 * =========================================================================== */
/* Stream `len` bytes into fixed-size chunks, handing each chunk to emit_run
 * with the current colours. Shared by print(), print_bytes() and println() so
 * the chunking lives in exactly one place.
 * `newline` rides in the LAST chunk rather than in an emit of its own: a
 * newline emitted separately is a separate operation on the direct path, and
 * the gap between the two is where another core's line lands.
 *
 * ‼ NOTHING IS SUBSTITUTED HERE, AND THAT IS THE POINT. This used to collapse
 * every multi-byte UTF-8 sequence into a single '?' — "until the kernel-side
 * font extension lands" — so `me` printed "6 cores ? 2 K-Core", and the
 * character was gone from the log too, because the replacement happened in
 * this address space, before the first syscall. Meanwhile the kernel wrote
 * the same em dash through untouched and it came out as three empty cells on
 * the board. Two printers, two different contracts on one character, and
 * neither of them could see the font. The fold now happens once, at the font
 * — canvas.c print_utf8_byte_locked, font/glyph_fold.h — which is the only
 * code that knows what a cell can hold. A chunk may end mid-sequence; the
 * Canvas assembles across calls, so a split costs nothing. */
static void emit_chunks(StrandPrintState *ps, const char* data, size_t len,
                        bool newline)
{
    char   chunk[256];
    size_t src = 0;
    do {
        size_t take = len - src;
        if (take > sizeof(chunk) - 1) take = sizeof(chunk) - 1;
        if (take) memcpy(chunk, data + src, take);
        src += take;

        int chunk_pos = (int)take;
        if (newline && src >= len) chunk[chunk_pos++] = '\n';
        if (chunk_pos == 0) break;
        emit_run(ps, chunk, chunk_pos, ps->color_fg, ps->color_bg);
    } while (src < len);
}

void print(const char* str)
{
    if (!str) return;
    StrandPrintState *ps = print_state_self();

    /* IO_MODE_IPC batches into the lane frame and flushes lazily;
     * IO_MODE_VGA wins a syscall reduction by feeding every internal
     * vga_setcolor/vga_puts into a single Manifest. The nested
     * vga_begin/vga_commit composes with an outer caller that may itself be
     * batching (printf, shell renderer). */
    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    emit_chunks(ps, str, strlen(str), false);

    if (we_began) vga_commit();
}

void print_bytes(const char* data, size_t len)
{
    if (!data || len == 0) return;
    StrandPrintState *ps = print_state_self();

    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    emit_chunks(ps, data, len, false);

    if (we_began) vga_commit();
}

void println(const char* str)
{
    StrandPrintState *ps = print_state_self();

    /* The newline goes with the text — same frame on the lane, same
     * operation on the direct path — never as an emit of its own. */
    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    if (str) emit_chunks(ps, str, strlen(str), true);
    else     emit_run(ps, "\n", 1, ps->color_fg, ps->color_bg);

    if (we_began) vga_commit();
}

void clear(void)
{
    StrandPrintState *ps = print_state_self();

    /* Display state resets on clear; invalidate the colour cache so the
     * next coloured run re-sends its pair. */
    ps->last_set = false;

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        lane_flush_run(ps);
        if (ps->lane_state == LANE_OPEN) {
            ConsoleRun f;
            memset(&f, 0, sizeof(f));
            f.kind = CONSOLE_RUN_CLEAR;
            f.fg   = COLOR_LIGHT_GRAY;
            f.bg   = COLOR_BLACK;
            f.order = console_order_next(ps);
            if (lane_push(ps, &f)) return;
        }
        /* Daemon died on the way — the fallback below still clears. */
    }
    vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
}

/* ===========================================================================
 * printf — BoxOS extension: %color consumes one Color (uint32_t RGB) and
 * switches the active foreground for following text runs.
 *
 * Implementation walks the format string, accumulates text into a working
 * buffer, and emits a colored run whenever:
 *   - %color is encountered (flush + change colour);
 *   - the buffer is about to overflow;
 *   - format string ends.
 *
 * UTF-8: passes through whole, format string and %s alike. What a character
 * looks like is settled once, at the font, by the only code that holds it
 * (kernel canvas.c / font/glyph_fold.h) — not here, where the font cannot be
 * seen and the substitution reached the log as well as the glass.
 *
 * One printf currently produces N×emit_run calls (N = colour changes). For
 * VGA direct mode each emit is 1 setcolor + 1 puts syscall; for IPC mode
 * everything coalesces into lane frames and flushes lazily.
 * =========================================================================== */

#define PRINTF_BUFLEN 512

static void fmt_htoa64(unsigned long v, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[17];
    int  i = 0;
    while (v > 0) { tmp[i++] = hex[v & 0xF]; v >>= 4; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static int append_str(char *buf, int pos, int max, const char *s)
{
    while (s && *s && pos < max) buf[pos++] = *s++;
    return pos;
}

/* WEAK on purpose. boxlib's printf is BoxOS's coloured-run console printer and
 * converts %s %d %i %u %x %X %c %p %% — the set a C program on this system has
 * always had. boxcxx's <cstdio> defines the full C set, including the floating
 * conversions, and a program that links boxcxx must get THAT one: [cstdio.syn]
 * asks for conversions this function does not have, and two functions cannot
 * share an ELF symbol.
 *
 * Weak rather than moved to its own translation unit because printf lives on
 * this file's statics — g_io_mode, the per-strand colour state, the lane
 * machinery — and splitting it would mean exposing all three across a boundary
 * to solve a linking question. The link order that makes this work is already
 * stated in apps/Makefile: libboxcxx.a before libbox.a, "so our runtime symbols
 * always win". */
__attribute__((weak)) int printf(const char *fmt, ...)
{
    if (!fmt) return -1;
    StrandPrintState *ps = print_state_self();

    va_list args;
    va_start(args, fmt);

    /* See print() for rationale — colored runs in VGA mode coalesce
     * into a single Manifest submit instead of N syscalls. */
    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    char buf[PRINTF_BUFLEN];
    int  pos       = 0;
    int  total_out = 0;
    Color cur_fg   = ps->color_fg;
    Color cur_bg   = ps->color_bg;
    char  numbuf[24];

    /* Local helpers — re-emit and reset the working buffer. */
    #define FLUSH() do {                                    \
        if (pos > 0) {                                      \
            emit_run(ps, buf, pos, cur_fg, cur_bg);         \
            total_out += pos;                               \
            pos = 0;                                        \
        }                                                   \
    } while (0)

    #define ENSURE(n) do {                                  \
        if (pos + (n) > PRINTF_BUFLEN - 4) FLUSH();         \
    } while (0)

    while (*fmt) {
        if (*fmt != '%') {
            /* Every byte of the format string, UTF-8 sequences included, goes
             * out as it was written. See emit_chunks: the character is folded
             * at the font, not here. */
            ENSURE(4);
            buf[pos++] = *fmt++;
            continue;
        }

        /* %... */
        const char *spec_start = fmt;
        fmt++;
        if (*fmt == '\0') { /* trailing % — emit literal */
            ENSURE(2);
            buf[pos++] = '%';
            break;
        }

        /* %color — extension: switch foreground, no text emitted. */
        if (*fmt == 'c' && fmt[1] == 'o' && fmt[2] == 'l' &&
            fmt[3] == 'o' && fmt[4] == 'r') {
            FLUSH();
            cur_fg = (Color)va_arg(args, uint32_t);
            fmt += 5;
            continue;
        }

        /* %bgcolor — the same for the background: colour is a (fg, bg)
         * pair everywhere, and printf can steer both halves. */
        if (*fmt == 'b' && fmt[1] == 'g' && fmt[2] == 'c' &&
            fmt[3] == 'o' && fmt[4] == 'l' && fmt[5] == 'o' &&
            fmt[6] == 'r') {
            FLUSH();
            cur_bg = (Color)va_arg(args, uint32_t);
            fmt += 7;
            continue;
        }

        /* Length modifier — `l` / `ll` / `z`. Determines the size of the
         * va_arg pulled for %d/%u/%x. C99 promotion rules: variadic
         * int8/int16 promote to int, so %hhd/%hd are not needed; long
         * stays long; long long stays long long. `z` matches size_t
         * (== uint64_t in our LP64 userspace).
         *
         * width = 0 → int / unsigned int   (default)
         *         1 → long / unsigned long (also size_t)
         *         2 → long long / unsigned long long
         */
        int  width = 0;
        if (*fmt == 'l') {
            width = 1;
            fmt++;
            if (*fmt == 'l') { width = 2; fmt++; }
        } else if (*fmt == 'z') {
            width = 1;
            fmt++;
        }

        switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                /* Stream byte-at-a-time, flushing on overflow. A flush may
                 * land mid-sequence and that is harmless: the Canvas
                 * assembles the character across whatever calls carry it. */
                while (*s) {
                    ENSURE(4);
                    buf[pos++] = *s++;
                }
                break;
            }
            case 'd': case 'i': {
                int64_t v;
                if (width == 0)      v = (int64_t)va_arg(args, int);
                else if (width == 1) v = (int64_t)va_arg(args, long);
                else                 v = (int64_t)va_arg(args, long long);
                int64_to_str(v, numbuf, sizeof(numbuf));
                ENSURE(24);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case 'u': {
                uint64_t v;
                if (width == 0)      v = (uint64_t)va_arg(args, unsigned int);
                else if (width == 1) v = (uint64_t)va_arg(args, unsigned long);
                else                 v = (uint64_t)va_arg(args, unsigned long long);
                uint64_to_str(v, numbuf, sizeof(numbuf));
                ENSURE(24);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case 'x': case 'X': {
                uint64_t v;
                if (width == 0)      v = (uint64_t)va_arg(args, unsigned int);
                else if (width == 1) v = (uint64_t)va_arg(args, unsigned long);
                else                 v = (uint64_t)va_arg(args, unsigned long long);
                /* uint64_to_hex emits a "0x" prefix; printf %x in C is
                 * "no prefix", so write the bare hex digits here. */
                if (v == 0) {
                    ENSURE(2);
                    buf[pos++] = '0';
                } else {
                    char hex_tmp[24];
                    fmt_htoa64((unsigned long)v, hex_tmp);
                    ENSURE(24);
                    pos = append_str(buf, pos, PRINTF_BUFLEN - 4, hex_tmp);
                }
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                ENSURE(2);
                buf[pos++] = c;
                break;
            }
            case 'p': {
                unsigned long v = va_arg(args, unsigned long);
                ENSURE(20);
                buf[pos++] = '0';
                buf[pos++] = 'x';
                fmt_htoa64(v, numbuf);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case '%': {
                ENSURE(2);
                buf[pos++] = '%';
                break;
            }
            default: {
                /* Unknown spec — copy verbatim from spec_start through *fmt. */
                ENSURE(4);
                buf[pos++] = '%';
                buf[pos++] = *fmt;
                (void)spec_start;
                break;
            }
        }
        fmt++;
    }

    FLUSH();

    #undef FLUSH
    #undef ENSURE

    if (we_began) vga_commit();

    va_end(args);
    return total_out;
}

/* ===========================================================================
 * The ear — where this strand's keys arrive (the editor is readline.c)
 *
 * With a daemon: the strand's own lane tag. The lane is ensured, the tag is
 * claimed once, and the daemon is told to listen on EVERY call — its ear
 * stack moves the lane to the top, which is idempotent and exactly right
 * after a child took the ear and died. Claim BEFORE the ask, so a key the
 * daemon has banked is said to a claimant that already exists.
 *
 * Without a daemon: "keyboard" itself, claimed once per strand.
 * =========================================================================== */
TouchTag console_listen(void)
{
    StrandPrintState *ps = print_state_self();
    io_flush_state(ps);

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        if (!(ps->ear_flags & EAR_CLAIMED)) {
            if (ps->ear == TOUCH_TAG_INVALID ||
                touch_claim(ps->ear, TOUCH_REST, 0, 0) != 0)
                return TOUCH_TAG_INVALID;
            ps->ear_flags |= EAR_CLAIMED;
        }
        uint8_t  req[6];
        uint32_t gen = strand_self_generation();
        req[0] = DISP_CMD_LISTEN;
        memcpy(req + 1, &gen, sizeof(gen));
        req[5] = 1;
        if (daemon_say(req, sizeof(req)) == 0) return ps->ear;

        /* Nobody at that pid any more: the daemon is gone — daemon_say has
         * already waited out every refusal that was merely a busy one. Say so
         * the way a failed push would, and hear the keyboard directly from
         * here on. */
        Brook *dead = (Brook *)ps->lane;
        ps->lane = NULL;
        lane_fail(ps);
        if (dead) brook_release(dead);
    }

    if (!(ps->ear_flags & KB_CLAIMED)) {
        TouchTag kb = touch_pair_choose(touch_intern(TOUCH_TAG_KEYBOARD));
        if (kb == TOUCH_TAG_INVALID || touch_claim(kb, TOUCH_REST, 0, 0) != 0)
            return TOUCH_TAG_INVALID;
        ps->kb         = kb;
        ps->ear_flags |= KB_CLAIMED;
    }
    return ps->kb;
}

/* The reading is over: give the ear back, so whoever reads next hears, and a
 * key typed while nobody reads stays banked at the daemon for that reader
 * rather than landing in this strand's ring. Nothing to tell when the strand
 * hears the keyboard itself. */
void console_unlisten(void)
{
    StrandPrintState *ps = print_state_self();
    if (g_io_mode != IO_MODE_IPC || ps->lane_state != LANE_OPEN ||
        !(ps->ear_flags & EAR_CLAIMED))
        return;
    uint8_t  req[6];
    uint32_t gen = strand_self_generation();
    req[0] = DISP_CMD_LISTEN;
    memcpy(req + 1, &gen, sizeof(gen));
    req[5] = 0;
    /* Given back the same way it was taken: a daemon that is merely behind is
     * waited for, because a lane that fails to hand the ear back keeps it, and
     * the next program to read would hear nothing. */
    (void)daemon_say(req, sizeof(req));
}

/* A cursor step is a frame on the lane, after whatever text is pending, so it
 * lands in the order it was said; without a daemon it is the same one op the
 * daemon would render it into. Nothing here knows, or needs to know, where the
 * caret is or how wide the screen is: a step says how FAR, and where that
 * lands is the console's business. */
void console_step(int32_t delta)
{
    if (delta == 0) return;
    StrandPrintState *ps = print_state_self();

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        lane_flush_run(ps);
        if (ps->lane_state == LANE_OPEN) {
            ConsoleRun f;
            memset(&f, 0, sizeof(f));
            f.kind = CONSOLE_RUN_STEP;
            f.len  = sizeof(delta);
            memcpy(f.text, &delta, sizeof(delta));
            f.fg   = BoxColorResolveFg(ps->color_fg);
            f.bg   = BoxColorResolveBg(ps->color_bg);
            f.order = console_order_next(ps);
            if (lane_push(ps, &f)) return;
        }
        /* The daemon died on the way — the fallback below still steps. */
    }

    /* No daemon: the same one op, straight to the deck. The arithmetic is the
     * kernel's either way — see hardware_ops.c HwVgaStepCursor. */
    (void)vga_step_cursor(delta);
}

/* ===========================================================================
 * Convenience integer printers (kept for source compat with apps).
 * =========================================================================== */
void print_int(int num)
{
    char b[12];
    to_str(num, b, sizeof(b));
    print(b);
}

void print_hex(uint32_t num)
{
    static const char hex[] = "0123456789abcdef";
    char b[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) { b[i] = hex[num & 0xF]; num >>= 4; }
    print(b);
}
