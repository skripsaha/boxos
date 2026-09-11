
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

#define LINE_CHILD_READ "bay_test read"
#define LINE_CHILD_RC   "bay_test rc"
#define ROLE_WORD_READ  "read"
#define ROLE_WORD_RC    "rc"

#define RC_CHILD_HELD   0xC1
#define RC_CHILD_NOHOLD 0xC2
#define RC_PARENT_DROP  0xD1

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

static int wait_child(int pid, uint32_t gen, int32_t *out_exit)
{
    return process_gone((uint32_t)pid, gen, out_exit);
}

static uint8_t await_word(uint32_t from)
{
    Result r;
    for (;;) {
        (void)receive_wait(&r, 0);
        if (r.sender_pid == from && r.data_length >= 1 && r.data_addr != 0)
            return *(const uint8_t *)(uintptr_t)r.data_addr;
    }
}


static void role_child_read(uint32_t parent_pid)
{
    (void)parent_pid;
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
        send(parent_pid, &word, 1);
        kdbg_print("[BT C-rc] open FAIL");
        exit(1);
    }

    const uint8_t *bytes = (const uint8_t *)p;
    if (bytes[0] != 0xC0 || bytes[63] != 0xCF) {
        bay_release(p);
        word = RC_CHILD_NOHOLD;
        send(parent_pid, &word, 1);
        kdbg_print("[BT C-rc] content FAIL");
        exit(2);
    }

    word = RC_CHILD_HELD;
    send(parent_pid, &word, 1);
    while (await_word(parent_pid) != RC_PARENT_DROP) { }

    if (bytes[0] != 0xC0 || bytes[63] != 0xCF) {
        bay_release(p);
        kdbg_print("[BT C-rc] post-parent-release FAIL");
        exit(3);
    }

    bay_release(p);
    exit(0);
}


static void run_parent_tests(void)
{
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

    {
        void *p = bay_open(TAG_HUGE, 4UL * 1024 * 1024, BAY_CREATE);
        if (!p) { fail(2, "open huge"); }
        else {
            uint8_t *b = (uint8_t *)p;
            for (int i = 0; i < 64; i++) b[i] = (uint8_t)(0xA0 + i);
            b[4UL * 1024 * 1024 - 1] = 0xFF;

            uint32_t gen   = 0;
            int      child = proc_exec_gen(LINE_CHILD_READ, NULL, &gen);
            if (child <= 0) { fail(2, "spawn child"); bay_release(p); }
            else {
                pass(2);
                int32_t child_exit = 0;
                int     gone       = wait_child(child, gen, &child_exit);
                if (gone == 0 && child_exit == 0) {
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
                bay_release(p);
                (void)wait_child(child, gen, NULL);
                fail(4, "child never held the Bay");
            } else {
                int     rc   = bay_release(p);
                uint8_t drop = RC_PARENT_DROP;
                send((uint32_t)child, &drop, 1);
                int32_t child_exit = 0;
                int     gone       = wait_child(child, gen, &child_exit);
                if (rc != 0)                              fail(4, "parent release");
                else if (gone == 0 && child_exit == 0)    pass(4);
                else                                      fail(4, "child exit");
            }
        }
    }

    {
        void *p = bay_open(TAG_MISS, 0, BAY_OPEN);
        if (p == NULL) pass(5);
        else { fail(5, "should be NULL"); bay_release(p); }
    }

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