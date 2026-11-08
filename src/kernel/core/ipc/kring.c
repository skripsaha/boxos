/*
 * KRing — kernel-side helpers for the lazy-growable Pocket/Result rings.
 *
 * The header sits at proc->pocket_ring_phys / proc->result_ring_phys (one
 * physical page each, accessible to the kernel via vmm_phys_to_virt). The
 * slot region is a separately reserved virtual range whose pages are mapped
 * on demand: PocketRing slots fault in via the user page-fault handler;
 * ResultRing slots are mapped proactively here in KResultPush.
 *
 * ResultRing is MPSC: multiple K-Cores can land in KResultPush concurrently
 * for the same target (e.g. two senders deliver IPC replies, plus the
 * sender's own manifest confirmation, all racing for one cabin's ring).
 * The producer side uses a per-slot Vyukov-style generation counter so the
 * slot[tail] write cannot stomp another producer's payload, and consumers
 * cannot read a half-written slot.
 */

#include "kring.h"
#include "chit.h"       /* ChitKeep — an answer redeems the chit left for it */
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "nightwatch.h"   /* a core mid-delivery is not an idle core */
#include "result.h"
#include "kresult.h"
#include "atomics.h"
#include "error.h"
#include "boxos_magic.h"
#include "amp.h"
#include "scheduler.h"   /* sched_ring_home — one transition, one bell */

void KRingPocketInitAt(PocketRing *hdr, uint64_t slots_base, uint32_t slot_count_max)
{
    if (!hdr) return;
    /* Straddle invariant: every slot translation must stay inside one page,
     * which requires the per-strand Hammock slots_base (a runtime VA, not the
     * page-aligned cabin_layout.h constant) to be page-aligned. Geometry
     * proves the per-slot translation is straddle-safe only on top of this. */
    if (slots_base & (VMM_PAGE_SIZE - 1))
        panic("ring slots_base 0x%lx not page-aligned - straddle invariant broken",
              (unsigned long)slots_base);
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = slots_base;
    hdr->hdr.slot_size      = POCKET_SLOT_SIZE;
    hdr->hdr.slot_count_max = slot_count_max;
    hdr->hdr.magic          = POCKET_RING_MAGIC;
}

void KRingResultInitAt(ResultRing *hdr, uint64_t slots_base, uint32_t slot_count_max)
{
    if (!hdr) return;
    if (slots_base & (VMM_PAGE_SIZE - 1))
        panic("ring slots_base 0x%lx not page-aligned - straddle invariant broken",
              (unsigned long)slots_base);
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = slots_base;
    hdr->hdr.slot_size      = RESULT_SLOT_SIZE;
    hdr->hdr.slot_count_max = slot_count_max;
    hdr->hdr.magic          = RESULT_RING_MAGIC;
}

void KRingPocketInit(PocketRing *hdr)
{
    KRingPocketInitAt(hdr, CABIN_POCKET_SLOTS_BASE, (uint32_t)POCKET_RING_SLOT_MAX);
}

void KRingResultInit(ResultRing *hdr)
{
    KRingResultInitAt(hdr, CABIN_RESULT_SLOTS_BASE, (uint32_t)RESULT_RING_SLOT_MAX);
}

/* -------------------------------------------------------------------------
 * PocketRing consumer
 * ------------------------------------------------------------------------- */

static PocketRing *kring_pocket_hdr(process_t *proc)
{
    /* P5a: route by the PER-STRAND ring (proc->pocket_ring_phys), which for
     * the main strand aliases the cabin ring and for a spawned strand is its
     * own Hammock-carved ring. The cabin guard stays because downstream paths
     * (KPocketPeek's porthole) deref the shared proc->cabin->vmm. */
    if (!proc || !proc->cabin || !proc->pocket_ring_phys) return NULL;
    return (PocketRing *)vmm_phys_to_virt(proc->pocket_ring_phys);
}

/* Reads of the producer-side cursor (tail) MUST use ACQUIRE so the
 * consumer sees the producer's slot store that preceded the tail bump.
 * On x86 TSO plain volatile reads happen to behave like ACQUIRE, but
 * the explicit semantics keep us honest on weaker memory models and
 * defeat any compiler reordering across the load. Audit 2026-04-29. */
bool KPocketIsEmpty(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return true;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    return head == tail;
}

