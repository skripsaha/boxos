#ifndef STRAND_RINGS_H
#define STRAND_RINGS_H

#include "ktypes.h"


typedef struct process_t process_t;

bool strand_rings_create(process_t *proc);

void strand_rings_destroy(process_t *proc);

#endif