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

#endif /* GUIDE_H */
