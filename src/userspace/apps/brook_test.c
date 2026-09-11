
#include "box/brook.h"
#include "box/memory.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/debug.h"
#include "box/time.h"
#include "box/string.h"
#include "box/error.h"
#include "box/core/result.h"
#include "box/core/cabin.h"

static void wait_ms(uint32_t ms)
{
    uint64_t deadline = 0;
    time_uptime_ms(&deadline);
    deadline += ms;
    for (;;) {
        uint64_t now = 0;
        time_uptime_ms(&now);
        if (now >= deadline) break;
        yield();
    }
}

#define TAG_BASIC    "brook:test:basic"
#define TAG_FULL     "brook:test:full"
#define TAG_EMPTY    "brook:test:empty"
#define TAG_SHAPE    "brook:test:shape"
#define TAG_BUSY     "brook:test:busy"
#define TAG_CROSS    "brook:test:cross"
#define TAG_EOF      "brook:test:eof"
#define TAG_BLOCK    "brook:test:block"
#define TAG_TIMEOUT  "brook:test:timeout"
#define TAG_FROZEN   "brook:test:frozen"
#define TAG_STREAM   "brook:test:stream"

#define ROLE_MAGIC          0xBA
#define ROLE_CHILD_READER   1
#define ROLE_CHILD_EOF_WR   2
#define ROLE_CHILD_DRAIN    3

static int g_passed = 0;
static int g_total  = 0;

static void pass(int n) { g_passed++; g_total++; kdbg_print("[BR %d] PASS", n); }
static void fail(int n, const char *why) { g_total++; kdbg_print("[BR %d] FAIL: %s", n, why); }

static int spawn_role(uint8_t role)
{
    int child = proc_exec("brook_test");
    if (child < 0) return child;
    uint8_t pkt[2] = { ROLE_MAGIC, role };
    send((uint32_t)child, pkt, 2);
    return child;
}

static int wait_child_exit(uint32_t child_pid)
{
    for (int i = 0; i < 800; i++) {
        proc_info_t info;
        int rc = proc_info((uint16_t)child_pid, &info);
        if (rc != 0)                             return 0;
        if (info.state == PROC_STATE_TERMINATED) return 0;
        wait_ms(10);
    }
    return -1;
}


static void role_child_reader(void)
{
    Brook *b = brook_open(TAG_CROSS, 0, 0, BROOK_READER);
    if (!b) { kdbg_print("[BR C-rd] open FAIL"); exit(1); }

    uint8_t frame[8];
    for (int i = 0; i < 32; i++) {
        int rc = brook_pop(b, frame);
        if (rc != 0) { kdbg_print("[BR C-rd] pop FAIL"); brook_release(b); exit(2); }
        uint32_t idx, mag;
        memcpy(&idx, frame,     sizeof(uint32_t));
        memcpy(&mag, frame + 4, sizeof(uint32_t));
        if (idx != (uint32_t)i || mag != 0xDEADBEEFu) {
            kdbg_print("[BR C-rd] content FAIL i=%d", i);
            brook_release(b); exit(3);
        }
    }
    brook_release(b);
    exit(0);
}

static void role_child_eof_writer(void)
{
    Brook *b = brook_open(TAG_EOF, 0, 0, BROOK_WRITER);
    if (!b) { kdbg_print("[BR C-ew] open FAIL"); exit(1); }
    uint8_t frame[16];
    for (int i = 0; i < 4; i++) {
        memset(frame, (uint8_t)(0x10 + i), sizeof(frame));
        int rc = brook_push(b, frame);
        if (rc != 0) { kdbg_print("[BR C-ew] push FAIL"); brook_release(b); exit(2); }
    }
    brook_release(b);
    exit(0);
}

static void role_child_drain(void)
{
    Brook *b = brook_open(TAG_BLOCK, 0, 0, BROOK_READER);
    if (!b) { kdbg_print("[BR C-dr] open FAIL"); exit(1); }
    wait_ms(80);
    uint8_t frame[8];
    int rc = brook_pop(b, frame);
    if (rc != 0) { kdbg_print("[BR C-dr] pop FAIL"); brook_release(b); exit(2); }
    while (brook_try_pop(b, frame) == 0) {}
    brook_release(b);
    exit(0);
}


