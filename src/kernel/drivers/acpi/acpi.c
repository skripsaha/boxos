#include "acpi_internal.h"
#include "aml.h"
#include "io.h"
#include "klib.h"
#include "idt.h"
#include "irqchip.h"
#include "ioapic.h"
#include "touch.h"
#include "logbook.h"
#include "system_halt.h"
#include "ioapic.h"
#include "baton.h"
#include "atomics.h"
#include "kernel_config.h"

acpi_state_t g_acpi = {0};

typedef struct {
    volatile TouchTag full;
    volatile TouchTag bare;
} AcpiTagPair;

static AcpiTagPair g_acpi_tag_power_button = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
static AcpiTagPair g_acpi_tag_sleep_button = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
static AcpiTagPair g_acpi_tag_rtc_alarm    = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
static AcpiTagPair g_acpi_tag_wake         = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
static AcpiTagPair g_acpi_tag_gpe          = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };

static void acpi_tag_resolve(AcpiTagPair *t, const char *name)
{
    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve(name, &full, &bare);
    __atomic_store_n(&t->full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&t->bare, bare, __ATOMIC_RELEASE);
}

static void acpi_publish_irq(const AcpiTagPair *t, uint16_t word)
{
    TouchTag full = __atomic_load_n(&t->full, __ATOMIC_ACQUIRE);
    TouchTag bare = __atomic_load_n(&t->bare, __ATOMIC_ACQUIRE);
    TouchPublishIrqPair(full, bare, &word, sizeof(word), 0, TOUCH_FLAG_KERNEL);
}

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

int acpi_enter_sleep(uint8_t state) {
    if (state < 1 || state > 5) return -1;
    if (!g_acpi.initialized || !g_acpi.fadt) return -2;
    if (state == 5) acpi_shutdown();

    char path[6] = { '\\', '_', 'S', '0' + (char)state, '_', 0 };
    uint64_t typa = 0;
    extern aml_status_t aml_read_integer(const char*, uint64_t*);
    if (aml_read_integer(path, &typa) != 0) {
        debug_printf("[ACPI] \\_S%u not found in namespace\n", state);
        return -3;
    }
    uint16_t slp_typa = (uint16_t)(typa & 0x7);
    uint16_t slp_typb = slp_typa;

    debug_printf("[ACPI] entering S%u (SLP_TYPa=0x%x)\n", state, slp_typa);

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

    struct { uint8_t state; uint16_t slp_typa; uint16_t slp_typb; } ev =
        { state, slp_typa, slp_typb };
    static const char *sleep_tags[] = {
        "acpi:sleep:s1", "acpi:sleep:s2", "acpi:sleep:s3", "acpi:sleep:s4"
    };
    if (state >= 1 && state <= 4) {
        TouchPublish(sleep_tags[state - 1], &ev, sizeof(ev));
    }

    uint32_t pm1a = g_acpi.fadt->pm1a_control_block;
    uint32_t pm1b = g_acpi.fadt->pm1b_control_block;
    uint16_t va = (uint16_t)((slp_typa << 10) | (1u << 13));
    uint16_t vb = (uint16_t)((slp_typb << 10) | (1u << 13));
    if (pm1a) outw((uint16_t)pm1a, va);
    if (pm1b) outw((uint16_t)pm1b, vb);

    debug_printf("[ACPI] resumed from S%u\n", state);
    return 0;
}

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
            acpi_publish_irq(&g_acpi_tag_gpe, idx);
            f = (uint8_t)(f & ~(1u << bit));
        }
    }
}

#define GAS_AS_SYSTEM_IO 1
#define PM1_EN_PWRBTN    (1u << 8)

typedef struct {
    uint16_t status;
    uint16_t enable;
} AcpiPm1Event;

static AcpiPm1Event g_pm1a_event;
static AcpiPm1Event g_pm1b_event;


static bool fadt_has_field(uint32_t len, size_t field_end) {
    return len >= field_end;
}

static uint16_t pm1_event_port(const acpi_gas_t *x, uint32_t legacy,
                               uint32_t fadt_len, size_t x_field_end,
                               const char *which)
{
    if (fadt_has_field(fadt_len, x_field_end) && x->address != 0) {
        if (x->address_space == GAS_AS_SYSTEM_IO) {
            return (uint16_t)x->address;
        }
        kprintf("[ACPI] %s event block lives in address space %u, not I/O — "
                "falling back to the legacy port\n", which, x->address_space);
    }
    return (uint16_t)legacy;
}

