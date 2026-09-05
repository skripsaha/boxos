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
 * The pause is a real park, not a spin: a spinning strand keeps a core hot, and
 * a hot core is exactly the condition this test needs to not exist. The two
 * pauses park in DIFFERENT ways on purpose, because there are two waits worth
 * proving and each one used to hold a core:
 *
 *   the FIRST is a blocked brook_pop. A sibling strand goes quiet, then pushes
 *   one frame; this strand is asleep in Brook for the whole of that, woken by
 *   the writer taking its bell. That is the reader half — the wait a Current,
 *   a console lane, and every stream consumer in this system sits in.
 *
 *   the SECOND is a touch_await on a tag nobody publishes to, which ends on
 *   its own deadline. It keeps the third marker honest about time.
 *
 * The line each pause is measured by says which kind it was, so a log reader
 * can tell a run that proved the brook sleep from one that fell back.
 */

#include "box/print.h"
#include "box/touch.h"
#include "box/brook.h"
#include "box/strand.h"
#include "box/clock.h"
#include "box/cpu.h"
#include "box/string.h"
#include "box/system.h"

#define QUIET_MS    3000u
#define QUIET_TAG   "quietprint:nobody"
#define STREAM_TAG  "quietprint:stream"
#define STREAM_FS   8u
#define STREAM_FC   4u

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

/* The sibling: stay quiet exactly as long as the reader expects to sleep, then
 * put one frame in. Opening the WRITER end only now is deliberate — a stream
 * with no writer yet is a reader's ordinary starting state, and this proves the
 * sleep survives it.
 *
 * ‼ IT THEN GOES QUIET AGAIN INSTEAD OF LEAVING, and that is not politeness.
 * A strand that exits publishes process:died, and a strand that releases a
 * Brook makes the KERNEL ring the survivor's bell — either of which wakes the
 * console daemon and the reader by a route that has nothing to do with what is
 * being measured. Doing both the instant it had pushed made this program prove
 * itself right no matter what: with the bell deliberately broken, output still
 * reached the screen, on the death. So it stays alive, holding its end, until
 * everything that had to be observed has been. Measured — the oracle went
 * green on a build whose bell did nothing at all. */
static void stream_writer(void *arg)
{
    (void)arg;
    be_quiet(QUIET_MS);

    Brook *w = brook_open(STREAM_TAG, STREAM_FS, STREAM_FC, BROOK_WRITER);
    if (w) {
        uint8_t frame[STREAM_FS];
        memset(frame, 0xA5, sizeof(frame));
        (void)brook_push(w, frame);
    }

    be_quiet(QUIET_MS);
    if (w) brook_release(w);
    strand_exit();
}

int main(void)
{
    uint64_t began = clock_uptime_ms();

    printf("[QP] line 1 at %lu ms\n", (unsigned long)(clock_uptime_ms() - began));
    io_flush();

    /* The reader end first, so the sibling's open finds a stream waiting. */
    const char *how = "quiet";
    Brook *r = cpu_has_fsgsbase()
             ? brook_open(STREAM_TAG, STREAM_FS, STREAM_FC,
                          BROOK_READER | BROOK_CREATE)
             : 0;
    if (r && strand_spawn(stream_writer, 0) != 0) {
        uint8_t frame[STREAM_FS];
        if (brook_pop(r, frame) == 0) how = "brook";   /* asleep until the push */
        brook_release(r);
    } else {
        if (r) brook_release(r);
        /* No strands on this machine (no FSGSBASE): still go quiet, so the
         * rest of the run measures what it always did. Saying so in the line
         * below is what keeps the oracle from reading a fallback as a proof. */
        be_quiet(QUIET_MS);
    }

    printf("[QP] line 2 at %lu ms (%s)\n",
           (unsigned long)(clock_uptime_ms() - began), how);
    io_flush();

    be_quiet(QUIET_MS);

    printf("[QP] done after %lu ms\n", (unsigned long)(clock_uptime_ms() - began));
    io_flush();
    return 0;
}
