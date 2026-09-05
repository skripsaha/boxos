/*
 * display.c — the console daemon.
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
 * Input is a state machine fed by "keyboard" Touch events. READLINE echoes
 * and edits; GETCHAR hands over the next character raw. While nobody asks
 * for input, keys are deliberately NOT consumed — the kernel TouchRing and
 * the per-strand tag stash bank them, so type-ahead needs no third buffer.
 *
 * process:died closes what the dead leave behind: granted-but-never-
 * attached lanes are revoked, attached lanes are drained to the last frame
 * (a dying process' final words reach the screen) and closed on
 * STREAM_CLOSED, pending input requests of the dead are dropped.
 */

#include "box/print.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/ipc.h"
#include "box/touch.h"
#include "box/brook.h"
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

/* ─────────────────────────────────────────────────────────────────────────
 * Lanes
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct ConsoleLane {
    Brook              *brook;
    uint32_t            owner_pid;   /* the strand the lane was granted to */
    uint32_t            number;      /* N of "console:N" */
    bool                closed;      /* drained to STREAM_CLOSED / revoked */
    bool                has_pending; /* `pending` holds the lane's head frame */
    ConsoleRun          pending;     /* popped but not yet rendered (merge) */
    struct ConsoleLane *next;
} ConsoleLane;

typedef struct FreeNumber {
    uint32_t           number;
    struct FreeNumber *next;
} FreeNumber;

static ConsoleLane *g_lanes;         /* append at tail — grant order */
static FreeNumber  *g_free_numbers;  /* reuse pool for lane numbers */
static uint32_t     g_next_number;   /* fresh numbers when the pool is dry */

/* The daemon's current on-screen pair — frames set it only when it differs. */
static uint32_t g_cur_fg;
static uint32_t g_cur_bg;
static bool     g_cur_set;

/* Touch tags, interned once at startup. */
static TouchTag g_kb    = TOUCH_TAG_INVALID;
static TouchTag g_pdied = TOUCH_TAG_INVALID;

/* ─────────────────────────────────────────────────────────────────────────
 * Pending input — FIFO of READLINE/GETCHAR requests; the head is active.
 * ───────────────────────────────────────────────────────────────────────── */

enum { INPUT_READLINE = 1, INPUT_GETCHAR = 2 };

typedef struct PendingInput {
    uint8_t              kind;       /* INPUT_* */
    uint8_t              echo;
    uint16_t             cap;        /* line capacity incl. NUL (READLINE) */
    uint16_t             len;        /* typed so far */
    uint32_t             requester;
    char                *buf;        /* cap bytes (READLINE), NULL (GETCHAR) */
    struct PendingInput *next;
} PendingInput;

static PendingInput *g_input_head;
static PendingInput *g_input_tail;

static void input_pop_head(void)
{
    PendingInput *pi = g_input_head;
    if (!pi) return;
    g_input_head = pi->next;
    if (!g_input_head) g_input_tail = NULL;
    if (pi->buf) free(pi->buf);
    free(pi);
}

