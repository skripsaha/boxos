#include "idt.h"
#include "right_of_way.h"
#include "gdt.h"
#include "tss.h"
#include "klib.h"
#include "klib_panic.h" /* PanicClaimForException, PanicEndAccount — one claim and one end for the dump and panic() */
#include "io.h"
#include "pic.h"
#include "irqchip.h"
#include "lapic.h"
#include "process.h"
#include "guide.h"
#include "ready_queue.h" // for g_ready_queue
#include "pocket_ring.h"
#include "kring.h"
#include "vmm.h"
#include "uaccess.h"          /* UACCESS_USER_VA_MAX for SMAP-fault diagnostic */
#include "nameplate.h"        /* nameplate_name_at — name the frames of a user fault */
#include "nameplate_format.h" /* NameplateHeader — the kernel table's own minimum size */
#include "atomics.h"
#include "touch.h" /* TouchTag types — Phase 2K #CP publish */
#include "scheduler.h"
#include "use_context.h"
#include "idle.h"   /* process_is_idle — the wake bell reschedules an idle core */
#include "context_switch.h"
#include "keyboard.h"
#include "ahci.h"
#include "amp.h"
#include "klib_logring.h" /* LogRingBellKnock — the tick knocks for the NMI's lines */
#include "serial.h"       /* WireTick — the tick pumps a line with no interrupt of its own */
#include "kcore.h"
#include "xhci_interrupt.h"
#include "linker_symbols.h"
#include "cpu_calibrate.h" // cpu_tsc_recal_tick (periodic recalibration)
#include "touch_queue.h"
#include "nightwatch.h"
#include "nightwatch.h"
#include "clockboard.h"
#include "pit.h"
#include "baton.h"
#include "lifeline.h" /* a fault inside foreign firmware is thrown back, not fatal */
#include "efi.h"      /* EfiDescribePhysical — CR2 named against the firmware's map */
#include "system_halt.h" /* g_halt_orderly, HaltFreezeHere — the halt IPI's two ends */

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

/* One record per VECTOR: what the line delivered under it IS, and who
 * answers it.
 *
 * The array is as long as the IDT, and that is not a budget anybody chose —
 * it is how many gates an IDT has (Intel SDM Vol 3A §6.10), so the registry
 * cannot be the wrong length for the table it describes. It used to be
 * IRQ_MAX_COUNT long and indexed by `vector - 32`, which was the same thing
 * written as arithmetic, and which quietly asserted that no line could ever
 * be delivered above vector 55. `builtin` is the one answerer
 * a line is born with, for callers of the ISA-era irq_register_handler that
 * have nowhere of their own to keep a link; every further answerer on a
 * shareable line brings its own node, so nothing here caps how many devices
 * may sit on one wire. */
typedef struct {
    Summons           line;
    SummonsResponder *answerers;     /* head; release-stored, acquire-read */
    SummonsResponder  builtin;
    bool              posted;        /* a Summons has described this line */
    bool              unanswered_said;
    /* Said once per PIN, not once per settling. A line is settled twice by
     * every driver that answers it and then arms it, and ata_async re-arms
     * its own on every lost-INTRQ reconcile; the facts these two name are
     * properties of the pin, and repeating them on a live console would bury
     * everything else. */
    bool              join_said;
    bool              wiring_said;
    /* Said once per VECTOR: an interrupt arrived under a number no line in
     * this kernel is recorded on. */
    bool              said_unarranged;
} IrqLine;

#define IDT_VECTOR_COUNT 256
static IrqLine irq_lines[IDT_VECTOR_COUNT];

/* ‼ WHAT KEEPS THIS TABLE CONSISTENT, since it is not a lock.
 *
 * The vector pool takes a spinlock and this registry does not, which looks
 * like an oversight and is worth being exact about instead.
 *
 * Every write here happens on the path that brings a line up — SummonsAnswer,
 * SummonsPost, SummonsSettleOnPin, SummonsUnanswer — and that path runs on the
 * boot processor during bring-up, single file, because this kernel arms every
 * line from one place in one order. The reads that race with it are
 * irq_handler's, on any core, and they are made safe by the one field that IS
 * ordered: `answerers` is published with a release store and read with an
 * acquire load, so a handler either does not see a line yet or sees it whole.
 *
 * That is a property of the current bring-up order rather than of this data
 * structure, and it stops holding the day a driver arms a line from an AP or
 * from a work item. If that day comes, the lock this table needs is the
 * pool's — one lock over both, because "who holds this vector" and "what is
 * this vector's line" are two halves of one fact. */

/* The frame the interrupt being served arrived on, per core.
 *
 * `irq_callback_t` takes nothing, and that is right for almost every answerer:
 * a disk driver has no business with the interrupted thread's registers. The
 * tick is the exception, and not by accident — its whole job is to preempt,
 * and preemption means writing a return frame. It used to read `frame`
 * directly because it was written inline in irq_handler; as an ordinary
 * answerer it needs the same thing by another route.
 *
 * Per core, because two cores can be inside two different handlers at once.
 *
 * ‼ VALID ONLY WHILE AN ANSWERER IS RUNNING. It points at the stack of the
 * core that took the interrupt, and that stack is reused the moment the
 * handler returns. An answerer that keeps this pointer is keeping a pointer
 * into somebody else's future stack frame. */
static interrupt_frame_t *volatile g_irq_frame[MAX_CORES];

interrupt_frame_t *IrqFrameInFlight(void)
{
    uint8_t core = amp_get_core_index();
    if (core >= MAX_CORES) return NULL;
    return g_irq_frame[core];
}

static volatile uint64_t exception_count = 0;
static volatile uint64_t irq_count[IDT_VECTOR_COUNT] = {0};

static void idt_load_asm(uint64_t idt_desc_addr)
{
    asm volatile("lidt (%0)" : : "r"(idt_desc_addr) : "memory");
}

void idt_load(void)
{
    idt_load_asm((uint64_t)&idt_desc);
}

void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type_attr, uint8_t ist)
{
    idt[index].offset_low = handler & 0xFFFF;
    idt[index].selector = selector;
    idt[index].ist = ist & 0x07;
    idt[index].type_attr = type_attr;
    idt[index].offset_middle = (handler >> 16) & 0xFFFF;
    idt[index].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].reserved = 0;
}

void idt_init(void)
{
    debug_printf("[IDT] Initializing Interrupt Descriptor Table (minimal)...\n");

    memset(idt, 0, sizeof(idt));

    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint64_t)idt;

    debug_printf("[IDT] Setting up exception handlers (0-31)...\n");

    for (int i = 0; i < 32; i++)
    {
        uint8_t ist = 0; // No IST by default

        // Critical exceptions use IST
        switch (i)
        {
        case 1: // Debug
            ist = IST_DEBUG;
            break;
        case 2: // NMI
            ist = IST_NMI;
            break;
        case 8: // Double Fault
            ist = IST_DOUBLE_FAULT;
            break;
        case 12: // Stack Fault
            ist = IST_STACK_FAULT;
            break;
        case 18: // Machine Check
            ist = IST_MACHINE_CHECK;
            break;
        }

        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, ist);
    }

    /* Every vector above the exceptions, installed here and never again.
     *
     * This used to install gates only where this kernel had a use for one —
     * 32..55 for the IOAPIC window, nine MSI numbers, the syscall, three IPIs,
     * two LAPIC specials — and left 185 descriptors with P=0. That is not a
     * harmless absence. Delivery into a not-present gate raises #NP with error
     * code (vector << 3) | 2 | EXT (Intel SDM Vol 3A §6.14, "Interrupt 11 —
     * Segment Not Present"), which on this kernel kills whatever ring-3
     * process happened to be running and leaves the ISR bit set in the LAPIC,
     * deafening that priority class and every class below it.
     *
     * Installing the whole run costs 185 more descriptors — three kilobytes of
     * a table that is already allocated in full — and buys two things. A
     * vector nothing asked for arrives at irq_handler carrying its own number,
     * so the log can say what it was. And handing a vector to a line stops
     * being a write into the IDT of a running machine: the gate is already
     * there, and only the responder registry changes.
     *
     * That second property is load-bearing rather than tidy. idt_set_entry
     * publishes the present bit before the top 48 bits of the offset, which is
     * harmless while this runs once before any AP is up and is a torn gate the
     * moment a driver arms a line at runtime — and drivers arm lines at
     * runtime, with the APs already taking interrupts.
     *
     * DPL is 0 for every one of them, deliberately and with one exception. The
     * syscall vector is the only gate in this kernel a ring-3 INT may reach;
     * if a device vector were ever given the same DPL, a user process could
     * raise its own device's interrupt by hand. */
    for (uint32_t v = 32; v < 256; v++)
    {
        uint8_t type = (v == SYSCALL_VECTOR) ? IDT_TYPE_USER_INTERRUPT
                                             : IDT_TYPE_INTERRUPT_GATE;
        idt_set_entry((int)v, (uint64_t)isr_table[v],
                      GDT_KERNEL_CODE, type, 0);
    }

    /* The pool is seeded here, at the end of idt_init, and both halves of
     * that placement are load-bearing.
     *
     * Here rather than in irqchip_init, because every gate above now exists:
     * from this line on "a vector is free" and "a vector has a gate nobody
     * uses" are the same statement. Seeding before the gates were installed
     * would have left the pool's idea of the machine and the IDT's as two
     * separate truths.
     *
     * In idt_init rather than in idt_load, because idt_load is what every
     * application processor calls on its way up. Seeding from there ran the
     * seeding once per core, late, and each run wiped what the boot processor
     * had already handed out — which is how the timer came to be armed on a
     * line with no vector at all. */
    VectorPoolInit();

    idt_load_asm((uint64_t)&idt_desc);

    debug_printf("[IDT] IDT loaded successfully at 0x%lx (limit=%d)\n", idt_desc.base, idt_desc.limit);
}

bool SummonsSettleOnPin(Summons *line)
{
    if (!line) return false;

    /* Who already holds this PIN? The registry is indexed by vector, so this
     * is a walk — over the whole vector space, at registration time only, and
     * it is the only place in the kernel that knows the answer. */
    for (uint32_t v = 0; v < IDT_VECTOR_COUNT; v++)
    {
        IrqLine *l = &irq_lines[v];
        if (!l->posted || l->line.gsi != line->gsi) continue;

        uint8_t held = (uint8_t)v;   /* the index IS the vector */
        if (held != line->vector)
        {
            if (!l->join_said)
            {
                l->join_said = true;
                kprintf("[IRQ] %s GSI %u already delivers under vector "
                        "0x%02x, so this line takes that one rather than "
                        "being given its own — one pin has one redirection "
                        "entry and therefore one vector\n",
                        line->bus, line->gsi, held);
            }
            line->vector = held;
        }

        if (l->answerers == NULL)
        {
            /* Described but unanswered: nothing depends on the wire yet, so
             * the newcomer's description stands as it is. */
            return true;
        }

        if ((l->line.polarity != line->polarity ||
             l->line.trigger  != line->trigger) && !l->wiring_said)
        {
            l->wiring_said = true;
            kprintf("[IRQ] %[E]%s GSI %u is already driven %s, %s on behalf "
                    "of %s, and this line describes it as %s, %s — the pin "
                    "keeps the wire it was given, because a redirection entry "
                    "other drivers depend on is not re-cut for a device that "
                    "arrived second%[D]\n",
                    line->bus, line->gsi,
                    l->line.trigger == SUMMONS_LEVEL ? "level" : "edge",
                    l->line.polarity == SUMMONS_ACTIVE_LOW ? "active low"
                                                           : "active high",
                    l->answerers->who ? l->answerers->who : "another driver",
                    line->trigger == SUMMONS_LEVEL ? "level" : "edge",
                    line->polarity == SUMMONS_ACTIVE_LOW ? "active low"
                                                         : "active high");
        }

        /* Shareable only if EVERYONE describing the pin says so. Everything
         * else is the incumbent's, because it is what the hardware is
         * already programmed with. */
        bool shared = l->line.shared && line->shared;
        uint8_t pin = line->intx_pin;
        *line = l->line;
        line->shared   = shared;
        line->intx_pin = pin;   /* the newcomer's own (device, pin) key */
        return true;
    }

    /* Nobody holds the pin, so this line is the first on it and this is where
     * it is given right of way. Not in the constructor: a line is built more
     * than once in a boot and a vector taken there would leak on every
     * rebuild; not at arming time either, because answering a line settles it
     * too and the answerer must find the same vector the armer will use. */
    if (line->vector == 0)
    {
        char who[40];
        ksnprintf(who, sizeof(who), "%s GSI %u", line->bus, line->gsi);

        if (irqchip_get_type() == IRQCHIP_PIC)
        {
            /* On an 8259 the number is not this kernel's to choose. ICW2 sets
             * the base of a run of eight consecutive vectors and the chip
             * ignores the low three bits of what is written there, so
             * "vector = base + input" is how the silicon is wired. The pool
             * is still told, because two owners of one vector is the one
             * thing it exists to prevent. */
            uint8_t fixed = irq_gsi_to_vector((uint8_t)line->gsi);
            if (!VectorPoolClaim(fixed, "an 8259 input")) return false;
            line->vector = fixed;
        }
        else
        {
            line->vector = VectorPoolTake(line->row, who);
            if (line->vector == 0) return false;   /* refused, and said so */
        }
    }

    IrqLine *l = &irq_lines[line->vector];
    if (l->posted && l->line.gsi != line->gsi)
    {
        /* Two pins, one vector. irq_handler is handed the vector and nothing
         * else, so it could not tell the two apart, and one LAPIC EOI would
         * clear Remote IRR on both. There is no join that fixes this. */
        kprintf("[IRQ] %[E]%s GSI %u cannot be settled: vector %u already "
                "delivers GSI %u, and a vector cannot name two pins%[D]\n",
                line->bus, line->gsi, line->vector, l->line.gsi);
        return false;
    }
    return true;
}

