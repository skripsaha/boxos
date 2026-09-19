#include "acpi_internal.h"
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
#include "hourglass.h"
#include "vmm.h"
#include "aml.h"       /* AmlRoutingAdopt — \_PIC is owed to the platform
                        * before this kernel asks it for its first interrupt */

acpi_state_t g_acpi = {0};

/* ============================================================
 * Events noticed in the SCI handler and said from a K-Core.
 *
 * The handler runs in IRQ context, where TouchPublish is forbidden (it
 * takes registry and bucket locks and may kmalloc). Each event therefore
 * goes the way every interrupt-side event goes: into the Touch IRQ ring
 * under a name resolved in advance (TouchPublishIrqPair, touch.c), and
 * the K-Core that reads the ring publishes it. The names are resolved by
 * acpi_sci_arm, which is also what unmasks the line — the door opens only
 * after the names exist, so no event arrives nameless. Until then a
 * level-triggered SCI waits, masked, in the IOAPIC.
 *
 * GPE events all go on one name, "acpi:gpe", with the GPE index as the
 * payload word: a name per GPE bit would mean interning two hundred and
 * fifty-six names for events most machines never raise, and an interrupt
 * cannot intern the one it needs when it needs it.
 * ============================================================ */
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

/* IRQ-side: the event's word (a PM1 status word, a GPE index) goes into the
 * Touch IRQ ring under the name resolved for it. */
static void acpi_publish_irq(const AcpiTagPair *t, uint16_t word)
{
    TouchTag full = __atomic_load_n(&t->full, __ATOMIC_ACQUIRE);
    TouchTag bare = __atomic_load_n(&t->bare, __ATOMIC_ACQUIRE);
    TouchPublishIrqPair(full, bare, &word, sizeof(word), 0, TOUCH_FLAG_KERNEL);
}

/* ============================================================
 * GPE registry — one C callback per GPE bit. Lookup is O(1).
 * ============================================================ */
static acpi_gpe_handler_t g_gpe_handlers[ACPI_MAX_GPES];

/* ============================================================
 * Firmware-described register blocks: the extended descriptor wins.
 *
 * ACPI 6.5 §5.2.9 says of every X_ field of the FADT that "if this field
 * contains a nonzero value which can be used by OSPM, then OSPM must use this
 * information instead of the [32-bit] field". A board that publishes a block
 * only through its GAS — legacy field zero, which ACPI 2.0+ firmware is
 * entitled to do — is fully described by the extended one, and a kernel that
 * reads only the legacy pair concludes the block does not exist at all.
 *
 * The length check is not decoration: an ACPI 1.0b FADT is 116 bytes and its
 * x_ fields lie past the end of the table, where the struct reads whatever
 * memory follows it.
 *
 * The blocks fadt_block_port resolves — the GPE pair and the PM1 event pair
 * — are reached with inb/outb, so a GAS naming any space but System I/O is
 * said out loud and the legacy address used instead of quietly addressing
 * the wrong thing with the wrong instruction. That policy is this function's
 * alone: the PM1 CONTROL registers, resolved further down by
 * pm1_control_locate, are read and written in whichever of System I/O and
 * System Memory §4.8.3.2.1 allows them, and only a third space sends that
 * resolver back to the legacy address.
 * ============================================================ */
#define GAS_AS_SYSTEM_MEMORY 0
#define GAS_AS_SYSTEM_IO     1

static bool fadt_has_field(uint32_t len, size_t field_end) {
    return len >= field_end;
}

static uint16_t fadt_block_port(const acpi_gas_t *x, uint32_t legacy,
                                uint32_t fadt_len, size_t x_field_end,
                                const char *which)
{
    if (fadt_has_field(fadt_len, x_field_end) && x->address != 0) {
        if (x->address_space == GAS_AS_SYSTEM_IO) {
            return (uint16_t)x->address;
        }
        kprintf("[ACPI] %s lives in address space %u, not I/O — falling back "
                "to the legacy address\n", which, x->address_space);
    }
    return (uint16_t)legacy;
}

/* ============================================================
 * The GPE blocks themselves.
 *
 * ACPI 6.5 §4.8.4.1: a GPE block is GPEx_BLK_LEN bytes wide, the first half
 * status and the second half enable, one bit per event.
 *
 * Two halves of one omission lived here. §16.3.1 says "the SCI interrupt can
 * only be signaled after OSPM has enabled one of the GPE/PM1 enable bits",
 * and nothing in this kernel had ever written a GPE enable bit — the walk in
 * the SCI handler READ the enable half and masked status against it, so
 * `s & e` was identically zero forever and no lid, thermal, EC or hotplug
 * event could ever be delivered.
 *
 * The other half is what happens when firmware hands one over already armed,
 * which §4.8.4.1 permits a design to do across a reset and which a warm
 * reboot out of another OS does routinely. acpi_enable_mode() raises SCI_EN
 * and every GPE the firmware had enabled for its own SMI use becomes an SCI
 * source with nothing in this kernel able to service it: the handler used to
 * acknowledge the status bit and leave the source enabled, so a level GPE
 * re-latched at once, the SCI never de-asserted, and the IOAPIC re-delivered
 * the instant EOI returned — the BSP pinned by a storm it could only ack.
 *
 * So: every block is disabled and cleared before the line opens (ACPICA's
 * AcpiDisableAllGpes, for the same reason), a GPE is armed only when a
 * handler is registered for it, and a GPE that fires with nobody to serve it
 * is switched off and stays off — "if there is no handler or method to run,
 * just disable the GPE and leave it disabled permanently", AcpiEvGpeDispatch.
 * ============================================================ */
#define ACPI_GPE_MAX_BYTES  (ACPI_MAX_GPES / 8)

typedef struct {
    uint16_t status;   /* I/O port of the status half; 0 when there is none */
    uint16_t enable;   /* I/O port of the enable half */
    uint8_t  bytes;    /* width of each half, in bytes */
    uint16_t base;     /* GPE index carried by bit 0 of byte 0 */
} AcpiGpeBlock;

static AcpiGpeBlock g_gpe0;
static AcpiGpeBlock g_gpe1;

/* The one lock over a GPE enable register.
 *
 * Three places move an enable bit: a driver registering a handler, a driver
 * dropping one, and the SCI handler quieting the byte it is about to serve.
 * Each is a read-modify-write of the same I/O byte; two run on whatever core
 * their caller sits on and the third runs in an interrupt. An SCI landing
 * between a registration's read and its write loses §5.6.4's first step
 * outright — the handler's "disable the fired bits" is overwritten by the
 * stale word, and a level GPE with nobody to serve it is left enabled with
 * its source still asserting, which is the unbounded storm this whole block
 * exists to prevent. The cross-core form is the same on any AP registering
 * while the BSP serves.
 *
 * spin_lock holds interrupts off for the whole hold (klib.h), so the SCI
 * cannot land on the core holding this, and the hold is a pair of port
 * accesses with nothing else inside it: no handler call, no kprintf, no Touch
 * publish. All of those happen outside. BSS-zero is an unlocked lock. */
static spinlock_t g_gpe_lock;

