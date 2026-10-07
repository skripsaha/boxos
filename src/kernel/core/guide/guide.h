#ifndef GUIDE_H
#define GUIDE_H

#include "ktypes.h"
#include "ready_queue.h"
#include "pocket.h"
#include "klib.h"

typedef struct process_t process_t;

void guide_init(void);
void guide(void);

/* Serve the next Pocket of one process's PocketRing — one, never the ring
 * (RT-11): its Result written, the rest left for its next turn. Used by the
 * K-Core guide loop, which queues the process again while its ring holds
 * more. Does NOT change process state — the caller manages that. */
void guide_process_one(process_t *proc);

/* Dispatch transport split: out[0] = Manifests that arrived enclosed in the
 * envelope, out[1] = Manifests read from cabin memory by address. Surfaced
 * via system.perf.dump. */
void guide_dispatch_stats(uint64_t out[2]);

#endif /* GUIDE_H */
