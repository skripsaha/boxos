#ifndef CROSSING_H
#define CROSSING_H

/*
 * This start's crossing (boxos_crossings.h) marked landed on the medium it
 * started from, once the display takes the screen — the point at which the
 * start counts as one that came up: the volume mounted, the start set running,
 * a program holding the glass. Called by the screen's claim with no lock held
 * (HwScreenClaim); once a start, whatever calls it again. A start whose pass
 * names no crossing — a BIOS boot, a medium whose front keeps none — has
 * nothing to mark, and nothing is said.
 */
void CrossingLand(void);

#endif /* CROSSING_H */
