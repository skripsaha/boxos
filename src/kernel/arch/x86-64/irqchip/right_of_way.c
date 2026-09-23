#include "right_of_way.h"
#include "irqchip.h"
#include "idt.h"
#include "klib.h"

/* ── Where the pool begins and ends, both derived rather than written ──────
 *
 * The floor is 32 because Intel reserves 0..31 (SDM Vol 3A §6.2, "Vector
 * numbers in the range 0 through 31 are reserved by the Intel 64 and IA-32
 * architectures for architecture-defined exceptions and interrupts"). Note
 * this is a DIFFERENT reason from the one people usually give: §11.8.3 only
 * forbids class 0, which is vectors 0..15. Both rules are satisfied here, and
 * the stricter one is the one that sets the floor.
 *
 * The ceiling is not a number either. It is one below the lowest vector this
 * kernel keeps for itself, so that nothing a device is given can outrank the
 * interrupts the kernel uses to stop, to reschedule, and to shoot down a TLB.
 * A device above IPI_PANIC_VECTOR could preempt the broadcast halt that a
 * panic sends — which is to say, a machine already in trouble would fail to
 * stop because the controller that caused the trouble kept talking. */
#define POOL_FLOOR_VECTOR   32
#define POOL_CEILING_VECTOR (IPI_WAKE_VECTOR - 1)

/* The class of a vector, and the first vector of a class. SDM Vol 3A §11.8.3:
 * the class is bits 7:4. Written as functions rather than open-coded shifts so
 * the one place this arithmetic lives is the one place it can be wrong. */
static inline uint8_t VectorClassOf(uint8_t vector)    { return (uint8_t)(vector >> 4); }
static inline uint8_t ClassFirstVector(uint8_t cls)    { return (uint8_t)(cls << 4); }
static inline uint8_t ClassLastVector(uint8_t cls)     { return (uint8_t)((cls << 4) | 0x0F); }

/* ── The rows, and where their boundaries sit ─────────────────────────────
 *
 * The pool spans classes 2..14. Three rows do not divide thirteen classes
 * evenly, and rather than split them arbitrarily the boundaries are put where
 * this machine already has them:
 *
 *   - every IOAPIC line in this kernel today lives in class 2 or 3, so the
 *     bottom row contains those classes and the room above them;
 *   - every MSI device today lives in class 7 (0x70..0x78), so the middle row
 *     is built around class 7;
 *   - what is left below the kernel's own class is the top row.
 *
 * ‼ WHAT ACTUALLY MOVED, corrected after reading a boot log rather than after
 * reasoning about one. This note used to claim "nothing which already works
 * changes class", and the first boot refuted it: ATA on GSI 14 and 15 went
 * from class 2 to class 6, and the xHCI message went to class 6 rather than
 * the 7 it used to be compiled with. That is not a slip in the boundaries —
 * it is the whole point working. A line lands in the class its CONSEQUENCE
 * puts it in, and a disk asking for "work is redone" belongs above a keyboard
 * whatever number it happened to carry before.
 *
 * The one move worth naming on its own is the tick, and it moves UP: at
 * vector 32 it sat in the lowest class in the machine, so when a disk
 * interrupt and a tick were pending together the LAPIC delivered the disk
 * first, every time. Nobody decided that — it fell out of "vector = 32 + GSI"
 * and the fact that the timer is GSI 0.
 *
 * What does NOT change is the ceiling: nothing a device asks for can land
 * above the class the kernel keeps for stopping, rescheduling and shooting
 * down a TLB. That bound is enforced by where the pool ends, not by where
 * these boundaries sit. */
typedef struct {
    uint8_t     first_class;
    uint8_t     last_class;
    const char *name;
    const char *cost;
} RightOfWayRow;

static const RightOfWayRow g_rows[RIGHT_OF_WAY_ROWS] = {
    [RIGHT_OF_WAY_A_PERSON_WAITS] = {
        .first_class = 2, .last_class = 5,
        .name = "a person waits",
        .cost = "nothing breaks and no work is lost; somebody notices"
    },
    [RIGHT_OF_WAY_WORK_IS_REDONE] = {
        .first_class = 6, .last_class = 9,
        .name = "work is redone",
        .cost = "a transfer in flight reaches its own deadline and is done again"
    },
    [RIGHT_OF_WAY_TIME_IS_LOST] = {
        .first_class = 10, .last_class = 14,
        .name = "time is lost",
        .cost = "the kernel cannot get it back; every deadline hangs off this line"
    },
};

/* One bit per IDT entry. 256 is not a size somebody chose — it is how many
 * gates an IDT has (SDM Vol 3A §6.10), so the map cannot be the wrong length
 * for the table it describes. */
#define POOL_BITS  256
static uint64_t     g_taken[POOL_BITS / 64];

