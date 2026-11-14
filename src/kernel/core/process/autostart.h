#ifndef AUTOSTART_H
#define AUTOSTART_H

#include "ktypes.h"

struct process_t;

/* The file of the program that reads the person's list from its Settings and
 * starts each program on it — the third of the system's start set. It starts
 * the list only for being started by the system (no spawner in its cabin); run
 * by a person it shows and changes the list. */
#define AUTOSTART_PROGRAM "autostart.box"

/*
 * Start the system's start set — display.box, hub.box and autostart.box, by
 * those names, in that order — from the mounted volume, at boot: the state is
 * set directly and one sweep afterwards enqueues everything. Returns how many
 * were started. Safe to call with no volume — it starts nothing. A volume
 * that arrives later is started by the watch (AutostartWatchVolume).
 */
int AutostartLaunchFromVolume(struct process_t **out_first);

/* Whether a volume is mounted: the boot says which it waits for — a volume,
 * or nothing at all, the volume here holding none of the start set. */
bool AutostartVolumeHere(void);

/* Say that a volume's programs are already running, so a later mount does not
 * start second copies. The boot calls this when it launched anything. */
void AutostartNoteVolumeLaunched(void);

/* Listen for a volume being mounted. Set AFTER the boot has had its go, so
 * this only ever catches a volume that arrived too late for it. */
void AutostartWatchVolume(void);

/* Whether `spawner` is the autostart.box the system started: the programs it
 * starts are the system's own, and the ones whose box says rise are watched
 * (rise.h) as the start set's are. */
bool AutostartStartedBy(const struct process_t *spawner);

/* The file started again as the start set's programs are started — still a
 * live file — made and held, not yet let run: the caller lets it run
 * (ProcessStartCreated) and gives the reference back. NULL when the file is
 * gone, or could not be started (said). For rise.h. */
struct process_t *AutostartAgain(uint64_t file_id);

#endif /* AUTOSTART_H */