static void gpe_enable_bit(const AcpiGpeBlock *blk, uint8_t byte, uint8_t mask,
                           bool on)
{
    spin_lock(&g_gpe_lock);
    uint8_t en = inb((uint16_t)(blk->enable + byte));
    outb((uint16_t)(blk->enable + byte),
         on ? (uint8_t)(en | mask) : (uint8_t)(en & ~mask));
    spin_unlock(&g_gpe_lock);
}

/* One byte's fired GPEs, taken off the line as they are read: §5.6.4's first
 * step ("disables the interrupt source") happens under the same hold that
 * read the enable half, so nothing can put those bits back between the
 * decision and the write. */
static uint8_t gpe_byte_take_fired(const AcpiGpeBlock *blk, uint8_t byte)
{
    spin_lock(&g_gpe_lock);
    uint8_t s = inb((uint16_t)(blk->status + byte));
    uint8_t e = inb((uint16_t)(blk->enable + byte));
    uint8_t fired = (uint8_t)(s & e);
    if (fired) outb((uint16_t)(blk->enable + byte), (uint8_t)(e & ~fired));
    spin_unlock(&g_gpe_lock);
    return fired;
}

/* A GPE switched off for good is said once, not on every re-arm the firmware
 * performs behind the kernel's back. */
static uint8_t g_gpe_quieted[ACPI_GPE_MAX_BYTES];

static bool gpe_quiet_mark(uint16_t gpe)
{
    uint8_t prev = __atomic_fetch_or(&g_gpe_quieted[gpe / 8],
                                     (uint8_t)(1u << (gpe % 8)),
                                     __ATOMIC_RELAXED);
    return (prev & (uint8_t)(1u << (gpe % 8))) == 0;
}

static void gpe_quiet_unmark(uint16_t gpe)
{
    __atomic_fetch_and(&g_gpe_quieted[gpe / 8],
                       (uint8_t)~(1u << (gpe % 8)), __ATOMIC_RELAXED);
}

/* Which block owns `gpe`, and where in it. NULL when this firmware describes
 * no block that carries the index. */
static AcpiGpeBlock *gpe_block_of(uint16_t gpe, uint8_t *byte, uint8_t *bit)
{
    AcpiGpeBlock *blocks[2] = { &g_gpe0, &g_gpe1 };

    for (int i = 0; i < 2; i++) {
        AcpiGpeBlock *blk = blocks[i];
        if (blk->status == 0 || gpe < blk->base) continue;
        uint32_t off = (uint32_t)gpe - blk->base;
        if (off >= (uint32_t)blk->bytes * 8) continue;
        *byte = (uint8_t)(off / 8);
        *bit  = (uint8_t)(off % 8);
        return blk;
    }
    return NULL;
}

static void gpe_block_locate(AcpiGpeBlock *blk, const acpi_gas_t *x,
                             uint32_t legacy, uint8_t len, uint16_t base,
                             uint32_t fadt_len, size_t x_field_end,
                             const char *which)
{
    blk->status = 0;
    blk->enable = 0;
    blk->bytes  = 0;
    blk->base   = base;

    /* X_GPEx_BLK before GPEx_BLK, §5.2.9, the same preference resolve_pm1_gas
     * gives the PM1 blocks. The WIDTH still comes from GPEx_BLK_LEN and not
     * from the GAS bit_width: a GPE block is two registers inside one span
     * and the spec measures that span in bytes, which is why ACPICA carries
     * the GPE blocks as the named exception in acpi_tb_convert_fadt. */
    uint16_t port = fadt_block_port(x, legacy, fadt_len, x_field_end, which);
    if (port == 0 || len < 2) return;

    /* Every index this block can produce has to land inside the handler
     * registry, which is what makes `base + b*8 + bit` safe to use as a
     * subscript further down. GPE1 starts at gpe1_base, so its width is
     * clamped against what is left of the registry rather than against the
     * registry's size. */
    uint8_t half  = (uint8_t)(len / 2);
    uint32_t room = (base >= ACPI_MAX_GPES) ? 0u
                                            : (ACPI_MAX_GPES - base) / 8u;
    if (room > ACPI_GPE_MAX_BYTES) room = ACPI_GPE_MAX_BYTES;

    if (room == 0) {
        kprintf("[ACPI] %s at 0x%x starts at GPE %u, past the last GPE this "
                "kernel keeps a handler slot for (%u); none of the block is "
                "followed\n", which, port, base,
                (unsigned)(ACPI_MAX_GPES - 1));
        return;
    }
    if (half > room) {
        kprintf("[ACPI] %s is %u bytes wide from GPE %u; this kernel follows "
                "the first %u byte(s) of it, which is GPE %u..%u\n",
                which, half, base, (unsigned)room,
                base, (unsigned)(base + room * 8 - 1));
        half = (uint8_t)room;
    }
    if (half == 0) return;

    blk->status = port;
    blk->enable = (uint16_t)(port + half);
    blk->bytes  = half;
}

/* The enable mask this kernel would choose for one byte: exactly the bits a
 * handler has been registered for. */
static uint8_t gpe_handler_mask(const AcpiGpeBlock *blk, uint8_t byte)
{
    uint8_t mask = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
        if (g_gpe_handlers[blk->base + byte * 8 + bit]) {
            mask = (uint8_t)(mask | (1u << bit));
        }
    }
    return mask;
}

/* Nothing the firmware armed for itself is inherited as an SCI source — and
 * nothing this kernel armed for itself is thrown out with it. A driver that
 * registered a GPE handler during its own init did so before the line ever
 * opened; its enable bit goes back inside the same hold that cleared the
 * byte, so the block never passes through a state where a registered handler
 * is disarmed and no arming is silently undone. */
static void gpe_block_scrub(const AcpiGpeBlock *blk, const char *which)
{
    if (blk->status == 0) return;

    bool kept = false;
    for (uint8_t b = 0; b < blk->bytes; b++) {
        uint8_t want = gpe_handler_mask(blk, b);
        spin_lock(&g_gpe_lock);
        outb((uint16_t)(blk->enable + b), 0x00);
        outb((uint16_t)(blk->status + b), 0xFF);
        if (want) outb((uint16_t)(blk->enable + b), want);
        spin_unlock(&g_gpe_lock);
        if (want) kept = true;
    }
    kprintf("[ACPI] %s at 0x%x: %u byte(s) disabled and cleared before the SCI "
            "opens (GPE %u..%u)%s\n", which, blk->status, blk->bytes,
            blk->base, (unsigned)(blk->base + blk->bytes * 8 - 1),
            kept ? "; the bits this kernel already holds handlers for are "
                   "re-armed" : "");
}

/* Where the blocks are — answered out of the FADT with no register touched,
 * which is why it runs in acpi_init rather than at arm time. A driver that
 * registers a GPE handler during its own init must find its block already
 * described; the alternative is telling it "GPE %u is in no block this
 * firmware describes" when the firmware describes it perfectly well and the
 * kernel had simply not looked yet. */
static void acpi_gpe_blocks_locate(void)
{
    if (!g_acpi.fadt) return;

    uint32_t flen = g_acpi.fadt->header.length;

    gpe_block_locate(&g_gpe0, &g_acpi.fadt->x_gpe0_block,
                     g_acpi.fadt->gpe0_block, g_acpi.fadt->gpe0_length, 0,
                     flen,
                     offsetof(acpi_fadt_t, x_gpe0_block) + sizeof(acpi_gas_t),
                     "GPE0 block");
    gpe_block_locate(&g_gpe1, &g_acpi.fadt->x_gpe1_block,
                     g_acpi.fadt->gpe1_block, g_acpi.fadt->gpe1_length,
                     g_acpi.fadt->gpe1_base, flen,
                     offsetof(acpi_fadt_t, x_gpe1_block) + sizeof(acpi_gas_t),
                     "GPE1 block");
}

