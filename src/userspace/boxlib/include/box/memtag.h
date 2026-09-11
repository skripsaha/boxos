#ifndef BOX_MEMTAG_H
#define BOX_MEMTAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"


#define MEMTAG_INVALID_REGION_ID  0xFFFFFFFFu
#define MEMTAG_MAX_QUERY_RESULTS  512u

typedef struct {
    uint64_t   base_phys;
    uint64_t   base_virt;
    uint64_t   pages;
    uint16_t   tag_count;
    uint16_t   flags;
    uint32_t   generation;
} mem_region_info_t;

typedef struct {
    uint32_t  tag_count;
    uint32_t  region_active;
    uint32_t  region_slot_count;
    uint32_t  region_slot_cap;
    uint64_t  registry_generation;
    uint64_t  region_generation;
    uint64_t  bitmap_generation;
    uint64_t  cache_hits;
    uint64_t  cache_misses;
} mem_stats_t;

int      mem_query(const char *const *required,
                   const char *const *any,
                   const char *const *excluded,
                   uint32_t *out, uint32_t max_results);

int      mem_region_info(uint32_t region_id, mem_region_info_t *out);

uint32_t mem_region_from_phys(uint64_t phys);

uint32_t mem_region_from_virt(const void *virt);

error_t  mem_region_from_phys_ex(uint64_t phys, uint32_t *out_region_id);
error_t  mem_region_from_virt_ex(const void *virt, uint32_t *out_region_id);

int      mem_region_tags(uint32_t region_id,
                         char *out_buf, uint32_t out_buf_size,
                         uint32_t *out_count);

int      mem_stats(mem_stats_t *out);


typedef struct {
    uint8_t  allowed;
    uint8_t  pad[3];
    uint16_t missing_tag_id;
    uint16_t reserved;
} mem_check_t;

int  mem_set_guard(const char *tag_str, int on);
int  mem_cabin_grant(uint32_t pid, const char *tag_str);
int  mem_cabin_revoke(uint32_t pid, const char *tag_str);
int  mem_cabin_tags(uint32_t pid,
                     char *out_buf, uint32_t out_buf_size,
                     uint32_t *out_count);
int  mem_check_access(uint32_t pid, uint32_t region_id, mem_check_t *out);

#ifdef __cplusplus
}
#endif

#endif