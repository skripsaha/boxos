/*
 * bay_test — verify cross-cabin shared memory via the Bay primitive.
 *
 * Test plan:
 *   T1  small Bay (4 KB): create, write, release — no kernel panic.
 *   T2  huge Bay (4 MB): create with BAY_CREATE — implicit 2 MB pages.
 *   T3  child opens the same tag, reads the parent-written bytes,
 *       writes a reply marker, releases. Parent reads the marker.
 *   T4  refcount: parent creates, child opens, parent releases first.
 *       Child should still see live memory. Then child releases — last
 *       drop frees the Bay.
 *   T5  open-missing without BAY_CREATE returns NULL.
 *   T6  size query reports the rounded-up size.
 *   T7  implicit huge-page user-heap: malloc(4 MB) succeeds and the
 *       returned pointer is writable end-to-end (regression check on
 *       SYSTEM_OP_HEAP_PREFAULT).
 *
 * Pass/fail markers follow the stress_matrix.sh aggregator convention
 * (`[BT N] PASS` / `[BT N] FAIL`).
 */

#include "box/bay.h"
#include "box/memory.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/debug.h"
#include "box/luggage.h"
#include "box/string.h"
#include "box/core/result.h"
#include "box/core/cabin.h"
#include "box/error.h"

#define TAG_SMALL "bay:test:small"
#define TAG_HUGE  "bay:test:huge"
#define TAG_RC    "bay:test:rc"
#define TAG_MISS  "bay:test:miss"
#define TAG_SIZE  "bay:test:size"

/* A child's role is its LUGGAGE — the line it was started with, in its cabin
 * before its first instruction — not a packet it has to wait for.
 *
 * The role used to arrive as an IPC pocket the child looked for twice, 200 ms
 * apart. A child that missed both fell through to run_parent_tests(): it
 * created the Bays again, spawned children of its OWN, and the real parent —
 * now waiting on its child as a state — waited on a process that had become a
 * second parent. There is no window here: word 1 is there or the program is
 * the parent, and nothing in between. The shell starts `bay_test` with no
 * second word; only this file adds one. */
#define LINE_CHILD_READ "bay_test read"
#define LINE_CHILD_RC   "bay_test rc"
#define ROLE_WORD_READ  "read"
#define ROLE_WORD_RC    "rc"

/* T4's rendezvous, one byte each way. The refcount claim under test is "the
 * parent drops WHILE the child holds", and that ordering is a fact each side
 * must hear from the other — see role_child_rc(). */
#define RC_CHILD_HELD   0xC1   /* child  → parent: the Bay is open in my cabin  */
#define RC_CHILD_NOHOLD 0xC2   /* child  → parent: I never got it — do not wait */
#define RC_PARENT_DROP  0xD1   /* parent → child:  my claim is gone; look again */

static int g_passed = 0;
static int g_total  = 0;

static void pass(int n)
{
    g_passed++;
    g_total++;
    kdbg_print("[BT %d] PASS", n);
}

static void fail(int n, const char *why)
{
    g_total++;
    kdbg_print("[BT %d] FAIL: %s", n, why);
}

/* A child's end is asked as a STATE: already finished answers at once, still
 * running parks. It replaces a poll of 800 × 10 ms — eight seconds by count,
 * after which every child-spawning Bay test printed FAIL on a machine that was
 * merely busy. There is no budget left to run out: a child that never ends is
 * a wedge, and a parked parent is exactly how Nightwatch proves one.
 *
 * Returns the child's exit code in *out_exit, or non-zero if the kernel says
 * that (pid, generation) was never issued. */
static int wait_child(int pid, uint32_t gen, int32_t *out_exit)
{
    return process_gone((uint32_t)pid, gen, out_exit);
}

/* Take the next pocket sent by `from`, sleeping until it lands. receive_wait
 * with no deadline is the sleeping wait — it returns only WITH a message, so
 * the caller's own condition is the loop's only exit. Messages from anyone
 * else are discarded: this program talks to one child at a time. */
static uint8_t await_word(uint32_t from)
{
    Result r;
    for (;;) {
        (void)receive_wait(&r, 0);
        if (r.sender_pid == from && r.data_length >= 1 && r.data_addr != 0)
            return *(const uint8_t *)(uintptr_t)r.data_addr;
    }
}

