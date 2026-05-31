#ifndef AMP_H
#define AMP_H

#include "ktypes.h"

#define MAX_CORES           256
#define AP_TRAMPOLINE_PHYS  0x8000

typedef struct {
    uint32_t lapic_id;        // xAPIC (8-bit) or x2APIC (32-bit) ID — full width
    uint8_t  core_index;
    bool     is_bsp;
    bool     is_kcore;
    /* online: written by the AP from ap_entry_c with __ATOMIC_RELEASE after all
     * per-core state has been published; read by the BSP in amp_boot_aps with
     * __ATOMIC_ACQUIRE for the wait loop, and by every cross-CPU peer (IPI
     * broadcast, kcore_find_least_loaded, panic-halt) with __ATOMIC_ACQUIRE to
     * prove the target CPU is actually servicing interrupts. The flag is plain
     * uint8_t (not volatile bool) because every access is wrapped in the
     * C11 atomic builtins — `volatile` would only suppress the compiler's
     * scheduler, not provide cross-CPU ordering. */
    uint8_t  online;
} CoreDescriptor;

typedef struct {
    CoreDescriptor cores[MAX_CORES];
    uint8_t  total_cores;
    uint8_t  k_count;
    uint8_t  app_count;
    uint8_t  bsp_index;
    uint32_t bsp_lapic_id;    // full-width APIC ID of the BSP
    bool     multicore_active;
} AmpLayout;

extern AmpLayout g_amp;

void amp_init(void);
void amp_boot_aps(void);
uint8_t amp_get_core_index(void);
bool amp_is_kcore(void);
bool amp_is_appcore(void);
uint32_t amp_calculate_kcores(uint32_t total_cores);

/* True when the descriptor's `online` flag is set with __ATOMIC_ACQUIRE
 * semantics. Wraps the C11 atomic load so every caller (kcore submit,
 * IPI broadcast, panic halt) picks up the same memory-ordering rule
 * the AP wrote with on the publish side. */
static inline bool amp_core_online(const CoreDescriptor *c) {
    return __atomic_load_n(&c->online, __ATOMIC_ACQUIRE) != 0;
}

#endif // AMP_H
