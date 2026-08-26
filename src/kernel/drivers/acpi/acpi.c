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
#include "irq_defer.h"
#include "atomics.h"
#include "kernel_config.h"

acpi_state_t g_acpi = {0};

/* ============================================================
 * Deferred Touch publish ring — copies (tag, payload) out of the
 * SCI IRQ context into a static, pre-allocated slot, then irq_defer's
 * a handler that calls TouchPublish from K-Core context (where
 * kmalloc + process_snapshot_pids + Touch claim-table lookup is
 * safe). Eliminates the kmalloc-from-IRQ deadlock path the audit
 * found at acpi.c:177-189 + acpi.c:133.
 *
 * The ring is a simple MPSC bump-allocator (claim via fetch_add &
 * mask). It is intentionally a CIRCULAR overwrite: under extreme
 * burst, the OLDEST unprocessed event is silently lost. That is
 * the correct semantics for ACPI events — losing one
 * power-button-blink in an event flood is preferable to blocking
 * the IRQ or panicking. The capacity comes from kernel_config.h,
 * not from a magic constant in code.
 * ============================================================ */
#ifndef CONFIG_ACPI_TOUCH_EVENT_RING_SIZE
#define CONFIG_ACPI_TOUCH_EVENT_RING_SIZE 64U  /* power-of-2 */
#endif

typedef struct {
    char     tag[40];     /* "acpi:gpe:NNN" longest */
    uint16_t payload;     /* sts or gpe idx */
} AcpiTouchEvent;

_Static_assert((CONFIG_ACPI_TOUCH_EVENT_RING_SIZE &
                (CONFIG_ACPI_TOUCH_EVENT_RING_SIZE - 1)) == 0,
               "CONFIG_ACPI_TOUCH_EVENT_RING_SIZE must be power-of-2");

static AcpiTouchEvent g_acpi_touch_ring[CONFIG_ACPI_TOUCH_EVENT_RING_SIZE];
static volatile uint32_t g_acpi_touch_idx = 0;
static volatile uint64_t g_acpi_touch_dropped = 0;

/* K-Core context — safe to call TouchPublish (kmalloc + process walk).
 * The event slot remains valid for the lifetime of the kernel (static
 * storage); rewriting by a fresh IRQ before we run is harmless because
 * we read tag/payload immediately and TouchPublish copies them. */
static void acpi_touch_deferred(void *ctx)
{
    AcpiTouchEvent *ev = (AcpiTouchEvent *)ctx;
    TouchPublish(ev->tag, &ev->payload, sizeof(ev->payload));
}

/* IRQ-side: copy tag + payload into the next ring slot and defer the
 * publish to K-Core. tag must be ≤ 39 chars; longer tags are
 * truncated (BoxOS internal callers stay well under). */
static void acpi_queue_touch_irq(const char *tag, uint16_t payload)
{
    uint32_t i = atomic_fetch_add_u32(&g_acpi_touch_idx, 1) &
                 (CONFIG_ACPI_TOUCH_EVENT_RING_SIZE - 1);
    AcpiTouchEvent *ev = &g_acpi_touch_ring[i];
    size_t tlen = 0;
    while (tag[tlen] && tlen < sizeof(ev->tag) - 1) {
        ev->tag[tlen] = tag[tlen];
        tlen++;
    }
    ev->tag[tlen] = '\0';
    ev->payload = payload;
    /* Slot writes complete before the deferred handler observes ctx —
     * irq_defer's release-store on slot.ready provides the fence. */
    irq_defer(acpi_touch_deferred, ev);
}

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

    /* Broadcast to subscribers before we tip the platform into the
     * sleep state. Daemons can flush, persist, drop hardware claims.
     * The hardware-halt path in system_halt.c already drained the
     * deferred work for S5; for S1/S2/S3 this is the canonical event
     * that says "going down". */
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

    /* On S1, control returns here on wake. Other states either reset
     * the CPU (S2-S3) or never return (S4-S5). */
    debug_printf("[ACPI] resumed from S%u\n", state);
    return 0;
}

