#ifndef BOX_HEAP_H
#define BOX_HEAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/error.h"


#define HEAP_TAG_NONE      0xFF
#define HEAP_TAG_NAME_MAX  32
#define HEAP_TAG_CAP       64


void* malloc(size_t size);
void* malloc_tagged(size_t size, const char *tag);
void  free(void* ptr);

void* aligned_alloc(size_t alignment, size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);


uint8_t heap_register_tag(const char *name);

uint8_t heap_lookup_tag(const char *name);

const char *heap_tag_name(uint8_t id);

size_t heap_count_tag(const char *tag);

typedef void (*HeapTagCallback)(void *ptr, size_t size, const char *tag_name, void *userdata);

void heap_iterate_tag(const char *tag, HeapTagCallback cb, void *userdata);

void heap_iterate_all_tagged(HeapTagCallback cb, void *userdata);

void heap_dump_tags(void);


error_t heap_get_last_error(void);

typedef struct {
    size_t total_allocated;
    size_t total_free;
    size_t heap_used;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t malloc_calls;
    uint32_t free_calls;
} heap_stats_t;

void heap_get_stats(heap_stats_t *out);


void strand_pool_flush_self(void);

int strand_pool_test_orphan_reclaim(unsigned n_blocks);

#ifdef __cplusplus
}
#endif

#endif