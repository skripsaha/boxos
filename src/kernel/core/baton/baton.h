#ifndef BATON_H
#define BATON_H


#include "ktypes.h"
#include "kernel_config.h"
#include "error.h"

typedef struct Baton Baton;
struct Baton {
    Baton *volatile next;
    void (*run)(void *ctx);
    void  *ctx;
};

typedef struct BatonQueue {
    Baton *volatile tail;
    char _pad_tail[CONFIG_CACHE_LINE_SIZE - sizeof(Baton *)];

    Baton *head;
    char _pad_head[CONFIG_CACHE_LINE_SIZE - sizeof(Baton *)];

    Baton  stub;

    volatile uint64_t  pushed;
    volatile uint64_t  popped;
} BatonQueue;

extern BatonQueue *g_baton;
extern volatile uint8_t        g_baton_ready;

void BatonInit(void);

bool BatonPass(Baton *n);

uint32_t BatonPump(uint8_t core_idx);

bool BatonPending(uint8_t core_idx);

bool BatonOutstanding(uint8_t core_idx);

typedef struct Knock {
    Baton             baton;
    volatile uint32_t raised;
} Knock;

void KnockInit(Knock *k, void (*run)(void *ctx), void *ctx);

void KnockOn(Knock *k);

void KnockOpen(Knock *k);

error_t BatonSelfTest(void);

#endif