uint32_t KPocketCount(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return 0;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t n = tail - head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

bool KPocketPeek(process_t *proc, uint64_t *pos_out, Pocket *out)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return false;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;
    if (pos_out) *pos_out = head;

    Porthole porthole;
    const Pocket *slot = PortholeOpen(&porthole, proc->cabin->vmm,
                                      pocket_ring_slot_uvaddr(r, head),
                                      sizeof(Pocket), /*for_write=*/false);
    if (!slot) return false;
    memcpy(out, slot, sizeof(*out));
    PortholeClose(&porthole);
    return true;
}

/* Take the ring past ONE named position, and only while it is still the head.
 *
 * The guide is the ring's only consumer now, but the pop still names the
 * position the caller looked at and takes nothing else. It learned to on a
 * machine frozen by a second consumer: the syscall gate used to pop a YIELD
 * pocket it found at the head, and when the guide had just taken that yield
 * itself, a pop that moved head from "whatever it reads now" took the pocket
 * BEHIND it, unread — a system.broadcast whose owner then waited for an
 * answer to a question nobody had opened (BIOS 16c, head 0xcbf, the pocket at
 * 0xcbe with pid still 0). The yield no longer enters the ring (GATE_YIELD),
 * and a compare-and-swap from the position actually seen is the shape that
 * cannot step over anything, whoever else may ever look. The RELEASE keeps the
 * userspace producer's view of "room in the ring" behind the consumed slot. */
bool KPocketPopAt(process_t *proc, uint64_t pos)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return false;
    uint64_t expected = pos;
    return __atomic_compare_exchange_n(&r->hdr.head, &expected, pos + 1,
                                       /*weak=*/false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* -------------------------------------------------------------------------
 * ResultRing producer
 * ------------------------------------------------------------------------- */

static ResultRing *kring_result_hdr(process_t *proc)
{
    /* P5a: per-strand ResultRing (see kring_pocket_hdr). */
    if (!proc || !proc->cabin || !proc->result_ring_phys) return NULL;
    return (ResultRing *)vmm_phys_to_virt(proc->result_ring_phys);
}

/* A porthole onto one ResultSlot of `target`'s cabin; NULL when the slot is
 * not the cabin's to read — or, for_write, to write. */
static ResultSlot *kring_slot_open(Porthole *porthole, process_t *target,
                                   uintptr_t uvaddr, bool for_write)
{
    return PortholeOpen(porthole, target->cabin->vmm, uvaddr,
                        sizeof(ResultSlot), for_write);
}

/* The cloakroom token of the submit a record ANSWERS, or 0 when it answers
 * none. An answer is the kernel's own — no sender — and carries the token of
 * the submit it answers. A record from another strand is a message even when
 * it carries a token: a question (KCTX_ASK) names its asker's, for the carrier
 * to answer by, and nobody is parked on it. boxlib routes the same way, the
 * sender first (result.c, route_of). */
static inline uint32_t kring_answer_token(uint32_t sender_pid, uint64_t context)
{
    return sender_pid == 0 ? KCTX_COOKIE24((uint32_t)context) : 0u;
}

/* True when the process's ResultRing holds a PUBLISHED, unconsumed entry
 * that answers a submit (kring_answer_token) — an answer to a synchronous
 * submit its owner has not read yet. Such a process must never
 * be committed to PROC_WAITING: every kernel-parked wait (addr_park,
 * touch_await, storage) ends exactly when this reply is consumed, so
 * sleeping past it is the "undelivered result" Nightwatch names — the
 * lost-wake wedge. process_set_state uses this as the final futex-style
 * re-check at the single place sleep is committed. Reads only the ring
 * header and slot seq/context; the owner's userspace is suspended for the
 * duration of the syscall that is asking, so nothing races the scan. */
bool KResultRingHasPendingReply(process_t *proc)
{
    ResultRing *rr = kring_result_hdr(proc);
    if (!rr) return false;
    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) return false;

    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;
    /* The scan runs under the strand's state_lock, interrupts off, and every
     * bound in the header is the cabin's to write: head, tail and
     * slot_count_max. A cabin that says its ring holds four billion replies
     * would hold this core for minutes. The ring has the slots the kernel made
     * it with (result_ring_slots), and there is never more than that to look
     * at — a lie beyond it only cuts the liar's own scan short. */
    uint64_t scan_end = tail;
    if (scan_end - head > cap) scan_end = head + cap;
    if (scan_end - head > proc->result_ring_slots) scan_end = head + proc->result_ring_slots;

    for (uint64_t pos = head; pos < scan_end; pos++) {
        Porthole    porthole;
        ResultSlot *slot = kring_slot_open(&porthole, proc,
                                           result_ring_slot_uvaddr(rr, pos), false);
        if (!slot) continue;
        uint64_t seq     = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        uint64_t context = slot->r.context;
        uint32_t sender  = slot->r.sender_pid;
        PortholeClose(&porthole);
        if (seq != 2u * (pos / cap) + 1u)
            continue;                    /* in-flight reservation or consumed */
        if (kring_answer_token(sender, context) != 0) return true;
    }
    return false;
}