void SummonsPost(const Summons *line)
{
    if (!line) return;

    if (line->vector < IRQ_VECTOR_BASE) return;  /* named by whoever tried to arm it */

    IrqLine *l = &irq_lines[line->vector];
    if (l->posted && l->line.gsi != line->gsi)
    {
        /* Unreachable through SummonsArm, which settles first. Enforced
         * rather than assumed, because "the registry and the redirection
         * entry can never disagree about what the line is" is an invariant
         * this function is the last to be able to hold. */
        kprintf("[IRQ] %[E]%s GSI %u not recorded on vector %u: that vector "
                "already describes GSI %u%[D]\n",
                line->bus, line->gsi, line->vector, l->line.gsi);
        return;
    }

    l->line   = *line;
    l->posted = true;
}

bool SummonsAnswer(const Summons *line, SummonsResponder *node,
                   irq_callback_t answer, const char *who)
{
    if (!line || !node || !answer) return false;

    const char *name = who ? who : "an answerer that did not name itself";

    /* No check on the vector here, deliberately. A line arrives with no
     * vector at all -- it is given one by the settling below -- so testing it
     * beforehand would refuse every line in the machine. */
    Summons described = *line;
    SummonsResolve(&described);
    if (!SummonsSettleOnPin(&described)) return false;

    /* The key is the SETTLED vector, which is the incumbent's when this line
     * is joining a pin somebody already holds. Taking it before settling is
     * how the ACPI SCI (ISA IRQ 9, pin 11, vector 41) and a PCI function
     * _PRT puts on GSI 11 (vector 43) came to occupy two separate chains for
     * one wire. */
    IrqLine *l = &irq_lines[described.vector];

    SummonsResponder *head = l->answerers;
    if (head != NULL)
    {
        /* A second answerer is ordinary on a PCI INTx line and a mistake on
         * an ISA one, and the description is what tells them apart. Refusing
         * BOTH used to be the behaviour, except that it refused by
         * overwriting: the warning went to debug_printf and the first
         * driver's handler was simply gone. */
        if (!described.shared || !l->line.shared)
        {
            kprintf("[IRQ] %[E]%s cannot answer %s GSI %u: %s already does, "
                    "and this line is exclusive — %s says one device drives "
                    "it%[D]\n",
                    name, described.bus, described.gsi,
                    head->who ? head->who : "another driver", described.bus);
            return false;
        }
    }

    /* A node can be on a line once. Registering the same one twice would
     * link it to itself and turn the walk in irq_handler into a loop the core
     * never leaves — and the ISA-era form hands out a node the registry owns,
     * so a driver calling irq_register_handler twice would do exactly that on
     * a line that ever became shareable. */
    for (SummonsResponder *seen = head; seen; seen = seen->next) {
        if (seen == node) {
            kprintf("[IRQ] %[E]%s already answers %s GSI %u — registering the "
                    "same link twice would close the chain on itself%[D]\n",
                    name, described.bus, described.gsi);
            return false;
        }
    }

    node->answer = answer;
    node->who    = name;
    node->next   = NULL;

    l->line   = described;
    l->posted = true;

    /* Appended at the tail, so answerers are offered the interrupt in the
     * order they arrived. The release store is what makes the node's fields
     * visible to a handler running on another core the instant the link is;
     * registration itself is single-threaded boot work, so nothing needs to
     * be serialised against another registration. */
    if (head == NULL)
    {
        __atomic_store_n(&l->answerers, node, __ATOMIC_RELEASE);
    }
    else
    {
        SummonsResponder *tail = head;
        while (tail->next) tail = tail->next;
        __atomic_store_n(&tail->next, node, __ATOMIC_RELEASE);
    }

    debug_printf("[IRQ] %s answers %s GSI %u (vector %u)\n",
                 name, described.bus, described.gsi, described.vector);
    return true;
}

/* ── Leaving ──────────────────────────────────────────────────────────────
 *
 * A vector that is handed out and never handed back is a vector the next
 * device to arrive cannot have. That did not matter while a line's vector was
 * its pin number plus thirty-two — the number came back by arithmetic whether
 * anybody returned it or not — and it matters now.
 *
 * ‼ THE ORDER IS THE WHOLE THING, so it is stated before the code:
 *   1. if this is the LAST answerer, mask the source first. A line whose
 *      driver has gone but whose pin is still armed delivers into a chain
 *      with nobody on it, and for a level-triggered line that is a wedge.
 *   2. unlink the node.
 *   3. only then give the number back. Returning it first means the next
 *      caller can be handed a vector the departing device can still raise.
 *
 * ‼ WHAT IS AND IS NOT GUARANTEED AGAINST THE LOCK-FREE WALK, said plainly.
 * irq_handler walks this chain with interrupts disabled, following `next`
 * with acquire loads. Unlinking is safe against that walk: the node stays
 * valid memory — every node in this kernel is static storage owned by its
 * driver — and its `next` is left pointing forward, so a walker standing on
 * the removed node still reaches everyone behind it. What is NOT covered is a
 * walker still standing on that node when the SAME node is registered again
 * later: registration sets `next` to NULL before splicing, and a walker there
 * at that instant would see the chain end early. Masking the source before
 * the last removal is what makes that window small rather than closed, and a
 * device that leaves and returns does so milliseconds apart at best.
 *
 * ‼ AND THE PART THAT IS EASY TO UNDERSTATE: a walker that had already loaded
 * this node when the unlink happened WILL STILL CALL IT. Returning from here
 * does not mean the answerer has stopped running — it means no walk started
 * after this point will reach it. A driver that frees state its handler reads
 * must therefore stop its device first and keep that state valid until it is
 * certain no handler is in flight; this function cannot promise that and does
 * not try to. It is a limit, not a proof, and it is written out so nobody
 * mistakes it for one.
 */
bool SummonsUnanswer(const Summons *line, SummonsResponder *node,
                     const char *who)
{
    if (!line || !node) return false;

    const char *name = who ? who : "an answerer that did not name itself";

    /* The caller may not know the vector, and that is not its fault:
     * SummonsAnswer settles a COPY of the description and does not write the
     * settled vector back, so a driver that answered a line and then failed to
     * arm it holds a Summons whose vector is still zero. The pin is the thing
     * it does know, and the registry can be asked by pin — the same walk
     * SummonsSettleOnPin does, for the same reason. */
    uint8_t vector = line->vector;
    if (vector < IRQ_VECTOR_BASE) {
        for (uint32_t v = 0; v < IDT_VECTOR_COUNT; v++) {
            if (irq_lines[v].posted && irq_lines[v].line.gsi == line->gsi) {
                vector = (uint8_t)v;
                break;
            }
        }
    }
    if (vector < IRQ_VECTOR_BASE) {
        kprintf("[IRQ] %[E]%s asked to stop answering %s GSI %u, and no vector "
                "in this kernel delivers that pin — there is nothing to take "
                "it off%[D]\n", name, line->bus, line->gsi);
        return false;
    }

    IrqLine *l = &irq_lines[vector];

    /* ‼ ONE WALK, and everything is decided from it before anything is done.
     *
     * There were two: one to count and decide whether this was the last
     * answerer, another inside the unlink. Between them the answer could
     * change, and worse, the pin was masked on the strength of the first walk
     * BEFORE the second had established that this node was on the line at all
     * — so a driver passing a node that belonged somewhere else masked a
     * working line for everybody on it and then returned false as though it
     * had done nothing. */
    SummonsResponder *head = __atomic_load_n(&l->answerers, __ATOMIC_ACQUIRE);
    unsigned count = 0;
    bool found = false;
    for (SummonsResponder *r = head; r; r = r->next) {
        count++;
        if (r == node) found = true;
    }

    if (!found) {
        kprintf("[IRQ] %[E]%s asked to stop answering %s GSI %u and is not on "
                "that line — either it never answered it, or it answered a "
                "different one and this is the wrong record. Nothing was "
                "masked and nothing was taken off%[D]\n",
                name, line->bus, line->gsi);
        return false;
    }

    bool last = (count == 1);

    /* Now, and only now, the source is stopped — and only if this really is
     * the last answerer, because the others are still using the line. */
    if (last && l->posted) SummonsDisarm(&l->line);

    if (head == node) {
        __atomic_store_n(&l->answerers, node->next, __ATOMIC_RELEASE);
    } else {
        for (SummonsResponder *r = head; r->next; r = r->next) {
            if (r->next == node) {
                __atomic_store_n(&r->next, node->next, __ATOMIC_RELEASE);
                break;
            }
        }
    }
    /* `node->next` is deliberately left alone — see the note above. */

    if (!last) {
        kprintf("[IRQ] %s stops answering %s GSI %u; %u other(s) still do, so "
                "the line stays armed and keeps its vector\n",
                name, line->bus, line->gsi, count - 1u);
        return true;
    }

    kprintf("[IRQ] %s was the last answerer on %s GSI %u — the pin is masked "
            "and the vector goes back, because a line nobody answers is a "
            "line that would deliver into an empty chain\n",
            name, line->bus, line->gsi);

    l->posted          = false;
    l->unanswered_said = false;
    l->join_said       = false;
    l->wiring_said     = false;
    l->said_unarranged = false;
    memset(&l->line, 0, sizeof(l->line));

    VectorPoolGiveBack(vector, name);
    return true;
}

bool IrqAnswerMessage(uint8_t vector, SummonsResponder *node,
                      irq_callback_t answer, const char *who)
{
    if (!node || !answer || vector < IRQ_VECTOR_BASE) return false;

    const char *name = who ? who : "an answerer that did not name itself";
    IrqLine *l = &irq_lines[vector];

    /* A message-signalled interrupt has no pin, and that is the whole
     * difference from SummonsAnswer. There is no redirection entry to cut, no
     * polarity or trigger to agree on, and no wire to share: the vector IS the
     * wire, point to point from the device to the LAPIC. So `posted` stays
     * false — no Summons describes this entry — and the level-line rescue in
     * irq_handler correctly leaves it alone, because there is no pin to mask.
     *
     * The chain is kept rather than a single callback, so that there stays one
     * path through irq_handler instead of two. */
    for (SummonsResponder *seen = __atomic_load_n(&l->answerers, __ATOMIC_ACQUIRE);
         seen; seen = seen->next)
    {
        if (seen == node)
        {
            kprintf("[IRQ] %[E]%s already answers vector 0x%02x — registering "
                    "the same link twice would close the chain on itself%[D]\n",
                    name, vector);
            return false;
        }
    }

    node->answer = answer;
    node->who    = name;
    node->next   = NULL;

    SummonsResponder *head = l->answerers;
    if (head == NULL)
        __atomic_store_n(&l->answerers, node, __ATOMIC_RELEASE);
    else
    {
        SummonsResponder *tail = head;
        while (tail->next) tail = tail->next;
        __atomic_store_n(&tail->next, node, __ATOMIC_RELEASE);
    }

    kprintf("[IRQ] %s answers vector 0x%02x, which nothing routes and nothing "
            "else shares — a message carries it straight to the LAPIC\n",
            name, vector);
    return true;
}

void irq_register_handler(uint8_t irq, irq_callback_t callback, RightOfWay row)
{
    /* Sixteen, because that is how many interrupt request lines an ISA bus
     * has — two cascaded 8259s, eight inputs each, with the second occupying
     * the first's input 2. It is a fact about the bus this function speaks
     * for, and it has nothing to do with how many vectors this kernel can
     * dispatch, which is what the bound here used to be. */
    if (irq >= ISA_INTERRUPT_REQUEST_LINES)
    {
        kprintf("[IRQ] %[E]no handler registered for IRQ %u: an ISA bus has "
                "%u interrupt request lines, numbered 0 to %u%[D]\n",
                irq, (unsigned)ISA_INTERRUPT_REQUEST_LINES,
                (unsigned)ISA_INTERRUPT_REQUEST_LINES - 1);
        return;
    }

    Summons line = SummonsForIsaLine(irq);
    line.row = row;

    /* Settle FIRST, and take the built-in node from the record the settling
     * chose.
     *
     * This used to hand SummonsAnswer `irq_lines[irq].builtin` — the record
     * indexed by the ISA NUMBER — while SummonsAnswer splices that node onto
     * the chain of the record at the SETTLED VECTOR. Those were the same
     * record only for as long as a vector was 32 + its ISA number. The moment
     * a line's vector comes from anywhere else, the node lives in one record
     * and hangs off another: a second registration for the same number does
     * not find it in the duplicate check, links node->next to itself, and
     * irq_handler walks that chain with interrupts disabled. */
    SummonsResolve(&line);
    if (!SummonsSettleOnPin(&line)) return;

    SummonsAnswer(&line, &irq_lines[line.vector].builtin, callback, NULL);
}

static process_t *find_process_by_kernel_stack_overflow(uint64_t rsp)
{
    // Called from IST exception handlers (double fault / stack fault).
    // process_lock may be held by the interrupted code, so use trylock
    // to avoid deadlock. If we can't get the lock, fall back to unlocked
    // iteration — best-effort crash recovery.
    bool locked = spin_trylock_process_list();

    process_t *proc = process_get_first();
    while (proc)
    {
        if (proc->magic == PROCESS_MAGIC && proc->kernel_stack_guard_base)
        {
            uintptr_t guard_start = (uintptr_t)proc->kernel_stack_guard_base;
            uintptr_t guard_end = guard_start + (CONFIG_KERNEL_STACK_GUARD_PAGES * CONFIG_PAGE_SIZE);
            if (rsp >= guard_start && rsp < guard_end)
            {
                if (locked)
                    process_list_unlock();
                return proc;
            }
        }
        proc = proc->next;
    }

    if (locked)
        process_list_unlock();
    return NULL;
}

/* Friendly mnemonic for the first 32 Intel exception vectors (SDM Vol 3
 * §6.15 Table 6-1). Returned strings are static literals; safe to embed
 * in kprintf without lifetime concerns. Vector >= 32 (= IRQ) returns
 * "INT" — exception_handler is only called for the 0-31 range, but the
 * defensive branch keeps the helper total. */
