#include "box/print.h"
#include "box/color.h"
#include "box/screen.h"   /* log_say — the road with no display */
#include "boxos_utf8.h"   /* BoxUtf8Whole — no piece ends inside a character */
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/debug.h"
#include "box/brook.h"
#include "box/bay.h"
#include "box/system.h"
#include "box/error.h"
#include "box/core/format.h"
#include "box/core/notify.h"
#include "box/core/strand_self.h"
#include "box/memory.h"
#include "box/display.h"
#include "box/touch.h"
#include "box/file.h"
#include "box/sync.h"
#include "box/core/form_internal.h"

/* ===========================================================================
 * Per-strand print state — thread-confined, NO lock.
 *
 * Every strand (main + spawned) owns its own console lane, frame under
 * construction and current colours. Concurrent strands
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
 * the strand says its text into the log ring (log_say), which the kernel's
 * Emergency Screen shows while no display holds the screen, so the machine
 * keeps talking.
 *
 * Storage: the main strand uses a static instance (g_main_print_state); a
 * spawned strand's instance lives inline in its StrandInfo (print_state[]).
 * The kernel zero-inits that block, so `initialized` starts false and
 * print_state_self() corrects fg/bg to the process defaults on first touch.
 * Per owner decision, a spawned strand's colours always start FRESH
 * (COLOR_DEFAULT both: the screen's own) — no inheritance from whoever
 * spawned it.
 * =========================================================================== */

enum {
    LANE_UNOPENED = 0,   /* kernel zero-init — no lane yet */
    LANE_OPEN     = 1,
    LANE_DEAD     = 2,   /* daemon refused/left; strand says into the log ring */
};

typedef struct StrandPrintState {
    void      *lane;        /* Brook* — this strand's console lane */
    uint64_t  *order;       /* the console's order, mapped by THIS strand */
    uint64_t   last_order;  /* the number of this strand's last frame; 0: none yet */
    ConsoleRun run;         /* frame under construction; run.len = fill */
    uint32_t   color_fg;    /* current colours (may hold sentinels) */
    uint32_t   color_bg;
    TouchTag   ear;         /* the lane's tag as a Touch tag (valid with EAR_CLAIMED) */
    TouchTag   kb;          /* "keyboard" tag (valid with KB_CLAIMED) */
    uint32_t   display;     /* pid of the display that granted the lane */
    uint32_t   listeners;   /* console_listen calls not yet given back */
    uint8_t    lane_state;  /* LANE_* */
    uint8_t    initialized;
    uint8_t    ear_flags;   /* EAR_* */
} StrandPrintState;

enum { EAR_CLAIMED = 1u, KB_CLAIMED = 2u };
_Static_assert(sizeof(StrandPrintState) == STRAND_PRINT_BYTES,
              "StrandPrintState must match strand_info.h STRAND_PRINT_BYTES");

static StrandPrintState g_main_print_state = {
    .color_fg = COLOR_DEFAULT, .color_bg = COLOR_DEFAULT, .initialized = 1
};

static StrandPrintState *print_state_self(void)
{
    StrandInfo *si = strand_info_or_null();
    if (!si) return &g_main_print_state;

    StrandPrintState *ps = (StrandPrintState *)(void *)si->print_state;
    if (!ps->initialized) {
        ps->color_fg    = COLOR_DEFAULT;
        ps->color_bg    = COLOR_DEFAULT;
        ps->initialized = 1;
    }
    return ps;
}

/* ===========================================================================
 * Cabin I/O state: g_io_mode — the display's lanes, or the log ring directly.
 * PROCESS-GLOBAL and single-writer (see io_set_mode), with one sanctioned
 * exception: learning that there is no display at all flips it to
 * IO_MODE_RING so the machine keeps talking. Which display a strand talks to
 * is not a cabin's fact but its lane's (StrandPrintState.display).
 * =========================================================================== */
static uint8_t  g_io_mode      = IO_MODE_IPC;

void     io_set_mode(uint8_t mode)
{
    /* Single-writer invariant: call before any strand_spawn. A strand that
     * already opened a console lane and is then switched to the ring simply
     * stops pushing; the daemon closes the idle lane when the strand exits
     * (writer-leave drains to STREAM_CLOSED). */
    g_io_mode = mode;
}