static void acpi_pm1_events_locate(void)
{
    if (!g_acpi.fadt) return;

    uint32_t flen = g_acpi.fadt->header.length;
    uint8_t  half = (uint8_t)(g_acpi.fadt->pm1_event_length / 2);

    if (half < 2) {
        kprintf("[ACPI] PM1 event block is %u byte(s) — too small to hold a "
                "status and an enable register; the power button cannot be "
                "heard\n", g_acpi.fadt->pm1_event_length);
        return;
    }

    uint16_t a = pm1_event_port(&g_acpi.fadt->x_pm1a_event_block,
                                g_acpi.fadt->pm1a_event_block, flen,
                                offsetof(acpi_fadt_t, x_pm1a_event_block) +
                                    sizeof(acpi_gas_t), "PM1a");
    uint16_t b = pm1_event_port(&g_acpi.fadt->x_pm1b_event_block,
                                g_acpi.fadt->pm1b_event_block, flen,
                                offsetof(acpi_fadt_t, x_pm1b_event_block) +
                                    sizeof(acpi_gas_t), "PM1b");

    if (a) {
        g_pm1a_event.status = a;
        g_pm1a_event.enable = (uint16_t)(a + half);
    }
    if (b) {
        g_pm1b_event.status = b;
        g_pm1b_event.enable = (uint16_t)(b + half);
    }
}

static void acpi_pm1_arm_power_button(void)
{
    bool armed = false;

    AcpiPm1Event *pair[2] = { &g_pm1a_event, &g_pm1b_event };
    const char   *name[2] = { "PM1a", "PM1b" };

    for (int i = 0; i < 2; i++) {
        if (pair[i]->status == 0) continue;

        outw(pair[i]->status, PM1_EN_PWRBTN);

        uint16_t en = inw(pair[i]->enable);
        outw(pair[i]->enable, (uint16_t)(en | PM1_EN_PWRBTN));

        uint16_t back = inw(pair[i]->enable);
        kprintf("[ACPI] power button armed on %s (status 0x%x enable 0x%x -> "
                "0x%x)\n", name[i], pair[i]->status, pair[i]->enable, back);
        armed = (back & PM1_EN_PWRBTN) != 0 || armed;
    }

    if (!armed) {
        kprintf("[ACPI] the power button could not be armed — pressing it will "
                "raise nothing\n");
    }
}

static void acpi_power_button_answer(void)
{
    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve("acpi:power-button", &full, &bare);

    bool claimed = (full != TOUCH_TAG_INVALID && TouchHasAnyListenersForTag(full)) ||
                   (bare != TOUCH_TAG_INVALID && TouchHasAnyListenersForTag(bare));

    if (claimed) {
        kprintf("[ACPI] power button pressed — a subscriber has it\n");
        return;
    }

    kprintf("[ACPI] power button pressed and nobody is listening — shutting "
            "down\n");
    system_halt(false);
}

static void acpi_power_button_answer_knocked(void *ctx);
static Knock g_acpi_power_button_knock = {
    .baton  = { .next = NULL, .run = acpi_power_button_answer_knocked, .ctx = NULL },
    .raised = 0,
};

static void acpi_power_button_answer_knocked(void *ctx)
{
    (void)ctx;
    KnockOpen(&g_acpi_power_button_knock);
    acpi_power_button_answer();
}

#define PM1_STS_PWRBTN  (1u << 8)
#define PM1_STS_SLPBTN  (1u << 9)
#define PM1_STS_RTC     (1u << 10)
#define PM1_STS_WAK     (1u << 15)