static const char *exception_mnemonic(uint8_t vector)
{
    switch (vector)
    {
    case 0:
        return "#DE";
    case 1:
        return "#DB";
    case 2:
        return "NMI";
    case 3:
        return "#BP";
    case 4:
        return "#OF";
    case 5:
        return "#BR";
    case 6:
        return "#UD";
    case 7:
        return "#NM";
    case 8:
        return "#DF";
    case 10:
        return "#TS";
    case 11:
        return "#NP";
    case 12:
        return "#SS";
    case 13:
        return "#GP";
    case 14:
        return "#PF";
    case 16:
        return "#MF";
    /* #AC (vector 17) — Intel SDM Vol 3 §6.15 Table 6-1. Two distinct
     * triggers reach here: (1) classical alignment-check from a
     * misaligned user-mode memory access while CR0.AM=1 and EFLAGS.AC=1,
     * and (2) split-lock detection when IA32_CORE_CAPABILITIES.bit5 +
     * TEST_CTL.bit29 are both set and a LOCK-prefixed instruction spans
     * a cache line. BoxOS clears TEST_CTL.bit29 at every BSP/AP bringup
     * (see cpu_test_ctl_init in cpuid.c), so #AC reaching this handler
     * indicates either a misconfigured boot path, a hostile guest VM
     * lying about CPUID, or a real classical AC — all worth a clear
     * diagnostic instead of an anonymous "Exception #17". */
    case 17:
        return "#AC";
    case 18:
        return "#MC";
    case 19:
        return "#XF";
    case 20:
        return "#VE";
    case 21:
        return "#CP";
    default:
        return "INT";
    }
}

#ifdef CONFIG_BRINGUP_HOLD_ON_FIRST_FAULT
/*
 * Bring-up hold. Off in every normal build; `make BRINGUP=on` turns it on.
 *
 * The user-mode path below is right for a running system: a process faults,
 * the kernel kills it and schedules the next one, and the machine survives.
 * On the first boot of a new machine it is exactly wrong. The next process
 * faults too, the screen scrolls, and the one dump that mattered — the FIRST
 * fault, the one with the cause still in CR2 and the stack not yet unwound —
 * is gone before anyone can read it. There is no scrollback on a bare screen
 * and, until the serial line is wired, nowhere for it to have gone.
 *
 * So: print the first fault in full, then stop. Every core that faults after
 * the hold is set stops without printing a word, because a second core's
 * dump would scroll the first one away just as effectively as a loop.
 */
static volatile uint32_t g_bringup_held = 0;

static void bringup_hold_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
#endif

/*
 * panic_probe_present — is this virtual address backed by a present page?
 *
 * Walked straight off CR3, level by level, exactly as the hardware would. It
 * deliberately consults nothing the kernel maintains — not the VMM's context
 * structures, not a cached translation, not a bitmap — because everything the
 * kernel maintains is on the list of things that might have caused the panic
 * this is being asked during. CR3 and the page tables it points at are the one
 * description of memory the CPU itself is already obeying.
 *
 * The only assumption left is that the direct map is intact, which is what
 * makes reading the tables possible at all. That is a far smaller surface than
 * dereferencing an arbitrary saved frame pointer and hoping.
 */
static bool panic_probe_present(uint64_t va)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    const uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;
    const uint64_t P = 1ULL << 0;
    const uint64_t PS = 1ULL << 7; /* 1 GiB / 2 MiB leaf */

    uint64_t *table = (uint64_t *)vmm_phys_to_virt(cr3 & ADDR_MASK);
    if (!table)
        return false;

    for (int level = 3; level >= 0; level--)
    {
        uint64_t entry = table[(va >> (12 + level * 9)) & 0x1FF];
        if (!(entry & P))
            return false;
        if (level > 0 && (entry & PS))
            return true; /* large page, and it is present */
        table = (uint64_t *)vmm_phys_to_virt(entry & ADDR_MASK);
        if (!table)
            return false;
    }
    return true;
}

/* The kernel's own Nameplate, linked in by the second pass — see the linker
 * script and the Makefile. Riveted to the image, so naming a frame costs no
 * disk, no lock and no matching kernel.elf at the other end of the world;
 * which is the situation a panic on a machine you are not sitting at leaves
 * you in, and the situation this table exists for. */
extern const char __nameplate_start[];
extern const char __nameplate_end[];

/* Print an address, and its name when the image can supply one.
 *
 * `for_return` turns a return address into the byte that belongs to the call
 * before naming it: the saved address points PAST the call, so naming it
 * directly names the NEXT function whenever a call is the last instruction of
 * its own. The address printed is still the one that was saved, because that
 * is what a reader compares against a disassembly. */
static void panic_name_addr(const char *prefix, uint64_t addr, bool for_return)
{
    NameplateName site;
    uint64_t bytes = (uint64_t)(__nameplate_end - __nameplate_start);
    uintptr_t look = (uintptr_t)(for_return && addr ? addr - 1 : addr);

    if (bytes >= sizeof(NameplateHeader) &&
        nameplate_name_at_kernel((uintptr_t)__nameplate_start, bytes, look, &site))
        kprintf("%s%016lx  %s+0x%lx\n", prefix, addr, site.Text,
                site.Offset + (for_return ? 1u : 0u));
    else
        kprintf("%s%016lx\n", prefix, addr);
}

/* Readable across `len` bytes, page boundary included. Two probes cover every
 * length this file asks about; nothing here reads more than sixteen bytes. */
static bool panic_probe_range(uint64_t va, uint64_t len)
{
    return panic_probe_present(va) &&
           panic_probe_present(va + len - 1);
}

/* The faulting address, named against the firmware's own memory map
 * (UEFI 2.10 §7.2).
 *
 * A ring-0 #PF inside a firmware call reports a CR2 that appears in no table
 * this kernel keeps, and the map is the one description left that can claim
 * it. Three answers, and they must be told apart: a descriptor carrying
 * EFI_MEMORY_RUNTIME (UEFI 2.10 §7.2 Table 7.10 — firmware is entitled to
 * this memory after ExitBootServices, so the fault is in how WE mapped it),
 * a descriptor without it (firmware reached memory it gave back), and no
 * descriptor at all (outside everything the firmware described).
 *
 * Safe from a fault handler: EfiDescribePhysical walks the boot copy of the
 * map through the Pull Map and takes no lock and no allocation to do it
 * (efi_runtime.c). The "no descriptor" line is gated on
 * efi_runtime_available(), which efi_runtime_init sets strictly after it
 * records the map, so reaching that branch proves there was a map to walk —
 * a BIOS boot, and a fault raised before the map exists, say nothing.
 *
 * ‼ CR2 IS LINEAR AND THE MAP IS PHYSICAL. Intel SDM Vol 3A §4.7 gives CR2
 * the linear address of the access; the descriptors carry physical_start. The
 * two are the same number only where this kernel installed a VA==PA window,
 * which exists below the top of the low canonical half — 2^47 under 4-level
 * paging (SDM Vol 3A §4.5). Above it the number is a virtual address of ours
 * and asking a physical map about it would answer about a page nobody
 * touched, so the map is not asked and the line says why. The caller has
 * already established that this is a page fault; on any other vector CR2
 * holds an earlier fault's address and must not reach here. */
#define PANIC_IDENTITY_VA_MAX  0x0000800000000000ULL

static void PanicNameFaultAddress(uint64_t addr)
{
    if (addr >= PANIC_IDENTITY_VA_MAX)
    {
        if (efi_runtime_available())
            kprintf("  CR2 is a kernel-half LINEAR address; the EFI memory map "
                    "answers about PHYSICAL ones and is not asked here\n");
        return;
    }

    EfiDescriptorFacts d;

    if (EfiDescribePhysical(addr, &d))
    {
        kprintf("  CR2 (linear == physical in the identity window) is EFI "
                "descriptor [%u] %s phys=%016lx pages=%lu "
                "virt=%016lx attr=%016lx  %s\n",
                d.index, EfiMemoryTypeName(d.type), d.physical_start,
                d.number_of_pages, d.virtual_start, d.attribute,
                (d.attribute & EFI_MEMORY_RUNTIME)
                    ? "RUNTIME — firmware keeps this across ExitBootServices"
                    : "no RUNTIME attribute — firmware reached memory it gave back");
    }
    else if (efi_runtime_available())
    {
        kprintf("  CR2 falls inside no EFI memory descriptor\n");
    }
}

/* Port 0x61 — System Control Port B, the legacy chipset's NMI status and
 * control latch. Bits 7 and 6 are READ-ONLY status and stay asserted until
 * the matching disable bit is written 1, which both clears the latch and
 * turns that source off. The low nibble is the writable half, and two of its
 * bits are not ours to invent: bit 0 is the PIT channel-2 gate and bit 1 the
 * speaker enable, both driven elsewhere in this kernel. So the clear writes
 * back the nibble it just read with one bit added, never a constant.
 *
 * Values match the reference every PC chipset is built against — Linux
 * arch/x86/include/asm/mach_traps.h (NMI_REASON_*). */
#define NMI_REASON_PORT        0x61
#define NMI_REASON_SERR        0x80 /* PCI SERR# latched  (read-only status) */
#define NMI_REASON_IOCHK       0x40 /* IOCHK# latched     (read-only status) */
#define NMI_REASON_CLEAR_MASK  0x0F /* the writable half, preserved verbatim */
#define NMI_REASON_CLEAR_SERR  0x04 /* write 1: clear + disable SERR# NMI    */
#define NMI_REASON_CLEAR_IOCHK 0x08 /* write 1: clear + disable IOCHK# NMI   */
#define NMI_REASON_FLOATING    0xFF /* nothing answering the port at all     */

/*
 * How loud an NMI is allowed to be.
 *
 * kprintf takes a non-recursive console lock with interrupts off. An NMI that
 * lands in the middle of this core's own kprintf and then calls kprintf hangs
 * that core for good, and an NMI source that re-fires — a chassis pin nothing
 * de-asserts, a BMC watchdog held down, a board that does not answer port
 * 0x61 — offers the machine that dice roll again on every single delivery.
 * An NMI handler that prints once per delivery is therefore not a handler
 * with a cosmetic problem; it is a handler that turns a repeating source into
 * a hang.
 *
 * So a core describes each KIND of NMI once and stays silent about that kind
 * afterwards. Deliberately not a budget of N lines and not a time window:
 * both are numbers picked out of the air, and both still allow an unbounded
 * count of console-lock acquisitions over a long enough run. A kind this core
 * has never seen still gets its full line — a new cause is always worth the
 * risk once — and a kind that repeats gets nothing.
 *
 * What replaces the repeated lines is the count. Every NMI a core answers is
 * counted, and whichever line does get printed carries that number, so the
 * operator reads the rate off the message instead of off the flood.
 *
 * The state is per core and touched only by its own core — an NMI cannot
 * migrate mid-handler — so the counter needs no atomic and the record needs
 * no lock, which is what makes it usable from NMI context at all.
 * amp_get_core_index returns uint8_t, so the index cannot leave the array.
 */
#define NMI_KIND_SERR      0x01 /* PCI SERR# latch, port 0x61 bit 7  */
#define NMI_KIND_IOCHK     0x02 /* IOCHK# latch, port 0x61 bit 6     */
#define NMI_KIND_UNCLAIMED 0x04 /* nobody owned it                   */

typedef struct NmiVoice
{
    uint64_t answered; /* every NMI this core has taken               */
    uint8_t  said;     /* NMI_KIND_* this core has already described  */
} NmiVoice;

static NmiVoice g_nmi_voice[MAX_CORES];

/* How deep this core is in its NMI or machine-check handler (IdtNmiOrMcHere).
 * Written by its own core only, so no atomic; the two nest inside each other
 * — a #MC inside an NMI, an NMI inside a #MC on its own IST stack — so it
 * counts rather than flags. */
static uint8_t g_nmi_or_mc_here[MAX_CORES];

bool IdtNmiOrMcHere(void)
{
    return g_nmi_or_mc_here[amp_get_core_index()] != 0;
}

/* True exactly once per core per kind — and it closes that kind's mouth in
 * the same breath in which it answers. */
static bool nmi_first_of_kind(NmiVoice *voice, uint8_t kind)
{
    if (voice->said & kind)
        return false;
    voice->said |= kind;
    return true;
}

/*
 * NMI (vector 2) — answered here, and returned from. It never falls through.
 *
 * An NMI is not a fault. Nothing about the interrupted instruction is wrong,
 * there is no faulting address to name, and the interrupted process did
 * nothing. It is the platform speaking: firmware-first error reporting (ACPI
 * 6.5 Table 18.14 — notification type 4 is NMI; §18.4.1: "the OSPM NMI handler
 * scans the list of generic error sources to find the error source that
 * reported the error and processes the error report", and then carries on), or
 * a platform NMI pin (ACPI 6.5 §5.2.12.7 Local APIC NMI and §5.2.12.6 NMI
 * Source — the watchdogs, chassis alerts and BMC diagnostic buttons that
 * irqchip_init programs onto LINT0/LINT1 and the IOAPIC, and that every AP
 * repeats), or a legacy chipset SERR#/IOCHK latch. Not one of those is a
 * reason to kill the interrupted process or to halt the machine — and until
 * this function existed, every one of them did exactly that: vector 2 read the
 * APEI sources, threw the answer away, and walked on into the ring-3 kill
 * branch or the kernel-panic block below.
 *
 * It is also why vector 2 must return from HERE and from nothing further down.
 * NMIs are blocked only until the next IRET, not until the IRET that ends this
 * handler (SDM Vol 2, IRET: "execution of the IRET instruction unblocks NMIs",
 * and "this unblocking occurs even if the instruction causes a fault"; Vol 3A,
 * "Handling Multiple NMIs"). The ring-3 dump path below walks the user RBP
 * chain with get_user_u64 and reads the faulting process's Nameplate — loads
 * that are MEANT to fault and be recovered by the uaccess fixup, and that
 * recovery returns by IRET. One such fixup taken inside NMI context unblocks
 * NMIs for the whole remainder of the handler; vector 2 is an IST vector
 * (IST_NMI, idt_init), so the next NMI reloads RSP from that same fixed
 * TSS.IST2 pointer and pushes its frame over the live one, and the first
 * handler then returns through a frame that is no longer its own. So this
 * handler touches no user pointer, walks no table, takes no scheduler
 * decision, and returns.
 *
 * The one thing left in here that can still block is the console lock, and it
 * is why nothing below prints twice for the same reason — see NmiVoice.
 */
