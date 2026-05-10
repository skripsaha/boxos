#include "acpi_internal.h"
#include "aml.h"
#include "io.h"
#include "klib.h"
#include "idt.h"
#include "irqchip.h"
#include "ioapic.h"

acpi_state_t g_acpi = {0};

/* ============================================================
 * GPE registry — one C callback per GPE bit. Lookup is O(1).
 * ============================================================ */
static acpi_gpe_handler_t g_gpe_handlers[ACPI_MAX_GPES];

int acpi_gpe_register(uint16_t gpe, acpi_gpe_handler_t cb) {
    if (gpe >= ACPI_MAX_GPES) return -1;
    g_gpe_handlers[gpe] = cb;
    return 0;
}

void acpi_gpe_unregister(uint16_t gpe) {
    if (gpe >= ACPI_MAX_GPES) return;
    g_gpe_handlers[gpe] = NULL;
}

/*
 * ACPI sleep entry. ACPI 6.5 §16.1.
 *
 *   1. Resolve \_S<state> from the AML namespace (Package of bytes;
 *      [0] = SLP_TYPa, [1] = SLP_TYPb).
 *   2. Call \_PTS(state) if present — gives firmware a chance to
 *      prepare. We only *find* the method today; full TermList
 *      execution belongs to the AML executor that lives next to
 *      `aml_call_method` (Phase DD).
 *   3. Write SLP_TYP|SLP_EN to PM1a_CNT (and PM1b if PM1b_CNT != 0).
 *   4. For S1 the CPU immediately enters low-power state; on wake the
 *      firmware clears SLP_EN, runs \_WAK and returns control here.
 *   5. S5 path delegates to acpi_shutdown() so the dedicated soft-off
 *      sequence runs (it also masks IRQs etc.).
 */
int acpi_enter_sleep(uint8_t state) {
    if (state < 1 || state > 5) return -1;
    if (!g_acpi.initialized || !g_acpi.fadt) return -2;
    if (state == 5) acpi_shutdown();   /* noreturn */

    char path[6] = { '\\', '_', 'S', '0' + (char)state, '_', 0 };
    uint64_t typa = 0;
    extern aml_status_t aml_read_integer(const char*, uint64_t*);
    if (aml_read_integer(path, &typa) != 0) {
        debug_printf("[ACPI] \\_S%u not found in namespace\n", state);
        return -3;
    }
    /* For SLP_TYPb we'd need a second integer; the namespace builder
     * only captures the first int of a Package. Most platforms use
     * the same value for both ports, which is the safe default. */
    uint16_t slp_typa = (uint16_t)(typa & 0x7);
    uint16_t slp_typb = slp_typa;

    debug_printf("[ACPI] entering S%u (SLP_TYPa=0x%x)\n", state, slp_typa);

    /* Invoke \_PTS(state) and \_BFS(state) if present — the AML
     * executor now handles integer-result methods. */
    {
        uint64_t arg = state;
        uint64_t ret = 0;
        if (aml_call_int("\\_PTS", &arg, 1, &ret) == AML_OK) {
            debug_printf("[ACPI] \\_PTS(%u) returned %lu\n",
                         state, (unsigned long)ret);
        }
        if (aml_call_int("\\_BFS", &arg, 1, &ret) == AML_OK) {
            debug_printf("[ACPI] \\_BFS(%u) returned %lu\n",
                         state, (unsigned long)ret);
        }
    }

    uint32_t pm1a = g_acpi.fadt->pm1a_control_block;
    uint32_t pm1b = g_acpi.fadt->pm1b_control_block;
    uint16_t va = (uint16_t)((slp_typa << 10) | (1u << 13));
    uint16_t vb = (uint16_t)((slp_typb << 10) | (1u << 13));
    if (pm1a) outw((uint16_t)pm1a, va);
    if (pm1b) outw((uint16_t)pm1b, vb);

    /* On S1, control returns here on wake. Other states either reset
     * the CPU (S2-S3) or never return (S4-S5). */
    debug_printf("[ACPI] resumed from S%u\n", state);
    return 0;
}

/* Dispatch every fired bit in `block_base..block_base+half_len` to the
 * registered handler. `gpe_base` is the global GPE index of bit 0 in
 * this byte. */
