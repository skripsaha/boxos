#include "acpi_internal.h"
#include "klib.h"


#define HPET_BLOCKID_VENDOR_SHIFT     16
#define HPET_BLOCKID_LEGACY_REPLACE   (1u << 15)
#define HPET_BLOCKID_COUNT_SIZE_CAP   (1u << 13)
#define HPET_BLOCKID_NUM_TIMERS_MASK  (0x1Fu << 8)
#define HPET_BLOCKID_NUM_TIMERS_SHIFT 8
#define HPET_BLOCKID_REV_ID_MASK      0xFFu

void acpi_parse_hpet(void) {
    g_acpi.hpet.present = false;

    acpi_hpet_t* hpet = (acpi_hpet_t*)acpi_find_table("HPET");
    if (!hpet) {
        debug_printf("[ACPI] No HPET table — system clock will rely on PIT/TSC\n");
        return;
    }

    if (hpet->base_address.address_space != 0) {
        debug_printf("[ACPI] HPET base GAS address_space=%u (expected SystemMemory)\n",
                     hpet->base_address.address_space);
        return;
    }
    if (hpet->base_address.address == 0) {
        debug_printf("[ACPI] HPET base address is zero — ignoring\n");
        return;
    }

    uint32_t block = hpet->event_timer_block_id;
    uint8_t  rev   = (uint8_t)(block & HPET_BLOCKID_REV_ID_MASK);
    if (rev == 0) {
        debug_printf("[ACPI] HPET rev_id=0 — firmware did not initialise table; ignoring\n");
        return;
    }

    g_acpi.hpet.base               = (uintptr_t)hpet->base_address.address;
    g_acpi.hpet.vendor_id          = (uint16_t)(block >> HPET_BLOCKID_VENDOR_SHIFT);
    g_acpi.hpet.legacy_replacement = (block & HPET_BLOCKID_LEGACY_REPLACE) ? 1 : 0;
    g_acpi.hpet.counter_size_64    = (block & HPET_BLOCKID_COUNT_SIZE_CAP) ? 1 : 0;
    g_acpi.hpet.comparator_count   =
        (uint8_t)(((block & HPET_BLOCKID_NUM_TIMERS_MASK) >> HPET_BLOCKID_NUM_TIMERS_SHIFT) + 1);
    g_acpi.hpet.hpet_number        = hpet->hpet_number;
    g_acpi.hpet.minimum_tick       = hpet->minimum_tick;
    g_acpi.hpet.present            = true;

    debug_printf("[ACPI] HPET#%u vendor=0x%04x base=0x%lx %s counter, %u comparators, "
                 "min_tick=%u%s\n",
                 g_acpi.hpet.hpet_number,
                 g_acpi.hpet.vendor_id,
                 (unsigned long)g_acpi.hpet.base,
                 g_acpi.hpet.counter_size_64 ? "64-bit" : "32-bit",
                 g_acpi.hpet.comparator_count,
                 g_acpi.hpet.minimum_tick,
                 g_acpi.hpet.legacy_replacement ? " [LegacyReplace]" : "");
}

const acpi_hpet_info_t *acpi_get_hpet(void) {
    return g_acpi.hpet.present ? &g_acpi.hpet : NULL;
}