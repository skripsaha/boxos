#ifndef AUTOSTART_H
#define AUTOSTART_H

#include "ktypes.h"

struct process_t;

int AutostartLaunchFromVolume(struct process_t **out_first, bool scheduler_live);

void AutostartNoteStandIn(uint32_t pid);

void AutostartNoteVolumeLaunched(void);

void AutostartWatchVolume(void);

#endif