static void run_parent_tests(void)
{
    {
        Brook *w = brook_open(TAG_BASIC, 32, 64, BROOK_WRITER | BROOK_CREATE);
        Brook *r = brook_open(TAG_BASIC, 32, 64, BROOK_READER);
        if (!w || !r) { fail(1, "open"); if (w) brook_release(w); if (r) brook_release(r); }
        else {
            int ok = 1;
            uint8_t frame[32];
            for (int i = 0; i < 64 && ok; i++) {
                memset(frame, (uint8_t)i, sizeof(frame));
                if (brook_push(w, frame) != 0) ok = 0;
            }
            for (int i = 0; i < 64 && ok; i++) {
                uint8_t got[32];
                if (brook_pop(r, got) != 0) { ok = 0; break; }
                for (int j = 0; j < 32; j++) if (got[j] != (uint8_t)i) { ok = 0; break; }
            }
            brook_release(w);
            brook_release(r);
            if (ok) pass(1); else fail(1, "round trip");
        }
    }

    {
        Brook *w = brook_open(TAG_FULL, 16, 4, BROOK_WRITER | BROOK_CREATE);
        Brook *r = brook_open(TAG_FULL, 16, 4, BROOK_READER);
        if (!w || !r) { fail(2, "open"); if (w) brook_release(w); if (r) brook_release(r); }
        else {
            int ok = 1;
            uint8_t f[16] = {0};
            for (int i = 0; i < 4; i++) if (brook_try_push(w, f) != 0) ok = 0;
            int rc = brook_try_push(w, f);
            if (rc != -ERR_WOULD_BLOCK) ok = 0;
            brook_release(w);
            brook_release(r);
            if (ok) pass(2); else fail(2, "back-pressure");
        }
    }

    {
        Brook *w = brook_open(TAG_EMPTY, 8, 4, BROOK_WRITER | BROOK_CREATE);
        Brook *r = brook_open(TAG_EMPTY, 8, 4, BROOK_READER);
        if (!w || !r) { fail(3, "open"); if (w) brook_release(w); if (r) brook_release(r); }
        else {
            uint8_t f[8];
            int rc = brook_try_pop(r, f);
            brook_release(w);
            brook_release(r);
            if (rc == -ERR_WOULD_BLOCK) pass(3); else fail(3, "rc not WOULD_BLOCK");
        }
    }

    {
        Brook *w = brook_open(TAG_SHAPE, 64, 8, BROOK_WRITER | BROOK_CREATE);
        if (!w) { fail(4, "create"); }
        else {
            Brook *bad = brook_open(TAG_SHAPE, 128, 8, BROOK_READER);
            if (bad) { fail(4, "shape mismatch should fail"); brook_release(bad); }
            else pass(4);
            brook_release(w);
        }
    }

    {
        Brook *w1 = brook_open(TAG_BUSY, 8, 4, BROOK_WRITER | BROOK_CREATE);
        if (!w1) { fail(5, "first writer"); }
        else {
            Brook *w2 = brook_open(TAG_BUSY, 8, 4, BROOK_WRITER);
            if (w2) { fail(5, "second writer should fail"); brook_release(w2); }
            else pass(5);
            brook_release(w1);
        }
    }

    {
        Brook *w = brook_open(TAG_CROSS, 8, 64, BROOK_WRITER | BROOK_CREATE);
        if (!w) { fail(6, "create writer"); }
        else {
            int child = spawn_role(ROLE_CHILD_READER);
            if (child < 0) { fail(6, "spawn"); brook_release(w); }
            else {
                int ok = 1;
                uint8_t frame[8];
                uint32_t magic = 0xDEADBEEFu;
                for (uint32_t i = 0; i < 32 && ok; i++) {
                    memcpy(frame,     &i,     sizeof(uint32_t));
                    memcpy(frame + 4, &magic, sizeof(uint32_t));
                    if (brook_push(w, frame) != 0) ok = 0;
                }
                int code = wait_child_exit((uint32_t)child);
                brook_release(w);
                if (ok && code == 0) pass(6); else fail(6, "cross-cabin");
            }
        }
    }

    {
        Brook *r = brook_open(TAG_EOF, 16, 8, BROOK_READER | BROOK_CREATE);
        if (!r) { fail(7, "create reader"); }
        else {
            int child = spawn_role(ROLE_CHILD_EOF_WR);
            if (child < 0) { fail(7, "spawn"); brook_release(r); }
            else {
                int ok = 1;
                uint8_t frame[16];
                for (int i = 0; i < 4 && ok; i++) {
                    if (brook_pop(r, frame) != 0) ok = 0;
                    for (int j = 0; j < 16 && ok; j++) {
                        if (frame[j] != (uint8_t)(0x10 + i)) ok = 0;
                    }
                }
                wait_child_exit((uint32_t)child);
                int rc = brook_pop(r, frame);
                brook_release(r);
                if (ok && rc == -ERR_STREAM_CLOSED) pass(7);
                else fail(7, "expected END_OF_FILE");
            }
        }
    }

    {
        Brook *w = brook_open(TAG_BLOCK, 8, 4, BROOK_WRITER | BROOK_CREATE);
        if (!w) { fail(8, "create writer"); }
        else {
            int child = spawn_role(ROLE_CHILD_DRAIN);
            if (child < 0) { fail(8, "spawn"); brook_release(w); }
            else {
                int ok = 1;
                uint8_t f[8] = {0};
                wait_ms(20);
                for (int i = 0; i < 4 && ok; i++) {
                    if (brook_push(w, f) != 0) ok = 0;
                }
                if (ok) {
                    int rc = brook_push(w, f);
                    if (rc != 0) ok = 0;
                }
                wait_child_exit((uint32_t)child);
                brook_release(w);
                if (ok) pass(8); else fail(8, "block/wake");
            }
        }
    }

    {
        Brook *w = brook_open(TAG_TIMEOUT, 8, 4, BROOK_WRITER | BROOK_CREATE);
        Brook *r = brook_open(TAG_TIMEOUT, 8, 4, BROOK_READER);
        if (!w || !r) { fail(9, "open"); if (w) brook_release(w); if (r) brook_release(r); }
        else {
            uint8_t f[8];
            uint64_t t0 = 0; time_uptime_ms(&t0);
            int rc = brook_pop_timeout(r, f, 100);
            uint64_t t1 = 0; time_uptime_ms(&t1);
            brook_release(w);
            brook_release(r);
            if (rc == -ERR_TIMEOUT && (t1 - t0) >= 80 && (t1 - t0) <= 500) pass(9);
            else fail(9, "timeout not respected");
        }
    }

    {
        Brook *r = brook_open(TAG_FROZEN, 8, 4, BROOK_READER | BROOK_CREATE);
        Brook *w1 = brook_open(TAG_FROZEN, 8, 4, BROOK_WRITER);
        if (!r || !w1) {
            fail(10, "initial open");
            if (r) brook_release(r);
            if (w1) brook_release(w1);
        } else {
            int ok = 1;

            uint8_t frame[8];
            memset(frame, 0xC1, sizeof(frame));
            if (brook_push(w1, frame) != 0) ok = 0;
            memset(frame, 0xC2, sizeof(frame));
            if (brook_push(w1, frame) != 0) ok = 0;
            brook_release(w1);

            if (ok && brook_pop(r, frame) != 0) ok = 0;
            else if (ok && frame[0] != 0xC1)    ok = 0;
            if (ok && brook_pop(r, frame) != 0) ok = 0;
            else if (ok && frame[0] != 0xC2)    ok = 0;

            if (ok && brook_pop(r, frame) != -ERR_STREAM_CLOSED) ok = 0;

            Brook *w2 = brook_open(TAG_FROZEN, 8, 4, BROOK_WRITER);
            if (w2 != 0) {
                brook_release(w2);
                ok = 0;
            }

            brook_release(r);
            if (ok) pass(10);
            else fail(10, "frozen-session not blocking re-attach");
        }
    }

    {
        Brook *r = brook_open(TAG_STREAM, 8, 8,
                              BROOK_READER | BROOK_CREATE | BROOK_STREAM);
        if (!r) { fail(11, "create reader"); }
        else {
            int ok = 1;
            uint8_t frame[8];

            Brook *w1 = brook_open(TAG_STREAM, 8, 8,
                                   BROOK_WRITER | BROOK_STREAM);
            if (!w1) ok = 0;
            else {
                memset(frame, 0xA1, sizeof(frame));
                if (brook_push(w1, frame) != 0) ok = 0;
                brook_release(w1);
            }

            Brook *w2 = brook_open(TAG_STREAM, 8, 8,
                                   BROOK_WRITER | BROOK_STREAM);
            if (!w2) ok = 0;
            else {
                memset(frame, 0xA2, sizeof(frame));
                if (brook_push(w2, frame) != 0) ok = 0;
                brook_release(w2);
            }

            if (ok) {
                uint8_t f[8];
                if (brook_pop_timeout(r, f, 500) != 0) ok = 0;
                else if (f[0] != 0xA1)                 ok = 0;
                if (ok && brook_pop_timeout(r, f, 500) != 0) ok = 0;
                else if (ok && f[0] != 0xA2)           ok = 0;
            }

            brook_release(r);
            if (ok) pass(11);
            else fail(11, "STREAM re-attach drain");
        }
    }

    kdbg_print("[BR *] %d/%d", g_passed, g_total);
    if (g_passed == g_total) kdbg_print("[BR *] PASS");
    else                     kdbg_print("[BR *] FAIL");
}

int main(void)
{
    {
        Result r;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (!receive_wait(&r, 200)) break;
            if (r.data_length >= 2 && r.data_addr != 0) {
                const uint8_t *buf = (const uint8_t *)(uintptr_t)r.data_addr;
                if (buf[0] == ROLE_MAGIC) {
                    switch (buf[1]) {
                    case ROLE_CHILD_READER:    role_child_reader();    break;
                    case ROLE_CHILD_EOF_WR:    role_child_eof_writer(); break;
                    case ROLE_CHILD_DRAIN:     role_child_drain();      break;
                    default: exit(255);
                    }
                    exit(0);
                }
            }
        }
    }

    run_parent_tests();
    exit(g_passed == g_total ? 0 : 1);
    return 0;
}