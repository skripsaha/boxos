#ifndef BOX_STRAND_H
#define BOX_STRAND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"

uint32_t strand_spawn(void (*fn)(void *arg), void *arg);

uint32_t strand_spawn_joinable(void (*fn)(void *arg), void *arg);

void strand_release(uint32_t pid);

void strand_exit(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif