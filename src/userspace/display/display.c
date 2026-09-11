
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


typedef struct ConsoleLane {
    Brook              *brook;
    uint32_t            owner_pid;
    uint32_t            owner_gen;
    uint32_t            number;
    TouchTagPair        ear;
    bool                closed;
    bool                has_pending;
    ConsoleRun          pending;
    struct ConsoleLane *next;
} ConsoleLane;

typedef struct FreeNumber {
    uint32_t           number;
    struct FreeNumber *next;
} FreeNumber;

static uint64_t *g_order;

static ConsoleLane *g_lanes;
static FreeNumber  *g_free_numbers;
static uint32_t     g_next_number;

static uint32_t g_cur_fg;
static uint32_t g_cur_bg;
static bool     g_cur_set;

static TouchTag g_kb    = TOUCH_TAG_INVALID;
static TouchTag g_pdied = TOUCH_TAG_INVALID;


typedef struct Ear {
    ConsoleLane *lane;
    struct Ear  *below;
} Ear;

static Ear *g_ear;

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
    if (!e) return;
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


static void render_run(const ConsoleRun *f)
{
    if (f->kind == CONSOLE_RUN_CLEAR) {
        vga_clear_rgb(f->fg, f->bg);
        g_cur_fg  = f->fg;
        g_cur_bg  = f->bg;
        g_cur_set = true;
        return;
    }
    if (f->kind == CONSOLE_RUN_STEP) {
        int32_t delta;
        memcpy(&delta, f->text, sizeof(delta));
        vga_step_cursor(delta);
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

    char     seg[CONSOLE_RUN_TEXT_MAX + 1];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = f->text[i];
        if (c == '\n' || c == '\b' || (unsigned char)c >= 0x20) seg[pos++] = c;
    }
    if (pos > 0) {
        seg[pos] = '\0';
        vga_puts(seg);
    }
}

static void render_took(int rc)
{
    static bool said = false;
    if (rc == 0 || said) return;
    said = true;
    kdbg_print("[display] the console refused a render (%d) — what it carried "
               "did not reach the glass", rc);
}

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
    return did;
}

static void lane_free(ConsoleLane *ln)
{
    EarDrop(ln);
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
    free(ln);
}


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


static Touch g_key;
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
        EarDrop(g_ear->lane);
        did = true;
    }
    return did;
}


static int say_to(uint32_t pid, const uint8_t *msg, uint16_t len)
{
    int rc = send(pid, msg, len);
    if (rc == 0) return 0;
    error_t why = box_errno_of(rc);
    if (why == ERR_ROUTE_TARGET_FULL || why == ERR_NO_MEMORY) {
        static bool refused_before = false;
        if (!refused_before) {
            refused_before = true;
            kdbg_print("[display] pid %u would not take its lane answer just "
                       "now (%s); it was not re-sent, and that cabin waits "
                       "until it asks again", pid,
                       why == ERR_ROUTE_TARGET_FULL ? "its ring is full"
                                                    : "no room for the record");
        }
    }
    return rc;
}

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
    if (!order_ensure()) {
        uint8_t refusal = DISP_CMD_LANE;
        (void)say_to(requester, &refusal, 1);
        return;
    }

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
        uint8_t refusal = DISP_CMD_LANE;
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
            if (len >= 5) {
                uint32_t gen;
                memcpy(&gen, data + 1, sizeof(gen));
                grant_lane(entry.sender_pid, gen);
            }
            break;
        case DISP_CMD_LISTEN:
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


int main(void)
{
    io_set_mode(IO_MODE_VGA);

    g_kb    = touch_pair_choose(touch_intern(TOUCH_TAG_KEYBOARD));
    g_pdied = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    if (g_kb    != TOUCH_TAG_INVALID) touch_claim(g_kb,    TOUCH_REST, 0, 0);
    if (g_pdied != TOUCH_TAG_INVALID) touch_claim(g_pdied, TOUCH_REST, 0, 0);

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
        progressed |= ipc_step();
        if (progressed) continue;

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