#include "amp.h"
#include "per_core.h"
#include "lapic.h"
#include "idt.h"
#include "irqchip.h"
#include "klib.h"
#include "scheduler.h"
#include "idle.h"
#include "kcore.h"
#include "cpuid.h"

void ap_entry_c(uint64_t core_index, uint64_t stack_top) {
    per_core_init_ap((uint8_t)core_index, stack_top);

    irqchip_apply_lapic_nmi_self();

    idt_load();

    scheduler_init_core((uint8_t)core_index);
    idle_process_init_core((uint8_t)core_index);

    __atomic_store_n(&g_amp.cores[core_index].online, (uint8_t)1, __ATOMIC_RELEASE);

    kprintf("[AMP] Core %u online (LAPIC ID %u, role=%s)\n",
            (uint32_t)core_index, lapic_get_id(),
            g_amp.cores[core_index].is_kcore ? "K-Core" : "App-Core");

    char prefix[24];
    ksnprintf(prefix, sizeof(prefix), "AP %u [%c]",
              (unsigned)core_index,
              g_amp.cores[core_index].is_kcore ? 'K' : 'A');
    cpu_log_identity(prefix);

    if (g_amp.cores[core_index].is_kcore) {
        extern void cet_supv_shstk_activate_and_jump(void (*)(void));
        cet_supv_shstk_activate_and_jump(kcore_run_loop);
    }

    extern void cet_supv_shstk_activate_and_jump(void (*)(void));
    extern void app_core_idle_loop(void);
    cet_supv_shstk_activate_and_jump(app_core_idle_loop);
}

__attribute__((noreturn))
void app_core_idle_loop(void) {
    __asm__ volatile("sti");
    while (1) {
        __asm__ volatile("hlt");
    }
}