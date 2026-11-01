/*
 * faretest — the scheduler's Fare, measured from a program.
 *
 * Two children of this program spin without ever entering the kernel. Both
 * wear faretest; the one called hot also wears faretest:hot. Their processor
 * time is read through proc_crew while this program sleeps in the kernel, and
 * judged:
 *
 *   with no Use Context, the two divide a core they share evenly;
 *   with the context faretest:hot — said before the children start, and said
 *   while they spin — hot is given three quarters of it (it pays a third of
 *   the Fare); cleared again, the two are even;
 *   and every sleep of this program ends when it was asked to, whatever the
 *   children do.
 *
 * Shares say something only while the two share a core. On a machine with an
 * application core for each of them, each is given one whole, and that is what
 * is judged instead; the shares are said to be out of reach, not passed.
 *
 * This utility wears `system` (see the image tags in the Makefile), so it may
 * say the Use Context; whatever context the user had is put back at the end.
 * A child is started as `faretest spin`. Each check says "[FARE] holds" or
 * "[FARE] FAIL"; the run ends with "[FARE] PASS" or "[FARE] FAIL: <how many>",
 * which logcheck's fare and the stress matrix gate on.
 */

#include "box/print.h"
#include "box/system.h"
#include "box/use.h"
#include "box/sync.h"
#include "box/clock.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/string.h"

#define FARE_TAG        "faretest"
#define FARE_HOT        "faretest:hot"
#define FARE_WINDOW_MS  2000u
#define FARE_SETTLE_MS  300u
#define FARE_SLACK      5u      /* percentage points a share may stray */
#define FARE_LATE_MS    50u     /* how late a sleep may end */

typedef struct Kid { uint32_t pid, gen; } Kid;

typedef struct Window {
    uint64_t hot_ms, cold_ms, wall_ms;
} Window;

static uint32_t g_failed;

/* One check: "[FARE] holds" or "[FARE] FAIL", what was judged, and the
 * window it was judged on. The verdict of the whole run is the only
 * "[FARE] PASS". */
static void Check(bool ok, const char *what, const Window *w)
{
    if (!ok) g_failed++;
    printf("[FARE] %s %s (hot %lu ms, cold %lu ms, wall %lu ms)\n", ok ? "holds" : "FAIL", what,
           (unsigned long)w->hot_ms, (unsigned long)w->cold_ms, (unsigned long)w->wall_ms);
}

/* A sleep in the kernel on a word nobody writes, as std::this_thread::sleep_for
 * sleeps — never a spin, which would itself be a third contender. */
static void Sleep(uint64_t ms)
{
    static volatile uint64_t never;
    uint64_t end = clock_uptime_ms() + ms;
    for (uint64_t now = clock_uptime_ms(); now < end; now = clock_uptime_ms())
        (void)addr_park(&never, 0, (uint32_t)(end - now));
}

static int Start(const char *tags, Kid *kid)
{
    int pid = proc_exec_gen("faretest spin", tags, &kid->gen);
    if (pid < 0) return pid;
    kid->pid = (uint32_t)pid;
    return 0;
}

static void Stop(Kid *kid)
{
    if (kid->pid == 0) return;
    (void)proc_finish(kid->pid, kid->gen);
    (void)process_gone(kid->pid, kid->gen, NULL);
    kid->pid = 0;
}

/* Both children's processor time, in microseconds. */
static void Spent(const Kid *hot, const Kid *cold, uint64_t *hot_us, uint64_t *cold_us)
{
    ProcMate *mates = NULL;
    int n = proc_crew(FARE_TAG, &mates, NULL);
    *hot_us = *cold_us = 0;
    for (int i = 0; i < n; i++) {
        if (mates[i].pid == hot->pid)  *hot_us  = mates[i].cpu_us;
        if (mates[i].pid == cold->pid) *cold_us = mates[i].cpu_us;
    }
    free(mates);
}

static Window Measure(const Kid *hot, const Kid *cold)
{
    uint64_t h0, c0, h1, c1;
    Spent(hot, cold, &h0, &c0);
    uint64_t t0 = clock_uptime_ms();
    Sleep(FARE_WINDOW_MS);
    Spent(hot, cold, &h1, &c1);
    Window w = { (h1 - h0) / 1000, (c1 - c0) / 1000, clock_uptime_ms() - t0 };
    return w;
}

static uint64_t HotShare(const Window *w)
{
    uint64_t both = w->hot_ms + w->cold_ms;
    return both ? w->hot_ms * 100 / both : 0;
}

/* The two spent more than one core's worth between them: each had its own. */
static bool Apart(const Window *w)
{
    return (w->hot_ms + w->cold_ms) * 10 > w->wall_ms * 14;
}

static bool Near(uint64_t share, uint64_t want)
{
    return share + FARE_SLACK >= want && share <= want + FARE_SLACK;
}

/* One judged window: the hot child's share against `want` while the two share
 * a core, each child's whole core while they do not, and this program's
 * sleep against the clock either way. */
static void Judge(const char *what, const Kid *hot, const Kid *cold, uint64_t want)
{
    Window w = Measure(hot, cold);
    char line[160];

    if (Apart(&w)) {
        snprintf(line, sizeof line, "%s: each child had a core of its own and all of it", what);
        Check(w.hot_ms * 10 >= w.wall_ms * 9 && w.cold_ms * 10 >= w.wall_ms * 9, line, &w);
        printf("[FARE] SKIP %s: the shares — the children did not share a core\n", what);
    } else {
        snprintf(line, sizeof line, "%s: hot was given %lu%%, %lu%% asked", what,
                 (unsigned long)HotShare(&w), (unsigned long)want);
        Check(Near(HotShare(&w), want), line, &w);
    }
    snprintf(line, sizeof line, "%s: a %u ms sleep ended within %u ms of its time", what,
             FARE_WINDOW_MS, FARE_LATE_MS);
    Check(w.wall_ms <= FARE_WINDOW_MS + FARE_LATE_MS, line, &w);
}

static int Pair(Kid *hot, Kid *cold)
{
    hot->pid = cold->pid = 0;
    int rc = Start(FARE_TAG "," FARE_HOT, hot);
    if (rc == 0) rc = Start(FARE_TAG, cold);
    if (rc < 0) {
        printf("[FARE] FAIL: a child would not start (error %d)\n", -rc);
        Stop(hot);
        return rc;
    }
    Sleep(FARE_SETTLE_MS);
    return 0;
}

static int Run(void)
{
    Kid hot, cold;

    (void)use_clear(NULL);
    if (Pair(&hot, &cold) < 0) return -1;
    Judge("no context", &hot, &cold, 50);
    Stop(&hot);
    Stop(&cold);

    (void)use_set(FARE_HOT, NULL);
    if (Pair(&hot, &cold) < 0) return -1;
    Judge("the context said before the children started", &hot, &cold, 75);
    Stop(&hot);
    Stop(&cold);

    (void)use_clear(NULL);
    if (Pair(&hot, &cold) < 0) return -1;
    (void)use_set(FARE_HOT, NULL);
    Sleep(FARE_SETTLE_MS);
    Judge("the context said while the children spun", &hot, &cold, 75);
    (void)use_clear(NULL);
    Sleep(FARE_SETTLE_MS);
    Judge("the context cleared while the children spun", &hot, &cold, 50);
    Stop(&hot);
    Stop(&cold);
    return 0;
}

int main(void)
{
    if (luggage_word_count() > 1 && strcmp(luggage_word(1), "spin") == 0) {
        volatile uint64_t turns = 0;
        for (;;) turns++;
    }

    /* The user's context, to be put back. */
    size_t need = 0;
    int    tags = use_get(NULL, 0, &need);
    char  *saved = NULL;
    if (tags < 0) {
        printf("[FARE] FAIL: use_get refused (error %d)\n", -tags);
        return 1;
    }
    if (need > 1) {
        saved = malloc(need);
        if (!saved || use_get(saved, need, NULL) < 0) {
            printf("[FARE] FAIL: the user's context could not be kept\n");
            free(saved);
            return 1;
        }
    }

    int rc = Run();
    (void)use_set(saved, NULL);
    free(saved);

    if (rc < 0) g_failed++;
    if (g_failed) printf("[FARE] FAIL: %u\n", g_failed);
    else          printf("[FARE] PASS\n");
    return g_failed ? 1 : 0;
}