static void gpe_dispatch_block(uint8_t* fired_bytes, uint8_t bytes,
                                uint16_t gpe_base) {
    for (uint8_t b = 0; b < bytes; b++) {
        uint8_t f = fired_bytes[b];
        while (f) {
            int bit = __builtin_ctz(f);
            uint16_t idx = (uint16_t)(gpe_base + b * 8 + bit);
            if (idx < ACPI_MAX_GPES && g_gpe_handlers[idx]) {
                g_gpe_handlers[idx](idx);
            }
            f = (uint8_t)(f & ~(1u << bit));
        }
    }
}

/*
 * SCI interrupt handler — fires for any ACPI-routed event:
 *   - PM1 status bits (power button, sleep button, RTC alarm, wake)
 *   - GPE block status (each device-described event)
 *   - GHES SCI-class notifications (fixed-source RAS errors)
 *
 * Without a full AML interpreter we cannot dispatch GPE handlers, so
 * this stub:
 *   1. Reads PM1a status,
 *   2. Acknowledges every set status bit by writing the same value back
 *      (W1C semantics on PM1_STS — required to clear the SCI line),
 *   3. Logs the power-button / sleep-button bits for visibility.
 *
 * The PM1 STATUS block is at PM1a_CNT - PM1_EVENT_LENGTH/2, but the
 * canonical FADT layout puts it in `pm1a_event_block`. We use that.
 */
#define PM1_STS_PWRBTN  (1u << 8)
#define PM1_STS_SLPBTN  (1u << 9)
#define PM1_STS_RTC     (1u << 10)
#define PM1_STS_WAK     (1u << 15)

static void acpi_sci_handler(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) {
        irqchip_send_eoi(g_acpi.fadt ? g_acpi.fadt->sci_interrupt : 9);
        return;
    }
    uint32_t pm1a_evt = g_acpi.fadt->pm1a_event_block;
    if (pm1a_evt == 0) goto eoi;

    uint16_t sts = inw((uint16_t)pm1a_evt);

    if (sts & PM1_STS_PWRBTN) {
        debug_printf("[ACPI] power button event\n");
    }
    if (sts & PM1_STS_SLPBTN) {
        debug_printf("[ACPI] sleep button event\n");
    }
    if (sts & PM1_STS_RTC) {
        debug_printf("[ACPI] RTC alarm event\n");
    }
    if (sts & PM1_STS_WAK) {
        debug_printf("[ACPI] wake event\n");
    }

    /* W1C: write the read value back to clear every set bit at once.
     * This also drops the SCI line so the IOAPIC can re-arm. */
    if (sts) outw((uint16_t)pm1a_evt, sts);

    /* GPE0 / GPE1 drain. The status and enable blocks live back-to-back
     * inside each GPE block: STS at offset 0, EN at offset length/2.
     * Walk byte-by-byte, mask STS against EN so we only clear events we
     * armed, and W1C any bit that fired. Without an AML interpreter we
     * cannot run the per-bit _Lxx / _Exx methods, so this acks the
     * event and logs visibility but does not dispatch device-level
     * work. That dispatch belongs to a future AML interpreter audit. */
    uint32_t gpe0 = g_acpi.fadt->gpe0_block;
    uint8_t  gpe0_len = g_acpi.fadt->gpe0_length;
    if (gpe0 && gpe0_len >= 2) {
        uint8_t half = (uint8_t)(gpe0_len / 2);
        uint8_t fired_buf[32] = {0};
        if (half > 32) half = 32;
        for (uint8_t b = 0; b < half; b++) {
            uint8_t s = inb((uint16_t)(gpe0 + b));
            uint8_t e = inb((uint16_t)(gpe0 + half + b));
            fired_buf[b] = (uint8_t)(s & e);
            if (fired_buf[b]) {
                debug_printf("[ACPI] GPE0 byte %u: fired=0x%02x\n", b, fired_buf[b]);
                outb((uint16_t)(gpe0 + b), fired_buf[b]);
            }
        }
        gpe_dispatch_block(fired_buf, half, 0);
    }
    uint32_t gpe1 = g_acpi.fadt->gpe1_block;
    uint8_t  gpe1_len = g_acpi.fadt->gpe1_length;
    if (gpe1 && gpe1_len >= 2) {
        uint8_t half = (uint8_t)(gpe1_len / 2);
        uint8_t fired_buf[32] = {0};
        if (half > 32) half = 32;
        for (uint8_t b = 0; b < half; b++) {
            uint8_t s = inb((uint16_t)(gpe1 + b));
            uint8_t e = inb((uint16_t)(gpe1 + half + b));
            fired_buf[b] = (uint8_t)(s & e);
            if (fired_buf[b]) {
                debug_printf("[ACPI] GPE1 byte %u: fired=0x%02x\n", b, fired_buf[b]);
                outb((uint16_t)(gpe1 + b), fired_buf[b]);
            }
        }
        gpe_dispatch_block(fired_buf, half, g_acpi.fadt->gpe1_base);
    }

