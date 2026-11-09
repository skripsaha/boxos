#ifndef AUTOSTART_H
#define AUTOSTART_H

#include "ktypes.h"

struct process_t;

/*
 * Start every program the mounted volume tags autostart, at boot: the state
 * is set directly and one sweep afterwards enqueues everything. Returns how
 * many were started. Safe to call with no volume — it starts nothing. A volume
 * that arrives later is started by the watch (AutostartWatchVolume).
 */
int AutostartLaunchFromVolume(struct process_t **out_first);

/* Whether a volume is mounted: the boot says which it waits for — a volume,
 * or nothing at all, the volume here asking to start no program. */
bool AutostartVolumeHere(void);

/* Say that a volume's programs are already running, so a later mount does not
 * start second copies. The boot calls this when it launched anything. */
void AutostartNoteVolumeLaunched(void);

/* Listen for a volume being mounted. Set AFTER the boot has had its go, so
 * this only ever catches a volume that arrived too late for it. */
void AutostartWatchVolume(void);

#endif /* AUTOSTART_H */