/* Dispatch every fired bit in `block_base..block_base+half_len`. Two
 * fan-outs in priority order:
 *   1. The C-callback registry (`acpi_gpe_register`) — fast in-kernel
 *      handlers (EC, thermal-zone). Runs first because subscribers there
 *      need the lowest latency.
 *   2. A Touch publish on a per-GPE tag (`acpi:gpe:NN`) — userspace
 *      daemons can subscribe to GPE bits without writing kernel code.
 * Touch publish is bounded MPSC push (zero blocking), so it doesn't
 * slow the IRQ path beyond the registry call.
 */
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
            /* Tag form: acpi:gpe:<decimal>. Build inline — itoa-free. */
            char tag[20] = "acpi:gpe:";
            uint16_t v = idx; int pos = 9;
            char tmp[6]; int tl = 0;
            if (v == 0) tmp[tl++] = '0';
            while (v) { tmp[tl++] = (char)('0' + v % 10); v /= 10; }
            while (tl-- > 0) tag[pos++] = tmp[tl];
            tag[pos] = 0;
            /* Defer the Touch publish to K-Core — TouchPublish takes
             * kmalloc and walks the process list, both forbidden in
             * IRQ context. The per-GPE tag and the bit index are
             * captured into a static ring slot before deferring. */
            acpi_queue_touch_irq(tag, idx);
            f = (uint8_t)(f & ~(1u << bit));
        }
    }
}

/* ============================================================
 * The power button
 * ============================================================
 *
 * The PM1 event block is one block with two halves of PM1_EVT_LEN/2 bytes
 * each: status first, enable second. The chipset raises a status bit whether
 * or not anybody asked for it, and raises an SCI only for the bits named in
 * the enable half.
 *
 * Nothing here had ever written that half. So on a real machine the button set
 * PWRBTN_STS, no interrupt was raised, and the kernel never heard about the
 * press at all — the only thing that turned the machine off was holding the
 * button down for four seconds, which is the chipset doing it without asking
 * anybody, and is exactly what the owner of this board had to do.
 */
#define GAS_AS_SYSTEM_IO 1
#define PM1_EN_PWRBTN    (1u << 8)

typedef struct {
    uint16_t status;        /* 0 when this half of the pair does not exist */
    uint16_t enable;
} AcpiPm1Event;

static AcpiPm1Event g_pm1a_event;
static AcpiPm1Event g_pm1b_event;


static bool fadt_has_field(uint32_t len, size_t field_end) {
    return len >= field_end;
}

/* An I/O port for a PM1 event block, preferring what the extended field says.
 * Every x86 firmware puts these in I/O space; one that does not is one this
 * kernel would silently read the wrong bytes from, so it says so instead. */
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

    /* Two registers of at least sixteen bits each — the specification's own
     * minimum. A block too small to hold them describes nothing usable. */
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

/* Switch the button on, in both halves of the pair.
 *
 * PM1b is not a spare copy of PM1a: a chipset that has one reports part of its
 * event state there and nowhere else, and a driver that reads only PM1a leaves
 * a status bit standing that nothing will ever clear — which on a level-
 * triggered line means the interrupt never stops being asserted.
 *
 * Only the power button is enabled. The sleep button would raise events this
 * kernel has nothing to do with, and by the rule below an event nobody claims
 * is one the kernel acts on — arming a button whose meaning is "sleep" and
 * answering it with "power off" is worse than leaving it silent.
 */