static void handle_nmi(interrupt_frame_t *frame)
{
    uint32_t  core  = (uint32_t)amp_get_core_index();
    NmiVoice *voice = &g_nmi_voice[core];
    uint64_t  count = ++voice->answered;

    /* Firmware first, and silent. apei_ghes_nmi_check reads each NMI-notify
     * GHES source's Error Status Address register — mapped once at
     * registration, so the read can neither fault nor take a lock — and
     * knocks a K-Core for a source that holds a record nobody has consumed,
     * or whose register names a block this kernel has no window for. A
     * source whose block IS mapped and whose Block Status reads zero is
     * passed over untouched, which is what keeps the unclaimed branch below
     * reachable at all. True means a source owned this NMI; the K-Core that
     * was knocked is what reports the error, at a moment when printing is
     * allowed. */
    extern bool apei_ghes_nmi_check(void);
    if (apei_ghes_nmi_check())
        return;

    /* Only an NMI nobody owned reaches the chipset latch, which is also what
     * Linux's default_do_nmi does and for the same reason: BoxOS never clears
     * port 0x61 at boot, so a SERR#/IOCHK bit firmware left set before the
     * kernel existed would otherwise be read during the first unrelated GHES
     * NMI, announced as a live bus error that is not happening, and that
     * source disabled for the rest of the run on the strength of it. The
     * latch belongs to the delivery that has no other explanation. */
    uint8_t reason = inb(NMI_REASON_PORT);

    /* 0xFF is a floating bus, not a status. That byte says bits 7 and 6 are
     * latched while bits 2 and 3 of the very same byte assert the disables
     * that hold exactly those two bits at zero — a state the port's own
     * definition forbids. So nothing is answering at 0x61 on this board, and
     * there is no legacy latch to clear. Costs nothing if some chipset ever
     * does return it honestly: the unclaimed line below prints the raw byte
     * either way. */
    uint8_t latched = (reason == NMI_REASON_FLOATING)
                          ? 0
                          : (uint8_t)(reason & (NMI_REASON_SERR | NMI_REASON_IOCHK));

    if (latched)
    {
        uint8_t ctrl = (uint8_t)(reason & NMI_REASON_CLEAR_MASK);
        if (latched & NMI_REASON_SERR)
            ctrl |= NMI_REASON_CLEAR_SERR;
        if (latched & NMI_REASON_IOCHK)
            ctrl |= NMI_REASON_CLEAR_IOCHK;

        /* Silence the hardware before saying a word about it: the latch is
         * level-held and re-delivers for as long as the machine runs. */
        outb(NMI_REASON_PORT, ctrl);

        /* Then ASK the port whether that worked, instead of announcing that
         * it did. This is a read-modify-write of a byte two other subsystems
         * also read-modify-write: amp_udelay's PIT gate path (amp.c caches
         * `tmp = inb(0x61)`, issues four other port writes, then writes that
         * stale byte back twice) and hourglass's pit2_start. An NMI landing
         * inside one of those windows has its disable bit overwritten by the
         * cached byte and the source comes back armed. Serializing it means a
         * lock both sides take — amp.c and hourglass.c, not this file — and a
         * lock acquired from NMI context is a hazard of its own. What this
         * file can do is not lie: the status is read back and the line reports
         * what the port actually says. A clear that lost the race needs no
         * retry loop here, because the source that survived it will deliver
         * the NMI again and be cleared again. */
        uint8_t after = inb(NMI_REASON_PORT);
        bool    spoke = false;

        if ((latched & NMI_REASON_SERR) && nmi_first_of_kind(voice, NMI_KIND_SERR))
        {
            kprintf("[NMI] Core %u: PCI SERR# asserted — a device reported a "
                    "system error (port 0x61 = 0x%02x, NMI #%lu on this core).\n",
                    core, (unsigned)reason, count);
            spoke = true;
        }
        if ((latched & NMI_REASON_IOCHK) && nmi_first_of_kind(voice, NMI_KIND_IOCHK))
        {
            kprintf("[NMI] Core %u: IOCHK# asserted — a bus device raised "
                    "channel check (port 0x61 = 0x%02x, NMI #%lu on this core).\n",
                    core, (unsigned)reason, count);
            spoke = true;
        }

        if (spoke && (after & latched) == 0)
        {
            /* Re-arming means driving the disable low again once the device
             * has stopped asserting, and the only way to learn that is to
             * wait — which is not something an NMI handler may do. So the
             * source stays off, and that is said out loud rather than left
             * for the operator to discover. */
            kprintf("[NMI] Core %u: source cleared and left DISABLED (port "
                    "0x61 <- 0x%02x, reads back 0x%02x). The machine "
                    "continues; this core reports each NMI cause once.\n",
                    core, (unsigned)ctrl, (unsigned)after);
        }
        else if (spoke)
        {
            kprintf("[NMI] Core %u: the clear did NOT take (port 0x61 <- "
                    "0x%02x, still reads 0x%02x) — the device is holding the "
                    "line, or another port 0x61 writer overwrote the disable. "
                    "The NMI will be delivered again and cleared again.\n",
                    core, (unsigned)ctrl, (unsigned)after);
        }
        return;
    }

    if (nmi_first_of_kind(voice, NMI_KIND_UNCLAIMED))
    {
        /* Nobody owned it, and that is one line of fact, not a crash: a
         * watchdog, an IPMI diagnostic interrupt, a performance-counter
         * overflow, or a GHES source whose status block another core drained
         * before this one looked. Once per core — an unclaimed source is the
         * one kind here that nothing in this handler can switch off, so it is
         * also the one kind that could otherwise print forever. */
        kprintf("[NMI] Core %u: unclaimed — no APEI source and no legacy latch "
                "(port 0x61 = 0x%02x). RIP=0x%lx CS=0x%lx, NMI #%lu on this "
                "core. Nothing to kill and nothing to halt; continuing, and "
                "counting the rest in silence.\n",
                core, (unsigned)reason, frame->rip, frame->cs, count);
    }
}

/* Phase 2K — #CP (vector 21) Control-Protection Exception. Fires
 * when a shadow-stack mismatch or IBT violation occurs. Error code
 * bits (Intel SDM Vol 3D §17.7):
 *   bits 14:0  — CP error type:
 *     1 = NEAR_RET     shadow-stack mismatch on RET
 *     2 = FAR_RET_IRET shadow-stack mismatch on FAR RET/IRET
 *     3 = ENDBRANCH    indirect branch missing ENDBR target
 *     4 = RSTORSSP     RSTORSSP token validation failed
 *     5 = SETSSBSY     SETSSBSY token validation failed
 *   bit 15     — ENCL  set when fault is in SGX enclave
 *
 * Logged and published to Touch; the caller then falls through to the
 * generic kill-process path. Recovery is the future per-process CET
 * lifecycle's job. */
static void ExceptionCpReport(const interrupt_frame_t *frame)
{
    uint16_t cp_type = (uint16_t)(frame->error_code & 0x7FFFu);
    bool cp_enclave = (frame->error_code & 0x8000u) != 0;
    kprintf("[CET] #CP fired: type=%u enclave=%d RIP=0x%lx\n",
            (unsigned)cp_type, (int)cp_enclave, frame->rip);
    if ((frame->cs & 3) == 0)
        panic_name_addr("[CET]   in ", frame->rip, false);
    /* Touch publish via pre-resolved tag (resolved in vmm_cet_probe
     * at BSP boot, outside IRQ context). TouchLogbookIntern in this
     * handler would take registry locks → deadlock against any
     * thread holding the Touch lock at the moment #CP fired. */
    TouchTag cp_tag = vmm_get_cet_cp_tag();
    struct
    {
        uint64_t rip;
        uint16_t cp_type;
        uint8_t enclave;
        uint8_t pad;
    } ev = {
        .rip = frame->rip,
        .cp_type = cp_type,
        .enclave = cp_enclave ? 1u : 0u,
        .pad = 0,
    };
    if (cp_tag != TOUCH_TAG_INVALID)
    {
        TouchPublishIrqPair(cp_tag, TOUCH_TAG_INVALID,
                            &ev, (uint16_t)sizeof(ev), 0u, 0u);
    }
    /* Bump the cet_lifecycle stat counter (RELAXED — never load-
     * bearing). Visible via `hw cet status`. */
    {
        extern void cet_lifecycle_record_cp_fault(void);
        cet_lifecycle_record_cp_fault();
    }
}

/* How the program fell: the first exception any of its strands takes, named
 * from the program's own Nameplate while its pages are the ones mapped, kept
 * on its cabin for every death of it to say (process:died). A second strand
 * that falls while the first is being written finds the record taken. */
static void exception_record_fall(process_t *proc, uint8_t vector, uintptr_t rip)
{
    cabin_t *cabin = proc->cabin;
    if (!cabin || __atomic_exchange_n(&cabin->fell_claimed, 1u, __ATOMIC_ACQ_REL) != 0)
        return;
    cabin->fell_vector = vector;
    cabin->fell_offset = 0;
    cabin->fell_in[0]  = '\0';
    NameplateName here;
    if (proc->nameplate_va && proc->nameplate_bytes &&
        nameplate_name_at(proc->nameplate_va, proc->nameplate_bytes, proc->image_base, rip, &here)) {
        size_t length = strlen(here.Text);
        if (length >= sizeof(cabin->fell_in)) length = sizeof(cabin->fell_in) - 1;
        memcpy(cabin->fell_in, here.Text, length);
        cabin->fell_in[length] = '\0';
        cabin->fell_offset     = here.Offset > UINT32_MAX ? UINT32_MAX : (uint32_t)here.Offset;
    }
    __atomic_store_n(&cabin->fell_said, 1u, __ATOMIC_RELEASE);
}


