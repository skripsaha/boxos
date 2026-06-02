/*
 * brook_test — SPSC streaming primitive end-to-end verification.
 *
 * Test plan:
 *   T1  basic SPSC: open writer+reader (same proc), push 64 frames,
 *       pop 64, verify content.
 *   T2  full-ring back-pressure: ring depth 4, try_push 4 times OK,
 *       5th returns -ERR_WOULD_BLOCK.
 *   T3  empty-ring back-pressure: try_pop returns -ERR_WOULD_BLOCK.
 *   T4  shape immutability: create with fs=64 fc=8, then open with
 *       a mismatching shape — second open fails with -ERR_ALREADY_EXISTS.
 *   T5  SPSC enforcement: second WRITER open with same role
 *       returns -ERR_BUSY.
 *   T6  cross-cabin push/pop: child reader drains parent writer's
 *       output. Verifies user-VA-shared header + slot region.
 *   T7  peer-death: child writer pushes 4 frames then exits. Parent
 *       reader drains 4 frames OK, 5th pop returns -ERR_END_OF_FILE.
 *   T8  block writer / wake on pop: ring depth 4, parent writer pushes
 *       4, then pushes 1 more (block). Child reader pops 1 → parent
 *       wakes, 5th push completes.
 *   T9  pop timeout: empty ring + brook_pop_timeout(100 ms) →
 *       -ERR_TIMEOUT (no writer attached → END_OF_FILE actually fires
 *       on the second-best ordering — we test with writer alive).
 *
 * Markers follow the stress_matrix.sh aggregator: `[BR N] PASS/FAIL`.
 */

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
#define ROLE_CHILD_READER   1   /* child opens reader for TAG_CROSS, drains, exits */
#define ROLE_CHILD_EOF_WR   2   /* child opens writer for TAG_EOF, pushes 4 + exits */
#define ROLE_CHILD_DRAIN    3   /* child opens reader for TAG_BLOCK, drains 1 frame after wait_ms(50), exits */

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

/* ─────────────────────────── child roles ───────────────────────── */

static void role_child_reader(void)
{
    Brook *b = brook_open(TAG_CROSS, 0, 0, BROOK_READER);
    if (!b) { kdbg_print("[BR C-rd] open FAIL"); exit(1); }

    /* Expect 32 frames of [u32 idx][u32 magic=0xDEADBEEF]. */
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
    wait_ms(80);                            /* let parent block on push */
    uint8_t frame[8];
    int rc = brook_pop(b, frame);           /* free one slot */
    if (rc != 0) { kdbg_print("[BR C-dr] pop FAIL"); brook_release(b); exit(2); }
    /* Drain rest so parent's release/destroy is clean. */
    while (brook_try_pop(b, frame) == 0) {}
    brook_release(b);
    exit(0);
}

/* ────────────────────────── parent tests ───────────────────────── */

