#ifndef PARCEL_HOST_H
#define PARCEL_HOST_H

/*
 * parcel_host — the kernel's own file_map/ (file_record.c, tract.c,
 * file_record_splices.c) built for the host, unchanged, to measure what a
 * change of a very large file copies (tools/parcel_host.c).
 *
 * Forced into every unit ahead of its first line (-include): it stands in for
 * the kernel headers file_record.h reaches — tagfs.h and plot/strip.h, whose
 * guards it defines so the kernel's own are read as already seen — with the
 * few things those units ask of them: Strip and TagFSMetadata as the kernel
 * lays them out, the error codes, and the heap, counted (ParcelHostHeap).
 */

#define TAGFS_H
#define TAGFS_STRIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_STRICT 1
#define STATIC_ASSERT(c, m) _Static_assert(c, m)

typedef int error_t;
#define OK                   0
#define ERR_INVALID_ARGUMENT 22
#define ERR_NO_MEMORY        12

typedef struct {
    uint64_t start;
    uint32_t count;
    uint32_t kind;
    uint64_t birth;
} Strip;
STATIC_ASSERT(sizeof(Strip) == 24, "Strip must be 24 bytes, as plot/strip.h lays it out");

#define STRIP_DATA 0u
#define STRIP_HOLE 1u

typedef struct {
    uint64_t  file_id;
    uint32_t  flags;
    uint64_t  size;
    uint64_t  created_time;
    uint64_t  modified_time;
    char*     filename;
    uint32_t* tag_ids;
    uint32_t  tag_count;
    Strip*    extents;
    uint32_t  extent_count;
} TagFSMetadata;

typedef int (*KSortOrder)(const void* a, const void* b, void* ctx);
void KSort(void* base, size_t count, size_t size, KSortOrder order, void* ctx);

/* The heap, counted: every allocation and its bytes, every free, and the
 * bytes live — what a change keeps once its scratch is let go. */
typedef struct {
    uint64_t allocs;
    uint64_t bytes;
    uint64_t frees;
    uint64_t live;
} ParcelHostHeap;
extern ParcelHostHeap g_parcel_host_heap;
void* ParcelHostAlloc(size_t bytes);
void  ParcelHostFree(const void* p);

#define kmalloc(n)  ParcelHostAlloc(n)
#define kfree(p)    ParcelHostFree(p)
#define kprintf     printf
#define panic(...)  do { printf(__VA_ARGS__); printf("\n"); abort(); } while (0)

#endif /* PARCEL_HOST_H */