static void acpi_sci_handler(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) {
        irqchip_send_eoi(g_acpi.fadt ? g_acpi.fadt->sci_interrupt : 9);
        return;
    }
    uint16_t sts_a = g_pm1a_event.status ? inw(g_pm1a_event.status) : 0;
    uint16_t sts_b = g_pm1b_event.status ? inw(g_pm1b_event.status) : 0;

    if (g_pm1a_event.status == 0 && g_pm1b_event.status == 0) goto eoi;

    if (sts_a == 0xFFFF && sts_b == 0xFFFF) {
        kprintf("[ACPI] both PM1 status ports read 0xFFFF — that is a port "
                "answering nothing, not every event at once; ignoring\n");
        goto eoi;
    }

    uint16_t en_a = g_pm1a_event.enable ? inw(g_pm1a_event.enable) : 0;
    uint16_t en_b = g_pm1b_event.enable ? inw(g_pm1b_event.enable) : 0;
    if (en_a == 0xFFFF) en_a = 0;
    if (en_b == 0xFFFF) en_b = 0;

    uint16_t sts = (uint16_t)((sts_a & en_a) | (sts_b & en_b));

    if (sts & PM1_STS_PWRBTN) {
        kprintf("[ACPI] PM1 says the power button was pressed "
                "(PM1a sts 0x%04x en 0x%04x, PM1b sts 0x%04x en 0x%04x)\n",
                sts_a, en_a, sts_b, en_b);
        acpi_publish_irq(&g_acpi_tag_power_button, sts);
        KnockOn(&g_acpi_power_button_knock);
    }
    if (sts & PM1_STS_SLPBTN) {
        debug_printf("[ACPI] sleep button event\n");
        acpi_publish_irq(&g_acpi_tag_sleep_button, sts);
    }
    if (sts & PM1_STS_RTC) {
        debug_printf("[ACPI] RTC alarm event\n");
        acpi_publish_irq(&g_acpi_tag_rtc_alarm, sts);
    }
    if (sts & PM1_STS_WAK) {
        debug_printf("[ACPI] wake event\n");
        acpi_publish_irq(&g_acpi_tag_wake, sts);
    }

    if (sts_a) outw(g_pm1a_event.status, sts_a);
    if (sts_b) outw(g_pm1b_event.status, sts_b);

    {
        extern uint32_t apei_ghes_sci_check(void);
        (void)apei_ghes_sci_check();
    }

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
    if (!ioapic_gsi_flags(sci, NULL)) {
        ioapic_describe_gsi(sci, 0x000F);
        kprintf("[ACPI] firmware described no override for the SCI — GSI %u "
                "programmed level-triggered and active low, as the "
                "specification requires\n", sci);
    }

    irq_register_handler((uint8_t)sci, acpi_sci_handler);
    kprintf("[ACPI] SCI handler on GSI %u — the line opens when its names "
            "exist (acpi_sci_arm)\n", sci);
}

void acpi_sci_arm(void)
{
    if (!g_acpi.initialized || !g_acpi.fadt) return;
    uint16_t sci = g_acpi.fadt->sci_interrupt;
    if (sci == 0 || sci >= IRQ_MAX_COUNT) return;

    acpi_tag_resolve(&g_acpi_tag_power_button, "acpi:power-button");
    acpi_tag_resolve(&g_acpi_tag_sleep_button, "acpi:sleep-button");
    acpi_tag_resolve(&g_acpi_tag_rtc_alarm,    "acpi:rtc-alarm");
    acpi_tag_resolve(&g_acpi_tag_wake,         "acpi:wake");
    acpi_tag_resolve(&g_acpi_tag_gpe,          "acpi:gpe");

    irqchip_enable_irq((uint8_t)sci);
    kprintf("[ACPI] SCI on GSI %u\n", sci);

    acpi_pm1_events_locate();
    acpi_pm1_arm_power_button();
}

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

    for (uint32_t i = 0; i < 3000; i++) {
        if (inw((uint16_t)pm1a) & 0x1) {
            debug_printf("[ACPI] SCI_EN raised after %u ms\n", i);
            return;
        }
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

    acpi_enable_mode();

    debug_printf("[ACPI] Initialization complete (HPET=%s MCFG=%s S5=%s)\n",
                 g_acpi.hpet.present ? "yes" : "no",
                 g_acpi.mcfg.present ? "yes" : "no",
                 g_acpi.s5_found     ? "yes" : "fallback");

    uint32_t caps =
        (g_acpi.hpet.present ? (1u <<  0) : 0) |
        (g_acpi.mcfg.present ? (1u <<  1) : 0) |
        (g_acpi.s5_found     ? (1u <<  2) : 0) |
        (g_acpi.numa.present ? (1u <<  3) : 0) |
        (g_acpi.dmar.present ? (1u <<  4) : 0) |
        (g_acpi.ivrs.present ? (1u <<  5) : 0) |
        (g_acpi.apei.hest_present ? (1u <<  6) : 0) |
        (g_acpi.apei.bert_present ? (1u <<  7) : 0) |
        (g_acpi.apei.erst_present ? (1u <<  8) : 0);
    TouchPublish("acpi:ready", &caps, sizeof(caps));

#if CONFIG_ACPI_DEBUG
    acpi_print_info();
#endif

    return ACPI_OK;
}