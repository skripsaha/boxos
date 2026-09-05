#ifndef BOX_TURNIN_H
#define BOX_TURNIN_H

#include "box/types.h"

/*
 * Turn In — what a strand does when it has looked at everything it waits on
 * and found nothing.
 *
 * Every other wait in this system names a place: touch_await names the Touch
 * ring, a submit's result_wait names its own answer. A strand whose work can
 * arrive from more than one direction had no way to say so, and so it did the
 * only thing left — it looked, and looked, and looked. That is what kept this
 * box at a hundred percent of a core with nothing on screen.
 *
 * The mark is what makes it safe. Take the mark FIRST, then look; the sleep
 * refuses itself if anything has arrived since the mark was taken. Taking it
 * afterwards is the lost wake — an arrival landing between the look and the
 * mark is invisible to both.
 *
 *     TurnInMark mark = box_mark();
 *     if (look_at_everything()) continue;
 *     box_turn_in(mark);
 *
 * box_turn_in makes no promise about how long it sleeps, or that it sleeps at
 * all: it can return because work arrived, because the kernel refused the sleep,
 * or because something else woke the strand. It is the caller's loop that
 * carries the liveness — look again, and ask again. That is why there is no
 * deadline here and no way to pass one. A wait for "something to happen" has no
 * honest deadline, and a guessed one is just a wake-up with nothing to do.
 *
 * A strand that also reads a Brook must hang its bells out before the last look
 * — see box/brook.h. The rings are the only thing the kernel can see arriving.
 */

typedef struct {
    uint64_t touch;    /* TouchRing  tail at the moment of the look */
    uint64_t result;   /* ResultRing tail at the moment of the look */
} TurnInMark;

/* Snapshot both delivery cursors. Two relaxed loads, no syscall. */
TurnInMark box_mark(void);

/* True once anything has arrived since the mark was taken. */
bool box_mark_moved(TurnInMark seen);

/* Look hot for a moment, then ask the kernel to put this strand down until
 * something arrives. Returns when the strand is running with (usually) work to
 * do; the caller must re-look either way. */
void box_turn_in(TurnInMark seen);

#endif /* BOX_TURNIN_H */