static void acpi_gpe_blocks_scrub(void)
{
    gpe_block_scrub(&g_gpe0, "GPE0 block");
    gpe_block_scrub(&g_gpe1, "GPE1 block");
}

int acpi_gpe_register(uint16_t gpe, acpi_gpe_handler_t cb) {
    /* Said out loud like every other refusal here. A bare -1 leaves the
     * caller holding a GPE it believes is armed and no sentence anywhere
     * saying it is not. */
    if (!cb) {
        kprintf("[ACPI] GPE %u cannot be registered: no handler was given, "
                "and registering is arming — there would be nothing to run "
                "when the bit fires\n", gpe);
        return -1;
    }
    if (gpe >= ACPI_MAX_GPES) {
        kprintf("[ACPI] GPE %u is past the %u this registry holds — no "
                "handler installed and no enable bit set\n",
                gpe, (unsigned)ACPI_MAX_GPES);
        return -1;
    }

    /* Told apart so the refusal below is never a lie: before acpi_init the
     * blocks are not described because nothing has read the FADT yet, which
     * is a different fact from the firmware describing no such block. */
    if (!g_acpi.initialized) {
        kprintf("[ACPI] GPE %u cannot be registered before acpi_init — no "
                "FADT has been read, so no block is described yet\n", gpe);
        return -1;
    }

    uint8_t byte = 0, bit = 0;
    AcpiGpeBlock *blk = gpe_block_of(gpe, &byte, &bit);
    if (!blk) {
        kprintf("[ACPI] GPE %u is in no block this firmware describes — there "
                "is no enable bit to set for it\n", gpe);
        return -1;
    }

    g_gpe_handlers[gpe] = cb;

    /* A latch from before this handler existed is not an event for it to
     * answer, so the status bit is cleared before the source is armed. */
    outb((uint16_t)(blk->status + byte), (uint8_t)(1u << bit));
    gpe_enable_bit(blk, byte, (uint8_t)(1u << bit), true);
    gpe_quiet_unmark(gpe);
    return 0;
}

void acpi_gpe_unregister(uint16_t gpe) {
    if (gpe >= ACPI_MAX_GPES) return;

    uint8_t byte = 0, bit = 0;
    AcpiGpeBlock *blk = gpe_block_of(gpe, &byte, &bit);
    if (blk) gpe_enable_bit(blk, byte, (uint8_t)(1u << bit), false);
    g_gpe_handlers[gpe] = NULL;
}

/* PM1 control register fields — ACPI 6.5 §4.8.3.2.1, Table 4.13. */
#define PM1_CNT_SCI_EN         0x0001u   /* bit 0      */
#define PM1_CNT_SLP_TYP_SHIFT  10
#define PM1_CNT_SLP_TYP_MASK   0x1C00u   /* bits 12:10 */
#define PM1_CNT_SLP_EN         0x2000u   /* bit 13     */

/* ============================================================
 * Where the PM1 control registers ARE — resolved once, out of the FADT.
 *
 * PM1a_CNT is not a port by definition. ACPI 6.5 §4.8.3.2.1 gives the PM1
 * control registers "System I/O or Memory Space", and §5.2.9 says of the
 * 32-bit PM1a_CNT_BLK field that "if the X_PM1a_CNT_BLK field contains a non
 * zero value which can be used by the OSPM, then this field must be ignored
 * by the OSPM".
 *
 * Two places in this file used to take the legacy 32-bit field as the whole
 * answer and reach it with inw/outw regardless: the SCI_EN test of the
 * ACPI-mode handoff, and the sleep vector written on the way to soft-off. On
 * a FADT where the extended descriptor supersedes the legacy one — a
 * different address, or the register in System Memory — both addressed
 * something that is not PM1a_CNT. A port nothing decodes answers inw with 0xFFFF, and bit 0
 * of that word is SCI_EN's: the handoff read it as "already in ACPI mode",
 * returned without ever writing SMI_CMD, and left the platform in SMI mode
 * with the power button unarmed and S5 a no-op. Every step of that is
 * silent, because the only thing it said was a debug_printf, which a shipped
 * build compiles to nothing. The mirror case is just as quiet: a FADT with
 * the legacy field at zero and only X_PM1a_CNT_BLK filled in never reached
 * the read at all.
 *
 * This is the ONLY resolver for the pair. acpi_shutdown.c used to carry a
 * second one — resolve_pm1_gas() with gas_read_word/gas_write_word — and that
 * one, not this one, was what the live S5 path used. The two preferred the X_
 * descriptor alike but were not the same policy: resolve_pm1_gas() synthesised
 * the legacy I/O GAS only when the X_ descriptor was absent or zero, and handed
 * an X_ descriptor in a space it cannot reach straight to gas_read_word, which
 * refused it; the resolver here treats an X_ description OSPM cannot use as the
 * §5.2.9 condition it is and goes back to the legacy address. On a board that
 * ships such a descriptor the handoff and the shutdown disagreed about whether
 * the register exists at all — one of them wrote it, the other did not. Both
 * paths now come through acpi_pm1_request_sleep() below.
 *
 * Address spaces other than System I/O and System Memory are refused here and
 * were accepted there (PCI config, via gas_write_word). That refusal is the
 * reference behaviour, not a narrowing: ACPICA's AcpiHwValidateRegister rejects
 * every register whose SpaceId is neither ACPI_ADR_SPACE_SYSTEM_MEMORY nor
 * ACPI_ADR_SPACE_SYSTEM_IO with AE_SUPPORT, and §4.8.3.2.1 gives PM1 control
 * exactly those two.
 * ============================================================ */
typedef struct {
    bool               described; /* the FADT names this block at all */
    bool               present;   /* and this kernel can reach it */
    uint8_t            space;     /* GAS_AS_SYSTEM_IO or GAS_AS_SYSTEM_MEMORY */
    uint16_t           port;      /* System I/O */
    volatile uint16_t *reg;       /* System Memory, mapped once */
} AcpiPm1Control;

static AcpiPm1Control g_pm1a_control;
static AcpiPm1Control g_pm1b_control;

/* Read one PM1 control register in its own address space.
 *
 * A word of all ones is not a register's answer. PM1_CNT has reserved bits
 * (Table 4.13), and ACPI 6.5 §4.6.1 is explicit about those: "ACPI hardware
 * registers are designed such that reserved bits always return zero, and
 * data writes to them have no side affects." So 0xFFFF is a decode that
 * never happened — the same reading the SCI handler gives an all-ones PM1
 * status word. False is returned and *out is left untouched. */
static bool pm1_control_read(const AcpiPm1Control *r, uint16_t *out)
{
    if (!r->present) return false;
    uint16_t v = (r->space == GAS_AS_SYSTEM_MEMORY) ? *r->reg : inw(r->port);
    if (v == 0xFFFFu) return false;
    *out = v;
    return true;
}