/* True when the slot at the ResultRing's HEAD is published and unconsumed —
 * a delivery its owner can pop this instant.
 *
 * Turn In asks this before it lets a strand lie down, and again after the
 * park (turnin_ops.c), because the cursors cannot see it: a producer claims
 * `tail` first and releases the slot's seq later, and between the two a
 * strand can take its mark (tail already moved), look (seq not released —
 * nothing to pop), and ask to sleep with a mark that already covers the
 * arrival. Nobody publishes that slot twice, so a sleep committed across it
 * is a sleep nobody ends. Measured on BIOS 16c (2026-09-06): the console
 * daemon parked at result 36 with pos 35 published and unread, and a printing
 * child waited for its lane grant for the rest of the run.
 *
 * Only the head slot is judged, and only a RELEASED one counts. A claim still
 * in flight at the head is deliberately not a reason to refuse a sleep: its
 * producer releases the seq and then reads the owner's state (step 9 above),
 * so a strand parked across it is woken by that very producer. The released
 * slot is the one no one comes back for. */
bool KResultRingHasUnreadAtHead(process_t *proc)
{
    ResultRing *rr = kring_result_hdr(proc);
    if (!rr) return false;
    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) return false;

    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;

    Porthole    porthole;
    ResultSlot *slot = kring_slot_open(&porthole, proc,
                                       result_ring_slot_uvaddr(rr, head), false);
    if (!slot) return false;
    uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    PortholeClose(&porthole);
    return seq == 2u * (head / cap) + 1u;
}

/* The two refusals that are said out loud are rate-limited by their own
 * counts; every other outcome of a push is a fact about ONE answer, and the
 * chit that answer redeems (or fails to) is where it is recorded now. */
static volatile uint64_t g_krp_full;
static volatile uint64_t g_krp_seq_broken;
static volatile uint64_t g_krp_refused;
static volatile uint64_t g_krp_token_astray;

/* A refusal is not a statistic when an ANSWER is being refused: somebody is
 * parked on it, every caller of KResultPush drops the return value, and the
 * strand would simply never wake — Nightwatch then finds a token nobody left
 * a chit for and can only say "the work was taken and nobody promised the
 * answer". Every refusal says why, names the victim and the token. Refused
 * unsolicited traffic is counted and said once in sixty-four, so a saturated
 * ring cannot drown the console it is warning. */
static void krp_refused(const process_t *target, const Result *r, const char *why)
{
    uint64_t n     = __atomic_fetch_add(&g_krp_refused, 1, __ATOMIC_RELAXED);
    uint32_t token = kring_answer_token(r->sender_pid, r->context);
    if (token != 0) {
        kprintf("[KRP] ERROR: dropped an ANSWER for pid %u (token %u): %s. The "
                "strand waiting on that answer will not be woken by it\n",
                (unsigned int)target->pid, (unsigned int)token, why);
    } else if ((n & 0x3Fu) == 0) {
        kprintf("[KRP] WARN: refused a message for pid %u: %s\n",
                (unsigned int)target->pid, why);
    }
}

void KResultStepAside(process_t *proc, bool step_aside)
{
    ResultRing *rr = kring_result_hdr(proc);
    if (!rr) return;
    uint64_t want = step_aside ? 1u : 0u;
    if (__atomic_load_n(&rr->hdr.step_aside, __ATOMIC_RELAXED) != want)
        __atomic_store_n(&rr->hdr.step_aside, want, __ATOMIC_RELEASE);
}