void exception_handler(interrupt_frame_t *frame)
{
    /* Scoped to THIS invocation. A kernel #PF claims the dump at the [VMM]
     * line, and a fatal kernel #MC before its record (mce.c), and each then
     * walks on into the panic block below, which claims again; without a
     * per-invocation record the second claim sees the flag its own first
     * claim set and halts the dump it was about to print. A re-entrant fault
     * gets a fresh handler frame and a fresh false: on another core it stops
     * at the claim without a word, and on the core whose dump it broke it
     * ends that dump where it broke off, its own account one line of its
     * vector, error code and RIP — or, the dump said whole, that line saying
     * it came after it (klib_panic.c). */
    bool panic_claimed_here = false;

    /*
     * ‼ CR2 IS READ HERE, FIRST, AND NOWHERE ELSE.
     *
     * CR2 holds the address that caused the LAST page fault on this core, and
     * nothing preserves it: the next #PF overwrites it. That makes "read it
     * when you happen to need it" wrong, and it was wrong — the user-mode dump
     * below read it AFTER nameplate_name_at, which walks the faulting
     * process's own symbol table through get_user_u32. That helper is built on
     * a .uaccess_fixup entry: the load really does fault and the handler
     * really does recover it, so a process whose nameplate is not fully mapped
     * — a half-loaded image, say, which is the very case worth dumping —
     * rewrote CR2 with the nameplate's address before the dump printed it. The
     * line then names an address nobody faulted on, which on a board is a
     * morning spent hunting a pointer that does not exist.
     *
     * Nothing can spoil it before this point: the gates are interrupt gates
     * (IDT_TYPE_INTERRUPT_GATE, IF clear), so no interrupt nests here, and the
     * only thing that writes CR2 is a page fault — which, between the CPU
     * delivering this one and this line, could only come from the stub's own
     * pushes onto the kernel stack, and that is a #DF, not a recoverable #PF.
     *
     * ‼ MEANINGFUL ONLY FOR VECTOR 14. For every other vector it is whatever
     * the last page fault on this core left, and every reader of it below —
     * the panic dump, PanicNameFaultAddress, the Lifeline fall record — says
     * so or refuses to speak. A stale address printed as this fault's is not a
     * near miss on a board: it gets resolved against the EFI memory map and
     * names a region nothing touched.
     */
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    /* A panic's stop before anything is counted. The core that holds a panic
     * stops every other core by an NMI, having published its claim before the
     * first was sent, so an NMI that finds another core's claim is that stop,
     * and the core stands here for good: nothing counted, printed or taken,
     * no user pointer, and no IRET — so no NMI after it either. Counted, the
     * stop would put one exception per stopped core into the dump's "Exception
     * count", a cascade that never was. Without a claim, or on the core that
     * holds it, the NMI is the platform's and goes on below exactly as before.
     * klib_panic.c says why an NMI and what one meets on a core already
     * standing, or inside another IST handler. */
    if (frame->vector == 2)
        PanicStandIfClaimedElsewhere();

    atomic_fetch_add_u64(&exception_count, 1);

#ifdef CONFIG_BRINGUP_HOLD_ON_FIRST_FAULT
    if (g_bringup_held)
        bringup_hold_forever(); /* the screen already holds the fault that matters */
#endif

    /* NMI (vector 2) is answered in full by handle_nmi and returns from here.
     * Nothing below this line may run in NMI context — see the block in front
     * of handle_nmi for why the return is the whole point. */
    if (frame->vector == 2)
    {
        uint8_t core = amp_get_core_index();
        g_nmi_or_mc_here[core]++;
        handle_nmi(frame);
        g_nmi_or_mc_here[core]--;
        return;
    }

    /* Phase 2F — #MC (vector 18) routes to the MCE subsystem. The handler
     * runs on the IST_MACHINE_CHECK stack (set by idt_init), walks every
     * IA32_MC<i>_STATUS bank, poisons phys pages and publishes Touch events.
     *
     * It returns true — this handler IRET-restarts — only when MCG_STATUS.MCIP
     * and MCG_STATUS.RIPV are both 1 AND no bank record it decoded was fatal;
     * the full contract is in mce.h above mce_handle. An EMPTY bank walk is
     * deliberately not a verdict of its own any more: without LMCE a #MC is
     * broadcast to every logical processor and only one wins the shared bank,
     * so an empty walk is the normal answer on every core but one, and it is
     * judged by RIPV alone. False means fall through to the standard
     * kill-process (ring 3) / panic-and-halt (ring 0) path below. */
    if (frame->vector == 18)
    {
        extern bool mce_handle(interrupt_frame_t *, bool *panic_claimed_here);
        uint8_t core = amp_get_core_index();
        g_nmi_or_mc_here[core]++;
        bool recovered = mce_handle(frame, &panic_claimed_here);
        g_nmi_or_mc_here[core]--;
        if (recovered)
        {
            return; /* recovered — IRET back to user/kernel */
        }
        /* fatal — drop into the generic exception path. (cs ring tells
         * exception_handler whether to kill the process or halt.) */
    }

    /* #CP said and published (ExceptionCpReport), then killed or panicked on
     * below — not on the owner of a panic: there it is a fault inside the
     * account, said by one line of numbers and nothing named (klib_panic.c). */
    if (frame->vector == 21 && !PanicOwnedHere())
        ExceptionCpReport(frame);

    if (frame->vector == 14)
    {
        /* uaccess fixup — kernel-mode #PF whose RIP lies inside a
         * copy_to/from_user / put_user / get_user inline asm region
         * gets handled here, BEFORE the demand-paging path runs. The
         * fixup table maps fault_rip → recovery_rip; we set
         * frame->rip to the recovery label and IRET. The recovery
         * code path executes CLAC + returns an error from the
         * affected helper.
         *
         * Why before vmm_handle_page_fault: that path can mutate
         * page-table state (demand-page-in, COW, etc.) which would
         * be the wrong response for a SMAP-blocked user pointer.
         * The fixup is the correct response: the user pointer is
         * bad (or its page got unmapped under us), bubble the
         * error to the syscall caller. */
        if ((frame->cs & 3) == 0)
        {
            extern uintptr_t uaccess_lookup_fixup(uintptr_t fault_rip);
            uintptr_t fixup_rip = uaccess_lookup_fixup(frame->rip);
            if (fixup_rip != 0)
            {
                frame->rip = fixup_rip;
                return;
            }
        }

        int result = vmm_handle_page_fault(fault_addr, frame->error_code);
        if (result == 0)
        {
            return;
        }
    }

    /* ‼ LIFELINE — a fault inside code this kernel did not compile.
     *
     * Asked HERE and nowhere else, and the position is the whole of its
     * safety. Later than the uaccess fixup and later than
     * vmm_handle_page_fault, because an EFI runtime region is demand-pageable
     * like any other and a lifeline asked first would refuse a fault the VMM
     * was about to answer correctly. Earlier than the panic's claim, because
     * a claimed panic ends in `cli; hlt` and there is no returning from it.
     * Outside the vector-14 block, because firmware answers a bad descriptor
     * or a forbidden MSR with #GP far more often than with #PF.
     *
     * LifelineCatch refuses unless the faulting RIP is inside a range the
     * caller named as foreign, so a kernel bug raised inside the armed window
     * still walks on into the panic below. See lifeline.h.
     *
     * The CR2 handed over is the one read at the top of this function; the
     * note exists so the catch path does not become a second reader of a
     * register the next fault overwrites. */
    LifelineNoteFaultAddress(fault_addr);
    if (LifelineCatch(frame))
        return;

    if (frame->vector == 14)
    {
        /* Unhandled ring-0 #PF — fall through to KERNEL PANIC.
         *
         * Previously this branch logged + returned, which meant iretq
         * resumed at the faulting RIP and immediately re-faulted. Under
         * stress (UEFI 16c write_stress) the retry loop was indistinguishable
         * from a silent system hang. Production demands fail-loud over
         * fail-silent: every unhandled kernel #PF is a real bug (missing
         * uaccess fixup, wild pointer, stack overflow past the guard) and
         * must surface with full diagnostic context, not livelock.
         *
         * SMAP-candidate hint: ring-0 #PF on a user-half VA with P=1 is
         * the canonical SMAP signature. The fixup table already missed
         * (else we'd have redirected RIP earlier), so this is a forgotten
         * STAC/CLAC bracket — adjacent kernel code dereferenced a user-
         * mapped page with RFLAGS.AC=0 and CR4.SMAP=1. The panic block
         * below dumps RFLAGS so the operator can read the AC bit. */
        if ((frame->cs & 3) == 0)
        {
            /* This line is the first thing an unhandled ring-0 #PF prints, so
             * the claim belongs here rather than at the panic banner below —
             * otherwise a fault raised while printing the dump announces
             * itself once before being silenced, and the screen ends with a
             * second address that is a consequence, not a cause. */
            PanicClaimForException(&panic_claimed_here, frame, fault_addr);

            /* RSVD (bit 3) is asked FIRST, and it silences the SMAP claim.
             *
             * A reserved-bit fault and a SMAP fault look alike to the old
             * test — both have P=1, both can land on a low address — and the
             * old test only asked those two things, so it called every RSVD
             * fault a SMAP candidate. That is not a near miss: it names the
             * wrong subsystem, and it named it on both real machines BoxOS
             * has booted, where the actual cause was bit 63 in a PTE while
             * IA32_EFER.NXE was still 0 (Intel SDM Vol 3A §4.5 — bit 63 is
             * RESERVED until NXE is set; Table 4-15 for the error code).
             * A fault BoxOS cannot have any more, but a diagnostic that
             * points away from the truth is worth removing regardless. */
            bool is_rsvd = (frame->error_code & 0x8ULL) != 0;
            bool is_smap_candidate = !is_rsvd &&
                                     (frame->error_code & 0x1ULL) &&
                                     fault_addr < UACCESS_USER_VA_MAX;
            kprintf("\n[VMM] Unhandled kernel #PF at 0x%lx err=0x%lx%s%s\n",
                    fault_addr, frame->error_code,
                    is_rsvd ? "  [reserved bit set in a paging entry]" : "",
                    is_smap_candidate ? "  [SMAP candidate]" : "");
            if (is_rsvd)
            {
                /* Read EFER here rather than guess: whether NXE is on is the
                 * single question that separates "bit 63 was illegal" from
                 * every other reserved-bit cause, and the operator cannot
                 * read an MSR off a halted screen. */
                uint32_t nxe_lo, nxe_hi;
                __asm__ volatile("rdmsr" : "=a"(nxe_lo), "=d"(nxe_hi)
                                         : "c"(0xC0000080u));
                bool nxe = (nxe_lo & (1u << 11)) != 0;
                kprintf("[VMM]   A paging-structure entry on the way to this "
                        "address has a reserved bit set.\n"
                        "[VMM]   IA32_EFER.NXE=%u — %s\n",
                        (unsigned)nxe,
                        nxe
                          ? "bit 63 is legal here, so look at bits above "
                            "MAXPHYADDR or at PS/PAT in a non-leaf entry"
                          : "bit 63 (NX) is RESERVED with NXE off, and an "
                            "entry carrying it faults on ANY access");
            }
            if (is_smap_candidate)
            {
                kprintf("[VMM]   Kernel dereferenced user-mapped page via "
                        "RIP 0x%lx without STAC/CLAC bracket.\n"
                        "[VMM]   Wrap the access in copy_to/from_user / "
                        "put_user / get_user, OR reach it through "
                        "a porthole (PortholeOpen).\n",
                        frame->rip);
            }
            /* Fall through to the kernel-panic block at the bottom of
             * exception_handler — full GPR + stack trace + halt-all-cores. */
        }
    }

    // User-mode exception: kill the faulting process, don't crash the kernel
    if ((frame->cs & 3) == 3)
    {
        process_t *proc = process_get_current();
        if (proc)
            exception_record_fall(proc, (uint8_t)frame->vector, (uintptr_t)frame->rip);
        if (proc)
        {
            kprintf("\n");
            kprintf("[EXCEPTION] User-mode %s (vector %u) in PID %u\n",
                    exception_mnemonic((uint8_t)frame->vector),
                    frame->vector, proc->pid);
            kprintf("[EXCEPTION] RIP=0x%lx RSP=0x%lx Error=0x%lx\n",
                    frame->rip, frame->rsp, frame->error_code);
            /* Named from the process's own table, not the kernel's: the
             * address belongs to its image, and the backtrace below already
             * reads that table. Naming the faulting instruction while naming
             * everything that called it was an odd place to stop. */
            {
                NameplateName here;
                if (proc->nameplate_va && proc->nameplate_bytes &&
                    nameplate_name_at(proc->nameplate_va, proc->nameplate_bytes, proc->image_base,
                                      (uintptr_t)frame->rip, &here))
                    kprintf("[EXCEPTION]   in %s+0x%lx\n", here.Text, here.Offset);
            }
            kprintf("[EXCEPTION] TagBits: 0x%lx\n", proc->cabin ? proc->cabin->tag_bits : 0);
            /* Split-lock #AC hint: if userspace fired #AC with error_code==0
             * while BoxOS was supposed to clear TEST_CTL.bit29, the
             * configuration drifted (BIOS re-asserted bit29 mid-runtime
             * is rare but documented). Surface the hint inline so the
             * operator knows what to investigate. */
            if (frame->vector == 17 && frame->error_code == 0)
            {
                kprintf("[EXCEPTION] Note: #AC error_code=0 suggests a "
                        "cache-line-spanning LOCK access; check TEST_CTL "
                        "MSR 0x33 bit 29 state on this core.\n");
            }

            if (frame->vector == 14)
            {
                kprintf("[EXCEPTION] Page Fault at 0x%lx (P=%d W=%d U=%d R=%d)\n",
                        fault_addr,
                        (int)(frame->error_code & 0x1),
                        (int)((frame->error_code >> 1) & 0x1),
                        (int)((frame->error_code >> 2) & 0x1),
                        (int)((frame->error_code >> 3) & 0x1));
                /* Full register dump on user page fault — enables remote
                 * debugging of "wild pointer in r10" / similar memory-
                 * corruption bugs without requiring a connected gdb. */
                kprintf("[EXCEPTION]  RAX=%016lx RBX=%016lx RCX=%016lx\n",
                        frame->rax, frame->rbx, frame->rcx);
                kprintf("[EXCEPTION]  RDX=%016lx RSI=%016lx RDI=%016lx\n",
                        frame->rdx, frame->rsi, frame->rdi);
                kprintf("[EXCEPTION]  RBP=%016lx  R8=%016lx  R9=%016lx\n",
                        frame->rbp, frame->r8, frame->r9);
                kprintf("[EXCEPTION]  R10=%016lx R11=%016lx R12=%016lx\n",
                        frame->r10, frame->r11, frame->r12);
                kprintf("[EXCEPTION]  R13=%016lx R14=%016lx R15=%016lx\n",
                        frame->r13, frame->r14, frame->r15);
            }

            /* Who called the faulting code.
             *
             * The registers above name the instruction that died; they never
             * name the caller, so a fault inside a shared leaf like memcpy or
             * memset says nothing about which call site passed the bad
             * pointer — and those are exactly the faults worth catching.
             *
             * Apps and the shell are compiled without -O, so RBP is a genuine
             * frame pointer: [RBP] is the caller's RBP and [RBP+8] its return
             * address. get_user_u64 reads through the SMAP bracket with
             * page-fault fixup, so a wild RBP — the very thing this dump
             * exists to report — ends the walk with -1 instead of faulting
             * the kernel inside its own exception handler.
             *
             * boxlib, though, is built at -O2 and omits frame pointers, so a
             * fault inside one of its leaves (memcpy, memset, strlen) reports
             * the chain starting at ITS caller's caller — the leaf's immediate
             * caller owns the RBP we start from and therefore names itself
             * only through the call instruction just before the first address
             * printed. Verified against a two-deep probe whose answer was
             * known in advance; do not read the first entry as "the function
             * that faulted".
             *
             * The names come from the image itself. Every BoxOS image carries
             * a Nameplate — a table the linker step builds from its own
             * symbols (src/include/nameplate_format.h) — and the loader noted
             * where it landed. Reading it means reaching into the faulting
             * process's address space, which is done the same way the frame
             * walk itself is done: through get_user, with page-fault fixup, so
             * a process that corrupted its own table gets an unnamed frame
             * rather than taking the kernel down while being diagnosed.
             *
             * A frame with no name is not a failure worth hiding — an address
             * in a leaf no symbol claims, or an image linked without a table,
             * prints as the bare address it always did. */
            {
                uint64_t fp = frame->rbp;
                kprintf("[EXCEPTION]  called from:\n");
                for (int depth = 0; depth < 8; depth++)
                {
                    uint64_t next = 0, ret = 0;
                    NameplateName site;

                    if (fp == 0 || (fp & 7) != 0)
                        break;
                    if (get_user_u64(&next, (const uint64_t *)(uintptr_t)fp) != 0)
                        break;
                    if (get_user_u64(&ret, (const uint64_t *)(uintptr_t)(fp + 8)) != 0)
                        break;
                    if (ret == 0)
                        break;

                    /* The return address points PAST the call, so the byte
                     * that belongs to the call is ret-1; naming ret itself
                     * would name the next function whenever a call is the
                     * last instruction of its own. */
                    if (nameplate_name_at(proc->nameplate_va, proc->nameplate_bytes, proc->image_base,
                                          (uintptr_t)(ret - 1), &site))
                        /* +1 turns the offset of the looked-up byte back into
                         * the offset of the return address, which is what a
                         * reader compares against a disassembly. */
                        kprintf("[EXCEPTION]    0x%lx  %s+0x%lx\n", ret, site.Text,
                                site.Offset + 1);
                    else
                        kprintf("[EXCEPTION]    0x%lx\n", ret);

                    /* Frames march toward higher addresses. A chain that
                     * stalls or reverses is a smashed stack, not a caller. */
                    if (next <= fp)
                        break;
                    fp = next;
                }
            }

#ifdef CONFIG_BRINGUP_HOLD_ON_FIRST_FAULT
            g_bringup_held = 1;
            kprintf("\n");
            kprintf("====================================================================\n");
            kprintf("FIRST FAULT — HELD FOR BRING-UP. Everything above is the whole of it:\n");
            kprintf("the faulting address in CR2, every register, and the call chain by\n");
            kprintf("name. Nothing further will print. Photograph this screen.\n");
            kprintf("(Normal builds kill the process and carry on; this is `make BRINGUP=on`.)\n");
            kprintf("====================================================================\n");
            bringup_hold_forever();
#endif

            kprintf("[EXCEPTION] Killing PID %u and scheduling next process\n", proc->pid);

            // Mark process as crashed so scheduler won't pick it again
            process_set_state(proc, PROC_CRASHED);
            /* A crash ends the program, so no join is coming: a joinable strand
             * the reaper waited on would never reach process_destroy, where its
             * program is ended with it. */
            __atomic_store_n(&proc->reap_blocked, 0u, __ATOMIC_SEQ_CST);

            // Force a reschedule via the frame
            schedule(frame);
            return;
        }
    }

    /* Kernel stack overflow recovery via double fault / stack fault.
     *
     * The guard page is the entire evidence, and the recovery runs ONLY when
     * the saved RSP actually landed in one. It used to run when it had not:
     * a miss substituted process_get_current() as the "overflow" victim, and
     * then announced a stack overflow the evidence had just disproved, killed
     * whichever process this core happened to be serving, force-released a
     * scheduler lock nothing had established the holder of, and IRETed — with
     * the real cause (a corrupt descriptor, a stale TSS.RSP0, an exhausted IST
     * stack) untouched, so the next delivery double-faulted again. Worse, the
     * substitute itself took a lock: process_get_current() acquires this core's
     * scheduler_lock, which is non-recursive and spun on with IRQs off, so a
     * #DF taken inside schedule() hung the core silently and printed nothing
     * at all.
     *
     * A miss now falls through to the kernel panic, which is where the
     * evidence points and what Linux does for every #DF (arch/x86/kernel/
     * traps.c, exc_double_fault, whose comment on the identical guard-page
     * heuristic ends "we're going to panic one way or another"). Intel SDM
     * Vol 3A, Exception and Interrupt Reference, "Double Fault Exception
     * (#DF)": exception class Abort, the saved CS:RIP is undefined and the
     * program cannot be resumed or restarted. A guard-page HIT still recovers,
     * because that path does not resume the aborted context — it discards it
     * (PROC_CRASHED) and schedules another, which is the one action the SDM
     * does leave open. Vector 12 is a Fault, not an Abort, and is unaffected
     * by that reading; for it the defect was purely the false cause and the
     * innocent victim.
     *
     * Never on the core that holds a panic. There the overflow is inside the
     * account (a kprintf deep in panic(), or the dump's own walk), every other
     * core is stopped where it stood, and the owner waits on no lock
     * (klib_lock.c): scheduling another process from here would be that
     * process run on one core through every lock the stopped ones hold, the
     * volume's among them. It is a fault inside the account, and the claim
     * below ends the account (klib_panic.c). */
    if ((frame->vector == 8 || frame->vector == 12) && !PanicOwnedHere())
    {
        process_t *overflow_proc = find_process_by_kernel_stack_overflow(frame->rsp);
        if (overflow_proc)
        {
            kprintf("\n");
            kprintf("================================================================\n");
            kprintf("KERNEL STACK OVERFLOW: Exception #%u recovered\n", frame->vector);
            kprintf("================================================================\n");
            kprintf("  PID: %u  TagBits: 0x%lx\n", overflow_proc->pid, overflow_proc->cabin ? overflow_proc->cabin->tag_bits : 0);
            kprintf("  RSP: 0x%lx  RIP: 0x%lx\n", frame->rsp, frame->rip);
            if ((frame->cs & 3) == 0)
                panic_name_addr("  in  ", frame->rip, false);
            if (overflow_proc->kernel_stack_guard_base)
            {
                kprintf("  Guard: 0x%lx  Stack: 0x%lx-0x%lx\n",
                        (uintptr_t)overflow_proc->kernel_stack_guard_base,
                        (uintptr_t)overflow_proc->kernel_stack,
                        (uintptr_t)overflow_proc->kernel_stack_top);
            }
            kprintf("  Killing PID %u, system continues.\n", overflow_proc->pid);
            kprintf("================================================================\n");

            process_set_state(overflow_proc, PROC_CRASHED);
            /* As for a user fault: its program ends at process_destroy, and a
             * reap-block would keep the reaper from it. */
            __atomic_store_n(&overflow_proc->reap_blocked, 0u, __ATOMIC_SEQ_CST);

            /* If the faulting code held the scheduler lock, force-release it
             * so schedule() can acquire it without deadlock. spin_force_release
             * (NOT spin_unlock) because we are on an IST stack — the original
             * holder's saved_flags are meaningless here.
             *
             * What this can and cannot claim: scheduler_get_state() returns
             * THIS core's state, and an IST stack never migrates, so the STATE
             * is certainly ours. The HOLDER need not be. scheduler_recalc_
             * parameters walks every core and takes each one's scheduler_lock
             * (scheduler.c, the g_core_sched scan), so a peer core can be
             * inside this lock at this instant, and force-releasing then cuts
             * its critical section in half. It is taken anyway because the
             * alternative on a guard-page hit is a certain deadlock against a
             * holder that no longer exists; the peer case is a window, not the
             * common one. Naming it here rather than claiming it away.
             *
             * And naming it OUT LOUD. Every other line of this dump is a
             * kprintf; this one was a debug_printf, which compiles to
             * nothing in the build that ships, so on a board the one action
             * in the whole recovery that can damage a core other than the
             * dead one left no trace at all. An operator reading the dump
             * afterwards has to be able to tell a clean guard-page recovery
             * from one that reached into a peer's critical section, because
             * the two have different consequences for everything that runs
             * after the machine "continues". */
            scheduler_state_t *sched = scheduler_get_state();
            if (sched->scheduler_lock.locked)
            {
                kprintf("[IST] Core %u forced the scheduler lock open for dead "
                        "PID %u — if a peer core was inside it, its critical "
                        "section was cut.\n",
                        amp_get_core_index(), overflow_proc->pid);
                spin_force_release(&sched->scheduler_lock);
            }
            __asm__ volatile("cli");

            schedule(frame);
            return;
        }
    }

    /* Kernel-mode exception: this is a real kernel panic.
     *
     * One panic, one dump. Everything below prints — and printing touches the
     * VGA buffer, the serial UART and, for the stack trace, memory the fault
     * may already have proved untrustworthy. Any fault raised in the middle of
     * that re-enters here, and so does every other core that hits the same bug
     * at the same moment. Without this claim the screen fills with dumps and
     * the first one, the only one with the cause still in CR2, scrolls away.
     * That is what a six-core machine did on its first real boot: the panic
     * path already ended in `cli; hlt`, and it still never got there.
     *
     * The loser of the race stops without printing a word, because a second
     * dump scrolls the first one away just as effectively as a loop does. */
    PanicClaimForException(&panic_claimed_here, frame, fault_addr);

    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("KERNEL PANIC: %s (vector %u)\n",
            exception_mnemonic((uint8_t)frame->vector), frame->vector);
    kprintf("====================================================================\n");
    kprintf("Error code: 0x%lx\n", frame->error_code);
    if (frame->vector == 17)
    {
        kprintf("Hint: #AC in kernel mode means BoxOS code emitted a "
                "cache-line-spanning LOCK instruction. Either a regression "
                "in atomic-target alignment or TEST_CTL bit 29 was re-asserted "
                "by firmware after boot. Fix the alignment in the offending "
                "atomic, don't mask the detection.\n");
    }
    if (frame->vector == 8 || frame->vector == 12)
    {
        /* Reaching the panic with these two vectors means one thing exactly:
         * the saved RSP matched no kernel-stack guard page, so the recovery
         * above declined it. Say what was tested, so nobody reads the banner
         * as "stack overflow" — and say what the test cannot see, so nobody
         * reads it as "certainly not one" either. */
        kprintf("Saved RSP 0x%lx is inside no kernel-stack guard page, so this "
                "is not the stack overflow the recovery path handles.\n"
                "Two blind spots of that test, for the operator: an idle "
                "process's stack is not in the process list, and a push that "
                "crosses the guard boundary in one step leaves the saved RSP "
                "just above the guard rather than inside it.\n", frame->rsp);
    }
    kprintf("Exception count: %lu  Core: %u\n", atomic_load_u64(&exception_count), (uint32_t)amp_get_core_index());

    // Full GPR dump
    kprintf("  RAX=%016lx  RBX=%016lx\n", frame->rax, frame->rbx);
    kprintf("  RCX=%016lx  RDX=%016lx\n", frame->rcx, frame->rdx);
    kprintf("  RSI=%016lx  RDI=%016lx\n", frame->rsi, frame->rdi);
    kprintf("  RBP=%016lx  RSP=%016lx\n", frame->rbp, frame->rsp);
    kprintf("  R8 =%016lx  R9 =%016lx\n", frame->r8, frame->r9);
    kprintf("  R10=%016lx  R11=%016lx\n", frame->r10, frame->r11);
    kprintf("  R12=%016lx  R13=%016lx\n", frame->r12, frame->r13);
    kprintf("  R14=%016lx  R15=%016lx\n", frame->r14, frame->r15);
    kprintf("  RIP=%016lx  RFL=%016lx\n", frame->rip, frame->rflags);
    kprintf("  CS=%04lx  SS=%04lx\n", frame->cs, frame->ss);
    panic_name_addr("  fault at ", frame->rip, false);

    /* The same value the handler read on the way in — see the note at the top.
     * CR3 is read here because nothing about a fault disturbs it.
     *
     * The register is dumped for every vector because it IS a register this
     * core holds, but only vector 14 wrote it (Intel SDM Vol 3A §4.7). On any
     * other vector it is an earlier page fault's address, and the line says so
     * rather than letting the reader take it for this exception's. */
    uint64_t panic_cr2 = fault_addr, panic_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(panic_cr3));
    kprintf("  CR2=%016lx%s  CR3=%016lx\n", panic_cr2,
            frame->vector == 14 ? ""
                                : " (stale — not this exception's address)",
            panic_cr3);

    if (frame->vector == 14)
    {
        kprintf("Page Fault at 0x%lx (P=%d W=%d U=%d R=%d I=%d)\n",
                panic_cr2,
                (int)(frame->error_code & 0x1),
                (int)((frame->error_code >> 1) & 0x1),
                (int)((frame->error_code >> 2) & 0x1),
                (int)((frame->error_code >> 3) & 0x1),
                (int)((frame->error_code >> 4) & 0x1));
        PanicNameFaultAddress(panic_cr2);
    }

    // RBP-chain stack walk
    kprintf("Stack trace:\n");
    uint64_t walk_rbp = frame->rbp;
    for (uint32_t depth = 0; depth < 20; depth++)
    {
        if (walk_rbp == 0 || (walk_rbp & 7) != 0)
            break;
        if (!vmm_is_kernel_addr(walk_rbp))
            break;
        /* vmm_is_kernel_addr answers a question about the address, not about
         * the memory: a kernel-half address is not therefore a mapped one, and
         * the two frame words below are read from whatever the faulting code
         * left in RBP. Reading them unverified is how a stack trace becomes a
         * second page fault, which is how the dump naming the first one gets
         * replaced by a dump naming itself. */
        if (!panic_probe_range(walk_rbp, 16))
        {
            kprintf("  #%u  <frame at %016lx is not mapped — chain ends here>\n",
                    depth, walk_rbp);
            break;
        }

        uint64_t *fp = (uint64_t *)walk_rbp;
        uint64_t saved_rbp = fp[0];
        uint64_t ret_addr = fp[1];

        bool in_text = (ret_addr >= (uint64_t)_text_start && ret_addr < (uint64_t)_text_end);
        if (in_text)
        {
            kprintf("  #%u  ", depth);
            panic_name_addr("", ret_addr, true);
        }
        else
        {
            /* Outside .text: not a return address at all, so there is nothing
             * to name and naming the nearest thing would be a guess wearing a
             * function's clothes. */
            kprintf("  #%u  %016lx  [!]\n", depth, ret_addr);
        }

        if (saved_rbp <= walk_rbp || saved_rbp == 0)
            break;
        walk_rbp = saved_rbp;
    }

    kprintf("====================================================================\n");
    kprintf("System halted.\n");

    /* Said whole: marked so, out to the memory the next boot reads and onto
     * the wire, and standing (klib_panic.c). */
    PanicEndAccount();
}

