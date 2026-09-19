#include "pmtimer.h"
#include "hourglass.h"
#include "io.h"
#include "vmm.h"
#include "klib.h"

/*
 * The timer's identity, owned here.
 *
 * It used to live in g_acpi, which meant the code that READ the counter and
 * the code that DESCRIBED it were in different subsystems and could drift
 * apart — and they had: acpi_tables.c stored a 32-bit address that pmtimer.c
 * truncated to 16 bits on every read, and acpi_tables.c accepted only a
 * SystemIO description while the struct comment claimed SystemMemory was
 * understood. One owner, one description.
 */
typedef struct {
    bool               present;
    bool               mmio;        /* SystemMemory GAS rather than a port */
    uint16_t           port;        /* SystemIO: x86 I/O space is 16 bits wide */
    volatile uint32_t *reg;         /* SystemMemory: UC mapping of the register */
    uint32_t           mask;        /* width in force — 24 bits until 32 is proven */
    bool               ext_claimed; /* FADT said TMR_VAL_EXT (32-bit) */
    uint32_t           high_seed;   /* bits 31:24 as first seen, to prove the claim */
} PmTimerRuler;

static PmTimerRuler g_pmt;

/* ACPI 6.5 §4.8.3.3: the counter is 24 bits, or 32 when FADT flags bit 8
 * (TMR_VAL_EXT) is set. Firmware lies about that bit, so a claim of 32 is
 * UNPROVEN until bits 31:24 are actually observed to change; until then the
 * 24-bit mask stands. Masking NARROWER than the hardware is always safe — a
 * 32-bit counter read as 24-bit simply wraps more often, and the wrap is
 * already handled by subtracting under the mask. Masking WIDER than the
 * hardware is not safe: the undefined upper bits of a 24-bit counter would
 * enter the delta as noise. */
#define PMTIMER_MASK_24  0x00FFFFFFu
#define PMTIMER_MASK_32  0xFFFFFFFFu
/* FADT Fixed Feature Flags, ACPI 6.5 §5.2.9. */
#define FADT_FLAG_TMR_VAL_EXT     (1u << 8)
#define FADT_FLAG_HW_REDUCED_ACPI (1u << 20)

/* ACPI 6.5 §5.2.3.2 — Generic Address Structure address space ids. */
#define GAS_SPACE_SYSTEM_MEMORY  0x00
#define GAS_SPACE_SYSTEM_IO      0x01

/* ACPI 6.5 §5.2.9 — PM_TMR_LEN is 4 when the timer is supported and 0 when
 * it is not. */
#define PM_TMR_LEN_SUPPORTED     4

static bool fadt_has(const acpi_fadt_t *fadt, size_t end_offset)
{
    return fadt->header.length >= end_offset;
}

