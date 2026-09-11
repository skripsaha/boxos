#ifndef BOX_BAY_H
#define BOX_BAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"


#define BAY_OPEN        0x00u
#define BAY_CREATE      0x01u
#define BAY_RO          0x02u

#define BAY_ENCRYPTED   0x04u

void   *bay_open(const char *tag, uint64_t size, uint32_t flags);

int     bay_release(void *ptr);

uint64_t bay_size(void *ptr);

#ifdef __cplusplus
}
#endif

#endif