#include "lock_rank.h"
#include "amp.h"
#include "atomics.h"
#include "nameplate.h"
#include "nameplate_format.h"

/*
 * What each core holds, and every place already named.
 *
 * A core's record is written only by that core, with its interrupts off: a
 * push right after the spinlock is taken, a pop right before it is let go, and
 * every read of it made with interrupts off too, so a thread that could be
 * moved to another core between reading "which core am I" and reading the
 * record cannot mistake another core's locks for its own. Nothing here takes
 * the state of IF for an answer: whether a TagFS lock is held is read off the
 * record, never off the interrupt flag.
 *
 * The deepest legal chain is three, one lock of each rank: cow -> state ->
 * one leaf — a leaf under a leaf is itself what LockRank names. The record
 * holds more than twice that, so that the takes of a chain gone wrong are still
 * named; a ninth lock is counted in Unwatched and said, so the record never
 * lies about what it cannot hold.
 */
#define LOCK_RANK_DEPTH   8
#define LOCK_RANK_PLACES  256

typedef struct {
    TagFSLock *Held[LOCK_RANK_DEPTH];   /* outermost first */
    uint8_t    Depth;
    uint8_t    Unwatched;               /* taken past LOCK_RANK_DEPTH, not in Held[] */
    uint32_t   Witnessed;
    uint32_t   OutOfOrder;
    uint32_t   Named;
} __attribute__((aligned(64))) LockRankCore;

static LockRankCore g_lock_rank[MAX_CORES];

/* A place is what a line names: its kind and up to four words (lock names,
 * __func__ of takes, the door's op). Named once per boot — the table is keyed
 * by the TEXT, so two copies of one string literal are one place — and after
 * LOCK_RANK_PLACES distinct places the rest are counted and not named, which
 * is said once.
 *
 * A place already named is found without a lock: slots are only ever filled,
 * never emptied, and a slot's Hash is published last, so a reader that sees
 * it sees the words under it. That is the path every door takes while a lock
 * is held. The table's own lock is taken only to fill a slot; it is below
 * every rank — nothing is taken under it and nothing is printed while it is
 * held. */
typedef enum {
    LOCK_RANK_PLACE_IO = 1,
    LOCK_RANK_PLACE_ORDER,
    LOCK_RANK_PLACE_TWICE,
    LOCK_RANK_PLACE_UNRANKED,
    LOCK_RANK_PLACE_DEPTH,
    LOCK_RANK_PLACE_STRANGER
} LockRankPlaceKind;

typedef struct {
    uint32_t    Hash;      /* 0: the slot is free */
    uint8_t     Kind;
    const char *Text[4];
} LockRankPlace;

typedef enum { LOCK_RANK_KNOWN, LOCK_RANK_NEW, LOCK_RANK_FULL } LockRankAnswer;

static LockRankPlace g_lock_rank_places[LOCK_RANK_PLACES];
static bool          g_lock_rank_full_said;
static spinlock_t    g_lock_rank_places_lock;

/* Whether a place whose locks are `a` and `b` (NULL for none) stops the
 * machine: in a STRICT kernel, unless a proof's own lock is one of them. */
static bool LockRankStops(const TagFSLock *a, const TagFSLock *b)
{
#if CONFIG_STRICT
    return !(a && a->Proof) && !(b && b->Proof);
#else
    (void)a;
    (void)b;
    return false;
#endif
}

/* The one line a place is named in — or, where the place stops the machine,
 * the panic that names it in the same words. */
#define LOCK_RANK_SAY(stop, ...)       \
    do {                               \
        if (stop)                      \
            panic(__VA_ARGS__);        \
        kprintf(__VA_ARGS__);          \
    } while (0)