/* WHO holds each vector, kept as TEXT rather than as a pointer.
 *
 * It was a `const char *` first, and that was a bug with a long fuse: the
 * settling path builds its caller's name on its own stack — "PCI GSI 19",
 * assembled into a local buffer — and storing the pointer left this table
 * holding an address inside a frame that returned. The name printed correctly
 * for as long as nothing else had used that stack, which is to say it printed
 * correctly every time anybody looked, and would have printed somebody else's
 * bytes on the boot where it mattered.
 *
 * The width is what a name here is: an interrupt line named by its bus and
 * number, or a controller named by its address — "the spurious vector, which
 * the SDM forbids routing anything to" is the longest this file writes, and
 * it is a sentence rather than a name. Longer ones are cut, and cut visibly,
 * because a truncated name that says so is worth more than a whole one that
 * might be rubble. */
#define POOL_HOLDER_CHARS 72
static char         g_holder[POOL_BITS][POOL_HOLDER_CHARS];
static spinlock_t   g_pool_lock;
static bool         g_pool_ready;

/* Copy a name in, and say so if it did not fit. */
static void HolderSet(uint8_t vector, const char *who)
{
    if (!who) who = "an unnamed caller";
    size_t n = 0;
    while (who[n] && n < POOL_HOLDER_CHARS - 1) { g_holder[vector][n] = who[n]; n++; }
    g_holder[vector][n] = '\0';
    if (who[n]) {
        kprintf("[Vectors] the name of whoever holds vector 0x%02x is longer "
                "than this table keeps and is written down as \"%s\"\n",
                (unsigned)vector, g_holder[vector]);
    }
}

static inline void HolderClear(uint8_t vector) { g_holder[vector][0] = '\0'; }

static inline bool BitIsSet(uint8_t v)  { return (g_taken[v >> 6] >> (v & 63)) & 1u; }
static inline void BitSet(uint8_t v)    { g_taken[v >> 6] |= (1ull << (v & 63)); }
static inline void BitClear(uint8_t v)  { g_taken[v >> 6] &= ~(1ull << (v & 63)); }

/* Spoken for before anybody asks, with the reason attached to each one so a
 * dump of this table explains itself. */
static void PoolSeatTheKernelsOwn(void)
{
    for (uint32_t v = 0; v < POOL_FLOOR_VECTOR; v++) {
        BitSet((uint8_t)v);
        HolderSet((uint8_t)v, "the architecture");
    }
    for (uint32_t v = POOL_CEILING_VECTOR + 1u; v < POOL_BITS; v++) {
        BitSet((uint8_t)v);
        HolderSet((uint8_t)v, "the kernel's own");
    }

    BitSet(SYSCALL_VECTOR);
    HolderSet((uint8_t)SYSCALL_VECTOR, "the syscall door");

    BitSet(IPI_WAKE_VECTOR);      HolderSet((uint8_t)IPI_WAKE_VECTOR, "the wake IPI");
    BitSet(IPI_SHOOTDOWN_VECTOR); HolderSet((uint8_t)IPI_SHOOTDOWN_VECTOR, "the TLB shootdown IPI");
    BitSet(IPI_PANIC_VECTOR);     HolderSet((uint8_t)IPI_PANIC_VECTOR, "the panic IPI");
    BitSet(LAPIC_TIMER_VECTOR);   HolderSet((uint8_t)LAPIC_TIMER_VECTOR, "the LAPIC timer");

    /* The spurious vector gets its own sentence because the prohibition is
     * explicit and the failure is silent: SDM Vol 3A §11.9 says the spurious
     * vector must not be programmed into an LVT entry or an IOAPIC redirection
     * entry. A line delivered under it is acknowledged by nobody, its ISR bit
     * stays set, and every class at or below 15 goes deaf — which on this
     * kernel means the panic IPI and the TLB shootdown stop arriving. */
    BitSet(LAPIC_SPURIOUS_VECTOR);
    HolderSet((uint8_t)LAPIC_SPURIOUS_VECTOR, "the spurious vector, which the SDM forbids routing anything to");
}

static uint32_t PoolFreeInRow(const RightOfWayRow *row)
{
    uint32_t free_here = 0;
    for (uint32_t v = ClassFirstVector(row->first_class);
         v <= ClassLastVector(row->last_class); v++) {
        if (!BitIsSet((uint8_t)v)) free_here++;
    }
    return free_here;
}

void VectorPoolInit(void)
{
    if (g_pool_ready) {
        /* Seeding twice would forget everything already handed out, and the
         * lines holding those vectors would go on delivering under numbers the
         * pool believed were free. Refused out loud rather than quietly
         * tolerated: a second call is a mistake in the order of boot, and the
         * one time it happened the seeding ran once per core from idt_load. */
        kprintf("[Vectors] %[E]the pool has already been seeded and will not "
                "be seeded again — a second seeding forgets every vector "
                "already handed out%[D]\n");
        return;
    }

    spinlock_init(&g_pool_lock);
    memset(g_taken, 0, sizeof(g_taken));
    memset(g_holder, 0, sizeof(g_holder));
    PoolSeatTheKernelsOwn();
    g_pool_ready = true;

    kprintf("[Vectors] a line asks for right of way, not a number: vectors "
            "%u..%u are the pool, %u of them free after the kernel has taken "
            "its own\n",
            (unsigned)POOL_FLOOR_VECTOR, (unsigned)POOL_CEILING_VECTOR,
            (unsigned)(PoolFreeInRow(&g_rows[0]) + PoolFreeInRow(&g_rows[1]) +
                       PoolFreeInRow(&g_rows[2])));

    for (uint32_t r = 0; r < RIGHT_OF_WAY_ROWS; r++) {
        const RightOfWayRow *row = &g_rows[r];
        kprintf("[Vectors]   \"%s\" — %s — priority classes %u to %u, "
                "vectors 0x%02x..0x%02x, %u free\n",
                row->name, row->cost,
                (unsigned)row->first_class, (unsigned)row->last_class,
                (unsigned)ClassFirstVector(row->first_class),
                (unsigned)ClassLastVector(row->last_class),
                (unsigned)PoolFreeInRow(row));
    }
}

