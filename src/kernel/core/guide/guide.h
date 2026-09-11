#ifndef GUIDE_H
#define GUIDE_H

#include "ktypes.h"
#include "ready_queue.h"
#include "pocket.h"
#include "klib.h"

typedef struct process_t process_t;

void guide_init(void);
void guide(void);

void guide_process_one(process_t *proc);

void guide_dispatch_stats(uint64_t out[2]);

#endif