static inline uint64_t LockRankIrqOff(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

/* Only IF is touched between the two, so putting it back is an sti when it was
 * set, and nothing at all for a holder whose interrupts are already off — the
 * common case, since a TagFS lock is an interrupts-off spinlock. */
static inline void LockRankIrqRestore(uint64_t flags)
{
    if (flags & (1u << 9))
        asm volatile("sti" ::: "memory");
}

static inline LockRankCore *LockRankHere(void)
{
    return &g_lock_rank[amp_get_core_index()];
}

static const char *LockRankWord(const char *s)
{
    return s ? s : "unnamed";
}

static bool LockRankSameText(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

static uint32_t LockRankHash(uint8_t kind, const char *const text[4])
{
    uint32_t h = (2166136261u ^ kind) * 16777619u;
    for (uint32_t i = 0; i < 4; i++) {
        for (const char *s = text[i]; s && *s; s++)
            h = (h ^ (uint8_t)*s) * 16777619u;
        h = (h ^ 0xFFu) * 16777619u;
    }
    return h ? h : 1u;
}

/* Open addressing: places are never removed, so a known place is always met
 * before the first free slot on its probe path. Returns the place's slot, or
 * the free slot its probe path ends at, or NULL when every slot is taken. */
static LockRankPlace *LockRankProbe(uint32_t hash, uint8_t kind, const char *const text[4],
                                    bool *known)
{
    uint32_t at = hash & (LOCK_RANK_PLACES - 1);

    *known = false;
    for (uint32_t probe = 0; probe < LOCK_RANK_PLACES; probe++) {
        LockRankPlace *p = &g_lock_rank_places[at];
        uint32_t seen = __atomic_load_n(&p->Hash, __ATOMIC_ACQUIRE);
        if (seen == 0)
            return p;
        if (seen == hash && p->Kind == kind &&
            LockRankSameText(p->Text[0], text[0]) && LockRankSameText(p->Text[1], text[1]) &&
            LockRankSameText(p->Text[2], text[2]) && LockRankSameText(p->Text[3], text[3])) {
            *known = true;
            return p;
        }
        at = (at + 1) & (LOCK_RANK_PLACES - 1);
    }
    return NULL;
}

/* Is this place new? Looked up without the lock first; filled under it, after
 * looking again, since another core may have filled it in between. */
static LockRankAnswer LockRankPlaceNew(uint8_t kind, const char *a, const char *b,
                                       const char *c, const char *d)
{
    const char *text[4] = { a, b, c, d };
    uint32_t hash = LockRankHash(kind, text);
    LockRankAnswer answer = LOCK_RANK_KNOWN;
    bool known;

    LockRankPlace *p = LockRankProbe(hash, kind, text, &known);
    if (known || (!p && __atomic_load_n(&g_lock_rank_full_said, __ATOMIC_ACQUIRE)))
        return LOCK_RANK_KNOWN;

    spin_lock(&g_lock_rank_places_lock);
    p = LockRankProbe(hash, kind, text, &known);
    if (p && !known) {
        p->Kind = kind;
        for (uint32_t i = 0; i < 4; i++)
            p->Text[i] = text[i];
        __atomic_store_n(&p->Hash, hash, __ATOMIC_RELEASE);
        answer = LOCK_RANK_NEW;
    } else if (!p && !g_lock_rank_full_said) {
        __atomic_store_n(&g_lock_rank_full_said, true, __ATOMIC_RELEASE);
        answer = LOCK_RANK_FULL;
    }
    spin_unlock(&g_lock_rank_places_lock);
    return answer;
}

/* Whether this core names the place: yes the first time it is met anywhere,
 * and the one time the table fills, in which case the line said is that one. */
static bool LockRankShouldSay(LockRankCore *core, uint8_t kind, const char *a,
                              const char *b, const char *c, const char *d)
{
    LockRankAnswer answer = LockRankPlaceNew(kind, a, b, c, d);
    if (answer == LOCK_RANK_KNOWN)
        return false;
    core->Named++;
    if (answer == LOCK_RANK_FULL) {
        kprintf("[LockRank] %u places have been named; later ones are counted, not named\n",
                (unsigned)LOCK_RANK_PLACES);
        return false;
    }
    return true;
}

/* The function a door's return address is in, as "<sym>+0x<hex>", or the bare
 * address when the kernel's Nameplate cannot say. The address saved is PAST
 * the call, so the byte before it is the one named (idt.c, panic_name_addr). */
extern const char __nameplate_start[];
extern const char __nameplate_end[];

static void LockRankNameOf(uintptr_t addr, char *out, size_t size)
{
    NameplateName name;
    uint64_t bytes = (uint64_t)(__nameplate_end - __nameplate_start);

    if (addr && bytes >= sizeof(NameplateHeader) &&
        nameplate_name_at_kernel((uintptr_t)__nameplate_start, bytes, addr - 1, &name))
        ksnprintf(out, size, "%s+0x%lx", name.Text, (unsigned long)(name.Offset + 1));
    else
        ksnprintf(out, size, "0x%lx", (unsigned long)addr);
}

static void LockRankSayIo(LockRankCore *core, const TagFSLock *held, const char *op,
                          uintptr_t asked_by)
{
    char who[NAMEPLATE_NAME_MAX + 24];
    bool stop = LockRankStops(held, NULL);

    if (!LockRankShouldSay(core, LOCK_RANK_PLACE_IO, held->Name, held->HeldBy, op, NULL) &&
        !stop)
        return;
    LockRankNameOf(asked_by, who, sizeof(who));
    if (strcmp(op, "wait") == 0)
        LOCK_RANK_SAY(stop, "[LockRank] %s is held across a wait for a Scribe: taken in %s, "
                            "the wait asked for by %s\n",
                      LockRankWord(held->Name), LockRankWord(held->HeldBy), who);
    else
        LOCK_RANK_SAY(stop, "[LockRank] %s is held across a medium %s: taken in %s, "
                            "the %s asked for by %s\n",
                      LockRankWord(held->Name), op, LockRankWord(held->HeldBy), op, who);
}

/* A lock with no rank, the lock this core already holds, and every lock held
 * here whose rank is not strictly above the one asked for. Interrupts off. */
static void LockRankCheckHere(LockRankCore *core, TagFSLock *lock, const char *site)
{
    bool broke = false;

    if (lock->Rank == LOCK_RANK_NONE &&
        LockRankShouldSay(core, LOCK_RANK_PLACE_UNRANKED, site, NULL, NULL, NULL))
        kprintf("[LockRank] a TagFS lock with no rank taken in %s\n", site);

    for (uint8_t i = 0; i < core->Depth; i++) {
        const TagFSLock *h    = core->Held[i];
        bool             stop = LockRankStops(lock, h);
        if (h == lock) {
            if (LockRankShouldSay(core, LOCK_RANK_PLACE_TWICE, lock->Name, site, h->HeldBy, NULL) ||
                stop)
                LOCK_RANK_SAY(stop, "[LockRank] %s taken in %s while this core already holds it, "
                                    "taken in %s — this core now waits for itself\n",
                              LockRankWord(lock->Name), site, LockRankWord(h->HeldBy));
            continue;
        }
        if (lock->Rank == LOCK_RANK_NONE || h->Rank < lock->Rank)
            continue;
        broke = true;
        if (LockRankShouldSay(core, LOCK_RANK_PLACE_ORDER, h->Name, h->HeldBy, lock->Name, site) ||
            stop)
            LOCK_RANK_SAY(stop, "[LockRank] %s taken in %s under %s, taken in %s — out of order\n",
                          LockRankWord(lock->Name), site, LockRankWord(h->Name),
                          LockRankWord(h->HeldBy));
    }
    if (broke) {
        core->OutOfOrder++;
        __atomic_add_fetch(&lock->OutOfOrder, 1u, __ATOMIC_RELAXED);
    }
}

/* Right after the spinlock is taken, interrupts off. */
static void LockRankPush(LockRankCore *core, TagFSLock *lock, const char *site)
{
    lock->HeldBy = site;
    if (core->Depth < LOCK_RANK_DEPTH) {
        core->Held[core->Depth++] = lock;
        return;
    }
    core->Unwatched++;
    if (LockRankShouldSay(core, LOCK_RANK_PLACE_DEPTH, lock->Name, site, NULL, NULL))
        kprintf("[LockRank] core %u holds more than %u TagFS locks: %s taken in %s is not watched\n",
                (unsigned)amp_get_core_index(), (unsigned)LOCK_RANK_DEPTH,
                LockRankWord(lock->Name), site);
}

void TagFSLockInit(TagFSLock *lock, const char *name, LockRankLevel rank)
{
    spinlock_init(&lock->Spin);
    lock->Name       = name;
    lock->HeldBy     = NULL;
    lock->Rank       = (uint8_t)rank;
    lock->Proof      = false;
    lock->Witnessed  = 0;
    lock->OutOfOrder = 0;
}

/* The lock is tried first. Taken at once, nothing waited, and the order is
 * checked right after — the same check, with the interrupts the spinlock has
 * already turned off. Refused, the take is about to spin, and the check comes
 * BEFORE the spin: a core taking a lock it holds, or one side of an ABBA, is
 * named before it could wedge, since only a spin can. */
void TagFSLockTakeAt(TagFSLock *lock, const char *site)
{
    LockRankCore *core;

    if (!spin_trylock(&lock->Spin)) {
        uint64_t flags = LockRankIrqOff();
        LockRankCheckHere(LockRankHere(), lock, site);
        LockRankIrqRestore(flags);
        spin_lock(&lock->Spin);
        LockRankPush(LockRankHere(), lock, site);
        return;
    }
    core = LockRankHere();
    LockRankCheckHere(core, lock, site);
    LockRankPush(core, lock, site);
}

/* A try that fails waits for nobody and breaks no order: only a take that
 * succeeded is checked and recorded. A caller that tries again and again is
 * waiting all the same, and is a take, not a loop of these. */
bool TagFSLockTryAt(TagFSLock *lock, const char *site)
{
    LockRankCore *core;

    if (!spin_trylock(&lock->Spin))
        return false;
    core = LockRankHere();
    LockRankCheckHere(core, lock, site);
    LockRankPush(core, lock, site);
    return true;
}

/* Searched from the top, so a lock let go out of the order it was taken in is
 * still found. No caller does that today — every release is the last take's —
 * and a record that could not survive one would name a stranger for every
 * lock taken after it. */
void TagFSLockDropAt(TagFSLock *lock, const char *site)
{
    uint64_t flags = LockRankIrqOff();
    LockRankCore *core = LockRankHere();
    int found = -1;

    for (int i = (int)core->Depth - 1; i >= 0; i--) {
        if (core->Held[i] == lock) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        for (uint8_t i = (uint8_t)found; i + 1 < core->Depth; i++)
            core->Held[i] = core->Held[i + 1];
        core->Depth--;
        core->Held[core->Depth] = NULL;
    } else if (core->Unwatched > 0) {
        core->Unwatched--;
    } else if (LockRankShouldSay(core, LOCK_RANK_PLACE_STRANGER, lock->Name, site, NULL, NULL)) {
        kprintf("[LockRank] %s let go in %s on a core that has no record of taking it\n",
                LockRankWord(lock->Name), site);
    }
    lock->HeldBy = NULL;
    LockRankIrqRestore(flags);
    spin_unlock(&lock->Spin);
}

void LockRankWitness(const char *op, uintptr_t asked_by)
{
    uint64_t flags = LockRankIrqOff();
    LockRankCore *core = LockRankHere();

    if (core->Depth == 0 && core->Unwatched == 0) {
        LockRankIrqRestore(flags);
        return;
    }
    core->Witnessed++;
    for (uint8_t i = 0; i < core->Depth; i++) {
        TagFSLock *h = core->Held[i];
        __atomic_add_fetch(&h->Witnessed, 1u, __ATOMIC_RELAXED);
        LockRankSayIo(core, h, op, asked_by);
    }
    LockRankIrqRestore(flags);
}

bool LockRankHeldHere(const TagFSLock *lock)
{
    uint64_t flags = LockRankIrqOff();
    LockRankCore *core = LockRankHere();
    bool held = false;

    for (uint8_t i = 0; i < core->Depth; i++) {
        if (core->Held[i] == lock) {
            held = true;
            break;
        }
    }
    LockRankIrqRestore(flags);
    return held;
}

bool LockRankAnyHeldHere(void)
{
    uint64_t flags = LockRankIrqOff();
    LockRankCore *core = LockRankHere();
    bool any = core->Depth > 0 || core->Unwatched > 0;
    LockRankIrqRestore(flags);
    return any;
}

LockRankCount LockRankCountHere(void)
{
    uint64_t flags = LockRankIrqOff();
    LockRankCore *core = LockRankHere();
    LockRankCount count = { core->Witnessed, core->OutOfOrder, core->Named };
    LockRankIrqRestore(flags);
    return count;
}
