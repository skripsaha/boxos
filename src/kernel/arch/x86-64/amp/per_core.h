#ifndef PER_CORE_H
#define PER_CORE_H

#include "ktypes.h"
#include "notify.h"
#include "tss.h"
#include "gdt.h"
#include "amp.h"

#define PER_CORE_GDT_ENTRIES  7

typedef struct {
    PerCpuData          notify;

    uint32_t            lapic_id;
    uint8_t             core_index;
    bool                is_kcore;
    bool                initialized;
    uint8_t             _pad0;

    gdt_entry_t         gdt[PER_CORE_GDT_ENTRIES] __attribute__((aligned(16)));
    gdt_descriptor_t    gdt_desc;

    tss_t               tss __attribute__((aligned(16)));

    uint64_t            kernel_stack_top;

    uint64_t            kernel_stack_floor;

    uintptr_t           pl0_ssp_phys;
    uintptr_t           pl0_ssp_top_va;
    uintptr_t           isst_phys;
    uintptr_t           isst_va;
    uintptr_t           ist_ssp_phys[5];
    uintptr_t           ist_ssp_top_va[5];
    bool                cet_supv_ready;
    uint8_t             _pad1[7];
} __attribute__((aligned(64))) PerCoreData;

extern PerCoreData g_per_core[MAX_CORES];
extern volatile bool g_per_core_active;

void per_core_init_bsp(void);

void per_core_init_ap(uint8_t core_index, uint64_t stack_top);

void per_core_set_kernel_rsp(uint64_t top, uint64_t floor);

static inline uint64_t per_core_current_kstack_top(void) {
    if (!__atomic_load_n(&g_per_core_active, __ATOMIC_ACQUIRE)) return 0;
    uint64_t top;
    __asm__ volatile("mov %%gs:%c1, %0"
                     : "=r"(top)
                     : "i"(__builtin_offsetof(PerCpuData, kernel_rsp)));
    return top;
}

static inline uint64_t per_core_current_kstack_floor(void) {
    if (!__atomic_load_n(&g_per_core_active, __ATOMIC_ACQUIRE)) return 0;
    uint64_t floor;
    __asm__ volatile("mov %%gs:%c1, %0"
                     : "=r"(floor)
                     : "i"(__builtin_offsetof(PerCoreData, kernel_stack_floor)));
    return floor;
}

void per_core_record_bsp_tsc_anchor(uint64_t now_us);

#endif