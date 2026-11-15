#ifndef JOURNEY_H
#define JOURNEY_H

#include "ktypes.h"
#include "boxos_journey.h"

/*
 * The kernel's part of the journey (boxos_journey.h): a step marked by the
 * time-stamp counter as it is reached, beside the loader's moments on the
 * Boarding Pass, and both answered by SYSTEM_OP_JOURNEY.
 *
 * The kernel marks, each the first time it is reached:
 *   kernel  — kernel_main's first line
 *   volume  — the first volume mounted ("volume:mounted", JourneyWatch)
 *   start   — the start set launched from it (AutostartWalk)
 *   display — the display program took the glass (ScreenClaim)
 *
 * A word marked again is not marked twice: the journey is when each step was
 * first reached. Any core, any context — an interrupt's or a Touch watch's on
 * the publisher's stack included; the lock under it is a leaf.
 */
void JourneyMark(const char *what);

/* Listen for the first volume mounted. Called once, after Touch is up and
 * before the boot mount. */
void JourneyWatch(void);

#endif /* JOURNEY_H */