static void pm1_control_write(const AcpiPm1Control *r, uint16_t v)
{
    if (!r->present) return;
    if (r->space == GAS_AS_SYSTEM_MEMORY) *r->reg = v;
    else                                  outw(r->port, v);
}

/* Bind one candidate description into `out`. False means this is not a
 * register this kernel can reach, and the caller may have another candidate
 * to try. */
static bool pm1_control_bind(AcpiPm1Control *out, uint8_t space,
                             uint64_t address, const char *which,
                             const char *whence)
{
    if (space == GAS_AS_SYSTEM_IO) {
        /* A GAS address is 64 bits wide and the x86 I/O space is 16. Casting
         * the wide one down would name a DIFFERENT, arbitrary port and then
         * READ it, here, during acpi_init — which is exactly the shape of
         * the boards Linux ships acpi_force_32bit_fadt_addr for, where the
         * X_ descriptor holds an address that was never an I/O address at
         * all. There is no port to probe, so the candidate is refused and
         * the caller goes on to the next one. */
        if (address > 0xFFFFu) {
            kprintf("[ACPI] %s control register (%s) names I/O address "
                    "0x%lx, past the 16-bit I/O space this machine has\n",
                    which, whence, (unsigned long)address);
            return false;
        }
        out->space   = GAS_AS_SYSTEM_IO;
        out->port    = (uint16_t)address;
        out->reg     = NULL;
        out->present = true;
        return true;
    }

    if (space == GAS_AS_SYSTEM_MEMORY) {
        volatile void *va = vmm_map_mmio((uintptr_t)address, sizeof(uint16_t),
                                          VMM_FLAGS_KERNEL_RW);
        if (!va) {
            kprintf("[ACPI] %s control register at 0x%lx (%s) is in System "
                    "Memory and cannot be mapped\n",
                    which, (unsigned long)address, whence);
            return false;
        }
        out->space   = GAS_AS_SYSTEM_MEMORY;
        out->port    = 0;
        out->reg     = (volatile uint16_t *)va;
        out->present = true;
        return true;
    }

    /* §4.8.3.2.1 gives these registers System I/O or System Memory and
     * nothing else. A GAS naming another space describes a register this
     * kernel has no instruction for, and taking its address for a port would
     * address some other device entirely. */
    kprintf("[ACPI] %s control register (%s) lives in address space %u; ACPI "
            "6.5 §4.8.3.2.1 puts PM1 control in System I/O or System "
            "Memory\n", which, whence, space);
    return false;
}

/* Undo a bind, mapping and all, so the next candidate starts from nothing.
 * `described` is what the FIRMWARE said and survives. */
static void pm1_control_release(AcpiPm1Control *r)
{
    if (r->space == GAS_AS_SYSTEM_MEMORY && r->reg)
        vmm_unmap_mmio(r->reg, sizeof(uint16_t));
    r->present = false;
    r->space   = GAS_AS_SYSTEM_IO;
    r->port    = 0;
    r->reg     = NULL;
}

static void pm1_control_locate(AcpiPm1Control *out, const acpi_gas_t *x,
                               uint32_t legacy, uint32_t fadt_len,
                               size_t x_field_end, const char *which)
{
    out->described = false;
    out->present   = false;
    out->space     = GAS_AS_SYSTEM_IO;
    out->port      = 0;
    out->reg       = NULL;

    /* The length guard is not decoration: an ACPI 1.0b FADT is 116 bytes and
     * x_pm1a_control_block lies past its end, where the struct reads whatever
     * memory follows the table. */
    bool have_x      = fadt_has_field(fadt_len, x_field_end) && x->address != 0;
    bool have_legacy = (legacy != 0);

    out->described = have_x || have_legacy;
    if (!out->described) return;     /* this firmware describes no such block */

    /* §5.2.9 makes the extended descriptor supersede the 32-bit field only
     * when it "contains a non zero value WHICH CAN BE USED BY THE OSPM". Two
     * things make an X_ description one OSPM cannot use, and on both of them
     * the 32-bit field is still a description of the same register and is
     * still worth asking — the same answer fadt_block_port gives for the GPE
     * and PM1 event blocks, and the one Linux reaches from the other side
     * with acpi_force_32bit_fadt_addr on boards that ship an invalid X_PM1*
     * address. */
    if (have_x) {
        if (pm1_control_bind(out, x->address_space, x->address, which,
                             "the X_ descriptor")) {
            uint16_t probe = 0;
            if (pm1_control_read(out, &probe)) return;

            /* Nothing decodes there. If the 32-bit field names the very same
             * port, there is no second candidate and the register simply
             * does not answer — pm1_sci_en_state will say so. */
            if (!have_legacy ||
                (out->space == GAS_AS_SYSTEM_IO &&
                 out->port == (uint16_t)legacy))
                return;

            kprintf("[ACPI] %s control register at 0x%lx (the X_ descriptor) "
                    "answers nothing that decodes; this FADT's 32-bit field "
                    "names 0x%x instead — falling back to the legacy "
                    "address\n", which, (unsigned long)x->address, legacy);
            pm1_control_release(out);
        } else if (have_legacy) {
            kprintf("[ACPI] %s: falling back to the legacy address 0x%x\n",
                    which, legacy);
        }
    }

    if (!have_legacy) return;        /* described, and out of reach */

    (void)pm1_control_bind(out, GAS_AS_SYSTEM_IO, legacy, which,
                           "this FADT's 32-bit field");
}

static void acpi_pm1_control_locate(void)
{
    if (!g_acpi.fadt) return;

    uint32_t flen = g_acpi.fadt->header.length;

    pm1_control_locate(&g_pm1a_control, &g_acpi.fadt->x_pm1a_control_block,
                       g_acpi.pm1a_cnt_blk, flen,
                       offsetof(acpi_fadt_t, x_pm1a_control_block) +
                           sizeof(acpi_gas_t), "PM1a");
    pm1_control_locate(&g_pm1b_control, &g_acpi.fadt->x_pm1b_control_block,
                       g_acpi.pm1b_cnt_blk, flen,
                       offsetof(acpi_fadt_t, x_pm1b_control_block) +
                           sizeof(acpi_gas_t), "PM1b");
}

/* What the pair says about SCI_EN. */
typedef enum {
    ACPI_SCI_EN_CLEAR = 0,   /* a register answered; SCI_EN is not set */
    ACPI_SCI_EN_SET,         /* a register answered; SCI_EN is set */
    ACPI_SCI_EN_NO_BLOCK,    /* this firmware describes no PM1 control block */
    ACPI_SCI_EN_UNREADABLE,  /* described, and no half of it answers */
} AcpiSciEnState;

/* ACPI 6.5 §4.8.3.2: the PM1 control bits "can be split between two
 * registers: PM1a_CNT or PM1b_CNT", and "the register block with
 * unimplemented bits ... always returns zeros", so the answer is the OR of
 * whichever halves answer — the shape of ACPICA's acpi_hw_read_multiple,
 * which reads xpm1a_control_block and xpm1b_control_block and ORs them.
 * Only PM1a was ever consulted here.
 *
 * "Nobody answered" is told apart from "SCI_EN is clear" and from "no such
 * block exists", because the three call for three different sentences and
 * the old code collapsed all of them into "already in ACPI mode". */