static bool adopt_gas(const acpi_gas_t *gas)
{
    /* A bit width other than 32 is firmware sloppiness, not a different
     * register — and it is not grounds to drop the timer.
     *
     * ACPI 6.5 §5.2.9, Table 5.9 imposes a width where it means to: RESET_REG
     * and the sleep registers carry "Register_Bit_Width must be 8 and
     * Register_Bit_Offset must be 0", and X_GPE0_BLK/X_GPE1_BLK explicitly
     * waive one ("the OSPM ignores Register_Bit_Width, Register_Bit_Offset
     * and Access_Size"). X_PM_TMR_BLK's entry carries neither sentence; the
     * "must be 32" this code used to cite is not in that section. Both
     * reference implementations agree: ACPICA repairs the field in place —
     * AcpiTbSetupFadtRegisters() rewrites a disagreeing BitWidth to
     * FadtInfoTable's DefaultLength, ACPI_PM_TIMER_WIDTH = 32, because
     * AcpiGbl_UseDefaultRegisterWidths defaults to TRUE — and linux's
     * acpi_parse_fadt() never reads the field at all. */
    if (gas->bit_width != 32) {
        kprintf("[PMT] X_PM_TIMER_BLOCK bit_width %u where the register is 32 "
                "— taking the address anyway, as ACPICA and linux do\n",
                (unsigned)gas->bit_width);
    }

    if (gas->address_space == GAS_SPACE_SYSTEM_IO) {
        /* x86 I/O space is 16 bits. An address that does not fit is not a
         * port this machine has; truncating it would aim `inl` at a port
         * nobody claims, which reads 0xFFFFFFFF forever and is exactly the
         * dead-counter case this driver exists to refuse. */
        if (gas->address > 0xFFFFu) {
            kprintf("[PMT] X_PM_TIMER_BLOCK SystemIO address 0x%lx exceeds "
                    "16-bit I/O space — unusable\n", (unsigned long)gas->address);
            return false;
        }
        g_pmt.mmio = false;
        g_pmt.port = (uint16_t)gas->address;
        return true;
    }

    if (gas->address_space == GAS_SPACE_SYSTEM_MEMORY) {
        /* Mapped UC, and READ-ONLY: the counter is read-only hardware, so
         * leaving the page unwritable turns a stray kernel write into a
         * page fault at the offending instruction instead of an undefined
         * write into a power-management register. */
        volatile void *map = vmm_map_mmio((uintptr_t)gas->address, 4,
                                          VMM_FLAG_PRESENT);
        if (!map) {
            kprintf("[PMT] X_PM_TIMER_BLOCK SystemMemory 0x%lx failed to map "
                    "— unusable\n", (unsigned long)gas->address);
            return false;
        }
        g_pmt.mmio = true;
        g_pmt.reg  = (volatile uint32_t *)map;
        return true;
    }

    kprintf("[PMT] X_PM_TIMER_BLOCK address space %u is neither SystemMemory "
            "nor SystemIO — unusable\n", (unsigned)gas->address_space);
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

    /* ACPI 6.5 §5.2.9, HW_REDUCED_ACPI: when set, the platform has no
     * fixed-hardware register blocks at all and PM_TMR_BLK / X_PM_TMR_BLK
     * "must be ignored by OSPM". Reading them anyway is how you end up
     * polling an unclaimed port. */
    if (fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) {
        debug_printf("[PMT] hardware-reduced ACPI — no PM Timer by definition\n");
        return false;
    }

    /* Precedence, in the spec's own words. ACPI 6.5 §5.2.9, X_PM_TMR_BLK:
     * "If this field contains a nonzero value WHICH CAN BE USED BY THE OSPM,
     * then the PM_TMR_BLK field must be ignored by the OSPM." The override
     * is conditioned on the extended block being usable, so a GAS we could
     * NOT use is not the end of the timer — it is the spec's own trigger to
     * consult PM_TMR_BLK. That is what linux's acpi_parse_fadt() does:
     * "'X' fields are optional extensions to the original V1.0 fields, so we
     * must selectively expand V1.0 fields if the corresponding X field is
     * zero." A GAS we CAN use decides everything, including deciding that
     * the timer lives in SystemMemory — reading the legacy port against a
     * usable SystemMemory declaration is how the previous code polled an
     * address the firmware never pointed at. */
    bool have_gas = fadt_has(fadt, offsetof(acpi_fadt_t, x_pm_timer_block) +
                                   sizeof(acpi_gas_t)) &&
                    fadt->x_pm_timer_block.address != 0;
    bool adopted = have_gas && adopt_gas(&fadt->x_pm_timer_block);

    if (!adopted) {
        if (fadt->pm_timer_length != PM_TMR_LEN_SUPPORTED ||
            fadt->pm_timer_block == 0) {
            if (have_gas)
                kprintf("[PMT] X_PM_TIMER_BLOCK is unusable and PM_TMR_BLK "
                        "names no port — this machine has no PM Timer\n");
            else
                debug_printf("[PMT] FADT advertises no PM Timer\n");
            return false;
        }
        if (fadt->pm_timer_block > 0xFFFFu) {
            kprintf("[PMT] legacy PM_TMR_BLK 0x%x exceeds 16-bit I/O space "
                    "— refused\n", (unsigned)fadt->pm_timer_block);
            return false;
        }
        g_pmt.mmio = false;
        g_pmt.port = (uint16_t)fadt->pm_timer_block;
        if (have_gas)
            kprintf("[PMT] X_PM_TIMER_BLOCK unusable — falling back to the "
                    "legacy PM_TMR_BLK port 0x%x\n", (unsigned)g_pmt.port);
    }

    g_pmt.ext_claimed = (fadt->flags & FADT_FLAG_TMR_VAL_EXT) != 0;
    g_pmt.mask        = PMTIMER_MASK_24;
    g_pmt.present     = true;

    /* Seed the high byte from the first read so a later change in bits
     * 31:24 can be recognised as proof of the 32-bit claim. */
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

    /* Promotion is one-way and to a constant, so two cores racing here both
     * write the same value into an aligned 32-bit word — no torn state, no
     * lock. A glass already running keeps its narrower cached mask, which
     * stays correct: a 24-bit subtraction of two 32-bit samples is right as
     * long as the true delta is under 2^24, and a polling loop's delta is
     * never within four orders of magnitude of that. */
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

    /* One wrap period of the width currently PROVEN, in microseconds:
     * (mask+1) ticks / 3579545 Hz. At 24 bits that is 16777216/3579545 =
     * 4.687 s — which is what the "5 second" clamp that used to sit here
     * really was. Its stated reason, that a longer wait would overflow the
     * arithmetic, was never true: elapsed is summed in 64 bits. The real
     * reason is that a busy-wait longer than one wrap period is a caller
     * mistake, and the honest answer to a mistake is the bound, not a
     * silent extra lap. A counter proven 32-bit raises the same bound to
     * 4294967296/3579545 = 1199.9 s. */
    uint64_t wrap_us = ((uint64_t)g_pmt.mask + 1ULL) * 1000000ULL / PMTIMER_FREQ_HZ;
    if (us > wrap_us)
        us = wrap_us;

    /* Turned on THIS ruler by name rather than on the best one available.
     * The caller is cpu_calibrate_tsc, measuring the TSC against the PM
     * Timer; a glass that had quietly picked the TSC would measure the TSC
     * against itself and hand back the result as an independent source. */
    HourGlass g;
    if (!HourGlassTurnOn(&g, us, HOURGLASS_RULER_PMTIMER))
        return false;

    while (!HourGlassRunOut(&g))
        __asm__ volatile("pause");

    return !HourGlassSourceDied(&g);
}
