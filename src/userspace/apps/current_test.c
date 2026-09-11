
#include "box/current.h"
#include "box/debug.h"
#include "box/string.h"
#include "box/error.h"

static int g_passed = 0;
static int g_total  = 0;

static void check(int n, int cond, const char *what)
{
    g_total++;
    if (cond) { g_passed++; kdbg_print("[CURRENT %d] PASS %s", n, what); }
    else      {             kdbg_print("[CURRENT %d] FAIL %s", n, what); }
}

int main(void)
{
    {
        Current *s = current_open("screen", CURRENT_WRITE, 0, 0);
        int ok = s != NULL
              && (current_caps(s) & CURRENT_CAP_WRITE)
              && current_write(s, "Current: screen ok\n", 19) == 19;
        check(1, ok, "screen write");
        if (s) current_release(s);
    }

    {
        Current *k = current_open("keyboard", CURRENT_READ, 0, 0);
        int ok = k != NULL
              && (current_caps(k) & CURRENT_CAP_READ)
              && !(current_caps(k) & CURRENT_CAP_CLOSEABLE);
        check(2, ok, "keyboard open");
        if (k) current_release(k);
    }

    {
        Current *lg = current_open("log:serial", CURRENT_WRITE, 0, 0);
        int ok = lg != NULL
              && (current_caps(lg) & CURRENT_CAP_WRITE)
              && current_write(lg, "Current: log backend reached serial\n", 36) == 36;
        check(3, ok, "log write");
        if (lg) current_release(lg);
    }

    {
        uint8_t pattern[32];
        for (int i = 0; i < 32; i++) pattern[i] = (uint8_t)(i * 7 + 1);

        Current *fw = current_open("file:current_test.dat", CURRENT_WRITE, 0, CURRENT_CREATE);
        int wok = fw != NULL
               && (current_caps(fw) & CURRENT_CAP_SEEKABLE)
               && current_write(fw, pattern, 32) == 32
               && current_tell(fw) == 32;
        if (fw) current_release(fw);
        check(4, wok, "file write+tell");

        Current  *fr = current_open("file:current_test.dat", CURRENT_READ, 0, 0);
        uint8_t   back[32];
        int n   = fr ? current_read(fr, back, 32) : -1;
        int rok = fr != NULL && n == 32 && memcmp(back, pattern, 32) == 0;
        int eos = fr ? current_read(fr, back, 32) : -1;
        rok = rok && eos == CURRENT_CLOSED;
        if (fr) current_seek(fr, 0);
        int n2 = fr ? current_read(fr, back, 8) : -1;
        rok = rok && n2 == 8 && current_tell(fr) == 8 && memcmp(back, pattern, 8) == 0;
        if (fr) current_release(fr);
        check(5, rok, "file read+seek+end");
    }

    {
        const char *tag = "current:test:stream";
        unsigned want = CURRENT_CAP_FRAMED | CURRENT_CAP_BACKPRESSURE
                      | CURRENT_CAP_CLOSEABLE | CURRENT_CAP_WRITE;
        Current *w = current_open(tag, CURRENT_WRITE, 16, 0);
        Current *r = current_open(tag, CURRENT_READ, 16, 0);
        int ok = w != NULL && r != NULL
              && (current_caps(w) & want) == want
              && (current_caps(r) & CURRENT_CAP_READ);

        for (int i = 0; ok && i < 5; i++) {
            uint8_t frame[16];
            memset(frame, 0, sizeof(frame));
            frame[0]  = (uint8_t)(0xA0 + i);
            frame[15] = (uint8_t)(i * 3);
            if (current_write(w, frame, 16) != 16) ok = 0;
        }
        for (int i = 0; ok && i < 5; i++) {
            uint8_t got[16];
            int rc = current_read(r, got, 16);
            if (rc != 16 || got[0] != (uint8_t)(0xA0 + i) || got[15] != (uint8_t)(i * 3)) ok = 0;
        }
        if (w) current_close(w);
        {
            uint8_t got[16];
            int rc = r ? current_read(r, got, 16) : -1;
            if (rc != CURRENT_CLOSED) ok = 0;
        }
        if (w) current_release(w);
        if (r) current_release(r);
        check(6, ok, "stream put/take/close");
    }

    {
        Current *bad = current_open("screen", CURRENT_READ, 0, 0);
        int ok = bad == NULL;

        Current *kb = current_open("keyboard", CURRENT_READ, 0, 0);
        ok = ok && kb && current_write(kb, "x", 1) == -ERR_INVALID_OPERATION;
        if (kb) current_release(kb);

        Current *sc = current_open("screen", CURRENT_WRITE, 0, 0);
        ok = ok && sc && current_seek(sc, 0) == -ERR_INVALID_OPERATION;
        if (sc) current_release(sc);

        check(7, ok, "honest caps");
    }

    {
        const char *tag = "current:test:small";
        uint16_t vals[4] = { 0x1234, 0xBEEF, 0x0001, 0xFFFF };
        Current *w = current_open(tag, CURRENT_WRITE, 2, CURRENT_CREATE);
        Current *r = current_open(tag, CURRENT_READ, 2, 0);
        int ok = w != NULL && r != NULL;
        for (int i = 0; ok && i < 4; i++)
            if (current_write(w, &vals[i], 2) != 2) ok = 0;
        for (int i = 0; ok && i < 4; i++) {
            uint16_t v = 0;
            if (current_read(r, &v, 2) != 2 || v != vals[i]) ok = 0;
        }
        if (w) current_release(w);
        if (r) current_release(r);
        check(8, ok, "small-item padding");
    }

    if (g_passed == g_total) kdbg_print("[CURRENT] ALL PASS (%d/%d)", g_passed, g_total);
    else                     kdbg_print("[CURRENT] %d/%d FAIL", g_passed, g_total);

    return 0;
}