static AcpiSciEnState pm1_sci_en_state(void)
{
    if (!g_pm1a_control.described && !g_pm1b_control.described)
        return ACPI_SCI_EN_NO_BLOCK;

    uint16_t a = 0, b = 0;
    bool got_a = pm1_control_read(&g_pm1a_control, &a);
    bool got_b = pm1_control_read(&g_pm1b_control, &b);
    if (!got_a && !got_b) return ACPI_SCI_EN_UNREADABLE;

    uint16_t merged = (uint16_t)((got_a ? a : 0) | (got_b ? b : 0));
    return (merged & PM1_CNT_SCI_EN) ? ACPI_SCI_EN_SET : ACPI_SCI_EN_CLEAR;
}

/* The word one PM1 control register must be given to be asked for a sleep
 * state.
 *
 * §4.6.1 has OSPM preserve the bits of a control register, and Table 4.13
 * names the one that matters most here: bit 0 is SCI_EN, which "OSPM always
 * preserves". The word written used to be synthesised whole — (SLP_TYP << 10)
 * | SLP_EN — so it drove SCI_EN and BM_RLD to zero along with every reserved
 * bit. S5 is the only state this kernel asks for and a successful one never
 * comes back, but a REFUSED one does: the platform that declines the request
 * leaves the machine running, and it would be running with SCI_EN cleared,
 * its PM events and every GPE routed to SMI instead of to the SCI handler
 * below — the power button included, so the second press could not work
 * either. ACPICA masks off SLP_TYP and SLP_EN and nothing else
 * (acpi_hw_legacy_sleep).
 *
 * Only the word is built here. Who writes it, and in what order against the
 * other block, is the caller's business: §4.8 puts the A block's SLP_TYP down
 * first and the B block's second, and only then the transition both blocks
 * sequence on. */
static bool pm1_control_sleep_word(const AcpiPm1Control *r, uint8_t slp_typ,
                                   const char *which, uint16_t *word)
{
    if (!r->present) return false;

    uint16_t cur = 0;
    if (!pm1_control_read(r, &cur)) {
        kprintf("[ACPI] %s control register answers nothing that decodes, so "
                "there is no register to ask for sleep\n", which);
        return false;
    }

    uint16_t keep = (uint16_t)(cur & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN));
    *word = (uint16_t)(keep |
                       (((uint16_t)((uint16_t)slp_typ <<
                                    PM1_CNT_SLP_TYP_SHIFT)) &
                        PM1_CNT_SLP_TYP_MASK));
    return true;
}

/* The one place in this kernel that asks the platform to sleep.
 *
 * Both blocks receive their sleep type before either receives SLP_EN —
 * ACPI 6.5 §4.8, and the shape of ACPICA's acpi_hw_legacy_sleep: two passes
 * over the pair, not one block finished before the other is begun. Driving
 * SLP_EN in A while B still holds the previous type is exactly what that
 * sentence forbids.
 *
 * Declared in acpi.h. acpi_shutdown() is its one caller, and S5 is the one
 * sleep state BoxOS asks the platform for: S1-S4 were removed with the AML
 * executor that served them, because BoxOS puts the machine down through
 * Turn In and Bell rather than through an ACPI S-state. */
bool acpi_pm1_request_sleep(uint8_t slp_typa, uint8_t slp_typb)
{
    uint16_t word_a = 0, word_b = 0;
    bool has_a = pm1_control_sleep_word(&g_pm1a_control, slp_typa, "PM1a",
                                        &word_a);
    bool has_b = pm1_control_sleep_word(&g_pm1b_control, slp_typb, "PM1b",
                                        &word_b);
    if (!has_a && !has_b) return false;

    if (has_a) pm1_control_write(&g_pm1a_control, word_a);
    if (has_b) pm1_control_write(&g_pm1b_control, word_b);
    if (has_a) pm1_control_write(&g_pm1a_control,
                                 (uint16_t)(word_a | PM1_CNT_SLP_EN));
    if (has_b) pm1_control_write(&g_pm1b_control,
                                 (uint16_t)(word_b | PM1_CNT_SLP_EN));
    return true;
}

/* One fired GPE, served in the order ACPI 6.5 §5.6.4 lays down.
 *
 * Its enable bit is already clear: the caller dropped every fired bit of the
 * byte before serving any of them, which is the spec's first step ("disables
 * the interrupt source (GPEx_BLK EN bit)") and the only thing that keeps a
 * level source from re-delivering while it is being served.
 *
 * Two fan-outs, in priority order: the C-callback registry
 * (`acpi_gpe_register`) for in-kernel handlers that need the lowest latency,
 * then a Touch publish on "acpi:gpe" carrying the bit's index, so userspace
 * can hear GPEs without kernel code. The publish is said from a K-Core —
 * TouchPublish takes kmalloc and walks the process list, both forbidden in an
 * interrupt — so the index rides the Touch IRQ ring under the one GPE name.
 *
 * The status bit is cleared AFTER the handler, which is the sequence §5.6.4
 * gives for a level event. The spec clears an edge event's status before
 * dispatch instead, and this kernel cannot yet tell the two apart: which one a
 * GPE is, is written in the name of the method that serves it (\_GPE._Lxx
 * level, _Exx edge) and nothing here walks the \_GPE scope. The level order
 * is the one that is unsafe to get wrong — clearing a level status bit while
 * its source still asserts re-latches it at once — so that is the order used,
 * and an edge GPE pays for it only by losing a re-edge that arrived while its
 * own handler was running.
 *
 * A GPE nobody serves stays disabled. After the bring-up scrub the only way
 * one can fire is firmware arming it behind the kernel's back, and switching
 * it off for good is what AcpiEvGpeDispatch does with the same case. */
static void gpe_serve_one(const AcpiGpeBlock *blk, uint8_t byte, uint8_t bit,
                          uint16_t idx)
{
    /* gpe_block_locate() clamps every block so `idx` is inside the registry. */
    acpi_gpe_handler_t cb = g_gpe_handlers[idx];

    if (cb) cb(idx);
    acpi_publish_irq(&g_acpi_tag_gpe, idx);

    outb((uint16_t)(blk->status + byte), (uint8_t)(1u << bit));

    if (!cb) {
        if (gpe_quiet_mark(idx)) {
            kprintf("[ACPI] GPE %u fired with nobody to serve it — the source "
                    "is switched off and stays off; a level GPE left enabled "
                    "here holds the SCI asserted for good\n", idx);
        }
        return;
    }

    gpe_enable_bit(blk, byte, (uint8_t)(1u << bit), true);
}

