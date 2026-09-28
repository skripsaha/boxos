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
    /* counted_at_boot: set by the BSP when this AP answered inside its wait
     * window. It exists to tell "came up on time" apart from "came up late",
     * which the online flag alone cannot — a core that misses its window and
     * arrives a moment later is not dead, and BoxOS no longer treats it as
     * though it were. Written and read only by the BSP during amp_boot_aps,
     * so it needs no atomics. */
    uint8_t  counted_at_boot;
    /* halted: this core's OWN word that it took the halt/panic IPI and is
     * about to cli;hlt. Written by the target from the IPI_PANIC branch of
     * irq_handler with __ATOMIC_RELEASE, read by the initiator (system_halt's
     * roll call) with __ATOMIC_ACQUIRE. It exists so a shutdown waits on the
     * FACT of each core's arrival instead of sleeping a guessed few
     * milliseconds and assuming: a core that answers late is not a core that
     * never answered, and a core that never answers can then be NAMED rather
     * than silently folded into the assumption. Cleared by the initiator
     * before it broadcasts, so an answer to an earlier broadcast cannot be
     * mistaken for an answer to this one. Plain uint8_t for the same reason
     * `online` is — every access goes through the C11 atomic builtins, and
     * `volatile` would order the compiler without ordering the CPUs. */
    uint8_t  halted;
    /* freeze_owed: an orderly halt's IPI found this core in the middle of a
     * Scribe pass (tagfs/scribe), so the core did not stop there: it owes the
     * halt its stop, and pays it in the ScribeGive that lets its last Scribe
     * go. freeze_asked_tsc is when the IPI came, for the line that says how
     * long the halt waited. Both written by this core alone, from its IPI_PANIC
     * branch, and read by it alone. */
    uint8_t  freeze_owed;
    uint64_t freeze_asked_tsc;
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