static bool kb_step(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Frame rendering
 * ───────────────────────────────────────────────────────────────────────── */

static void render_run(const ConsoleRun *f)
{
    if (f->kind == CONSOLE_RUN_CLEAR) {
        vga_clear_rgb(f->fg, f->bg);
        g_cur_fg  = f->fg;
        g_cur_bg  = f->bg;
        g_cur_set = true;
        return;
    }
    if (f->kind != CONSOLE_RUN_TEXT) return;

    if (!g_cur_set || g_cur_fg != f->fg || g_cur_bg != f->bg) {
        vga_setcolor_rgb(f->fg, f->bg);
        g_cur_fg  = f->fg;
        g_cur_bg  = f->bg;
        g_cur_set = true;
    }

    uint32_t len = f->len;
    if (len > CONSOLE_RUN_TEXT_MAX) len = CONSOLE_RUN_TEXT_MAX;

    /* '\n' is content; other control bytes are dropped (the writer's UTF-8
     * filter never emits them, so anything else here is line noise). */
    char     seg[CONSOLE_RUN_TEXT_MAX + 1];
    uint32_t pos = 0;
    for (uint32_t i = 0; i <= len; i++) {
        int at_end = (i == len);
        char c     = at_end ? '\0' : f->text[i];
        if (!at_end && c != '\n' && (unsigned char)c >= 0x20) {
            seg[pos++] = c;
            continue;
        }
        if (pos > 0) {
            seg[pos] = '\0';
            vga_puts(seg);
            pos = 0;
        }
        if (!at_end && c == '\n') vga_newline();
    }
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
    if (rc == -ERR_STREAM_CLOSED) ln->closed = true;
    return false;
}

/* Render pending frames across ALL lanes in global push order — always the
 * frame with the smallest push-time TSC first. Causal chains (a child's
 * last words → its death → the shell's next prompt) are milliseconds apart
 * in TSC, so cause order survives any scheduling of the daemon; a lane is
 * never allowed to overtake an older frame parked on another lane. The
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
            if (lane_refill(ln) &&
                (!best ||
                 (int64_t)(ln->pending.tsc - best->pending.tsc) < 0)) {
                best = ln;
            }
        }
        if (!best) break;

        render_run(&best->pending);
        best->has_pending = false;
        did = true;
        if ((++rendered & 31u) == 0) {
            vga_commit();
            kb_step();
            vga_begin();
        }
    }
    vga_commit();
    return did;
}

static void lane_free(ConsoleLane *ln)
{
    brook_release(ln->brook);

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
 * lanes need nothing here, because the min-TSC merge already renders a
 * dead writer's banked tail before anything a survivor pushes in reaction
 * to the death (the tail's frames carry strictly older stamps).
 *
 * The revoke is guarded by liveness: a recycled pid (the death event names
 * an earlier incarnation) must not cost the CURRENT incarnation a lane it
 * was just granted and is about to attach to. If proc_info says the pid is
 * alive, the grant stays; the stale lane then lives at most until the new
 * incarnation's own death. The guard's own race window (the pid recycling
 * between the event and the query) is micro-seconds wide and fails SAFE —
 * a revoked-too-early lane makes the writer's open fail, which it reports
 * and survives by falling back to direct VGA. */
static bool death_step(void)
{
    bool  did = false;
    Touch t;

    while (touch_try_pop_tag(g_pdied, &t)) {
        did = true;
        if (t.payload_len < sizeof(TouchProcessDied)) continue;

        TouchProcessDied d;
        memcpy(&d, t.payload, sizeof(d));

        for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
            if (ln->owner_pid != d.pid || ln->closed) continue;
            if (!brook_writer_ever_attached(ln->brook)) {
                proc_info_t info;
                if (proc_info((uint16_t)d.pid, &info) == 0) continue;
                ln->closed = true;
            }
        }

        /* Input requests of the dead: nobody is left to answer. */
        PendingInput **pp = &g_input_head;
        while (*pp) {
            PendingInput *pi = *pp;
            if (pi->requester == d.pid) {
                proc_info_t info;
                if (proc_info((uint16_t)d.pid, &info) == 0) { pp = &pi->next; continue; }
                *pp = pi->next;
                if (g_input_tail == pi) {
                    g_input_tail = NULL;
                    for (PendingInput *q = g_input_head; q; q = q->next)
                        g_input_tail = q;
                }
                if (pi->buf) free(pi->buf);
                free(pi);
                continue;
            }
            pp = &pi->next;
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
 * Input machine
 * ───────────────────────────────────────────────────────────────────────── */

static void input_reply_line(PendingInput *pi)
{
    static uint8_t reply[4 + 1024];

    uint32_t len = pi->len;
    memcpy(reply, &len, 4);
    memcpy(reply + 4, pi->buf, len);
    send(pi->requester, reply, (uint16_t)(4 + len));
}

static void feed_key(char ch)
{
    PendingInput *pi = g_input_head;
    if (!pi || ch == 0) return;

    if (pi->kind == INPUT_GETCHAR) {
        uint8_t b = (uint8_t)ch;
        send(pi->requester, &b, 1);
        input_pop_head();
        return;
    }

    /* READLINE line discipline — same rules kb_readline always had. */
    if (ch == '\b' || ch == 0x7F) {
        if (pi->len > 0) {
            pi->len--;
            if (pi->echo) vga_puts("\b \b");
        }
        return;
    }
    if (ch == '\r' || ch == '\n') {
        pi->buf[pi->len] = '\0';
        if (pi->echo) vga_puts("\n");
        input_reply_line(pi);
        input_pop_head();
        return;
    }
    if (pi->len < pi->cap - 1) {
        pi->buf[pi->len++] = ch;
        if (pi->echo) vga_putchar(ch);
    }
}

/* Consume keys ONLY while someone is asking — otherwise they stay banked
 * on the TouchRing/stash as type-ahead for the next request. (The stash
 * matters: death_step's tag-selective pop parks any keys it runs into
 * there, so this must always pull by tag, never gate on ring emptiness.) */
static bool kb_step(void)
{
    bool  did = false;
    Touch t;
    while (g_input_head && touch_try_pop_tag(g_kb, &t)) {
        did = true;
        if (t.payload_len < sizeof(kb_event_t)) continue;
        const kb_event_t *kp = (const kb_event_t *)t.payload;
        feed_key(kp->ascii);
    }
    return did;
}

/* ─────────────────────────────────────────────────────────────────────────
 * IPC — grants, input requests, ping.
 * ───────────────────────────────────────────────────────────────────────── */

static void grant_lane(uint32_t requester)
{
    /* Idempotent per requester. A strand IS a process here, so one lane per
     * pid is the whole rule — and a writer whose grant reply was lost (or
     * who simply asked again while the answer was in flight) must get the
     * SAME lane back, never a second one. Granting twice would strand the
     * first Brook: the daemon would hold a reader nobody writes to and the
     * writer would push into whichever tag it learned last, leaking a lane
     * per re-ask. Re-answering makes the writer's re-ask safe, which is what
     * lets its wait use a re-ask as its liveness probe. */
    for (ConsoleLane *ln = g_lanes; ln; ln = ln->next) {
        if (ln->owner_pid != requester || ln->closed) continue;
        char     again[32];
        uint8_t  reply[2 + sizeof(again)];
        memcpy(again, "console:", 8);
        uint64_to_str(ln->number, again + 8, sizeof(again) - 8);
        size_t alen = strlen(again);
        reply[0] = DISP_CMD_LANE;
        memcpy(reply + 1, again, alen + 1);
        send(requester, reply, (uint16_t)(2 + alen));
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
        FreeNumber *fn = (FreeNumber *)malloc(sizeof(FreeNumber));
        if (fn) { fn->number = number; fn->next = g_free_numbers; g_free_numbers = fn; }
        uint8_t refusal = DISP_CMD_LANE;      /* 1-byte reply = honest no */
        send(requester, &refusal, 1);
        kdbg_print("[display] lane grant failed for pid %u", requester);
        return;
    }

    ln->brook     = b;
    ln->owner_pid = requester;
    ln->number    = number;
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
    send(requester, reply, (uint16_t)(2 + tlen));
}

static void queue_input(uint8_t kind, uint32_t requester, uint16_t cap, uint8_t echo)
{
    PendingInput *pi = (PendingInput *)malloc(sizeof(PendingInput));
    if (!pi) return;
    memset(pi, 0, sizeof(*pi));
    pi->kind      = kind;
    pi->echo      = echo;
    pi->requester = requester;
    if (kind == INPUT_READLINE) {
        pi->cap = cap > 0 ? cap : 128;
        if (pi->cap > 1024) pi->cap = 1024;
        pi->buf = (char *)malloc(pi->cap);
        if (!pi->buf) { free(pi); return; }
    }

    if (g_input_tail) g_input_tail->next = pi;
    else              g_input_head       = pi;
    g_input_tail = pi;

    /* The requester's prompt was pushed on its lane BEFORE this request was
     * sent; render everything pending now (in global order) so the prompt
     * is on screen before the echo of the first key. */
    lanes_render(2u * CONSOLE_LANE_FRAMES);
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
        case DISP_CMD_LANE:
            grant_lane(entry.sender_pid);
            break;
        case DISP_CMD_READLINE:
            if (len >= 4) {
                uint16_t cap = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
                queue_input(INPUT_READLINE, entry.sender_pid, cap, data[3]);
            }
            break;
        case DISP_CMD_GETCHAR:
            queue_input(INPUT_GETCHAR, entry.sender_pid, 0, 0);
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

    g_kb    = touch_pair_choose(touch_intern(TOUCH_TAG_KEYBOARD));
    g_pdied = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    if (g_kb    != TOUCH_TAG_INVALID) touch_claim(g_kb,    TOUCH_REST, 0, 0);
    if (g_pdied != TOUCH_TAG_INVALID) touch_claim(g_pdied, TOUCH_REST, 0, 0);

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
        progressed |= ipc_step();
        if (progressed) continue;

        /* Nothing moved. Hang the bells FIRST, then take the mark, then look
         * one last time — in that order, and the order is the whole proof.
         * Anything arriving from here on either finds a bell out (and rings,
         * which is itself an arrival on the Result ring), or moves a cursor
         * past the mark, or turns up in the look below. There is no fourth
         * way for it to arrive and no gap between the three. */
        lanes_bell_hang(me);
        TurnInMark mark = box_mark();

        if (kb_step() | death_step() | lanes_step() | ipc_step()) {
            lanes_bell_take();
            continue;
        }

        box_turn_in(mark);
        lanes_bell_take();
    }

    return 0;
}
