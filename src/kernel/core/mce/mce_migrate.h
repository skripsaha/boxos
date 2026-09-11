
#ifndef MCE_MIGRATE_H
#define MCE_MIGRATE_H

#include "ktypes.h"
#include "mce.h"


void mce_migrate_init(void);

bool mce_migrate_is_initialized(void);

bool mce_migrate_request(uintptr_t phys, mce_severity_t sev, uint64_t status);

bool mce_migrate_note_nested(uintptr_t phys);


typedef struct {
    uint64_t requests;
    uint64_t drops;
    uint64_t completed;
    uint64_t failed;
    uint64_t no_owner;
    uint64_t nested_aborts;
    uint64_t cabins_touched;
    uint64_t pages_migrated;
    uint64_t skipped_2m;
} mce_migrate_stats_t;

void mce_migrate_get_stats(mce_migrate_stats_t *out);
void mce_migrate_dump(void);


uint32_t mce_migrate_run_sync(uintptr_t phys);

#endif