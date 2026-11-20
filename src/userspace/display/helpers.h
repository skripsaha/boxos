#ifndef BOXOS_DISPLAY_HELPERS_H
#define BOXOS_DISPLAY_HELPERS_H

// The display's helpers: strands of its own, one for each App Core the
// machine has beyond the first, three at most, that compose a frame's tiles
// beside its main strand (frame.h Hands) — every program's, whatever it
// shows, so a frame takes as many cores as there are tiles to share. Between
// frames they sleep, parked: they cost nothing while nothing changes.
// Nothing else reaches them: what is addressed to the display's tag comes to
// its main strand (the kernel's rule for a program's strands), and so do the
// keys.

// The helpers started and given to the frames; on a machine of one App Core
// there are none, and the main strand composes every tile.
void HelpersStart() noexcept;

#endif
