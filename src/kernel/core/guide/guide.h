#ifndef GUIDE_H
#define GUIDE_H

#include "ktypes.h"
#include "ready_queue.h"
#include "pocket.h"
#include "klib.h"

typedef struct process_t process_t;

void guide_init(void);
void guide(void);

/* Process all pending Pockets for one process (drain PocketRing, write
 * Results). Used by the K-Core guide loop. Does NOT change process state —
 * the caller manages that. */
void guide_process_one(process_t *proc);

/* Dispatch transport split: out[0] = Manifests that arrived enclosed in the
 * envelope, out[1] = Manifests read from cabin memory by address. Surfaced
 * via system.perf.dump. */
void guide_dispatch_stats(uint64_t out[2]);

#endif /* GUIDE_H */