/*
 * Is this kernel ready to be interrupted at all?
 *
 * Set once, from kernel_main, the moment scheduler_init() returns — because
 * that is what irq_handler needs in order to do its job, and interrupts
 * arriving before it find a handler with nowhere to record a tick. Anything
 * delivered while this is false got in through a door the kernel did not
 * open, and that is worth a sentence: on an i5-9400F booting UEFI the first
 * HPET tick landed thirty-six lines early, inside TSC calibration, and the
 * only evidence was a #PF at 0x18 with no hint of how an interrupt got in.
 * The handler is safe either way now; this names the how.
 */
volatile bool g_kernel_ready_for_interrupts = false;

/* 8042 status register (port 0x64), the two bits IRQ1 has to read before it
 * touches the output register at 0x60. Values per Linux include/linux/
 * i8042.h — I8042_STR_OBF 0x01, I8042_STR_AUXDATA 0x20 — which is the
 * reference every PC keyboard controller is built against. Named here rather
 * than pulled from keyboard.c, where the full set is file-local. */
#define KBC_STR_OBF      0x01  /* 0x60 holds a byte for the system        */
#define KBC_STR_AUXDATA  0x20  /* that byte came from the second port     */

/* -- The two lines this kernel answers itself --------------------------
 *
 * These used to be a `switch (vector - 32)` at the bottom of irq_handler,
 * reached only when a line had no answerer at all -- which is to say the
 * tick and the keyboard were dispatched by their ISA numbers while every
 * other line in the machine was dispatched through the registry. That
 * worked for exactly as long as a vector was 32 + its ISA number. It would
 * have stopped working, silently, the moment a vector came from anywhere
 * else: `case 0` would never match again, the tick would be accepted,
 * acknowledged and thrown away, and the machine would look alive while
 * nothing in it moved.
 *
 * So they answer their lines the same way a disk does. There is one path
 * through this handler now, and no line in this kernel is dispatched by a
 * number.
 *
 * They are exported rather than static because the registration belongs to
 * whoever brings the line up, while the bodies belong next to the state
 * they touch -- which is here. */
