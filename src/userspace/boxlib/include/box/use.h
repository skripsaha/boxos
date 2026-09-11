#ifndef BOX_USE_H
#define BOX_USE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"



int use_set(const char *tags, bool *remembered);

int use_clear(bool *remembered);

int use_get(char *buf, size_t cap, size_t *needed);

#ifdef __cplusplus
}
#endif

#endif