/* ===========================================================================
 * Console lane — the strand's output stream to the display daemon.
 *
 * Grant protocol ("checkroom"): ask DISP_CMD_LANE (box/ipc.h ask); the daemon
 * opens the reader side of a fresh "console:N" Brook FIRST and only then
 * answers [DISP_CMD_LANE][u32 its pid][tag NUL] — the writer arrives at a laid
 * table. A 1-byte answer is an honest refusal.
 * =========================================================================== */

static void emit_run(StrandPrintState *ps, const char *bytes, int len,
                     Color fg, Color bg);

/* This STRAND's lane is over: it says its text into the log ring from here on.
 *
 * ‼ WHAT THIS USED TO DO AND WHY IT WAS WRONG. It also flipped the CABIN onto
 * the direct road, and that is a decision about everybody taken on one
 * strand's evidence. A sibling with a living lane and frames already banked in
 * it would, on its very next emit, take the direct road too — past the order,
 * past the merge — while the daemon was still rendering what that same strand
 * had pushed a moment earlier. One strand's broken lane reordered another
 * strand's output.
 *
 * A strand's lane failing says nothing about anyone else's. What DOES say
 * something about everyone is "there is no display at all", and that fact has
 * its own name and its own door (g_no_daemon, set where it is learned rather
 * than guessed at from a symptom). So this is strictly local: LANE_DEAD makes
 * lane_ensure return false for this strand forever, which is the whole of what
 * it needs. A cabin with no daemon still converges — every strand learns it
 * from its own ask, and the ask is answered at once. */
static void lane_fail(StrandPrintState *ps)
{
    ps->lane_state = LANE_DEAD;
    /* The order was mapped for the lane's sake; with no lane there is nothing
     * left to number, and holding the claim would pin a page for a strand that
     * will never read through it again. */
    if (ps->order) { bay_release(ps->order); ps->order = NULL; }
    kdbg_print("[print] this strand's console lane is unavailable; it says its "
               "text into the log ring from here on");
}

/* Learned, never guessed: the kernel answers a question asked of "display"
 * with ERR_ROUTE_NO_SUBSCRIBERS the instant nobody wears the tag. THAT is a
 * fact about the whole cabin, and it is the only thing that may move the whole
 * cabin onto the direct road. */
static bool g_no_daemon;

/* Say something to the display daemon, and tell a REFUSAL from a DEATH.
 *
 * ‼ A FULL RING IS NOT A FUNERAL.
 *
 * `send` is a Manifest op, and the kernel refuses it for a reason that has
 * nothing to do with whether the daemon exists: there was no room to copy the
 * payload, or to hold it as a debt while the daemon's ring is full
 * (ERR_NO_MEMORY, system_ops.c SysRoute). That means "not now". Only
 * ERR_PROCESS_NOT_FOUND means "nobody is there".
 *
 * Reading the first as the second is what this closes, and the place it hurt
 * was the prompt. console_listen runs the instant after a shell prints one —
 * the busiest moment the daemon has, directly behind a screenful of output —
 * and on ANY refusal the caller used to conclude the daemon had died: it tore
 * this strand's lane down, released the Brook with the prompt still sitting
 * unread inside it, and moved the strand off the console. So the
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
static int daemon_say(const StrandPrintState *ps, const uint8_t *req, uint16_t len)
{
    /* Said once per cabin, the first time a word to the daemon has to be
     * waited for. Not a threshold and not a clock: the FACT that a word had
     * to wait at all is the news, because until this was found nobody knew it
     * happened — the refusal was swallowed as a death and the machine quietly
     * changed how it prints. One line, and the next board run says whether
     * this road is the one being taken. */
    static bool waited_before = false;

    for (;;) {
        int rc = send(ps->display, req, len);
        if (rc == 0) return 0;
        error_t why = box_errno_of(rc);
        if (why != ERR_NO_MEMORY) return rc;
        if (!waited_before) {
            waited_before = true;
            kdbg_print("[print] the display daemon would not take a word just "
                       "now (no room for the record); waiting for room rather "
                       "than burying it");
        }
        yield();
    }
}

/* The console in cells, asked of the display. */
int display_size(uint32_t *cols, uint32_t *rows)
{
    if (!cols || !rows) return -ERR_NULL_POINTER;
    uint8_t  question = DISP_CMD_SIZE;
    uint8_t  answer[1 + 2 * sizeof(uint32_t)];
    uint32_t got = 0;
    int rc = ask(DISPLAY_TAG, &question, sizeof(question), answer, sizeof(answer), &got);
    if (rc != OK) return rc;
    if (got != sizeof(answer) || answer[0] != DISP_CMD_SIZE) return -ERR_CORRUPTED;
    memcpy(cols, answer + 1, sizeof(*cols));
    memcpy(rows, answer + 1 + sizeof(*cols), sizeof(*rows));
    return OK;
}

/* The lane, asked (box/display.h DISP_CMD_LANE): the tag of a Brook the
 * display already reads, and the display's own pid — this lane's display.
 * OK, or the ask's own refusal, or -ERR_ACCESS_DENIED for the display's. */
static int lane_ask(StrandPrintState *ps, char *tag, size_t tag_cap)
{
    uint8_t  question = DISP_CMD_LANE;
    uint8_t  answer[1 + sizeof(uint32_t) + 64];
    uint32_t got = 0;
    int rc = ask(DISPLAY_TAG, &question, sizeof(question), answer, sizeof(answer), &got);
    if (rc != OK) return rc;
    if (got < 1 || answer[0] != DISP_CMD_LANE) return -ERR_CORRUPTED;
    if (got == 1) return -ERR_ACCESS_DENIED;

    const uint32_t head = 1 + sizeof(uint32_t);
    if (got <= head || got - head > tag_cap || answer[got - 1] != '\0' || answer[head] == '\0')
        return -ERR_CORRUPTED;
    memcpy(&ps->display, answer + 1, sizeof(uint32_t));
    memcpy(tag, answer + head, got - head);
    return OK;
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
 * order gives the lane back and says its text into the log ring — which keeps
 * its own order, one byte after another. LANE_OPEN therefore implies the
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
                   "strand gives the lane back and says its text into the log ring");
        return false;
    }
    return true;
}

/* The next number in the console's one order. LANE_OPEN is the caller's
 * proof that it is mapped — see console_order_ensure. */
static uint64_t console_order_next(StrandPrintState *ps)
{
    ps->last_order = __atomic_add_fetch(ps->order, 1u, __ATOMIC_SEQ_CST);
    return ps->last_order;
}