bool KResultPush(process_t *target, const Result *r)
{
    /* Delivering is work, and a core doing it is not a core with nothing to
     * do. The mark matters because this runs from interrupt context as well
     * as from a K-Core, and an interrupt does not otherwise disturb the idle
     * mark — so Nightwatch could look during the gap between reserving a slot
     * (step 3) and publishing it (step 8), read a ring that is not empty and
     * an owner not yet woken, and call a delivery in progress an undelivered
     * result. It said so once, out loud, the first hour this machine was able
     * to reach idle at all. One byte store, on the same path that already
     * pays for a page walk. */
    nightwatch_core_busy(amp_get_core_index());

    if (!target || !r) return false;

    ResultRing *rr = kring_result_hdr(target);
    if (!rr) { krp_refused(target, r, "it has no reply ring"); return false; }

    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) { krp_refused(target, r, "its reply ring has no slots"); return false; }

    /* The waiter's half of the token, if this answers a submit at all. */
    uint32_t token = kring_answer_token(r->sender_pid, r->context);

    /* ── Claim a position, or refuse. Nothing here waits on the consumer. ──
     *
     * The claim used to be an unconditional fetch_add, and everything that
     * followed was repair work for a claim taken before it was known to be
     * good: a re-check for having passed the consumer, a cross-page map that
     * could fail with the position already spent, and a wait on the slot's
     * Vyukov gate capped at a guessed 16384 PAUSEs. Past the guess the code
     * declared the consumer lost and published a synthetic ERR in place of the
     * caller's answer. The guess is a lie in both directions on a loaded host
     * — long enough to hold a K-Core for half a millisecond, short enough to
     * discard a live reply because a vCPU was descheduled — and its failure
     * mode was to answer a syscall with an error it never earned.
     *
     * The claim is now taken only when it is provably good, by CAS on the
     * tail, and it rests on one fact about the consumer:
     *
     *   THE CONSUMER RELEASES A SLOT'S seq BEFORE IT ADVANCES head.
     *   (boxlib result.c: `slot->seq = expected+1` RELEASE, then
     *    `hdr.head = pos+1` RELEASE — in that order, always.)
     *
     * So every position in [head, head + cap) names a slot the consumer has
     * already finished with. A producer that claims only inside that window
     * claims a slot that is ALREADY free: no gate spin, no budget, no
     * destroying-probe woven through it, no synthetic ERR, no "consumer lost".
     *
     * Every failure — full ring, no page, no translation — now happens BEFORE
     * the claim, so a claimed slot is always filled. That closes the window
     * this function used to document and accept: "this single slot remains
     * stuck until process exit", which froze one modulo position of a cabin's
     * reply ring for the life of the process.
     *
     * `head` lives in a page the guest can write. A guest that reports a head
     * it has not reached makes the kernel reuse a slot it is still reading —
     * and corrupts its OWN reply stream, in its OWN page. It is counted and
     * said out loud rather than waited on; the previous code's answer to the
     * same lie was to stall for half a millisecond and corrupt it anyway. */
    /* ── The answer's room is reserved, so an answer is never refused. ─────
     *
     * Two different things arrive in a strand's reply ring, and only one of
     * them has somebody asleep on it:
     *
     *   an ANSWER is the kernel's and carries a cloakroom token
     *   (kring_answer_token) — it is the reply to a Pocket this strand
     *   submitted, and its owner is parked on it. Lose it and the strand
     *   sleeps forever on a question already answered.
     *
     *   UNSOLICITED traffic — IPC another process chose to send, a question
     *   with its asker's token among it — answers nothing. Nobody is committed
     *   to it, and its sender learns of the refusal and may act on it.
     *
     * A strand can have at most `pocket_cap` submits outstanding, because that
     * is how many Pockets fit in the ring it submits through — so if the last
     * `pocket_cap` slots of the reply ring are kept for answers, an answer can
     * never find the ring full. The geometry already grants it (a Hammock
     * strand has 256 Pocket slots against 512 Result slots; the cabin's rings
     * are the same 1 MiB against 1 MiB, and a Result slot is the smaller), so
     * this reserves nothing that unsolicited traffic had any right to.
     *
     * That is the invariant the rest of the system's honesty rests on: it is
     * what makes it correct for a caller to wait for an answer WITHOUT a
     * deadline, and a deadline on a guaranteed answer is exactly how a slow
     * machine turns a completed operation into a false refusal — while the
     * abandoned caller's stack-resident Manifest and Crates are still going to
     * be read and written by the K-Core (see boxlib manifest.c ManifestSubmit).
     * Reserve the room here, and that whole class of bug has nowhere to live. */
    uint32_t limit = cap;
    if (token == 0) {
        PocketRing *pr = kring_pocket_hdr(target);
        uint32_t reserve = pr ? pr->hdr.slot_count_max : (cap / 2u);
        limit = (cap > reserve) ? (cap - reserve) : (cap / 2u);
        if (limit == 0) limit = 1u;
    }

    Porthole    porthole;
    ResultSlot *slot    = NULL;
    uintptr_t   ensured = 0;   /* page whose mapping this call has secured */
    uint64_t    pos     = 0;

    for (;;) {
        /* head BEFORE tail, and never the other way round.
         *
         * Both cursors only ever grow and tail >= head is the ring's own
         * invariant — but that is a statement about the ring at one instant,
         * not about two loads taken at two. Read tail first and the consumer
         * can advance head past it in between; the snapshot then has head >
         * pos, `pos - head` wraps to a colossal unsigned number, and the ring
         * declares itself full when it is in fact empty. Reading head FIRST
         * makes the order do the work: tail is sampled later, so it cannot be
         * behind the head we already have.
         *
         * MEASURED, not reasoned into place afterwards: with the loads the
         * other way round this refused an ANSWER once in a two-hour matrix,
         * reporting "full at 4294967295 of 32768 slots", and the strand
         * waiting on that answer never woke. */
        uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
        pos           = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
        if (pos - head >= limit) {
            uint64_t n = __atomic_fetch_add(&g_krp_full, 1, __ATOMIC_RELAXED);
            /* Refusing an ANSWER is not a statistic. Somebody is parked on it,
             * and every caller of this function drops the return value, so the
             * refusal would otherwise be perfectly silent — and the strand
             * would simply never wake. Say it, name the victim, and rate-limit
             * so a saturated ring cannot drown the console it is warning. */
            if (token != 0 && (n & 0x3Fu) == 0) {
                kprintf("[KRP] ERROR: dropped an ANSWER for pid %u (token %u) — "
                        "reply ring full at %u of %u slots. The strand waiting "
                        "on that answer will not be woken by it\n",
                        (unsigned int)target->pid,
                        (unsigned int)token,
                        (unsigned int)(pos - head), (unsigned int)cap);
            }
            return false;
        }

        uintptr_t uvaddr = result_ring_slot_uvaddr(rr, pos);
        uintptr_t page   = uvaddr & ~(uintptr_t)(VMM_PAGE_SIZE - 1);
        if (page != ensured) {
            if (vmm_ensure_user_page(target->cabin->vmm, uvaddr,
                                     /*writable=*/true) != 0) {
                krp_refused(target, r, "the slot's page could not be mapped");
                return false;
            }
            ensured = page;
        }

        /* The porthole opens onto the slot BEFORE the claim and stays open to
         * the publish: a claimed slot is always filled, so nothing that can
         * fail may come between the two, and the slot's page cannot be given
         * away while it is being filled. It is opened for WRITE on the walk
         * the store goes through — the slot's address came from a header the
         * cabin writes, and the ensure above looked at that page once, not at
         * what this store lands in. */
        slot = kring_slot_open(&porthole, target, uvaddr, /*for_write=*/true);
        if (!slot) { krp_refused(target, r, "no porthole would open onto the slot"); return false; }

        /* The linearisation point. A failure here means ANOTHER PRODUCER won
         * the tail — the ring moved forward, so this is lock-free progress and
         * not a spin on someone else's liveness. The failing exchange refreshes
         * `pos` with the winner's value, so the next turn considers the
         * position that actually came free. */
        if (__atomic_compare_exchange_n(&rr->hdr.tail, &pos, pos + 1,
                                        /*weak=*/true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
        PortholeClose(&porthole);
    }

    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    /* The claim's own guarantee, read back once. One ACQUIRE load, and it is
     * the only thing that can tell a broken consumer from a healthy one — so
     * it is read, counted and named, never waited on. Named once the porthole
     * is closed: nothing that waits is done through one. */
    bool seq_broken = (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != expected);

    slot->r = *r;

    /* What the waiter says it waits on, read before the publish: once the
     * answer is out, its consumer may take it and say the next token. */
    uint64_t awaiting = __atomic_load_n(&rr->hdr.awaiting, __ATOMIC_ACQUIRE);

    /* Publish — the RELEASE store makes the payload above visible to the
     * consumer's ACQUIRE load of the same word. */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    PortholeClose(&porthole);

    if (seq_broken) {
        uint64_t n = __atomic_fetch_add(&g_krp_seq_broken, 1, __ATOMIC_RELAXED);
        if ((n & 0x3FFu) == 0) {
            kprintf("[KRP] WARN: pid %u reported a head its consumer has not "
                    "reached (slot %lu of round %lu was not released); its own "
                    "reply stream is what gets overwritten\n",
                    (unsigned int)target->pid, (unsigned long)(pos % cap),
                    (unsigned long)round);
        }
    }

    /* boxlib drops on sight a reply whose token is not the one it waits on —
     * the orphan of a call abandoned on a deadline — and with every guaranteed
     * answer waited on without a deadline nothing is ever abandoned: an answer
     * published under one token for a strand that waits on another is a
     * defect, and it is said here, the one place both halves are in view. A
     * strand that has not yet said what it waits for (0) is simply ahead of
     * its own wait. */
    if (token != 0 && awaiting != 0 && awaiting != (uint64_t)token) {
        __atomic_fetch_add(&g_krp_token_astray, 1, __ATOMIC_RELAXED);
        kprintf("[KRP] DEFECT: pid %u waits on token 0x%06x, but the answer "
                "being published for it carries token 0x%06x — its wait will "
                "drop this reply as an orphan\n",
                (unsigned int)target->pid, (unsigned int)awaiting, (unsigned int)token);
    }

    /* The chit this answer redeems, if one was left for it — BEFORE the wake,
     * so a strand that wakes and asks again finds its old promise KEPT and not
     * still DUE, which its next chit would call an answer dropped. */
    ChitKeep(target, token);

    /* Wake.
     *
     * A token-carrying Result ANSWERS a submit — its owner is kernel-parked on
     * it, or mid-transition to that park. The flip is unconditional for that
     * case: a conditional one lost the race against a parker that had not yet
     * committed PROC_WAITING, and the strand then slept forever on a reply
     * already published. For a running target the transition is a benign
     * no-op, and process_set_state's death-guard still refuses corpses.
     *
     * A flip that enqueued has rung the core (sched_enqueue); otherwise the
     * scheduler's doorbell rings it only if the strand is not on a CPU. */
    bool enqueued = false;
    if (token != 0) {
        enqueued = process_set_state(target, PROC_WORKING);
    } else if (process_get_state(target) == PROC_WAITING) {
        enqueued = process_set_state(target, PROC_WORKING);
    }
    if (!enqueued)
        sched_ring_home(target);
    return true;
}

/* ────────────────────────────────────────────────────────────────────────
 * Owed — an answer that will not fit is a DEBT, not a loss.
 *
 * A full reply ring used to end the story: KResultPush said no, SysRoute
 * turned that into ERR_ROUTE_TARGET_FULL, and the payload it had already
 * deposited in the target's cabin heap was simply abandoned there — mapped,
 * paid for, and pointed at by nothing. The sender was told to come back later,
 * which for a daemon answering a request means either standing in a retry loop
 * (the display daemon's own rotation stops, measured: two matrix failures) or
 * dropping the answer and hoping the asker asks again.
 *
 * So the kernel TAKES the answer instead and holds it, in the order the ring
 * would have carried it, and hands it over at the next door the target itself
 * comes to. Nobody waits, and nothing is woken for it: the debtor settles its
 * own debt by walking in. The waiter does not stand at a busy table and does
 * not bin the plate; he leaves it on the pass, and the guest picks it up next
 * time he comes by.
 *
 * This is Touch's Owed queue (touch.c), one ring over and with the same word
 * for it on purpose — whoever learns one already knows the other. The same
 * three decisions hold, for the same reasons:
 *
 *   WHERE IT HANGS. On the ring's owner (process_t). It is the RING that is
 *   full; one ring is one stream with one order.
 *
 *   WHICH LOCK. result_owed_lock, a LEAF: held only to link or unlink one node
 *   and to publish the depth into the ring's slip. KResultPush is never called
 *   under it — that call walks the VMM and CASes a shared cursor, and a
 *   producer on another core must not queue behind it for a lock whose whole
 *   job is to move a pointer.
 *
 *   THE SINGLE-DRAINER TOKEN. Order is the whole value of the queue, and two
 *   drainers popping concurrently would interleave it.
 *
 * ‼ WHERE IT DIFFERS FROM TOUCH: THERE IS NO DEPTH BOUND, AND THAT IS NOT AN
 * OVERSIGHT. Touch's queue is bounded at one ring depth because a TouchOwed
 * node carries the EVENT ITSELF — its 96-byte payload included — so the queue's
 * depth IS kernel memory, and a subscriber that has consumed nothing for a
 * whole ring's worth of events is a stopped consumer, not a congested one.
 *
 * Here the payload is already gone from the kernel's books. ipc_copy_to_heap
 * (system_ops.c) deposited it into the TARGET'S OWN cabin heap before the push
 * was ever attempted, and that resource is already bounded by the only bound
 * that means anything: the deposit failing with ERR_NO_MEMORY, which the
 * sender is told about. What is left to hold is a 24-byte Result descriptor
 * plus a next pointer, and its bound is kmalloc refusing. Inventing a depth
 * number on top would be a SECOND limit on a resource that already has a real
 * one — and a number chosen for a rate nobody measured. When the node cannot
 * be allocated the answer is dropped, loudly (krp_refused, this file's own
 * announcer) and the sender is told ERR_NO_MEMORY, which is the truth: the
 * kernel had no room to hold the promise.
 *
 * Each node OWNS the deposit its Result names. That ownership is what closes
 * the leak: a debt that cannot be taken releases the deposit before returning,
 * and a strand that dies owing answers takes every deposit down with it.
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct ResultOwed {
    struct ResultOwed *next;
    Result             r;
} ResultOwed;

static ResultRing *result_owed_ring(process_t *proc)
{
    if (!proc || !proc->result_ring_phys) return NULL;
    return (ResultRing *)vmm_phys_to_virt(proc->result_ring_phys);
}

/* Publish the depth into the ring header's slip. Called only with
 * result_owed_lock held: two writers racing here could leave 0 behind while an
 * answer is still owed, and the slip's whole job is to be a true statement
 * about what the kernel is holding. The store also writes the cacheline the
 * consumer's UMONITOR is armed on (result_ring.h) — a full ring's tail does
 * not move, so this is the only thing that can stir a bounded wait that has
 * drained everything and gone back to sleep. */
static void result_owed_slip_locked(process_t *proc, uint32_t depth)
{
    ResultRing *rr = result_owed_ring(proc);
    if (rr) __atomic_store_n(&rr->hdr.owed, (uint64_t)depth, __ATOMIC_RELEASE);
}

/* Give back the cabin-heap deposit an undeliverable answer was carrying.
 *
 * cabin_heap_deposit (system_deck.c) bump-allocates the VA and maps one fresh
 * frame per page, so the release is the exact inverse: translate, unmap, free
 * the frame. The VA itself is not reclaimed — buf_heap_next only ever goes
 * forward — and that is deliberate: a reused deposit address would alias a
 * payload the guest may still be reading through an older Result. The frames
 * are what cost RAM, and the frames come back.
 *
 * A zero data_addr means the send carried no payload; there is nothing to
 * release and nothing to say about it. */
static void result_owed_deposit_release(process_t *target, const Result *r)
{
    if (!target || !target->cabin || !r || r->data_addr == 0 || r->data_length == 0)
        return;

    uint32_t pages = (r->data_length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uintptr_t va   = (uintptr_t)r->data_addr + (uintptr_t)i * PMM_PAGE_SIZE;
        uintptr_t phys = vmm_virt_to_phys(target->cabin->vmm, va);
        vmm_unmap_page(target->cabin->vmm, va);
        if (phys) pmm_free((void *)(phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1)), 1);
    }
}

