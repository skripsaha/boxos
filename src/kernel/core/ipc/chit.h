#ifndef CHIT_H
#define CHIT_H

#include "ktypes.h"
#include "klib.h"


struct process_t;
struct OpContext;

typedef enum {
    CHIT_NONE    = 0,
    CHIT_PENDING = 1,
    CHIT_DUE     = 2,
    CHIT_KEPT    = 3,
} ChitState;

typedef struct ChitView {
    uint32_t    cookie;
    uint8_t     state;
    const char *holder;
    uint64_t    detail;
    uint64_t    since_ms;
} ChitView;

typedef struct Chit {
    spinlock_t  lock;
    ChitView    view;
} Chit;

void ChitInit(Chit *c);

void ChitGive(const struct OpContext *ctx, const char *holder, uint64_t detail);

void ChitDue(struct process_t *p, uint32_t cookie);

void ChitKeep(struct process_t *p, uint32_t cookie);

void ChitPeek(struct process_t *p, ChitView *out);

#endif