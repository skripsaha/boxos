#include "pmtimer.h"
#include "hourglass.h"
#include "io.h"
#include "vmm.h"
#include "klib.h"

typedef struct {
    bool               present;
    bool               mmio;
    uint16_t           port;
    volatile uint32_t *reg;
    uint32_t           mask;
    bool               ext_claimed;
    uint32_t           high_seed;
} PmTimerRuler;

static PmTimerRuler g_pmt;

#define PMTIMER_MASK_24  0x00FFFFFFu
#define PMTIMER_MASK_32  0xFFFFFFFFu
#define FADT_FLAG_TMR_VAL_EXT     (1u << 8)
#define FADT_FLAG_HW_REDUCED_ACPI (1u << 20)

#define GAS_SPACE_SYSTEM_MEMORY  0x00
#define GAS_SPACE_SYSTEM_IO      0x01

#define PM_TMR_LEN_SUPPORTED     4

static bool fadt_has(const acpi_fadt_t *fadt, size_t end_offset)
{
    return fadt->header.length >= end_offset;
}

static bool adopt_gas(const acpi_gas_t *gas)
{
    if (gas->bit_width != 32) {
        kprintf("[PMT] X_PM_TIMER_BLOCK bit_width %u, expected 32 — refused\n",
                (unsigned)gas->bit_width);
        return false;
    }

    if (gas->address_space == GAS_SPACE_SYSTEM_IO) {
        if (gas->address > 0xFFFFu) {
            kprintf("[PMT] X_PM_TIMER_BLOCK SystemIO address 0x%lx exceeds "
                    "16-bit I/O space — refused\n", (unsigned long)gas->address);
            return false;
        }
        g_pmt.mmio = false;
        g_pmt.port = (uint16_t)gas->address;
        return true;
    }

    if (gas->address_space == GAS_SPACE_SYSTEM_MEMORY) {
        volatile void *map = vmm_map_mmio((uintptr_t)gas->address, 4,
                                          VMM_FLAG_PRESENT);
        if (!map) {
            kprintf("[PMT] X_PM_TIMER_BLOCK SystemMemory 0x%lx failed to map "
                    "— refused\n", (unsigned long)gas->address);
            return false;
        }
        g_pmt.mmio = true;
        g_pmt.reg  = (volatile uint32_t *)map;
        return true;
    }

    kprintf("[PMT] X_PM_TIMER_BLOCK address space %u is neither SystemMemory "
            "nor SystemIO — refused\n", (unsigned)gas->address_space);
    return false;
}

bool pmtimer_adopt_from_fadt(const acpi_fadt_t *fadt)
{
    if (g_pmt.mmio && g_pmt.reg)
        vmm_unmap_mmio(g_pmt.reg, 4);
    memset(&g_pmt, 0, sizeof(g_pmt));

    if (!fadt)
        return false;

    if (!fadt_has(fadt, offsetof(acpi_fadt_t, flags) + sizeof(uint32_t))) {
        kprintf("[PMT] FADT length %u does not reach the flags word — no timer\n",
                (unsigned)fadt->header.length);
        return false;
    }

    if (fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) {
        debug_printf("[PMT] hardware-reduced ACPI — no PM Timer by definition\n");
        return false;
    }

    bool have_gas = fadt_has(fadt, offsetof(acpi_fadt_t, x_pm_timer_block) +
                                   sizeof(acpi_gas_t)) &&
                    fadt->x_pm_timer_block.address != 0;

    if (have_gas) {
        if (!adopt_gas(&fadt->x_pm_timer_block))
            return false;
    } else if (fadt->pm_timer_length == PM_TMR_LEN_SUPPORTED &&
               fadt->pm_timer_block != 0) {
        if (fadt->pm_timer_block > 0xFFFFu) {
            kprintf("[PMT] legacy PM_TMR_BLK 0x%x exceeds 16-bit I/O space "
                    "— refused\n", (unsigned)fadt->pm_timer_block);
            return false;
        }
        g_pmt.mmio = false;
        g_pmt.port = (uint16_t)fadt->pm_timer_block;
    } else {
        debug_printf("[PMT] FADT advertises no PM Timer\n");
        return false;
    }

    g_pmt.ext_claimed = (fadt->flags & FADT_FLAG_TMR_VAL_EXT) != 0;
    g_pmt.mask        = PMTIMER_MASK_24;
    g_pmt.present     = true;

    uint32_t raw = g_pmt.mmio ? *g_pmt.reg : inl(g_pmt.port);
    g_pmt.high_seed = raw >> 24;

    if (g_pmt.mmio)
        kprintf("[PMT] PM Timer at SystemMemory 0x%lx, %s, 3.579545 MHz\n",
                (unsigned long)fadt->x_pm_timer_block.address,
                g_pmt.ext_claimed ? "32-bit claimed (unproven, reading 24)"
                                  : "24-bit");
    else
        kprintf("[PMT] PM Timer at I/O 0x%x, %s, 3.579545 MHz\n",
                (unsigned)g_pmt.port,
                g_pmt.ext_claimed ? "32-bit claimed (unproven, reading 24)"
                                  : "24-bit");
    return true;
}

bool pmtimer_is_present(void)
{
    return g_pmt.present;
}

uint32_t pmtimer_mask(void)
{
    return g_pmt.present ? g_pmt.mask : 0u;
}

bool pmtimer_read(uint32_t *out)
{
    if (!g_pmt.present || !out)
        return false;

    uint32_t raw = g_pmt.mmio ? *g_pmt.reg : inl(g_pmt.port);

    if (g_pmt.ext_claimed && g_pmt.mask == PMTIMER_MASK_24 &&
        (raw >> 24) != g_pmt.high_seed) {
        g_pmt.mask = PMTIMER_MASK_32;
    }

    *out = raw & g_pmt.mask;
    return true;
}

bool pmtimer_busy_wait_us(uint64_t us)
{
    if (!g_pmt.present)
        return false;

    uint64_t wrap_us = ((uint64_t)g_pmt.mask + 1ULL) * 1000000ULL / PMTIMER_FREQ_HZ;
    if (us > wrap_us)
        us = wrap_us;

    HourGlass g;
    if (!HourGlassTurnOn(&g, us, HOURGLASS_RULER_PMTIMER))
        return false;

    while (!HourGlassRunOut(&g))
        __asm__ volatile("pause");

    return !HourGlassSourceDied(&g);
}