/* Take the answer onto the queue's tail. False only when kmalloc refuses — the
 * one case that is a real loss, and the caller still owns the deposit then. */
static bool result_owed_append(process_t *proc, const Result *r)
{
    ResultOwed *n = (ResultOwed *)kmalloc(sizeof(ResultOwed));
    if (!n) return false;
    n->next = NULL;
    n->r    = *r;

    spin_lock(&proc->result_owed_lock);
    if (proc->result_owed_tail) proc->result_owed_tail->next = n;
    else                        proc->result_owed_head       = n;
    proc->result_owed_tail = n;
    uint32_t depth = __atomic_add_fetch(&proc->result_owed_count, 1, __ATOMIC_ACQ_REL);
    result_owed_slip_locked(proc, depth);
    spin_unlock(&proc->result_owed_lock);

    /* No wake. The strand that owes itself this answer is the same strand that
     * must drain the ring to make room for it, so the only thing that can
     * resolve the situation is that strand running — and it is running, or it
     * is parked on something the ring's own unread head already refuses to
     * sleep across (KResultRingHasUnreadAtHead, turnin_ops.c). Stirring it here
     * would be inventing a wake for a consumer that is on its way anyway; the
     * slip's cacheline store above is the whole of what this costs. */
    return true;
}

void ResultOwedHandOver(process_t *proc)
{
    if (!proc) return;
    if (__atomic_load_n(&proc->result_owed_count, __ATOMIC_RELAXED) == 0) return;

    /* One hand-over at a time. A caller that loses the token does not wait for
     * it: its own answer goes behind the drainer's on the tail, which is the
     * same order either way. */
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&proc->result_owed_draining, &expected, 1u,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;

    for (;;) {
        spin_lock(&proc->result_owed_lock);
        ResultOwed *n = proc->result_owed_head;
        if (n) {
            proc->result_owed_head = n->next;
            if (!proc->result_owed_head) proc->result_owed_tail = NULL;
            n->next = NULL;
        }
        spin_unlock(&proc->result_owed_lock);
        if (!n) break;

        /* result_owed_count is NOT decremented here. It counts what is still
         * owed — the queue plus the one node in this drainer's hand — so a
         * producer reading it can never see 0 while an answer is still
         * undelivered and jump the queue into the ring. */
        if (!KResultPush(proc, &n->r)) {
            /* Still no room. Back to the head it goes, exactly where it was.
             * This is what makes a visit self-limiting: it can never push more
             * than the consumer just drained. */
            spin_lock(&proc->result_owed_lock);
            n->next = proc->result_owed_head;
            proc->result_owed_head = n;
            if (!proc->result_owed_tail) proc->result_owed_tail = n;
            spin_unlock(&proc->result_owed_lock);
            break;
        }

        kfree(n);
        spin_lock(&proc->result_owed_lock);
        uint32_t left = __atomic_sub_fetch(&proc->result_owed_count, 1, __ATOMIC_ACQ_REL);
        result_owed_slip_locked(proc, left);
        spin_unlock(&proc->result_owed_lock);
    }

    __atomic_store_n(&proc->result_owed_draining, 0u, __ATOMIC_RELEASE);
}