static bool lane_ensure(StrandPrintState *ps)
{
    if (ps->lane_state == LANE_OPEN) return true;
    if (ps->lane_state == LANE_DEAD) return false;

    /* Learned once: with nobody wearing the display tag there is nothing to
     * ask, and asking again per strand would only repeat the same answer. */
    if (g_no_daemon) { lane_fail(ps); return false; }

    char tag[64];
    int  asked = lane_ask(ps, tag, sizeof(tag));
    if (asked != OK) {
        if (asked == -ERR_ROUTE_NO_SUBSCRIBERS) {
            /* This one IS about the whole cabin, and it is a fact rather than
             * an inference: the kernel says nobody wears the tag. */
            g_no_daemon = true;
            io_set_mode(IO_MODE_RING);
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

static void ring_say(const char *bytes, size_t len);

/* The display died with this strand's frames in its lane: the text of every
 * frame it never took, and of every one it took past what it had said
 * (CONSOLE_ORDER_SAID), said into the log ring here, oldest first — nothing
 * this strand pushed is lost to a display that died. The frames are the lane's
 * own slots, untouched since: only this writer's next push writes one again.
 * Before lane_fail, which gives the order's page back. */
static void lane_resay(StrandPrintState *ps, Brook *dead)
{
    if (!dead || !ps->order) return;
    uint64_t   said    = __atomic_load_n(&ps->order[CONSOLE_ORDER_SAID], __ATOMIC_ACQUIRE);
    uint32_t   untaken = brook_available(dead);
    ConsoleRun f;
    uint32_t   count = 0;
    while (brook_written(dead, count, &f) == OK && (count < untaken || f.order > said))
        count++;
    while (count-- > 0) {
        if (brook_written(dead, count, &f) == OK && f.kind == CONSOLE_RUN_TEXT)
            ring_say(f.text, f.len < CONSOLE_RUN_TEXT_MAX ? f.len : CONSOLE_RUN_TEXT_MAX);
    }
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
 * On daemon death this flips the strand onto the ring and returns false so
 * the caller re-delivers its content there. */
static bool lane_push(StrandPrintState *ps, const ConsoleRun *f)
{
    int rc = brook_push((Brook *)ps->lane, f);
    if (rc == 0) return true;

    Brook *dead = (Brook *)ps->lane;
    ps->lane = NULL;
    lane_resay(ps, dead);
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
        /* The frame never reached the daemon; its text still must be said.
         * lane_fail moved this strand onto the ring, so this re-emit goes
         * there (run.fg/bg are already on the wire — putting them there
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

static void arrow_flush(StrandPrintState *ps);

/* The strand's staged run to the screen, and what the arrow gathered to its
 * file. */
void io_flush(void)
{
    StrandPrintState *ps = print_state_self();
    io_flush_state(ps);
    arrow_flush(ps);
}

/* Everything this strand pushed into its lane is said: the display's mark has
 * reached the strand's last number (CONSOLE_ORDER_SAID). */
static bool lane_said(void *ctx)
{
    StrandPrintState *ps = ctx;
    return __atomic_load_n(&ps->order[CONSOLE_ORDER_SAID], __ATOMIC_ACQUIRE) >= ps->last_order;
}

/* A strand that leaves with its last lines still in the lane would lose them
 * to a display that died in that moment — the program that ends the display
 * is the first to do it. So it waits, on its lane's bell, until the display
 * has said them, and says them itself if the display goes first. */
void io_depart(void)
{
    StrandPrintState *ps = print_state_self();
    io_flush_state(ps);
    arrow_flush(ps);
    if (g_io_mode != IO_MODE_IPC || ps->lane_state != LANE_OPEN) return;
    if (brook_wait((Brook *)ps->lane, lane_said, ps) == OK) return;

    Brook *dead = (Brook *)ps->lane;
    ps->lane = NULL;
    lane_resay(ps, dead);
    lane_fail(ps);
    brook_release(dead);
}

/* ===========================================================================
 * Color state.
 *
 * Colour is frame METADATA — every ConsoleRun carries its (fg, bg) pair, a
 * real triple each or COLOR_DEFAULT for the screen's own (BoxColorOnWire), so
 * set_color is pure state and the pair rides the next emitted run.
 * The log ring carries no colour at all: text said there is drawn in the
 * plain tone.
 * =========================================================================== */
void  set_color(Color fg)
{
    print_state_self()->color_fg = fg;
}
Color get_color(void) { return print_state_self()->color_fg; }

void  set_color_bg(Color bg)
{
    print_state_self()->color_bg = bg;
}
Color get_color_bg(void) { return print_state_self()->color_bg; }

/* ===========================================================================
 * Low-level emit — push a chunk of bytes with a given colour pair.
 * On the console the IPC path packs ConsoleRun frames onto the lane ('\n'
 * travels inside the text — it is content, not a control record); with no
 * display the text is said into the log ring.
 *
 * ‼ A CHARACTER IS NEVER CUT IN TWO. Every piece this file hands on — a lane
 * frame, a run said into the ring — ends on a whole UTF-8 character. A frame
 * is the unit the display interleaves with every other writer and with the
 * kernel's own lines; half a character on one side of another writer's line
 * is two pieces of rubbish in the display and in the ring, which the serial
 * line and `logsave` read. The length is in the lead byte, so the cut is
 * moved back to before it.
 * =========================================================================== */

/* No display: the text said into the log ring, a page at most each time, a
 * piece cut at the last newline that fits and never inside a character. */
static void ring_say(const char *bytes, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > LOG_SAY_MAX) {
            take = LOG_SAY_MAX;
            size_t line = 0;
            for (size_t k = take; k > 0; k--) {
                if (bytes[off + k - 1u] == '\n') { line = k; break; }
            }
            take = line ? line : BoxUtf8Whole(bytes + off, take);
        }
        int rc = log_say(bytes + off, (uint32_t)take, NULL, NULL);
        if (rc != OK) {
            /* The ring is the last road there is: what it will not take is
             * lost, and that is said — once, on the kernel's own line, which
             * does not come back through here. */
            static bool said = false;
            if (!said) {
                said = true;
                kdbg_print("[print] the log ring would not take this program's "
                           "text (%d); what it refused is lost", rc);
            }
            return;
        }
        off += take;
    }
}

static void emit_run(StrandPrintState *ps, const char *bytes, int len, Color fg, Color bg)
{
    if (len <= 0) return;

    fg = BoxColorOnWire(fg);
    bg = BoxColorOnWire(bg);

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        if (ps->run.len > 0 && (ps->run.fg != fg || ps->run.bg != bg))
            lane_flush_run(ps);

        int off = 0;
        while (off < len && ps->lane_state == LANE_OPEN) {
            if (ps->run.len == 0) {
                ps->run.fg = fg;
                ps->run.bg = bg;
            }
            uint32_t space = CONSOLE_RUN_TEXT_MAX - ps->run.len;
            uint32_t chunk = (uint32_t)(len - off);
            if (chunk > space)
                chunk = (uint32_t)BoxUtf8Whole(bytes + off, space);
            if (chunk == 0) {
                /* The frame has no room for the next whole character. The
                 * cut leaves either nothing or the unfinished line, which
                 * the character then follows into the next frame. */
                lane_flush_lines(ps);
                continue;
            }
            memcpy(ps->run.text + ps->run.len, bytes + off, chunk);
            ps->run.len = (uint8_t)(ps->run.len + chunk);
            off += (int)chunk;
            if (ps->run.len == CONSOLE_RUN_TEXT_MAX) lane_flush_lines(ps);
        }
        /* A finished line leaves NOW; only an unfinished tail stays staged.
         * A strand's completed lines must never sit in its frame waiting for
         * the frame to fill: measured, a worker's last two lines stayed
         * behind while the main strand exited the cabin, the push then found
         * the lane closed and the lines fell off the console — after the
         * shell's next prompt, glued to it. */
        if (ps->lane_state == LANE_OPEN && memchr(ps->run.text, '\n', ps->run.len))
            lane_flush_lines(ps);
        if (off >= len) return;

        /* The daemon died mid-run (lane_flush_run above fell back); the
         * pending frame was already re-emitted, deliver the remainder the
         * same way. */
        bytes += off;
        len   -= off;
    }

    ring_say(bytes, (size_t)len);
}

/* ===========================================================================
 * The arrow — `cmd -> file`
 *
 * When the shell filled the Luggage with a `screen #ID`, what the program
 * prints is added to the end of that file instead of the screen: bytes only,
 * the colours a run carries have no place in a file. The end is found once, as
 * the file's size when the first byte comes, and moved on by what is written;
 * strands share one offset under one lock. The person's screen is still
 * reached by print_to_screen — the editor's echo and a prompt are for the
 * person, not for the file. A file that will not take the output ends the
 * arrow: it is said on the screen once and everything goes there from then on.
 *
 * What is printed is gathered, and added to the file a gathering at a time —
 * when it is full, and at io_flush, which exit() and strand_exit() call — not a
 * write a call. A write is a change of the volume, and a run is as short as a
 * colour: MEASURED, `loupe say.elf -> say.hex`, which colours each byte by its
 * kind, wrote its 772 KB in 193 thousand writes and took 87 s, longer than the
 * same lines took to draw; gathered, 1 s. The shell reads the file once the
 * program is gone, and by then the program's exit has added the rest; a
 * program that dies without exiting loses what it had gathered, as its
 * unfinished line on the screen is lost.
 * =========================================================================== */
#define ARROW_GATHER 65536u

static struct {
    umutex_t lock;
    uint64_t offset;
    bool     found;
    bool     given_up;
    char    *gathered;     /* ARROW_GATHER bytes, had with the first byte */
    size_t   used;
} g_arrow = { UMUTEX_INIT, 0, false, false, NULL, 0 };

/* As much of `bytes` as the file takes, added at its end. */
static size_t arrow_put_locked(file_id_t file, const char *bytes, size_t len)
{
    if (!g_arrow.found) {
        FileMetadata metadata;
        if (FileMetadataRead(file, &metadata) != 0) return 0;
        g_arrow.offset = metadata.Size;
        g_arrow.found  = true;
        FileMetadataRelease(&metadata);
    }

    size_t took = 0;
    while (took < len) {
        int64_t wrote = fwrite(file, g_arrow.offset, bytes + took, len - took);
        if (wrote <= 0) break;
        g_arrow.offset += (uint64_t)wrote;
        took           += (size_t)wrote;
    }
    return took;
}

/* What was gathered, into the file. False when the file would not take all
 * of it: the arrow is given up, and *Spill is the part never written. */
static bool arrow_empty_locked(file_id_t file, const char **spill, size_t *spill_len)
{
    size_t took = arrow_put_locked(file, g_arrow.gathered, g_arrow.used);
    bool   ok   = took == g_arrow.used;
    if (!ok) {
        *spill     = g_arrow.gathered + took;
        *spill_len = g_arrow.used - took;
    }
    g_arrow.used = 0;
    return ok;
}

/* The arrow given up, by the one who gave it up: said once, and the bytes it
 * gathered and the file never took shown in its place. Nobody gathers once it
 * is given up, so the gathering is this caller's alone to show and free. */
static void arrow_end(StrandPrintState *ps, const char *spill, size_t spill_len)
{
    print_to_screen("[print] the file after -> would not take the output; it goes to the screen\n");
    if (spill_len) emit_run(ps, spill, (int)spill_len, COLOR_DEFAULT, COLOR_DEFAULT);

    umutex_lock(&g_arrow.lock);
    free(g_arrow.gathered);
    g_arrow.gathered = NULL;
    umutex_unlock(&g_arrow.lock);
}

/* Did the file take these bytes — written, or gathered to be; false sends
 * them to the screen. Without memory for a gathering each call is written as
 * it comes, as it always was. */
static bool arrow_write(StrandPrintState *ps, const char *bytes, size_t len)
{
    file_id_t file = form_screen_file();
    if (file == 0) return false;

    const char *spill     = NULL;
    size_t      spill_len = 0;
    bool        ended     = false;

    umutex_lock(&g_arrow.lock);
    bool ok = !g_arrow.given_up;
    if (ok && !g_arrow.gathered) g_arrow.gathered = malloc(ARROW_GATHER);
    if (ok && g_arrow.gathered && len > ARROW_GATHER - g_arrow.used)
        ok = arrow_empty_locked(file, &spill, &spill_len);
    if (ok) {
        if (g_arrow.gathered && len <= ARROW_GATHER - g_arrow.used) {
            memcpy(g_arrow.gathered + g_arrow.used, bytes, len);
            g_arrow.used += len;
        } else {
            ok = arrow_put_locked(file, bytes, len) == len;
        }
    }
    if (!ok && !g_arrow.given_up) {
        g_arrow.given_up = true;
        ended = true;
    }
    umutex_unlock(&g_arrow.lock);

    if (ended) arrow_end(ps, spill, spill_len);
    return ok;
}

/* The gathering into the file now. */
static void arrow_flush(StrandPrintState *ps)
{
    file_id_t file = form_screen_file();
    if (file == 0) return;

    const char *spill     = NULL;
    size_t      spill_len = 0;
    bool        ended     = false;

    umutex_lock(&g_arrow.lock);
    if (!g_arrow.given_up && g_arrow.used > 0 && !arrow_empty_locked(file, &spill, &spill_len)) {
        g_arrow.given_up = true;
        ended = true;
    }
    umutex_unlock(&g_arrow.lock);

    if (ended) arrow_end(ps, spill, spill_len);
}

static void emit_text(StrandPrintState *ps, const char *bytes, int len, Color fg, Color bg)
{
    if (len > 0 && arrow_write(ps, bytes, (size_t)len)) return;
    emit_run(ps, bytes, len, fg, bg);
}

/* ===========================================================================
 * Public output API
 *
 * print() streams the input in fixed-size chunks: each chunk, cut on a whole
 * character, is copied into a stack scratch buffer and handed to emit_run.
 * There is no upper bound on input length — the loop keeps consuming until
 * the source NUL. The chunk size is purely an internal staging optimisation.
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
 * — boxos_glyph_fold.h, in the hands that draw — which is the only code that
 * knows what a cell can hold. A chunk ends on a whole character (BoxUtf8Whole),
 * so nothing downstream ever holds half of one. */
static void emit_chunks(StrandPrintState *ps, const char* data, size_t len,
                        bool newline, bool screen)
{
    char   chunk[256];
    size_t src = 0;
    do {
        size_t take = len - src;
        if (take > sizeof(chunk) - 1) {
            take = BoxUtf8Whole(data + src, sizeof(chunk) - 1);
        }
        if (take) memcpy(chunk, data + src, take);
        src += take;

        int chunk_pos = (int)take;
        if (newline && src >= len) chunk[chunk_pos++] = '\n';
        if (chunk_pos == 0) break;
        if (screen) emit_run(ps, chunk, chunk_pos, ps->color_fg, ps->color_bg);
        else        emit_text(ps, chunk, chunk_pos, ps->color_fg, ps->color_bg);
    } while (src < len);
}

/* `screen` is the person's screen whatever the arrow says. */
static void print_chunks(const char *data, size_t len, bool screen)
{
    emit_chunks(print_state_self(), data, len, false, screen);
}

void print(const char* str)
{
    if (!str) return;
    print_chunks(str, strlen(str), false);
}

void print_bytes(const char* data, size_t len)
{
    if (!data || len == 0) return;
    print_chunks(data, len, false);
}

void print_to_screen(const char* str)
{
    if (!str) return;
    print_chunks(str, strlen(str), true);
}

void print_bytes_to_screen(const char* data, size_t len)
{
    if (!data || len == 0) return;
    print_chunks(data, len, true);
}

void println(const char* str)
{
    StrandPrintState *ps = print_state_self();

    /* The newline goes with the text — the same frame on the lane, the same
     * run said into the ring — never as an emit of its own. */
    if (str) emit_chunks(ps, str, strlen(str), true, false);
    else     emit_text(ps, "\n", 1, ps->color_fg, ps->color_bg);
}

/* With no display there is nothing to clear: the log ring is an account,
 * and an account is not wiped. */
void clear(void)
{
    StrandPrintState *ps = print_state_self();

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        lane_flush_run(ps);
        if (ps->lane_state == LANE_OPEN) {
            ConsoleRun f;
            memset(&f, 0, sizeof(f));
            f.kind = CONSOLE_RUN_CLEAR;
            f.fg   = COLOR_DEFAULT;
            f.bg   = COLOR_DEFAULT;
            f.order = console_order_next(ps);
            (void)lane_push(ps, &f);
        }
    }
}

/* ===========================================================================
 * printf — C's conversions, written by format.c, and BoxOS's %color and
 * %bgcolor, which consume one Color (uint32_t RGB) and switch the foreground
 * or the background of the text that follows.
 *
 * The text gathers in a working buffer and goes out as a coloured run
 * whenever %color or %bgcolor changes the colour, the buffer fills, or the
 * format ends.
 *
 * UTF-8: passes through whole, format string and %s alike. What a character
 * looks like is settled once, at the font, by the only code that holds it
 * (boxos_glyph_fold.h, where the glyphs are drawn) — not here, where the font cannot be
 * seen and the substitution reached the log as well as the glass.
 *
 * One printf produces N×emit_run calls (N = colour changes); on the lane
 * everything coalesces into frames and flushes lazily.
 * =========================================================================== */

#define PRINTF_BUFLEN 512

/* The sink printf gives format.c: a run of text in the colours of the moment. */
typedef struct {
    BoxFormatSink     sink;
    StrandPrintState *ps;
    Color             fg;
    Color             bg;
    int               used;
    char              text[PRINTF_BUFLEN];
} PrintfRun;

static void printf_flush(PrintfRun *run)
{
    if (run->used > 0) {
        emit_text(run->ps, run->text, run->used, run->fg, run->bg);
        run->used = 0;
    }
}

/* A full buffer goes out up to its last whole character; the beginning of
 * one the format has not finished saying stays for the next run. */
static void printf_put(BoxFormatSink *sink, const char *bytes, size_t count)
{
    PrintfRun *run = (PrintfRun *)sink;

    while (count != 0) {
        size_t room = (size_t)(PRINTF_BUFLEN - run->used);
        size_t step = count < room ? count : room;

        memcpy(run->text + run->used, bytes, step);
        run->used += (int)step;
        bytes += step;
        count -= step;
        if (run->used == PRINTF_BUFLEN) {
            size_t whole = BoxUtf8Whole(run->text, PRINTF_BUFLEN);
            if (whole == 0) whole = PRINTF_BUFLEN;
            size_t held = PRINTF_BUFLEN - whole;
            run->used = (int)whole;
            printf_flush(run);
            memmove(run->text, run->text + whole, held);
            run->used = (int)held;
        }
    }
}

static void printf_color(BoxFormatSink *sink, int background, uint32_t rgb)
{
    PrintfRun *run = (PrintfRun *)sink;

    printf_flush(run);
    if (background) run->bg = (Color)rgb;
    else            run->fg = (Color)rgb;
}

/* WEAK on purpose. boxlib's printf is BoxOS's coloured-run console printer,
 * and boxcxx's <cstdio> defines a printf of its own: a program that links
 * boxcxx gets THAT one, since two functions cannot share an ELF symbol. Both
 * write C's full set of conversions; this one's are format.c's.
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
    PrintfRun run;
    va_list args;
    int written;

    run.sink.Put = printf_put;
    run.sink.Color = printf_color;
    run.ps = ps;
    run.fg = ps->color_fg;
    run.bg = ps->color_bg;
    run.used = 0;

    va_start(args, fmt);
    written = BoxFormat(&run.sink, fmt, args);
    va_end(args);
    printf_flush(&run);
    return written;
}

/* Weak, as printf is: a C++ program links <cstdio>'s, which is the same
 * function by name (boxcxx std/cstdio). */
__attribute__((weak)) int vsnprintf(char *str, size_t n, const char *fmt, va_list args)
{
    return BoxFormatInto(str, n, fmt, args);
}

__attribute__((weak)) int snprintf(char *str, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(str, n, fmt, args);
    va_end(args);
    return written;
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
 * The listens are COUNTED, and only the last console_unlisten gives the ear
 * back: a strand that reads in two places at once — a Stage open around a
 * readline — keeps hearing after the inner one is done.
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
        if (daemon_say(ps, req, sizeof(req)) == 0) {
            ps->listeners++;
            return ps->ear;
        }

        /* Nobody at that pid any more: the daemon is gone — daemon_say has
         * already waited out every refusal that was merely a busy one. Say so
         * the way a failed push would, and hear the keyboard directly from
         * here on. */
        Brook *dead = (Brook *)ps->lane;
        ps->lane = NULL;
        lane_resay(ps, dead);
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
        !(ps->ear_flags & EAR_CLAIMED) || ps->listeners == 0)
        return;
    if (--ps->listeners > 0) return;   /* somebody in this strand still reads */
    uint8_t  req[6];
    uint32_t gen = strand_self_generation();
    req[0] = DISP_CMD_LISTEN;
    memcpy(req + 1, &gen, sizeof(gen));
    req[5] = 0;
    /* Given back the same way it was taken: a daemon that is merely behind is
     * waited for, because a lane that fails to hand the ear back keeps it, and
     * the next program to read would hear nothing. */
    (void)daemon_say(ps, req, sizeof(req));
}