void IrqTheTickAnswers(void)
{
        // Timer IRQ (PIT via PIC or IO-APIC GSI 0)
        //
        // ‼ The NULL check is not defensive dressing — it is the whole
        // difference between a machine that boots and one that does not, and
        // it took a board to say so. On an i5-9400F booting UEFI the first
        // HPET tick arrived inside cpu_calibrate_tsc, thirty-six lines of
        // kernel_main before scheduler_init() allocated any state, and this
        // line wrote through the NULL it got back: #PF at 0x18, dead. The
        // BIOS path never showed it because stage2 hands the kernel a machine
        // with interrupts already off, so nothing could be delivered this
        // early; _start now clears IF for both paths, which closes the window
        // rather than making it narrower. Everything below this point is
        // safe to run without a scheduler, and must stay that way.
        pit_tick();

        /* Advance the global scheduler clock from the MONOTONIC wall-clock at a
         * FIXED logical rate, decoupled from the adaptive PIT IRQ frequency.
         * The PIT IRQ rate swings 10-500 Hz with load, so a per-IRQ "+1" made a
         * tick's wall-clock duration drift — starvation/affinity thresholds
         * (counted in ticks) wobbled with load. Deriving the tick from
         * pit_get_uptime_us() pins it to SCHEDULER_DEFAULT_TICK_HZ (the rate
         * consumers were tuned for), now stable. Monotonic (uptime only grows);
         * single writer (BSP IRQ0); all consumers use deltas. */
        __atomic_store_n(&g_global_tick,
                         pit_get_uptime_us() / (1000000ULL / SCHEDULER_DEFAULT_TICK_HZ),
                         __ATOMIC_RELAXED);

        /* Periodic scheduler parameter recalculation */
        scheduler_recalc_parameters();

        /* APEI/GHES periodic poll — walks Polled-notify HEST sources
         * whose TSC deadline has elapsed and processes their GESB.
         * Cheap when nothing is due (per-source TSC compare in the
         * runtime path; no work outside that). Routes firmware-side
         * ECC events into the same poison + page-migration pipeline
         * as #MC-delivered events. */
        {
            extern void apei_ghes_poll_tick(void);
            apei_ghes_poll_tick();
        }

        /* Periodic TSC recalibration tick. Cheap — just stamps a
         * timestamp + sets a pending flag every TSC_RECAL_INTERVAL_US.
         * The actual ~20ms measurement runs in idle context
         * (cpu_tsc_recal_if_pending) — never in IRQ. */
        cpu_tsc_recal_tick(pit_get_uptime_us());

        /* Software key repeat driven by PIT tick */
        keyboard_timer_tick();

        /* The log ring's bell for the utterances that could not knock for
         * themselves — an NMI handler's line (klib_logring.h). BSP to BSP:
         * the baton needs no IPI from here. */
        LogRingBellKnock();

        /* The line, for as long as it has no interrupt of its own (serial.c). */
        WireTick();

        /* The moments named in advance that came: the touch queue's keeper
         * fires them on its own one-shot (DEADLINE_VECTOR); this is the way
         * they fire where it has none, and the net under it where it has. */
        TouchQueueFireDue(pit_get_uptime_us());

        /* Nightwatch's looks that cannot wait for an idle core. */
        nightwatch_tick();

        /* xHCI: poll for events when the controller has no usable interrupt,
         * and run the command and enumeration watchdogs either way. */
        xhci_tick();

        /* Ф26 M1 — AHCI async-completion watchdog (safety backstop). Reconciles
         * a lost/coalesced completion MSI from the port's PxSACT/PxCI level, and
         * fails + COMRESETs a genuinely wedged port so a lost completion cannot
         * hang a waiter forever. BSP-only (this is IRQ0) — the same core the
         * AHCI MSI targets; no-op unless multi-core async I/O is in flight. */
        ahci_watchdog_scan();

        /* Ф26 BMIDE — the legacy-PATA analogue: reconcile a lost IDE INTRQ from
         * the channel's BMISR latch, and mark a genuinely wedged channel's
         * SRST recovery owed, which the waiter on the channel then runs itself
         * and fails the command ERR_IO. The waiter holds its own command to
         * the same liveness bound, so a dropped completion ends even while
         * this tick does not run; the tick is the quicker of the two. Same
         * BSP-only tick (both need the disk's irqsave locks). No-op unless
         * multi-core BMIDE async I/O is in flight (dormant whenever AHCI owns
         * block I/O). */
        {
            extern void bmide_watchdog_scan(void);
            bmide_watchdog_scan();
        }

        /* Single-core mode: pump the batons here, when the PIT IRQ
         * interrupted USERSPACE code.
         *
         * Background: the baton is the one hand-off from an interrupt to a
         * K-Core for work that touches kmalloc / tagfs / process tables. In
         * multi-core configurations the drain core pumps inside
         * `kcore_run_loop`. Single-core mode does NOT run kcore_run_loop
         * (the BSP runs userspace directly via the scheduler), so without an
         * explicit pump nothing would ever run the queued continuations
         * (keyboard Touch, USB Touch, SCI/GPE notifications, write_job
         * completions, a park deadline's ERR_TIMEOUT).
         *
         * Lock-safety: the continuations take heap_lock, tagfs locks,
         * process_lock, etc. If the PIT IRQ interrupted a kernel-mode
         * syscall holding one of those locks, pumping here would deadlock
         * against the interrupted thread on the same core. We gate the pump
         * on `(frame->cs & 3) == 3` — the interrupted code was in user mode
         * (CS=USER_CS, RPL=3) — which guarantees no kernel lock is held.
         * Multi-core takes the `amp_is_appcore()` branch above and does not
         * pump here. On one core this and the idle loop are the only drains,
         * and the ring-3 gate is what keeps the two from ever interleaving
         * on the single-consumer queue. */
        interrupt_frame_t *in_flight = IrqFrameInFlight();
        if (g_amp.total_cores == 1 && in_flight && (in_flight->cs & 3) == 3)
        {
            BatonPumpAtRest(0);
            /* And the Use Context's verdicts, when one went stale. On one core
             * the idle loop is the only other place the recount runs, and a
             * program that computes without entering the kernel keeps idle
             * from ever running: its cabin would go on paying the Fare of a
             * context that no longer stands. Same lock-safety as the pump. */
            UseContextServiceIfPending();
        }

        /* PIT IRQ 0: same scheduling rule as the LAPIC timer above —
         * preempt on App Cores or in single-core mode where the BSP
         * itself runs userspace. Use total_cores (set during amp_init
         * before any AP boots) — `multicore_active` is set LATE and was
         * causing AP K-Cores to be hijacked into idle on early ticks. */
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            interrupt_frame_t *in_flight = IrqFrameInFlight();
            if (in_flight) schedule(in_flight);
        }
}

void IrqTheKeyboardAnswers(void)
{
        /* Ask the controller WHICH byte is in the output register before
         * taking it. One register at 0x60 serves both 8042 ports, and IRQ1
         * says only that the controller raised a line — not what put the
         * byte there, nor that there is one.
         *
         * Bit 0 (OBF) says a byte is there at all. IRQ1 can arrive with the
         * register already emptied: a shared level line, a second edge for a
         * byte keyboard_init's polling loop took during bring-up, a spurious
         * delivery. Reading 0x60 then re-reads whatever the controller last
         * left there and types that key a second time. Nothing about an
         * unconditional read makes the register refill.
         *
         * Bit 5 (AUXDATA) says the byte came from the second port. Scan-code
         * set 1 and a mouse packet share the same byte values, so a packet
         * fed to the decoder types characters nobody pressed. keyboard_init
         * drives that port down (AUX_DISABLE plus AUXDIS in the command
         * byte), so this should never fire; it is here because the read is
         * what clears OBF, and a byte that reached the register by some
         * other route — firmware, a warm handover, a controller quirk —
         * must be consumed and discarded rather than decoded, or it blocks
         * the keyboard's next byte forever. Discarding it is not silent:
         * see the line below.
         *
         * Bit values are the reference every PC controller is built
         * against: Linux include/linux/i8042.h, I8042_STR_OBF 0x01 and
         * I8042_STR_AUXDATA 0x20. i8042_interrupt makes these same two
         * decisions in this same order.
         *
         * Returning early is safe: the EOI is owed either way, and it is
         * owed by irq_handler, which sends it after every answerer on the
         * line has had its turn. An IRQ the kernel declines to act on still
         * has to be acknowledged, or the line stays asserted. */
        uint8_t status = inb(KEYBOARD_STATUS_PORT);
        if (!(status & KBC_STR_OBF))
            return;
        uint8_t byte = inb(KEYBOARD_DATA_PORT);
        if (status & KBC_STR_AUXDATA)
        {
            /* A dropped byte is a keystroke nobody will ever see, so it is
             * said out loud. What it distinguishes is not guessable from a
             * dead keyboard: either something really is talking on a port
             * this kernel powered down, or this controller does not mean
             * "second port" by bit 5 — the pre-PS/2 AT parts and a few
             * emulated KBCs use it for a transmit timeout instead, and on
             * those the byte being thrown away is the operator's.
             *
             * Once, not per byte. This runs inside IRQ1 with interrupts
             * off; a controller stuck with the bit asserted would turn the
             * console itself into the fault. One line names the condition,
             * and the status byte in it is what tells the two cases
             * apart. */
            static volatile bool aux_byte_said = false;
            if (!aux_byte_said)
            {
                aux_byte_said = true;
                kprintf("[IRQ] the 8042 handed IRQ1 the byte 0x%02x marked as "
                        "the second port's (status 0x%02x) although "
                        "keyboard_init powered that port down — dropped, not "
                        "decoded; further ones go unremarked\n",
                        byte, status);
            }
            return;
        }
        keyboard_handle_scancode(byte);
}