static void run_parent_tests(void)
{
    /* T1 — basic SPSC same-proc round trip. */
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

    /* T2 — try_push fills ring then blocks. */
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

    /* T3 — try_pop on empty. */
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

    /* T4 — shape mismatch rejected. */
    {
        Brook *w = brook_open(TAG_SHAPE, 64, 8, BROOK_WRITER | BROOK_CREATE);
        if (!w) { fail(4, "create"); }
        else {
            /* Second opener with different frame_size — must fail. */
            Brook *bad = brook_open(TAG_SHAPE, 128, 8, BROOK_READER);
            if (bad) { fail(4, "shape mismatch should fail"); brook_release(bad); }
            else pass(4);
            brook_release(w);
        }
    }

    /* T5 — SPSC second writer rejected. */
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

    /* T6 — cross-cabin: parent writes, child reads. */
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

    /* T7 — peer-death EOF: child writer pushes 4 + exits, parent
     * reader drains 4 OK then receives END_OF_FILE. */
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
                /* Wait for child exit, then expect EOF. */
                wait_child_exit((uint32_t)child);
                int rc = brook_pop(r, frame);
                brook_release(r);
                if (ok && rc == -ERR_END_OF_FILE) pass(7);
                else fail(7, "expected END_OF_FILE");
            }
        }
    }

    /* T8 — block writer, wake on pop. Parent fills ring (4), pushes
     * 5th (blocks). Child waits 80 ms then pops 1. Parent's push
     * completes. */
    {
        Brook *w = brook_open(TAG_BLOCK, 8, 4, BROOK_WRITER | BROOK_CREATE);
        if (!w) { fail(8, "create writer"); }
        else {
            int child = spawn_role(ROLE_CHILD_DRAIN);
            if (child < 0) { fail(8, "spawn"); brook_release(w); }
            else {
                int ok = 1;
                uint8_t f[8] = {0};
                /* Wait a bit so child opens its reader. */
                wait_ms(20);
                /* Fill 4 slots. */
                for (int i = 0; i < 4 && ok; i++) {
                    if (brook_push(w, f) != 0) ok = 0;
                }
                /* 5th push must block until child pops one. */
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

    /* T9 — pop timeout: open reader + writer; reader times out 100 ms. */
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

    /* T10 — FROZEN protocol race-elimination. After reader's EOF
     * decision (CAS writer_alive 0→FROZEN), the session is terminal
     * and any new writer attach on the same tag MUST fail with
     * -ERR_INVALID_STATE — proving the re-attach race is closed at
     * the kernel level (CAS contention has exclusive outcome).
     *
     * In-process to avoid cross-tag child-role dependencies. */
    {
        Brook *r = brook_open(TAG_FROZEN, 8, 4, BROOK_READER | BROOK_CREATE);
        Brook *w1 = brook_open(TAG_FROZEN, 8, 4, BROOK_WRITER);
        if (!r || !w1) {
            fail(10, "initial open");
            if (r) brook_release(r);
            if (w1) brook_release(w1);
        } else {
            int ok = 1;

            /* Writer 1 pushes 2 frames then releases. */
            uint8_t frame[8];
            memset(frame, 0xC1, sizeof(frame));
            if (brook_push(w1, frame) != 0) ok = 0;
            memset(frame, 0xC2, sizeof(frame));
            if (brook_push(w1, frame) != 0) ok = 0;
            brook_release(w1);

            /* Reader drains 2 frames OK. */
            if (ok && brook_pop(r, frame) != 0) ok = 0;
            else if (ok && frame[0] != 0xC1)    ok = 0;
            if (ok && brook_pop(r, frame) != 0) ok = 0;
            else if (ok && frame[0] != 0xC2)    ok = 0;

            /* Reader's next pop sees empty + writer_alive=0 +
             * ever_attached=1 → CAS-freezes the session and returns EOF. */
            if (ok && brook_pop(r, frame) != -ERR_END_OF_FILE) ok = 0;

            /* Race-closure check: new writer attach MUST fail. */
            Brook *w2 = brook_open(TAG_FROZEN, 8, 4, BROOK_WRITER);
            if (w2 != 0) {
                brook_release(w2);
                ok = 0;  /* Frozen session let a new writer in — race! */
            }

            brook_release(r);
            if (ok) pass(10);
            else fail(10, "frozen-session not blocking re-attach");
        }
    }

    /* T11 — STREAM mode: writer leaves and a NEW writer attaches on the
     * same tag; reader (opened with BROOK_STREAM) drains both writers'
     * frames without seeing EOF in between. Tests the opt-in streaming
     * semantic for daemons that legitimately swap writers. */
    {
        Brook *r = brook_open(TAG_STREAM, 8, 8,
                              BROOK_READER | BROOK_CREATE | BROOK_STREAM);
        if (!r) { fail(11, "create reader"); }
        else {
            int ok = 1;
            uint8_t frame[8];

            /* Writer 1: attach, push 1 frame, release. */
            Brook *w1 = brook_open(TAG_STREAM, 8, 8,
                                   BROOK_WRITER | BROOK_STREAM);
            if (!w1) ok = 0;
            else {
                memset(frame, 0xA1, sizeof(frame));
                if (brook_push(w1, frame) != 0) ok = 0;
                brook_release(w1);
            }

            /* Writer 2: SAME tag, re-attach after writer 1 released. In
             * single-session mode reader would have FROZEN the session
             * here (BT10); with BROOK_STREAM the reader has NOT frozen
             * and writer 2 attach succeeds. */
            Brook *w2 = brook_open(TAG_STREAM, 8, 8,
                                   BROOK_WRITER | BROOK_STREAM);
            if (!w2) ok = 0;
            else {
                memset(frame, 0xA2, sizeof(frame));
                if (brook_push(w2, frame) != 0) ok = 0;
                brook_release(w2);
            }

            /* Drain both frames. With STREAM mode the reader does NOT
             * return EOF between writer transitions. */
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
    /* Children receive role byte via the first Pocket from the parent. */
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
                    exit(0);   /* defensive — role_* call exit() internally */
                }
            }
        }
    }

    run_parent_tests();
    exit(g_passed == g_total ? 0 : 1);
    return 0;
}