/* ───────────────────────────── child roles ───────────────────────── */

static void role_child_read(uint32_t parent_pid)
{
    (void)parent_pid;
    /* Open the parent-created Bay and verify content. */
    void *p = bay_open(TAG_HUGE, 0, BAY_OPEN);
    if (!p) {
        kdbg_print("[BT C-read] open FAIL");
        exit(1);
    }

    const uint8_t *bytes = (const uint8_t *)p;
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        if (bytes[i] != (uint8_t)(0xA0 + i)) { ok = 0; break; }
    }
    if (!ok) {
        bay_release(p);
        kdbg_print("[BT C-read] content FAIL");
        exit(2);
    }

    /* Write a reply marker at offset 4096. */
    uint8_t *w = (uint8_t *)p + 4096;
    for (int i = 0; i < 32; i++) w[i] = (uint8_t)(0xB0 + i);

    bay_release(p);
    exit(0);
}

static void role_child_rc(uint32_t parent_pid)
{
    uint8_t word;
    void *p = bay_open(TAG_RC, 0, BAY_OPEN);
    if (!p) {
        word = RC_CHILD_NOHOLD;
        send(parent_pid, &word, 1);     /* the parent is waiting on this byte */
        kdbg_print("[BT C-rc] open FAIL");
        exit(1);
    }

    /* Verify parent's marker still visible. */
    const uint8_t *bytes = (const uint8_t *)p;
    if (bytes[0] != 0xC0 || bytes[63] != 0xCF) {
        bay_release(p);
        word = RC_CHILD_NOHOLD;
        send(parent_pid, &word, 1);
        kdbg_print("[BT C-rc] content FAIL");
        exit(2);
    }

    /* Say the claim is HELD, then wait to be told the parent's own is gone.
     *
     * Both halves used to be guesses — wait_ms(20) in the parent before its
     * release, wait_ms(50) here after it. Neither measured the thing it stood
     * for, and on a loaded host the parent's release could land BEFORE this
     * open: the last drop, the Bay freed, and T4 then failed on the child's
     * open having proved nothing about refcounts. Each side now waits for the
     * other's word, so the order under test is the order that happens. */
    word = RC_CHILD_HELD;
    send(parent_pid, &word, 1);
    while (await_word(parent_pid) != RC_PARENT_DROP) { }

    /* Verify content is STILL there after parent's release (our claim
     * keeps it alive). */
    if (bytes[0] != 0xC0 || bytes[63] != 0xCF) {
        bay_release(p);
        kdbg_print("[BT C-rc] post-parent-release FAIL");
        exit(3);
    }

    bay_release(p);
    exit(0);
}

/* ───────────────────────────── main test driver ───────────────── */

