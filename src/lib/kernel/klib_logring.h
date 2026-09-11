#ifndef KLIB_LOGRING_H
#define KLIB_LOGRING_H

#include "ktypes.h"

#define LOGRING_CAPACITY (1024u * 1024u)

void LogRingLockInit(void);

void LogRingPut(char c);

uint64_t LogRingWritten(void);
char     LogRingByteAt(uint64_t pos);

void LogRingForceRelease(void);

uint64_t LogRingRead(uint64_t from, void *dst, uint64_t max,
                     uint64_t *out_oldest, uint64_t *out_written);

#ifdef CONFIG_PRINTTOFILE


void LogKeepInit(void);

bool LogKeepWindow(uintptr_t *out_phys, uint64_t *out_bytes);

uint64_t LogKeepPreviousBytes(void);
uint32_t LogKeepPreviousBoot(void);

uint64_t LogKeepPreviousRead(uint64_t from, void *dst, uint64_t max,
                             uint64_t *out_oldest, uint64_t *out_written);

#else

static inline void LogKeepInit(void)     { }
static inline bool LogKeepWindow(uintptr_t *p, uint64_t *b)
{ (void)p; (void)b; return false; }
static inline uint64_t LogKeepPreviousBytes(void) { return 0; }
static inline uint32_t LogKeepPreviousBoot(void)  { return 0; }
static inline uint64_t LogKeepPreviousRead(uint64_t from, void *dst,
                                           uint64_t max, uint64_t *o,
                                           uint64_t *w)
{ (void)from; (void)dst; (void)max; if (o) *o = 0; if (w) *w = 0; return 0; }

#endif

#endif