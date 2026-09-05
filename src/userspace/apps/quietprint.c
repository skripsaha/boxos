/*
 * quietprint — output must reach the screen when nothing else is happening.
 *
 * Every other console test prints in a storm, and a storm hides the thing that
 * actually has to work. A console lane is a Brook: the writer stores a frame
 * into a shared page and the kernel never learns of it. While frames keep
 * coming the daemon never runs out of work and never sleeps, so a storm proves
 * only that a busy reader keeps reading. What has to be proven is the opposite
 * case — one line, into a lane, at a moment when the daemon has already gone to
 * sleep and NOTHING else is going to wake it.
 *
 * So this program prints and then deliberately goes quiet:
 *
 *      print a marked line
 *      park, saying nothing at all, for QUIET_MS
 *      print a second marked line
 *      park again
 *      exit
 *
 * During each quiet stretch there is no key, no message, no death — the daemon
 * has nothing to look at and turns in. The only thing that can carry the first
 * line to the screen is the bell: the daemon hung its pid on this lane before
 * going down, and this strand's push is supposed to take it and ring.
 *
 * How it fails when the bell is gone: nothing is lost and nothing crashes — the
 * lines simply sit in the lane until this process DIES, because process:died is
 * a Touch and a Touch wakes the daemon. Everything then appears at once, on
 * time-of-death instead of time-of-print. That is why the oracle around this
 * program watches WHEN the first line lands, not whether it lands, and why this
 * program says its own timing out loud: the last line reports how long it was
 * alive, so a reader of the log can tell a line that arrived while it ran from
 * a line that arrived when it died.
 *
 * The pause is a real park (touch_await on a tag nobody ever publishes), not a
 * spin: a spinning strand keeps a core hot, and a hot core is exactly the
 * condition this test needs to not exist.
 */

#include "box/print.h"
#include "box/touch.h"
#include "box/clock.h"
#include "box/system.h"

#define QUIET_MS   3000u
#define QUIET_TAG  "quietprint:nobody"

/* Say nothing for `ms`, and hold no core while saying it.
 *
 * touch_await on a tag with no publisher parks the strand in the kernel and
 * ends on its own deadline. The tag is interned and claimed once; a claim
 * nobody publishes to costs one registry entry and delivers nothing, ever,
 * which is the entire point. */
static void be_quiet(uint32_t ms)
{
    static TouchTag tag = TOUCH_TAG_INVALID;
    if (tag == TOUCH_TAG_INVALID) {
        tag = touch_pair_choose(touch_intern(QUIET_TAG));
        if (tag != TOUCH_TAG_INVALID) touch_claim(tag, TOUCH_REST, 0, 0);
    }

    uint64_t deadline = clock_uptime_ms() + ms;
    for (;;) {
        uint64_t now = clock_uptime_ms();
        if (now >= deadline) return;
        Touch t;
        if (tag == TOUCH_TAG_INVALID) return;   /* no tag: do not spin, just go */
        (void)touch_await(tag, &t, (uint32_t)(deadline - now));
    }
}

int main(void)
{
    uint64_t began = clock_uptime_ms();

    printf("[QP] line 1 at %lu ms\n", (unsigned long)(clock_uptime_ms() - began));
    io_flush();
    be_quiet(QUIET_MS);

    printf("[QP] line 2 at %lu ms\n", (unsigned long)(clock_uptime_ms() - began));
    io_flush();
    be_quiet(QUIET_MS);

    printf("[QP] done after %lu ms\n", (unsigned long)(clock_uptime_ms() - began));
    io_flush();
    return 0;
}
