/*
 * handset — who holds the handset hears the keys.
 *
 * The oracle of the console's ear: says [HANDSET] ask, reads one line the
 * way any program does (readline over the strand's lane tag), says what it
 * got and leaves. The harness types the line — before the ask (type-ahead
 * banked by the daemon) or after it — and then types for the shell, which
 * must hear again once this program is gone.
 *
 *   handset         ask, read one line, say it, leave.
 *   handset deaf    a reader that vanished with the handset in its hand: a
 *                   strand takes the ear (asks the daemon to listen) and then
 *                   gives its CLAIM up without giving the ear back — the shape
 *                   of a reader that died in the middle of its reading, with
 *                   its lane still open. Main reads a moment later. The line
 *                   typed after "deaf ear on top" must reach main: the daemon
 *                   says the first key to the deaf lane, the kernel says
 *                   nobody heard, the ear goes, and the key is said again to
 *                   whoever listens next.
 */

#include "box/print.h"
#include "box/luggage.h"
#include "box/strand.h"
#include "box/touch.h"
#include "box/sync.h"
#include "box/system.h"
#include "box/string.h"

static volatile uint64_t g_deaf;   /* 1 once the strand holds the ear and wears no tag */

static void deaf_reader(void *arg)
{
    (void)arg;
    TouchTag ear = console_listen();                      /* the ear is this lane's now */
    if (ear != TOUCH_TAG_INVALID) touch_release(ear);     /* … and nobody wears its tag */
    __atomic_store_n(&g_deaf, 1u, __ATOMIC_RELEASE);
    for (;;) yield();   /* alive, lane open: only the count can tell the daemon */
}

static int read_and_say(void)
{
    char line[256];
    int n = readline(line, sizeof(line));
    if (n < 0) {
        printf("[HANDSET] FAILED: readline said %d\n", n);
        return 1;
    }
    printf("[HANDSET] got: %s\n", line);
    return 0;
}

int main(void)
{
    const char *mode = luggage_word(1);
    if (mode && strcmp(mode, "deaf") == 0) {
        if (strand_spawn(deaf_reader, NULL) == 0) {
            printf("[HANDSET] FAILED: no strand for the deaf reader\n");
            return 1;
        }
        while (__atomic_load_n(&g_deaf, __ATOMIC_ACQUIRE) == 0) yield();
        printf("[HANDSET] deaf ear on top\n");
        /* The harness types now. Main asks only after a moment, so every key
         * is said to the deaf lane first — the park's own deadline is the
         * moment, and g_deaf never changes again. */
        addr_park((void *)&g_deaf, 1, 3000);
        return read_and_say();
    }

    printf("[HANDSET] ask\n");
    return read_and_say();
}
