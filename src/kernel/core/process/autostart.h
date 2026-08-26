#ifndef AUTOSTART_H
#define AUTOSTART_H

#include "ktypes.h"

struct process_t;

/*
 * Start every program the mounted volume tags autostart.
 *
 * `scheduler_live` says whether processes have to put themselves on a run
 * queue: false during boot, where the state is set directly and one sweep
 * afterwards enqueues everything; true once the machine is up. Returns how
 * many were started. Safe to call with no volume — it starts nothing.
 */
int AutostartLaunchFromVolume(struct process_t **out_first, bool scheduler_live);

/* Remember the embedded shell the kernel started because there was nothing to
 * start from a volume. It is relieved of the watch if a volume turns up later
 * carrying programs of its own. Pass 0 to forget it. */
void AutostartNoteStandIn(uint32_t pid);

/* Say that a volume's programs are already running, so a later mount does not
 * start second copies. The boot calls this when it launched anything. */
void AutostartNoteVolumeLaunched(void);

/* Listen for a volume being mounted. Set AFTER the boot has had its go, so
 * this only ever catches a volume that arrived too late for it. */
void AutostartWatchVolume(void);

#endif /* AUTOSTART_H */