const char *VectorPoolHolder(uint8_t vector)
{
    return g_holder[vector][0] ? g_holder[vector] : NULL;
}

uint8_t VectorPoolTake(RightOfWay row, const char *who)
{
    if (!who) who = "an unnamed caller";

    if (!g_pool_ready) {
        kprintf("[Vectors] %s asked for right of way before the pool was "
                "seeded — nothing can be handed out yet, and this is a call "
                "in the wrong order rather than a machine that ran out\n", who);
        return 0;
    }
    if ((uint32_t)row >= RIGHT_OF_WAY_ROWS) {
        kprintf("[Vectors] %s asked for a right of way this kernel does not "
                "name (%u) — refused\n", who, (unsigned)row);
        return 0;
    }

    const RightOfWayRow *r = &g_rows[row];
    uint8_t found = 0;

    spin_lock(&g_pool_lock);
    /* Bottom up inside the row. Within one class the larger low nibble wins
     * (SDM Vol 3A §11.8.3), so filling from the bottom means the line that
     * asked first is the one that yields — and "asked first" is the line the
     * kernel brought up earliest, which is the one that has been working the
     * longest and can most afford to wait a moment. */
    for (uint32_t v = ClassFirstVector(r->first_class);
         v <= ClassLastVector(r->last_class); v++) {
        if (!BitIsSet((uint8_t)v)) {
            BitSet((uint8_t)v);
            HolderSet((uint8_t)v, who);
            found = (uint8_t)v;
            break;
        }
    }
    spin_unlock(&g_pool_lock);

    if (!found) {
        /* Loud, and naming the row rather than the shortage. Sliding into the
         * next row would hand this line a right of way its driver never asked
         * for, and nobody would ever find out. */
        kprintf("[Vectors] %s asked for \"%s\" and there is no vector left in "
                "that row — classes %u to %u are full. Refusing rather than "
                "moving it to another row: a line that is given a right of way "
                "it did not ask for is worse than a line that is told no\n",
                who, r->name, (unsigned)r->first_class, (unsigned)r->last_class);
        return 0;
    }

    kprintf("[Vectors] %s takes vector 0x%02x — \"%s\", so if it is late, %s\n",
            who, (unsigned)found, r->name, r->cost);
    return found;
}

bool VectorPoolClaim(uint8_t vector, const char *who)
{
    if (!who) who = "an unnamed caller";

    if (!g_pool_ready) {
        kprintf("[Vectors] %s claimed vector 0x%02x before the pool was "
                "seeded\n", who, (unsigned)vector);
        return false;
    }

    bool ok;
    spin_lock(&g_pool_lock);
    ok = !BitIsSet(vector);
    if (ok) {
        BitSet(vector);
        HolderSet(vector, who);
    }
    spin_unlock(&g_pool_lock);

    if (!ok) {
        kprintf("[Vectors] %s cannot have vector 0x%02x — %s holds it. Two "
                "owners of one vector is not a sharing arrangement: the IOAPIC "
                "clears Remote IRR on an EOI with a matching vector (82093AA "
                "§3.2.4), so one line's acknowledgement would release the "
                "other's, and one of the two would stop being served\n",
                who, (unsigned)vector,
                g_holder[vector][0] ? g_holder[vector] : "something unnamed");
    }
    return ok;
}

void VectorPoolGiveBack(uint8_t vector, const char *who)
{
    if (!who) who = "an unnamed caller";

    if (vector < POOL_FLOOR_VECTOR || vector > POOL_CEILING_VECTOR) {
        kprintf("[Vectors] %s tried to give back vector 0x%02x, which was "
                "never the pool's to hand out — %s holds it and always will\n",
                who, (unsigned)vector,
                g_holder[vector][0] ? g_holder[vector] : "the architecture");
        return;
    }

    spin_lock(&g_pool_lock);
    bool was_held = BitIsSet(vector);
    if (was_held) {
        BitClear(vector);
        HolderClear(vector);
    }
    spin_unlock(&g_pool_lock);

    if (!was_held) {
        kprintf("[Vectors] %s gave back vector 0x%02x, which nobody was "
                "holding — either it was returned twice, or it was never "
                "taken, and both mean a driver's idea of what it owns has "
                "come apart from this table\n", who, (unsigned)vector);
        return;
    }

    kprintf("[Vectors] %s gives back vector 0x%02x\n", who, (unsigned)vector);
}