eoi:
    irqchip_send_eoi(g_acpi.fadt->sci_interrupt);
}

void acpi_sci_register(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return;
    uint16_t sci = g_acpi.fadt->sci_interrupt;
    if (sci == 0) {
        debug_printf("[ACPI] FADT advertises no SCI GSI\n");
        return;
    }
    if (sci >= IRQ_MAX_COUNT) {
        debug_printf("[ACPI] SCI GSI %u beyond IOAPIC range\n", sci);
        return;
    }
    irq_register_handler((uint8_t)sci, acpi_sci_handler);
    irqchip_enable_irq((uint8_t)sci);
    debug_printf("[ACPI] SCI handler registered on GSI %u\n", sci);
}

/* ACPI 6.5 §16.3.2: many real firmwares boot in PIC/SMI mode (SCI_EN=0).
 * The OS must write FADT.ACPI_ENABLE to FADT.SMI_CMD then poll PM1a_CNT
 * until bit 0 (SCI_EN) becomes 1 before any PM1 write will take effect.
 * UEFI implementations usually have SCI_EN=1 already; legacy BIOS often
 * doesn't. Idempotent. */
static void acpi_enable_mode(void) {
    if (!g_acpi.fadt) return;
    uint32_t smi   = g_acpi.fadt->smi_command_port;
    uint8_t  byte  = g_acpi.fadt->acpi_enable;
    uint32_t pm1a  = g_acpi.pm1a_cnt_blk;

    if (smi == 0 || byte == 0 || pm1a == 0) {
        debug_printf("[ACPI] SCI handoff not needed (smi=%u en=0x%x pm1a=0x%x)\n",
                     smi, byte, pm1a);
        return;
    }
    uint16_t sts = inw((uint16_t)pm1a);
    if (sts & 0x1) {
        debug_printf("[ACPI] SCI_EN already set — platform in ACPI mode\n");
        return;
    }
    debug_printf("[ACPI] handing off to ACPI mode (smi=0x%x byte=0x%x)\n",
                 smi, byte);
    outb((uint16_t)smi, byte);

    /* Spin for SCI_EN bit. Spec gives no max time; we cap at 3 seconds
     * using a coarse delay so we don't hang the boot on a misbehaved
     * firmware. */
    for (uint32_t i = 0; i < 3000; i++) {
        if (inw((uint16_t)pm1a) & 0x1) {
            debug_printf("[ACPI] SCI_EN raised after %u ms\n", i);
            return;
        }
        /* ~1 ms IO delay — outb 0x80 is the classic post-port no-op. */
        for (uint32_t j = 0; j < 1000; j++) outb(0x80, 0);
    }
    debug_printf("[ACPI] timed out waiting for SCI_EN (firmware bug)\n");
}

acpi_error_t acpi_init(void) {
    debug_printf("[ACPI] Initializing ACPI subsystem...\n");

    memset(&g_acpi, 0, sizeof(acpi_state_t));

    g_acpi.rsdp = acpi_find_rsdp();
    if (!g_acpi.rsdp) {
        debug_printf("[ACPI] RSDP not found\n");
        return ACPI_ERR_RSDP_NOT_FOUND;
    }

    debug_printf("[ACPI] RSDP found: OEM=%.6s, Revision=%u\n",
                 g_acpi.rsdp->oem_id, g_acpi.rsdp->revision);

    acpi_error_t err = acpi_parse_tables(g_acpi.rsdp);
    if (err != ACPI_OK) {
        debug_printf("[ACPI] Failed to parse ACPI tables: %d\n", err);
        return err;
    }

    g_acpi.initialized = true;

    /* Switch the platform into ACPI mode if firmware booted us in
     * legacy/SMI mode. Must happen after FADT is captured but BEFORE
     * any PM1 write is attempted. */
    acpi_enable_mode();

    debug_printf("[ACPI] Initialization complete (HPET=%s MCFG=%s S5=%s)\n",
                 g_acpi.hpet.present ? "yes" : "no",
                 g_acpi.mcfg.present ? "yes" : "no",
                 g_acpi.s5_found     ? "yes" : "fallback");

#if CONFIG_ACPI_DEBUG
    acpi_print_info();
#endif

    return ACPI_OK;
}