void ResultOwedRelease(process_t *proc)
{
    if (!proc) return;

    spin_lock(&proc->result_owed_lock);
    ResultOwed *n = proc->result_owed_head;
    proc->result_owed_head = NULL;
    proc->result_owed_tail = NULL;
    __atomic_store_n(&proc->result_owed_count, 0u, __ATOMIC_RELEASE);
    result_owed_slip_locked(proc, 0u);
    spin_unlock(&proc->result_owed_lock);

    while (n) {
        ResultOwed *next = n->next;
        /* The deposit goes with the answer. For a main strand the cabin's
         * address space is about to be torn down and would have reclaimed it
         * anyway; for a spawned strand the cabin outlives it, and these frames
         * would be the leak nobody ever comes back for. */
        result_owed_deposit_release(proc, &n->r);
        kfree(n);
        n = next;
    }
}

bool KResultPushOrOwe(process_t *target, const Result *r)
{
    if (!target || !r) return false;

    /* Anything already owed to this ring comes first: the queue IS the ring's
     * order, and an answer that went round it into a slot the queue was waiting
     * for would arrive before answers accepted earlier. Settle the debt, and if
     * it does not settle, join its tail. */
    if (__atomic_load_n(&target->result_owed_count, __ATOMIC_RELAXED) != 0) {
        ResultOwedHandOver(target);
        if (__atomic_load_n(&target->result_owed_count, __ATOMIC_RELAXED) != 0) {
            if (result_owed_append(target, r)) return true;
            krp_refused(target, r, "the kernel had no room to hold it as a debt");
            return false;
        }
    }

    if (KResultPush(target, r)) return true;
    if (result_owed_append(target, r)) return true;

    krp_refused(target, r, "the kernel had no room to hold it as a debt");
    return false;
}