static void gpe_block_service(const AcpiGpeBlock *blk)
{
    if (blk->status == 0) return;

    for (uint8_t b = 0; b < blk->bytes; b++) {
        /* §5.6.4 step one, for the whole byte at once and under the enable
         * register's lock: the sources go quiet before any of them is
         * served, and no concurrent registration can put them back. */
        uint8_t fired = gpe_byte_take_fired(blk, b);
        if (!fired) continue;

        debug_printf("[ACPI] GPE block 0x%x byte %u: fired=0x%02x\n",
                     blk->status, b, fired);

        uint8_t f = fired;
        while (f) {
            int bit = __builtin_ctz(f);
            f = (uint8_t)(f & ~(1u << bit));
            gpe_serve_one(blk, b, (uint8_t)bit,
                          (uint16_t)(blk->base + b * 8 + bit));
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
#define PM1_EN_PWRBTN    (1u << 8)

typedef struct {
    uint16_t status;        /* 0 when this half of the pair does not exist */
    uint16_t enable;
} AcpiPm1Event;

static AcpiPm1Event g_pm1a_event;
static AcpiPm1Event g_pm1b_event;

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

    uint16_t a = fadt_block_port(&g_acpi.fadt->x_pm1a_event_block,
                                 g_acpi.fadt->pm1a_event_block, flen,
                                 offsetof(acpi_fadt_t, x_pm1a_event_block) +
                                     sizeof(acpi_gas_t), "PM1a event block");
    uint16_t b = fadt_block_port(&g_acpi.fadt->x_pm1b_event_block,
                                 g_acpi.fadt->pm1b_event_block, flen,
                                 offsetof(acpi_fadt_t, x_pm1b_event_block) +
                                     sizeof(acpi_gas_t), "PM1b event block");

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

/* Carried out of the interrupt handler on a knock (baton.h): the press is
 * noticed in the SCI and answered where waiting is allowed. On a machine with
 * K-Cores that is the drain core's guide loop, which is the context the
 * shutdown sequence documents that it needs; on a machine that is one core it
 * is the timer's own pump, gated on having interrupted user mode so no kernel
 * lock is held. Presses that land before the answer runs are one knock and
 * one answer; a press after the door opened is answered again. */
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

/*
 * SCI interrupt handler — fires for any ACPI-routed event:
 *   - PM1 status bits (power button, sleep button, RTC alarm, wake)
 *   - GPE block status (each device-described event)
 *   - GHES SCI-class notifications (fixed-source RAS errors)
 *
 * What it does:
 *   1. Reads both PM1 status halves and masks each against its enable half,
 *   2. Acknowledges every set status bit by writing the same value back
 *      (W1C semantics on PM1_STS — required to clear the SCI line),
 *   3. Serves both GPE blocks in the order ACPI 6.5 §5.6.4 gives.
 *
 * The per-bit \_GPE._Lxx / _Exx methods are not run, and there is no AML
 * executor in this kernel to run them with: the one that existed served the
 * S1-S4 sleep path alone, was reachable from nowhere, and was removed. A GPE
 * is heard, acknowledged and said out loud under "acpi:gpe"; what the
 * firmware's method would have done about it, nothing here does.
 *
 * The PM1 STATUS block is at PM1a_CNT - PM1_EVENT_LENGTH/2, but the
 * canonical FADT layout puts it in `pm1a_event_block`. We use that.
 */
#define PM1_STS_PWRBTN  (1u << 8)
#define PM1_STS_SLPBTN  (1u << 9)
#define PM1_STS_RTC     (1u << 10)
#define PM1_STS_WAK     (1u << 15)

/* ‼ THIS HANDLER OWES NO EOI, AND USED TO SEND ONE ANYWAY.
 *
 * A responder is offered the interrupt and reads its own device; the single
 * EOI for the line is sent by irq_handler after the LAST responder, which is
 * the order a level-triggered line requires — the pin is held asserted until
 * a driver clears the device, so acknowledging before that re-delivers. This
 * function sent one of its own as well, so every SCI cost TWO LAPIC EOIs.
 * Intel SDM Vol 3A, "EOI Register": an EOI "clears the highest priority bit
 * in the ISR", so the second one did not un-acknowledge the SCI — it cleared
 * the ISR bit of whatever lower-priority interrupt the SCI had preempted,
 * ending that handler's service early and, if it was a level line, releasing
 * a pin its device was still holding. Harmless-looking for as long as the
 * SCI was the only thing on its wire; not something to keep now that it is
 * shareable and level by description. */
static void acpi_sci_handler(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) {
        return;
    }
    /* Both halves of the pair. PM1b is not a spare copy of PM1a: a chipset
     * that has one reports part of its event state there and nowhere else,
     * and a status bit nobody clears holds a level-triggered line asserted
     * for good. Only PM1a was ever read. */
    uint16_t sts_a = g_pm1a_event.status ? inw(g_pm1a_event.status) : 0;
    uint16_t sts_b = g_pm1b_event.status ? inw(g_pm1b_event.status) : 0;

    /* No PM1 event block on this machine — but a GPE block may still be
     * asserting, and jumping to the EOI from here left its status bit
     * un-acknowledged, which on a level GPE is a line that never drops. */
    if (g_pm1a_event.status == 0 && g_pm1b_event.status == 0) goto gpe_blocks;

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
        goto gpe_blocks;
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
    /* Nothing below publishes from here: each event's word goes into the
     * Touch IRQ ring under its name and is published by the K-Core that
     * reads the ring (acpi_publish_irq). */
    if (sts & PM1_STS_PWRBTN) {
        /* Said out loud, not with debug_printf: the next thing this does is
         * turn the machine off, and the one line explaining why must survive
         * into a shipped build. */
        kprintf("[ACPI] PM1 says the power button was pressed "
                "(PM1a sts 0x%04x en 0x%04x, PM1b sts 0x%04x en 0x%04x)\n",
                sts_a, en_a, sts_b, en_b);
        acpi_publish_irq(&g_acpi_tag_power_button, sts);
        /* Answered where waiting is allowed: deciding what a press means ends
         * either in a Touch delivery or in the whole shutdown sequence, and
         * neither of those belongs in an interrupt handler. */
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
gpe_blocks:
    {
        extern uint32_t apei_ghes_sci_check(void);
        (void)apei_ghes_sci_check();
    }

    /* Both GPE blocks, located in acpi_init out of the FADT and scrubbed by
     * acpi_sci_arm before this line was ever unmasked. */
    gpe_block_service(&g_gpe0);
    gpe_block_service(&g_gpe1);
}

/* The SCI's own link into the responder chain. One node, because there is
 * one SCI; it lives here rather than in the registry's built-in slot
 * because this line is described by the FADT and not by irq_register_handler
 * — see SummonsForAcpiSci. */
static SummonsResponder g_sci_responder;

void acpi_sci_register(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return;

    /* ‼ THE PLATFORM IS TOLD WHICH INTERRUPT CONTROLLER IT IS TALKING TO,
     * HERE, BEFORE THIS KERNEL ASKS IT FOR ANYTHING.
     *
     * ACPI 6.5 §5.8.1, \_PIC: "This optional object is a control method that
     * OSPM must call to indicate the interrupt model being used ... If the
     * platform does not implement the \_PIC method, OSPM assumes the platform
     * is in PIC mode." A board that is never told hands back the description
     * it prepared for the 8259s — routing entries whose numbers are ISA
     * interrupts behind a PIRQ router, meaning nothing as GSIs — and on some
     * boards more than the routing changes.
     *
     * This is the instant for it: irqchip_init has just chosen the controller
     * (so there is a true answer to give), and the SCI registered below is
     * the first interrupt this kernel takes from the platform (so nothing has
     * been asked for yet). Reading the routing tables follows in the same
     * call, because they are what the announcement is for. */
    AmlRoutingAdopt();

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
     * ACPI 6.5 §5.2.9, Table 5.9, SCI_INT (offset 46): "System vector the SCI
     * interrupt is wired to in 8259 mode. On systems that do not contain the
     * 8259, this field contains the Global System interrupt number of the SCI
     * interrupt. OSPM is required to treat the ACPI SCI interrupt as a
     * shareable, level, active low interrupt." (The citation that stood here,
     * §5.2.15, is the Embedded Controller Boot Resources Table and says
     * nothing about the SCI.)
     *
     * Two things follow from that sentence, and neither was done.
     *
     * SCI_INT is an 8259 IRQ number on every machine that has an 8259, so the
     * line to describe is ioapic_isa_to_gsi(SCI_INT). The canonical PCAT case
     * is the SCI on IRQ 9 remapped to INTIN 11 (§5.2.12.5): asking
     * ioapic_gsi_flags about 9 there asks about a pin belonging to somebody
     * else, and describing 9 hangs a level/active-low description on that
     * other device while irqchip_enable_irq programs 11 from the firmware's
     * own flags.
     *
     * And "required to treat as level, active low" is unconditional — not a
     * default that firmware's silence selects. §5.2.12.5, Table 5.26 encodes
     * polarity 00 and trigger 00 as "conforms to the specifications of the
     * bus", which for ISA means active high and edge, so an override that
     * exists and defers to the bus describes the SCI wrongly just as loudly as
     * no override at all. The guard here fired only on total silence, so a
     * board publishing {source 9, GSI 9, flags 0x0000} got an edge,
     * active-high SCI: the line idles high and asserts low, the IOAPIC never
     * latches that falling edge, the status bit is never cleared, and no ACPI
     * event is delivered for the rest of the session. Linux normalises exactly
     * these two fields in acpi_sci_ioapic_setup ("compatible SCI trigger is
     * level" / "compatible SCI polarity is low").
     *
     * Only fields firmware left at 00 are replaced; a board that states a
     * polarity or a trigger for its own SCI keeps it.
     */
    uint32_t gsi = (sci < 16) ? ioapic_isa_to_gsi((uint8_t)sci) : sci;
    /* FIRMWARE'S table, not the resolved one. The question asked here is
     * literally "did the board describe this line", and ioapic_gsi_flags
     * falls through to the descriptions this kernel writes itself — which
     * would make the sentence printed below report this kernel's own
     * conclusion as the firmware's. Unreachable today, because nothing has
     * described a line by the time acpi_sci_register runs; asked correctly
     * anyway, since the whole point of the provenance is that it cannot be
     * fabricated. */
    uint16_t said = 0;
    bool described = SummonsFirmwareSaid(gsi, &said);
    uint16_t forced = said;
    if ((forced & 0x0003u) == 0) forced |= 0x0003u;   /* polarity 11: active low */
    if ((forced & 0x000Cu) == 0) forced |= 0x000Cu;   /* trigger  11: level      */

    if (!described || forced != said) {
        if (sci < 16) {
            /* The correction has to live where the lookup looks first, which
             * for an ISA line is the Interrupt Source Override table — the
             * very place a firmware override that defers to the bus sits. */
            ioapic_register_iso((uint8_t)sci, gsi, forced);
        } else {
            ioapic_describe_gsi(gsi, forced);
        }
        kprintf("[ACPI] SCI is IRQ %u on GSI %u; firmware said %s0x%04x — "
                "programmed 0x%04x, level-triggered and active low, as "
                "ACPI 6.5 §5.2.9 requires\n",
                sci, gsi, described ? "" : "nothing, i.e. ", said, forced);
    }

    /* ‼ THE SCI IS DESCRIBED BY ITS SPECIFICATION, NOT BY THE ISA BUS RULE
     * ITS NUMBER CAME FROM. irq_register_handler builds SummonsForIsaLine,
     * which states edge, active high and EXCLUSIVE; the override planted
     * above corrects the first two and cannot touch the third, because
     * sharing is not a field of the MPS INTI word. §5.2.9 says "shareable,
     * level, active low" in one breath, so all three are stated in one
     * place — SummonsForAcpiSci — and a PCI function that _PRT routes onto
     * this same pin can now join the line instead of being refused it. */
    Summons line = SummonsForAcpiSci((uint8_t)sci);
    if (!SummonsAnswer(&line, &g_sci_responder, acpi_sci_handler,
                       "the ACPI SCI")) {
        return;
    }
    kprintf("[ACPI] SCI handler on GSI %u — the line opens when its names "
            "exist (acpi_sci_arm)\n", sci);
}

/* The door, after the names.
 *
 * Every event the SCI handler notices is said under a name it must already
 * hold — an interrupt cannot resolve one — and the names live in the Touch
 * registry, which comes up inside guide_init, long after the handler was
 * registered. So the line is unmasked here, once the names are resolved and
 * the APEI runtime the handler also consults is up. A level-triggered SCI
 * raised meanwhile waits in the IOAPIC and lands the moment the line opens:
 * nothing is lost by opening late, and everything would be lost by opening
 * early. The power button is armed last, for the same reason. */
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

    /* Before the door, not after: a GPE the firmware left armed and asserting
     * would otherwise be delivered the instant the line is unmasked, and this
     * kernel could only acknowledge it, never quiet it. Where the blocks ARE
     * was settled back in acpi_init, so a driver that registered a handler
     * during its own init found its block described; what that driver armed
     * survives the scrub. */
    acpi_gpe_blocks_scrub();

    /* Armed from the same description the handler was registered against,
     * so the pin's redirection entry and the registry cannot say different
     * things about a line this kernel is about to start taking. */
    Summons line = SummonsForAcpiSci((uint8_t)sci);
    if (!SummonsArm(&line)) {
        kprintf("[ACPI] %[E]the SCI line could not be armed — no ACPI event, "
                "no power button, and no GPE will be delivered%[D]\n");
        return;
    }
    kprintf("[ACPI] SCI on GSI %u\n", line.gsi);

    acpi_pm1_events_locate();
    acpi_pm1_arm_power_button();
}

/* How long the firmware gets to finish the handoff into ACPI mode.
 *
 * DECLARED POLICY, not a spec quantity. ACPI 6.5 §16.3.1 "Placing the System
 * in ACPI Mode" has OSPM "generat[e] a write of the ACPI_ENABLE value to the
 * SMI_CMD port (as described in the FADT)" and then says only that "the
 * hardware platform will set the SCI_EN bit to indicate to OSPM that the
 * hardware platform is now configured for ACPI". It names no deadline, and it
 * does not prescribe polling at all — polling is ACPICA's practice, not spec
 * text. Three seconds is this kernel's patience, written once so there is one
 * place to argue with it. (The citation that stood here, §16.3.2, is
 * "Platform Boot Firmware Initialization of Memory" and mentions none of
 * SMI_CMD, ACPI_ENABLE or SCI_EN.) */
#define ACPI_SCI_HANDOFF_PATIENCE_US   3000000u

/* ACPI 6.5 §5.2.9, FADT Flags, HW_REDUCED_ACPI: "If set, indicates that the
 * ACPI Hardware Interface is not implemented." That is the one shape in
 * which a FADT legitimately describes no PM1 control block at all. */
#define FADT_FLAG_HW_REDUCED_ACPI   (1u << 20)

/* ACPI 6.5 §16.3.1: many real firmwares boot in PIC/SMI mode (SCI_EN=0).
 * The OS writes FADT.ACPI_ENABLE to FADT.SMI_CMD and the platform raises
 * SCI_EN when it has handed the machine over; no PM1 write takes effect
 * before that. UEFI implementations usually have SCI_EN=1 already; legacy
 * BIOS often doesn't. Idempotent.
 *
 * SCI_EN is read through the PM1 control registers resolved out of the FADT
 * (acpi_pm1_control_locate, above) and from BOTH halves of the pair — not
 * out of the legacy PM1a_CNT_BLK field with a bare inw, which is what used to
 * make an undecoded port's 0xFFFF look like "already in ACPI mode". */
static void acpi_enable_mode(void) {
    if (!g_acpi.fadt) return;
    uint32_t smi   = g_acpi.fadt->smi_command_port;
    uint8_t  byte  = g_acpi.fadt->acpi_enable;

    switch (pm1_sci_en_state()) {
        case ACPI_SCI_EN_SET:
            debug_printf("[ACPI] SCI_EN already set — platform in ACPI mode\n");
            return;
        case ACPI_SCI_EN_NO_BLOCK:
            /* No PM1 control block is described at all. On a
             * hardware-reduced platform that is the whole truth — §4.8's
             * fixed hardware is not implemented and there is no SCI_EN to
             * hand over. On anything else it is a malformed FADT, and it
             * costs this boot the power button and S5, so the flag is
             * consulted instead of assumed and both sentences are kprintf:
             * a debug_printf is nothing in a shipped build. */
            if (g_acpi.fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) {
                kprintf("[ACPI] hardware-reduced platform (FADT "
                        "HW_REDUCED_ACPI): no PM1 control register exists "
                        "and there is no SCI_EN to hand over\n");
            } else {
                kprintf("[ACPI] this FADT names no PM1 control register in "
                        "either the X_ descriptor or the 32-bit field, yet "
                        "HW_REDUCED_ACPI is clear — ACPI 6.5 §4.8 requires "
                        "the register on such a platform. SCI_EN cannot be "
                        "read or handed over: PM1 writes (the power button, "
                        "S5) will not take effect\n");
            }
            return;
        case ACPI_SCI_EN_UNREADABLE:
            kprintf("[ACPI] the PM1 control register this FADT describes "
                    "answers nothing that decodes — SCI_EN cannot be read, so "
                    "the ACPI-mode handoff is not attempted. PM1 writes (the "
                    "power button, S5) may not take effect on this boot\n");
            return;
        case ACPI_SCI_EN_CLEAR:
            break;
    }

    if (smi == 0 || byte == 0) {
        kprintf("[ACPI] SCI_EN is clear and this FADT names no way to raise "
                "it (SMI_CMD=0x%x, ACPI_ENABLE=0x%x) — the platform stays in "
                "SMI mode: PM1 writes (the power button, S5) will not take "
                "effect\n", smi, byte);
        return;
    }

    debug_printf("[ACPI] handing off to ACPI mode (smi=0x%x byte=0x%x)\n",
                 smi, byte);
    outb((uint16_t)smi, byte);

    /* Wait for the firmware's SMI handler to hand the platform over — on the
     * FACT, bounded by real time.
     *
     * What stood here counted three thousand units of "about a millisecond",
     * where the unit was a thousand `outb %al,$0x80`. On a board where port
     * 0x80 is absorbed by the eSPI bridge those three million writes retire in
     * a handful of milliseconds, so a legacy BIOS whose SMI handler needs
     * forty loses the handoff — and the only complaint was a debug_printf,
     * which a shipped build compiles to nothing. Everything downstream then
     * writes PM1 registers the chipset is not honouring: the power button does
     * nothing and acpi_shutdown's SLP_TYP write is a no-op, so the machine
     * halts where it should have powered off.
     *
     * The ruler is already here. acpi_init calls acpi_parse_tables — which
     * hands the PM Timer to the timer driver through pmtimer_adopt_from_fadt —
     * and only then calls this function, so HourGlass has something to measure
     * with before the first poll. */
    HourGlass glass;
    if (!HourGlassTurn(&glass, ACPI_SCI_HANDOFF_PATIENCE_US)) {
        kprintf("[ACPI] no time source on this machine yet — waiting for "
                "SCI_EN on the fact itself, with no bound\n");
        for (;;) {
            AcpiSciEnState st = pm1_sci_en_state();
            if (st == ACPI_SCI_EN_SET) {
                kprintf("[ACPI] SCI_EN raised — the platform is in ACPI "
                        "mode\n");
                return;
            }
            /* The wait ends on a fact or on the loss of the fact. A register
             * that answered a moment ago and answers nothing now is not a
             * reason to spin for the life of the machine. */
            if (st != ACPI_SCI_EN_CLEAR) {
                kprintf("[ACPI] the PM1 control register stopped answering "
                        "while the handoff was in flight — SCI_EN can no "
                        "longer be read and the platform's mode is "
                        "unknown\n");
                return;
            }
            cpu_pause();
        }
    }
    for (;;) {
        if (pm1_sci_en_state() == ACPI_SCI_EN_SET) {
            /* One sample so the elapsed figure below is the time that really
             * passed rather than whatever the last poll left behind. */
            (void)HourGlassRunOut(&glass);
            kprintf("[ACPI] SCI_EN raised after %lu us, measured on the %s\n",
                    (unsigned long)HourGlassElapsedUs(&glass),
                    HourGlassRulerName(&glass));
            return;
        }
        if (HourGlassRunOut(&glass)) break;
        cpu_pause();
    }

    /* kprintf, not debug_printf: this line decides whether the power button
     * and S5 work at all on this boot, and a line like that must survive
     * into a shipped build. */
    kprintf("[ACPI] SCI_EN never rose — %lu us on the %s and the %s. The "
            "platform stays in SMI mode: PM1 writes (power button, S5) will "
            "not take effect\n",
            (unsigned long)HourGlassElapsedUs(&glass),
            HourGlassRulerName(&glass),
            HourGlassSourceDied(&glass) ? "ruler died mid-wait"
                                        : "span ran out");
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

    /* Where the PM1 control registers are — X_ first, with a FADT-length
     * guard, and in whatever address space the descriptor names. The handoff
     * below reads SCI_EN out of them and acpi_shutdown() writes its S5 sleep
     * vector to them, so this has to stand before either. */
    acpi_pm1_control_locate();

    /* Switch the platform into ACPI mode if firmware booted us in
     * legacy/SMI mode. Must happen after FADT is captured but BEFORE
     * any PM1 write is attempted. */
    acpi_enable_mode();

    /* Out of the FADT, before anything can ask to register a GPE handler.
     * Arming the line, and scrubbing whatever the firmware left armed, is a
     * separate and much later step (acpi_sci_arm). */
    acpi_gpe_blocks_locate();

    debug_printf("[ACPI] Initialization complete (HPET=%s MCFG=%s S5=%s)\n",
                 g_acpi.hpet.present ? "yes" : "no",
                 g_acpi.mcfg.present ? "yes" : "no",
                 g_acpi.s5_found     ? "yes" : "no");

    /* Broadcast capability bitmap so userspace daemons can decide what
     * features to bring up (an IOMMU service only if DMAR/IVRS exposed
     * something, a power daemon only if _S5 was found, etc.).
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