/* Once heard, for the whole program: every strand reads the same input. */
static bool g_input_over;

void console_input_ends(void)
{
    __atomic_store_n(&g_input_over, true, __ATOMIC_RELEASE);
}

bool console_input_ended(void)
{
    return __atomic_load_n(&g_input_over, __ATOMIC_ACQUIRE);
}

/* A cursor step is a frame on the lane, after whatever text is pending, so it
 * lands in the order it was said. Nothing here knows, or needs to know, where
 * the caret is or how wide the screen is: a step says how FAR, and where that
 * lands is the console's business. With no display there is no caret to move
 * — the log ring is an account, not a screen — and a step says so. */
bool console_step(int32_t delta)
{
    StrandPrintState *ps = print_state_self();
    if (g_io_mode != IO_MODE_IPC || !lane_ensure(ps)) return false;
    if (delta == 0) return true;

    lane_flush_run(ps);
    if (ps->lane_state != LANE_OPEN) return false;
    ConsoleRun f;
    memset(&f, 0, sizeof(f));
    f.kind = CONSOLE_RUN_STEP;
    f.len  = sizeof(delta);
    memcpy(f.text, &delta, sizeof(delta));
    f.fg   = BoxColorOnWire(ps->color_fg);
    f.bg   = BoxColorOnWire(ps->color_bg);
    f.order = console_order_next(ps);
    return lane_push(ps, &f);
}

