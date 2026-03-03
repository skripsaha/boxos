#ifndef BOX_HEAP_H
#define BOX_HEAP_H

#include "box/defs.h"

void* malloc(size_t size);
void  free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

#endif // BOX_HEAP_H
