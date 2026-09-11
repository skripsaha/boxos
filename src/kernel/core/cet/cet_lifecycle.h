
#ifndef CET_LIFECYCLE_H
#define CET_LIFECYCLE_H

#include "ktypes.h"
#include "error.h"

struct process_t;

error_t cet_lifecycle_init_bsp(void);

void cet_lifecycle_init_ap(void);

error_t cet_lifecycle_init_supervisor_ssp(uint8_t core_idx);

void cet_lifecycle_release_supervisor_ssp(uint8_t core_idx);

error_t cet_process_create_kernel_ssp(struct process_t *proc, uintptr_t entry_rip);

void cet_process_destroy_kernel_ssp(struct process_t *proc);

__attribute__((noreturn))
void cet_supv_shstk_activate_and_jump(void (*target)(void));

error_t cet_process_create(struct process_t *proc);

void cet_process_destroy(struct process_t *proc);

bool cet_is_enabled(void);

void cet_load_user_ssp_for_iretq(struct process_t *proc);

void cet_lifecycle_record_cp_fault(void);

typedef struct {
    bool     enabled;
    bool     shstk_active;
    bool     ibt_active;
    bool     xsave_cet_s;
    bool     xsave_cet_u;
    uint64_t s_cet_msr;
    uint64_t u_cet_msr;
    uint64_t cp_faults;
    uint64_t ssp_allocs;
    uint64_t ssp_frees;
} cet_lifecycle_stats_t;

void cet_lifecycle_get_stats(cet_lifecycle_stats_t *out);

void cet_lifecycle_dump(void);

#endif