/* Hints ride the lane as frames of their own kind, cut on whole characters
 * like text, after whatever text is pending. A hint the lane would not take is
 * gone with it — it was never part of the account, so the ring is no place to
 * put it instead. */
void console_hint(const char *bytes, uint32_t len)
{
    StrandPrintState *ps = print_state_self();
    if (len == 0 || g_io_mode != IO_MODE_IPC || !lane_ensure(ps)) return;

    lane_flush_run(ps);
    uint32_t off = 0;
    while (off < len && ps->lane_state == LANE_OPEN) {
        uint32_t take = len - off;
        if (take > CONSOLE_RUN_TEXT_MAX)
            take = (uint32_t)BoxUtf8Whole(bytes + off, CONSOLE_RUN_TEXT_MAX);
        ConsoleRun f;
        memset(&f, 0, sizeof(f));
        f.kind  = CONSOLE_RUN_HINT;
        f.len   = (uint8_t)take;
        memcpy(f.text, bytes + off, take);
        f.fg    = BoxColorOnWire(ps->color_fg);
        f.bg    = BoxColorOnWire(ps->color_bg);
        f.order = console_order_next(ps);
        if (!lane_push(ps, &f)) return;
        off += take;
    }
}

/* Everything this strand has printed, into its lane, and the number of the
 * last frame it pushed there: the mark a question that cares about the
 * console's order carries (box/display.h). 0 when the strand has no lane. */
uint64_t console_order_mark(void)
{
    StrandPrintState *ps = print_state_self();
    io_flush_state(ps);
    return ps->lane_state == LANE_OPEN ? ps->last_order : 0;
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