static void acpi_pm1_arm_power_button(void)
{
    bool armed = false;

    AcpiPm1Event *pair[2] = { &g_pm1a_event, &g_pm1b_event };
    const char   *name[2] = { "PM1a", "PM1b" };

    for (int i = 0; i < 2; i++) {
        if (pair[i]->status == 0) continue;

        /* A press from before this kernel existed is not a press for it to
         * answer. Clear it before switching the line on. */
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

/*
 * A press nobody has claimed is a press the kernel answers.
 *
 * The owner of a machine expects the button to turn it off. A program that
 * wants to be asked first says so by subscribing, and then the decision is
 * its own — save the file, refuse,
 * ask the person in front of it. Nobody subscribed means
 * nobody is going to answer, and waiting for an answer that is not coming is
 * how a button ends up doing nothing at all.
 *
 * There is no timer here and nothing to wait for: whether anyone is listening
 * is a question with an answer at the instant of the press.
 */
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

/* Carried out of the interrupt handler by the same mechanism that carries the
 * Touch publish beside it — the one place in this kernel that turns "noticed
 * in an interrupt" into "done where waiting is allowed". On a machine with
 * K-Cores that is their guide loop, which is the context the shutdown sequence
 * documents that it needs; on a machine that is one core it is the timer's own
 * deferred pump, gated on having interrupted user mode so no kernel lock is
 * held. There is no second arrangement to keep in step with the first. */
static void acpi_power_button_deferred(void *ctx)
{
    (void)ctx;
    acpi_power_button_answer();
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
    /* Both halves of the pair. PM1b is not a spare copy of PM1a: a chipset
     * that has one reports part of its event state there and nowhere else,
     * and a status bit nobody clears holds a level-triggered line asserted
     * for good. Only PM1a was ever read. */
    uint16_t sts_a = g_pm1a_event.status ? inw(g_pm1a_event.status) : 0;
    uint16_t sts_b = g_pm1b_event.status ? inw(g_pm1b_event.status) : 0;

    if (g_pm1a_event.status == 0 && g_pm1b_event.status == 0) goto eoi;

    /*
     * ‼ AN EVENT IS A STATUS BIT **AND** ITS ENABLE BIT (ACPI 6.5 §4.8.3.1.1)
     *
     * The hardware raises status bits whether or not the matching event is
     * enabled — PWRBTN_STS, RTC_STS and WAK_STS all latch on their own — so a
     * status word on its own says what the chipset has SEEN, not what it is
     * asking this kernel to act on. The GPE walk further down has always got
     * this right (`fired = s & e`); this half took the raw word.
     *
     * What that cost, on a real board: the machine ran fine for as long as no
     * ACPI event happened at all, and the FIRST SCI of the session — raised by
     * the chipset when a flash drive was pulled out of a USB port — made this
     * handler read PM1, find PWRBTN_STS standing, conclude the power button
     * had been pressed with nobody listening, and switch the machine off.
     * Instantly, mid-session, with no warning. Nobody had touched the button.
     *
     * A port that is not there answers `inw` with 0xFFFF, which is every bit
     * set — including the power button's. That is not an event either, and it
     * is now told apart from one instead of being obeyed.
     */
    if (sts_a == 0xFFFF && sts_b == 0xFFFF) {
        kprintf("[ACPI] both PM1 status ports read 0xFFFF — that is a port "
                "answering nothing, not every event at once; ignoring\n");
        goto eoi;
    }

    uint16_t en_a = g_pm1a_event.enable ? inw(g_pm1a_event.enable) : 0;
    uint16_t en_b = g_pm1b_event.enable ? inw(g_pm1b_event.enable) : 0;
    if (en_a == 0xFFFF) en_a = 0;
    if (en_b == 0xFFFF) en_b = 0;

    /* Decisions are made on what is enabled; the CLEARING below still writes
     * the raw word back, because a latched bit nobody acknowledges holds a
     * level-triggered SCI asserted for good whether it was enabled or not. */
    uint16_t sts = (uint16_t)((sts_a & en_a) | (sts_b & en_b));

    /* Every PM1 event class fires both a debug line (keeps the boot log
     * useful) and a Touch publish (lets every userspace listener that
     * subscribed to the matching tag wake up and react — power-manager,
     * lockscreen, RTC alarm daemon, etc.). The wire payload is the raw
     * PM1 status word so subscribers can decode flags they care about. */
    /* All TouchPublish calls below moved off the IRQ path via
     * acpi_queue_touch_irq → irq_defer. The handler runs in K-Core
     * context where kmalloc + process_snapshot_pids + Touch claim
     * table lookup are safe. */
    if (sts & PM1_STS_PWRBTN) {
        /* Said out loud, not with debug_printf: the next thing this does is
         * turn the machine off, and the one line explaining why must survive
         * into a shipped build. */
        kprintf("[ACPI] PM1 says the power button was pressed "
                "(PM1a sts 0x%04x en 0x%04x, PM1b sts 0x%04x en 0x%04x)\n",
                sts_a, en_a, sts_b, en_b);
        acpi_queue_touch_irq("acpi:power-button", sts);
        /* Answered where waiting is allowed: deciding what a press means ends
         * either in a Touch delivery or in the whole shutdown sequence, and
         * neither of those belongs in an interrupt handler. */
        irq_defer(acpi_power_button_deferred, NULL);
    }
    if (sts & PM1_STS_SLPBTN) {
        debug_printf("[ACPI] sleep button event\n");
        acpi_queue_touch_irq("acpi:sleep-button", sts);
    }
    if (sts & PM1_STS_RTC) {
        debug_printf("[ACPI] RTC alarm event\n");
        acpi_queue_touch_irq("acpi:rtc-alarm", sts);
    }
    if (sts & PM1_STS_WAK) {
        debug_printf("[ACPI] wake event\n");
        acpi_queue_touch_irq("acpi:wake", sts);
    }

    /* W1C: write each half's own bits back to clear them. Writing PM1a's
     * value into PM1b would acknowledge events that never happened there and
     * leave the ones that did. This is what drops the SCI line so the IOAPIC
     * can re-arm. */
    if (sts_a) outw(g_pm1a_event.status, sts_a);
    if (sts_b) outw(g_pm1b_event.status, sts_b);

    /* APEI/GHES SCI-notify path. Walks every HEST GHES source whose
     * notify type == 3 (SCI), reads its Generic Error Status Block,
     * routes Memory Error sections into mce_migrate_request and
     * publishes Touch events for every other section. Production
     * server firmware uses this delivery mode for SMI-correlated
     * MCE events. */
    {
        extern uint32_t apei_ghes_sci_check(void);
        (void)apei_ghes_sci_check();
    }

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
    /*
     * ACPI 6.5 §5.2.15: the SCI is a level-sensitive, shareable, active-low
     * interrupt — and that is not the ISA bus default the IOAPIC applies to a
     * line nobody described. Firmware usually supplies an Interrupt Source
     * Override saying so, and is not required to.
     *
     * Without one the line was programmed edge-triggered and active high,
     * which on a level, active-low line reads as asserted the moment it is
     * unmasked and never asserts again after the first event is cleared. QEMU
     * forgives it. A chipset does not.
     */
    if (!ioapic_gsi_flags(sci, NULL)) {
        /* polarity 11 = active low, trigger 11 = level */
        ioapic_describe_gsi(sci, 0x000F);
        kprintf("[ACPI] firmware described no override for the SCI — GSI %u "
                "programmed level-triggered and active low, as the "
                "specification requires\n", sci);
    }

    irq_register_handler((uint8_t)sci, acpi_sci_handler);
    irqchip_enable_irq((uint8_t)sci);
    kprintf("[ACPI] SCI on GSI %u\n", sci);

    acpi_pm1_events_locate();
    acpi_pm1_arm_power_button();
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

    /* Broadcast capability bitmap so userspace daemons can decide what
     * features to bring up (battery service only matters if S3 works,
     * IOMMU service only if DMAR/IVRS exposed something, etc.).
     * Bits are stable across boots: this is the wire format that
     * userspace subscribers see. */
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