void irq_handler(interrupt_frame_t *frame)
{
    uint8_t vector = frame->vector;

    if (!g_kernel_ready_for_interrupts)
    {
        /* Once. A storm of early interrupts is one fact, not a hundred. */
        static volatile bool said = false;
        if (!said)
        {
            said = true;
            kprintf("[IRQ] vector %u arrived before this kernel was ready to "
                    "be interrupted — something outside it set RFLAGS.IF "
                    "(a loader, or firmware returning from a runtime call)\n",
                    vector);
        }
    }

    // LAPIC spurious vector: do NOT send EOI
    if (vector == LAPIC_SPURIOUS_VECTOR)
    {
        return;
    }

    /* LAPIC timer vector: an App Core's turn has come to its end — its
     * one-shot, armed by schedule() for that moment (scheduler.c).
     *
     * Multi-core layout: BSP is a K-Core running kcore_run_loop, App
     * Cores run user processes. Only App Cores need preemptive
     * `schedule()` — calling it on a K-Core would hijack the kernel
     * loop. Hence the `amp_is_appcore()` gate. A single core's turns end
     * at IRQ0 (IrqTheTickAnswers); its LAPIC timer is the touch queue
     * keeper's (DEADLINE_VECTOR), never this one. */
    if (vector == LAPIC_TIMER_VECTOR)
    {
        /* A tick can land before there is a scheduler to count it into:
         * scheduler_get_state() answers NULL until scheduler_init() has
         * allocated the per-core array. An interrupt handler may never assume
         * the init order of anything it touches.
         *
         * Use total_cores (set ONCE during amp_init() before any AP boots)
         * instead of multicore_active (set LATE, after every AP comes
         * online): an AP K-Core must never reach schedule(), which would
         * pick idle and hijack the K-Core's stack. */
        if (scheduler_get_state() && amp_is_appcore() && g_amp.total_cores > 1)
            schedule(frame);
        lapic_send_eoi();
        return;
    }

    /* IPI_WAKE: a strand came to this App Core's runqueue, or to the one core
     * — whether the strand running here keeps the core a moment longer is the
     * scheduler's (scheduler_woken). On K-Cores it is only ACKed: the
     * interrupt breaks HLT in kcore_run_loop. */
    if (vector == IPI_WAKE_VECTOR)
    {
        if (amp_is_appcore() || g_amp.total_cores == 1)
            scheduler_woken(frame);
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_SHOOTDOWN_VECTOR)
    {
        vmm_tlb_shootdown_handler();
        lapic_send_eoi();
        return;
    }

    /* The touch queue's keeper: its one-shot came at the nearest moment named
     * in advance, or a core named a nearer one (touch_queue.h). */
    if (vector == DEADLINE_VECTOR)
    {
        TouchQueueDeadline();
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_HALT_VECTOR)
    {
        /* Stop — unless an orderly halt found this core inside a held span:
         * a Scribe pass, or a command it stands over on a medium. Reaching
         * this branch proves this core was not in a stretch that kept IF=0:
         * the IPI waited in the LAPIC IRR until such a spin_lock section was
         * released. Both spans run with interrupts ON, so this IPI can land in
         * the middle of one, and a core stopped there would leave the medium
         * half of a structure, or a command slot or a whole-port door held
         * for ever — and the halt's own sync and flush waiting on a holder
         * that never gives it back. So under an orderly halt such a core owes
         * its stop and returns: it runs the span to its end and stops where it
         * lets its last one go (ScribeGive, HaltStopIfOwedHere), and the roll
         * call waits for that as it waits for any arrival. A halt that is not
         * orderly (a power cut, settle_cut.c) does not raise g_halt_orderly —
         * every core stops where it stands, holding what it holds; a panic does
         * not come here at all, it stops cores by an NMI (klib_panic.c). A
         * core waiting on a legacy-IDE DMA inside such a section keeps its
         * caller's interrupts off — it takes the drive's answer itself
         * (BmideAnswerHere, drivers/disk/ata_async.c) — so it too takes this
         * IPI only once the caller's lock is let go. */
        if (__atomic_load_n(&g_halt_orderly, __ATOMIC_ACQUIRE) && SchedulerHeldHere())
        {
            CoreDescriptor *me = &g_amp.cores[amp_get_core_index()];
            me->freeze_asked_tsc = rdtsc();
            __atomic_store_n(&me->freeze_owed, (uint8_t)1, __ATOMIC_RELAXED);
            lapic_send_eoi();
            return;
        }
        /* The EOI first, and the core's own word that it arrived last (inside
         * HaltFreezeHere), so the word means "this core touches nothing
         * again". A core stopped here holds no span and no other core's
         * completion — a legacy-IDE waiter claims and publishes one with its
         * interrupts off (ata_async.c) — and a command of its own still in
         * flight is retired by whoever waits on its channel next. It pairs
         * with the initiator's acquire load: the initiator (system_halt) used
         * to wait a guessed few milliseconds and then assume every core had
         * arrived. */
        lapic_send_eoi();
        HaltFreezeHere();
    }

    /* There are no message-signalled branches here any more.
     *
     * Two used to stand at this spot, one testing `vector == AHCI_MSI_VECTOR`
     * and one testing a range of eight xHCI numbers, because those vectors
     * were constants compiled into the drivers. They are handed out by the
     * pool now, so there is no constant left to compare against — and there
     * does not need to be. A message-signalled interrupt is an entry in the
     * same registry as everything else, with an answerer and no pin, and it
     * goes down the same path below. The LAPIC EOI it needs is the one
     * irqchip_send_eoi already sends on the APIC backend, which is the only
     * backend on which MSI exists at all. */

    /* The 8259's input number for this vector, kept for ONE purpose: the EOI
     * below.
     *
     * On the APIC backend apic_chip_eoi ignores the argument entirely — the
     * LAPIC's EOI register takes no vector. On the PIC backend it is not a
     * convention this kernel chose but a fact about the silicon: ICW2 sets
     * the base of a run of eight consecutive vectors and the 8259 ignores the
     * low three bits of what is written there, so "vector = 32 + input" is
     * how the chip is wired, and subtracting 32 is the only way back to the
     * input that has to be acknowledged.
     *
     * It is NOT the registry key any more. The registry is indexed by the
     * vector itself, so a line delivered anywhere in the pool finds its own
     * record instead of falling off the end of a twenty-four-entry array. */
    uint8_t irq = irq_vector_to_gsi(vector);

    atomic_fetch_add_u64(&irq_count[vector], 1);

    /* Published before any answerer runs and dropped after the last one, so
     * that "there is a frame in flight" and "an answerer is running" are the
     * same statement rather than two that can drift apart. */
    uint8_t this_core = amp_get_core_index();
    if (this_core < MAX_CORES) g_irq_frame[this_core] = frame;

    /* Dropped again on every path out of this function, so that "there is a
     * frame in flight" and "an answerer is running" stay the same statement.
     * It used to be published and never cleared, which made the promise in
     * idt.h — NULL outside a handler — untrue from the first interrupt
     * onwards, and left a pointer into a stack frame that had returned. */
#define IRQ_FRAME_DONE() do { if (this_core < MAX_CORES) g_irq_frame[this_core] = NULL; } while (0)

    IrqLine *l = &irq_lines[vector];
    SummonsResponder *r = __atomic_load_n(&l->answerers, __ATOMIC_ACQUIRE);

    if (r != NULL)
    {
        /* Everyone on the wire is offered it, in the order they arrived.
         * Each looks at its own device and does nothing if the device is
         * silent — which is what "PCI device drivers are required to support
         * shared interrupts" (PCI 3.0 §6.5) asks of a driver, and what every
         * handler in this kernel already did when it was the only one.
         *
         * EOI comes after the last of them, once, and that order is the
         * level-triggered rule: the pin is still asserted until the device
         * that raised it has been cleared, so acknowledging first would
         * re-deliver the same interrupt for as long as the line is held. */
        do
        {
            r->answer();
            r = __atomic_load_n(&r->next, __ATOMIC_ACQUIRE);
        } while (r != NULL);

        irqchip_send_eoi(irq);
        IRQ_FRAME_DONE();
        return;
    }

    /* A LEVEL LINE WITH NOBODY ON IT IS A WEDGE, NOT A STRAY INTERRUPT.
     *
     * An edge-triggered line that fires with no answerer costs one EOI and is
     * over. A level-triggered one is held low by its device until a driver
     * clears it (PCI 3.0 §2.2.6: "Once the INTx# signal is asserted, it
     * remains asserted until the device driver clears the pending request"),
     * so the EOI below hands it straight back and the core re-enters this
     * handler for ever, at interrupt priority, with nothing else able to run.
     * The machine does not crash; it stops, in a way that leaves no evidence.
     *
     * So the pin is masked. The fact that decides it is "this line fired and
     * has no answerer at all" — not a count of how many times, which would be
     * a number standing in for the fact. Masking is also exactly recoverable:
     * a driver that arrives later arms the line again through SummonsArm. */
    if (l->posted && l->line.trigger == SUMMONS_LEVEL)
    {
        SummonsDisarm(&l->line);
        if (!l->unanswered_said)
        {
            l->unanswered_said = true;
            kprintf("[IRQ] %s GSI %u asserted and no driver answers it — "
                    "masked at the pin, because a level-triggered line is "
                    "held asserted until somebody clears the device and this "
                    "core would otherwise never leave the handler\n",
                    l->line.bus, l->line.gsi);
        }
        irqchip_send_eoi(irq);
        IRQ_FRAME_DONE();
        return;
    }

    /* An edge-triggered line that fired with nobody on it, or a vector nothing
     * in this kernel arranged to receive. One EOI and it is over — the pin is
     * not held, so there is nothing to mask and nothing to wedge, and saying
     * so on every stray pulse would bury the log.
     *
     * Said ONCE per vector, and the pool is asked who holds it, because the
     * useful question about an interrupt nobody expected is not "which
     * number" but "whose number". A vector the pool has never handed out is a
     * message from a device this kernel did not program; one it HAS handed out
     * is a driver that armed a line and then did not answer it.
     *
     * The flag lives in the registry record rather than in a counter: the
     * fact is "this vector has surprised us", not how many times. */
    if (!l->posted && !l->said_unarranged) {
        l->said_unarranged = true;
        const char *holder = VectorPoolHolder(vector);
        kprintf("[IRQ] vector 0x%02x arrived and nothing in this kernel "
                "answers it — %s\n", vector,
                holder ? holder : "the pool has never handed this number out, "
                                  "so it came from a device nobody here "
                                  "programmed");
    }
    /* An edge-triggered line that fired with nobody on it. One EOI and it is
     * over — the pin is not held, so there is nothing to mask and nothing to
     * wedge, and saying so on every stray pulse would bury the log.
     *
     * Legacy IDE IRQ 14 and 15 arrive here by design: the PIO driver sets
     * nIEN=1 on every drive it finds and leaves those pins masked, so the
     * channel line never fires. If something re-enables it, this is where it
     * lands, and it lands harmlessly. */
    irqchip_send_eoi(irq);
    IRQ_FRAME_DONE();
}
#undef IRQ_FRAME_DONE

// ---------------------------------------------------------------------------
// Syscall dispatch: sync (single-core) vs async (multi-core)
// ---------------------------------------------------------------------------

// Sync path: process Pockets immediately on the calling core (N=1).
static void sync_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    KCallGate(proc);
    KCallTaken(proc, 0);   /* one core: the gate serves the call itself */
    context_save_from_frame(proc, frame);
    ready_queue_push(&g_ready_queue, proc);
    /* Being served is a state of the counter, not of the passenger, and it
     * used to be written here as PROC_WAITING — "queued for the guide, not
     * for the scheduler". It established nothing: this core arrives with IF
     * clear (SFMASK carries IF for SYSCALL; vector 0x80 is an interrupt gate;
     * spin_lock saves and restores RFLAGS rather than enabling), there is no
     * second core on this path, and the strand is running, so no scheduler
     * pass can observe the mark between here and guide()'s return.
     *
     * What it did do was destroy every park a handler took. A handler that
     * parks — addr_park, touch_await, ObjRead, a WriteJob — writes the same
     * PROC_WAITING to mean the opposite thing: "woken by event, not by the
     * next pass". Arriving already marked, its write was a no-op (old == new,
     * so no sched_dequeue), and guide()'s matching restore then read the mark
     * as its own and enqueued the strand it was supposed to leave asleep. A
     * 300 ms sleep cost 279 ms of processor time, the machine never reached
     * idle, and every wake gated on PROC_WAITING — touch_interrupt_deliver,
     * touch_queue_fire_wake, both arms of sync_ops — was skipped, because the
     * state those paths look for no longer existed on a single core.
     *
     * PROC_WAITING now has exactly one writer's intent in the kernel: a
     * handler put this strand to sleep. Do not mark anything here again. */
    guide();
    /* P5b: single-core never runs kcore_run_loop, so the strand reaper +
     * deferred cleanup must be driven from here (throttled). Without it,
     * exited strands accumulate and std::thread-style churn exhausts the
     * process table on uniprocessor. Cheap on 1c: vmm shootdown degrades to
     * a local invlpg (total_cores <= 1), so no IPI round-trips.
     *
     * The reaper MAY select the calling proc: if this very syscall was a
     * strand's own strand_exit, guide() above already flipped it to PROC_DONE,
     * so it now matches the reaper's filter. That is safe — it is still
     * current_process on this core, so process_destroy's is-running scan
     * declines it (and the snapshot ref is released); it is reaped a later
     * tick after schedule() switches away. */
    static uint32_t reap_tick = 0;
    if ((++reap_tick & 0x7u) == 0u)
    {
        process_reap_strands();
        process_cleanup_deferred();
    }
    schedule(frame);
}

// Async path: non-blocking doorbell to K-Core (N>1).
// Process stays PROC_WORKING and returns to userspace immediately.
// K-Core processes Pockets in parallel, writes Results to ResultRing.
// Process spins in result_wait() with pause until Result appears.
// No state change, no schedule(), no IPI back — true parallelism.
static void async_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    (void)frame; // not needed — process continues running

    KCallGate(proc);
    uint8_t core_idx = amp_get_core_index();

    // CAS: only submit if not already queued (dedup)
    if (atomic_cas_u8(&proc->kcore_pending, 0, 1))
    {
        debug_printf("[A%u] ASYNC submit PID %u to K-Core\n", core_idx, proc->pid);
        KPocketRingInHand(proc, true);   /* before the queue: the K-Core clears it */
        kcore_submit(proc);
    }
    else
    {
        debug_printf("[A%u] ASYNC skip PID %u (already pending)\n", core_idx, proc->pid);
    }
    // Return to userspace — iretq restores frame, process keeps running.
}

// Function pointer set at boot — never changes at runtime.
static void (*g_syscall_dispatch)(process_t *, interrupt_frame_t *) = sync_syscall_dispatch;
static bool g_gate_async;   /* the same choice, for the yield branch of the gate */

void idt_set_syscall_mode(bool multicore)
{
    g_gate_async = multicore;
    if (multicore)
    {
        g_syscall_dispatch = async_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: ASYNC (multi-core, non-blocking)\n");
    }
    else
    {
        g_syscall_dispatch = sync_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: SYNC (single-core)\n");
    }
}

// ---------------------------------------------------------------------------
// syscall_handler — unified entry point for SYSCALL and INT 0x80
// ---------------------------------------------------------------------------

void syscall_handler(interrupt_frame_t *frame)
{
    /* The strand this core runs: written by this core alone, with its
     * interrupts off (schedule, process_start_initial), and they are off here
     * too — SFMASK carries IF — so it is read as it stands, with no lock. */
    process_t *proc = scheduler_get_state()->current_process;
    if (!proc || proc->magic != PROCESS_MAGIC)
    {
        frame->rax = (uint64_t)-1;
        return;
    }

    frame->rax = 0;

    /* The door: settle what the kernel owes this strand's Touch ring before
     * anything else it came for.
     *
     * It stands HERE, at the syscall gate, and not in guide_process_pocket,
     * because the guide is not the gate — a yield is answered below without
     * ever entering guide(), and a yield is precisely how a consumer that has
     * read the `owed` slip comes to ask for the rest. A door the asking never
     * reaches is not a door. Every other path into the kernel passes through
     * here too, so one check covers them all: sync dispatch, async K-Core
     * submit, and the yield.
     *
     * A strand arriving here has been running, so it has had its chance to
     * drain its ring; what did not fit before may fit now. Handing over BEFORE
     * the dispatch is also what lets a touch_await park correctly — the slots
     * land first, and the park's own ring re-check then undoes itself.
     *
     * Costs one relaxed load when nothing is owed, which is nearly always. The
     * hand-over is self-limiting: it stops the moment the ring refuses again,
     * so it can never push more than the consumer just drained. */
    if (__atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED) != 0)
        TouchOwedHandOver(proc);

    /* The same door, for the reply ring. An answer somebody sent this strand
     * that its ResultRing had no slot for is held by the kernel (kring.c) and
     * handed over right here, for every reason the Touch hand-over above stands
     * at this line rather than in the guide. The strand is the only thing that
     * can make room in its own reply ring, and it made that room by running, so
     * the moment it comes back in is exactly when what did not fit before fits
     * now. Self-limiting the same way: the drainer stops at the first refusal,
     * so one visit never pushes more than the consumer just drained. */
    if (__atomic_load_n(&proc->result_owed_count, __ATOMIC_RELAXED) != 0)
        ResultOwedHandOver(proc);

    /* The yield. A strand that says GATE_YIELD in RDI is giving its core
     * away — that is the whole request, and it is answered here, not by the
     * guide. It is not a pocket: a yield that entered the ring landed behind
     * whatever real pocket was waiting at the head, where this gate could not
     * take it, and a strand yielding for its Turn In to be taken filled its
     * ring in half a second — every submit after that was refused (BIOS 16c,
     * 2026-09-06). Pockets that ARE waiting are still handed on first: on one
     * core the guide runs them here and schedules; on many, the doorbell is
     * rung (deduplicated by kcore_pending) and then the core is given away —
     * the strand stands behind the next one (sched_yield_turn). */
    if (frame->rdi == GATE_YIELD)
    {
        sched_yield_turn();
        if (!g_gate_async)
        {
            sync_syscall_dispatch(proc, frame);
            return;
        }
        if (!KPocketIsEmpty(proc))
            async_syscall_dispatch(proc, frame);
        context_save_from_frame(proc, frame);
        schedule(frame);
        return;
    }

    // Dispatch: sync blocks + schedules, async returns immediately.
    g_syscall_dispatch(proc, frame);
}