static void run_parent_tests(void)
{
    /* T1 — small Bay: create 4 KB, write, release, no panic. */
    {
        void *p = bay_open(TAG_SMALL, 4096, BAY_CREATE);
        if (!p) { fail(1, "open small"); }
        else {
            volatile uint8_t *b = (volatile uint8_t *)p;
            b[0] = 0x42; b[4095] = 0x55;
            if (b[0] == 0x42 && b[4095] == 0x55) {
                int rc = bay_release(p);
                if (rc == 0) pass(1);
                else fail(1, "release");
            } else {
                bay_release(p);
                fail(1, "readback");
            }
        }
    }

    /* T2 — huge Bay (4 MB): create, fill prefix, release. */
    {
        void *p = bay_open(TAG_HUGE, 4UL * 1024 * 1024, BAY_CREATE);
        if (!p) { fail(2, "open huge"); }
        else {
            uint8_t *b = (uint8_t *)p;
            for (int i = 0; i < 64; i++) b[i] = (uint8_t)(0xA0 + i);
            /* Touch the last byte to confirm 2 MB pages are mapped. */
            b[4UL * 1024 * 1024 - 1] = 0xFF;

            /* T3 — child reads back. We DON'T release yet; child opens
             * the same tag. */
            uint32_t gen   = 0;
            int      child = proc_exec_gen(LINE_CHILD_READ, NULL, &gen);
            if (child <= 0) { fail(2, "spawn child"); bay_release(p); }
            else {
                pass(2);
                int32_t child_exit = 0;
                int     gone       = wait_child(child, gen, &child_exit);
                if (gone == 0 && child_exit == 0) {
                    /* Check reply marker. */
                    const uint8_t *w = (const uint8_t *)p + 4096;
                    int ok = 1;
                    for (int i = 0; i < 32; i++) {
                        if (w[i] != (uint8_t)(0xB0 + i)) { ok = 0; break; }
                    }
                    if (ok) pass(3); else fail(3, "reply marker");
                } else {
                    fail(3, "child exit non-zero");
                }
                bay_release(p);
            }
        }
    }

    /* T4 — refcount: parent creates, child opens, parent releases first,
     * child verifies content still live, child releases (last drop). */
    {
        void *p = bay_open(TAG_RC, 4096, BAY_CREATE);
        if (!p) { fail(4, "open rc"); }
        else {
            uint8_t *b = (uint8_t *)p;
            b[0]  = 0xC0;
            b[63] = 0xCF;

            uint32_t gen   = 0;
            int      child = proc_exec_gen(LINE_CHILD_RC, NULL, &gen);
            if (child <= 0) { fail(4, "spawn rc"); bay_release(p); }
            else if (await_word((uint32_t)child) != RC_CHILD_HELD) {
                /* The child says it never got the Bay — releasing now would be
                 * the last drop and there is nothing left to prove. */
                bay_release(p);
                (void)wait_child(child, gen, NULL);
                fail(4, "child never held the Bay");
            } else {
                int     rc   = bay_release(p);   /* parent drops first */
                uint8_t drop = RC_PARENT_DROP;
                send((uint32_t)child, &drop, 1); /* sent even on a failed release:
                                                  * the child is waiting for it */
                int32_t child_exit = 0;
                int     gone       = wait_child(child, gen, &child_exit);
                if (rc != 0)                              fail(4, "parent release");
                else if (gone == 0 && child_exit == 0)    pass(4);
                else                                      fail(4, "child exit");
            }
        }
    }

    /* T5 — open missing without CREATE → NULL. */
    {
        void *p = bay_open(TAG_MISS, 0, BAY_OPEN);
        if (p == NULL) pass(5);
        else { fail(5, "should be NULL"); bay_release(p); }
    }

    /* T6 — size query. */
    {
        uint64_t expect_size = 4UL * 1024 * 1024;
        void *p = bay_open(TAG_SIZE, expect_size, BAY_CREATE);
        if (!p) { fail(6, "open size"); }
        else {
            uint64_t s = bay_size(p);
            if (s == expect_size) pass(6);
            else fail(6, "size mismatch");
            bay_release(p);
        }
    }

    /* T7 — user-heap implicit huge: malloc(4 MB) and write end-to-end. */
    {
        void *m = malloc(4UL * 1024 * 1024);
        if (!m) fail(7, "malloc 4M");
        else {
            uint8_t *b = (uint8_t *)m;
            b[0] = 0xE0;
            b[4UL * 1024 * 1024 - 1] = 0xEF;
            if (b[0] == 0xE0 && b[4UL * 1024 * 1024 - 1] == 0xEF) pass(7);
            else fail(7, "readback");
            free(m);
        }
    }

    kdbg_print("[BT *] %d/%d", g_passed, g_total);
    if (g_passed == g_total) kdbg_print("[BT *] PASS");
    else                     kdbg_print("[BT *] FAIL");
}

int main(void)
{
    CabinInfo *ci = cabin_info();

    /* Word 1 of the luggage names the role, and it is already there. A word we
     * do not know is a caller's mistake — say so and end, rather than run the
     * parent suite a second time inside a child. */
    const char *role = luggage_word(1);
    if (role) {
        if (strcmp(role, ROLE_WORD_READ) == 0) role_child_read(ci->spawner_pid);
        if (strcmp(role, ROLE_WORD_RC)   == 0) role_child_rc(ci->spawner_pid);
        kdbg_print("[BT C-?] unknown role '%s'", role);
        exit(255);
    }

    run_parent_tests();
    exit(g_passed == g_total ? 0 : 1);
    return 0;
}
