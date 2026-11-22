#ifndef CABIN_H
#define CABIN_H

#include "ktypes.h"
#include "atomics.h"
#include "klib.h"
#include "vmm.h"
#include "memtag.h"   /* MEMTAG_HELD_L1_ENTRIES — the held-tag set's geometry */
#include "image_thread.h"
#include "proc_exit.h"

/*
 * cabin_t — shared address-space object.
 *
 * One cabin owns the vmm_context (CR3), IPC rings, MemTag state, ASLR
 * bases, and all Bay/Brook/Touch claim lists.  In P1 there is always
 * exactly one strand per cabin (strand_count==1).  P4 will add ≥2 strands.
 *
 * Lock ordering inside cabin: bay_lock / brook_lock / subs_lock are
 * independent per-cabin locks.  No global lock is taken by cabin_destroy.
 *
 * strand_count != process_t.ref_count — they track different lifetimes.
 */

/* Retired (grown-past) tag-overflow buffers. On growth the old buffer is
 * NOT freed — a lock-free reader (process_has_tag_id on the hot publish
 * path) may still hold the pointer — it is chained here and freed only in
 * cabin_destroy. Reallocs are rare and bounded (~7/cabin), so the retained
 * memory is negligible. */
typedef struct TagOverflowRetired
{
    uint32_t                  *buf;
    struct TagOverflowRetired *next;
} TagOverflowRetired;

typedef struct cabin_t
{
    vmm_context_t *vmm;

    uint64_t cabin_info_phys;
    /* The MAIN strand's IPC rings (fixed low VAs 0x2000/0x3000/0x5000),
     * allocated + mapped by vmm_create_cabin and reclaimed by
     * vmm_destroy_context at cabin teardown. P5a: the main strand's
     * process_t.{pocket,result,touch}_ring_phys ALIAS these; each spawned
     * strand instead carries its OWN per-strand rings carved from the Hammock
     * window (strand_rings.c). kring.c / touch_ring.c route by the per-strand
     * process_t fields, never these. */
    uint64_t pocket_ring_phys;
    uint64_t result_ring_phys;
    uint64_t touch_ring_phys;

    uint64_t tag_bits;

    /* Fixed op-authority bitmask (auth_tags.h, NOT keyed by registry id).
     * Holds AUTH_TAG_X iff this cabin carries the bare auth key X. Kept in sync
     * with tag_bits at the three mutation points via AuthBitForKey; read by
     * the manifest gate, the spawn gate and the Ink guard's warrant. */
    uint32_t auth_bits;

    uint32_t *tag_overflow_ids;
    uint32_t  tag_overflow_count;      /* u32: a u16 capacity doubled past 32768 is 0 */
    uint32_t  tag_overflow_capacity;
    TagOverflowRetired *tag_overflow_retired;  /* old buffers, freed at cabin_destroy */

    /* The Use Context's verdict on this cabin, one word: the epoch it was
     * counted at, over the cabin's Fit — how much of the context it wears.
     * The scheduler reads it and compares one epoch (use_context.h); 0 is
     * stale from birth. */
    uint64_t use_verdict;

    /* MemTag held-tag set, level 1 — see the block above MemCabinGrant in
     * memtag.h for the shape and why it is two levels. Each slot carries
     * the address of a leaf bitmap covering MEMTAG_HELD_LEAF_IDS
     * consecutive tag_ids; the sixteen together span the registry's whole
     * 16-bit id space. Zero means "no leaf yet", which reads as "this
     * cabin holds nothing in that window" — the read path never installs
     * one. Leaves are CAS-installed by MemCabinGrant and freed by
     * MemTagHeldSetRelease in cabin_destroy.
     *
     * Typed uint64_t rather than as a leaf-pointer array because
     * process.c's process_active_memtags hands this table out as
     * uint64_t* and that file is owned elsewhere; on LP64 x86_64 a leaf
     * address is exactly a uint64_t, so the slot carries the pointer
     * itself rather than an index into some second table that would then
     * have to be kept in step with this one. */
    uint64_t active_memtags[MEMTAG_HELD_L1_ENTRIES];

    uintptr_t code_start;
    size_t    code_size;

    /* The template every strand's thread-local storage is made from, as the
     * loader kept it (ImageThreadKeep): written once, before any strand of
     * the cabin runs, and let go with the cabin. */
    ImageThread thread;

    /* ASLR: per-cabin randomized addresses (set at creation time). */
    uint64_t aslr_heap_base;
    uint64_t aslr_stack_top;    /* main stack: its reserve below it, as its program was prepared */
    uint64_t aslr_buf_heap_base;

    /* The deposit window, from aslr_buf_heap_base up: where the kernel lends
     * the program the bytes of each message and question it receives, a page
     * at least apiece (system_deck.c). One bit a page in each map:
     * deposit_held — the place is taken, lent or being mapped or unmapped by
     * the kernel; deposit_lent — the program holds it and may give it back;
     * deposit_mapped — a page of the program's is mapped at the place. All
     * three deposit_words long, in one kmalloc'd block, grown by doubling; no
     * free place lies below deposit_low; deposit_kept counts the free places
     * a page is still mapped at, kept for the next deposit. All guarded by
     * deposit_lock, a leaf: nothing is mapped, freed or allocated under it. A
     * place given back is taken again before the window grows, so the window
     * and its page tables are as large as the most the program ever held at
     * once. */
    uint64_t   *deposit_held;
    uint64_t   *deposit_lent;
    uint64_t   *deposit_mapped;
    uint32_t    deposit_words;
    uint32_t    deposit_low;
    uint32_t    deposit_kept;
    spinlock_t  deposit_lock;

    uint32_t spawner_pid;
    /* Generation of spawner_pid captured at spawn time. A bare pid is not a
     * stable identity — pids recycle — so "own child" authority compares this
     * snapshot against the spawner's live generation; a later process that
     * inherits a dead spawner's pid carries a different generation and so
     * inherits no authority. Zeroed by cabin_create's memset. */
    uint32_t spawner_gen;

    /* When the program began: the machine's uptime at cabin_create, in
     * microseconds (system.roster says it). */
    uint64_t began_us;

    /* Touch subscriber list. */
    void       *subs_head;
    spinlock_t  subs_lock;

    /* Bay claim list. */
    void       *bay_claims_head;
    spinlock_t  bay_lock;
    uint64_t    bay_va_next;
    uint16_t    tme_keyids_held;

    /* Brook claim list. */
    void       *brook_claims_head;
    spinlock_t  brook_lock;
    uint64_t    brook_va_next;

    /* Hammock slots — one fixed-size VA slot per spawned strand for its user
     * stack + CET shadow stack + per-strand IPC rings + StrandInfo TLS block
     * (see cabin_layout.h + strand_rings.c). The main strand does NOT consume
     * a slot. hammock_va_next is the bump cursor over never-used slots
     * (mirrors bay_va_next / brook_va_next); a slot a strand has left, every
     * page of it unmapped, is pushed on hammock_free (hammock_free_count of
     * hammock_free_capacity entries, kmalloc'd, grown by doubling) and spawn
     * takes from there before it bumps. All four guarded by hammock_lock, so
     * concurrent strand_spawn / strand exit from sibling strands never hand
     * out the same VA. */
    uint64_t    hammock_va_next;
    uint64_t   *hammock_free;
    uint32_t    hammock_free_count;
    uint32_t    hammock_free_capacity;
    spinlock_t  hammock_lock;

    /*
     * strand_count: number of strands currently attached to this cabin.
     * In P1 always 1.  cabin_ref_dec calls cabin_destroy when it hits 0.
     * This is NOT the same as process_t.ref_count (which tracks external
     * K-Core / pointer refs to the strand).
     */
    atomic_u32_t strand_count;

    /* The program is ending (CabinEnd): no strand is born into it any more.
     * Written under process_lock and read there, so a strand_spawn either
     * finished joining the process list before CabinEnd's walk, and is ended
     * by it, or sees this and is refused. Never cleared — which is why
     * ScreenClaim may also read it without the lock: it is set before any
     * death of the program is said, so a claimant that heard the death sees
     * it set. */
    uint8_t ending;

    /* How the program fell, when it did: the first exception of any of its
     * strands (idt.c), said with every death of it (process:died). Taken once
     * by fell_claimed; vector, place and name are written by the taker before
     * fell_said is set, and read only after it is. */
    uint8_t  fell_claimed;
    uint8_t  fell_said;
    uint8_t  fell_vector;
    uint32_t fell_offset;
    char     fell_in[PROC_FELL_IN_MAX];

    /* Its box says `rise`: when the system started it, a fall raises it again
     * (rise.h). Set by the load, before the program runs. */
    uint8_t  rise;

    /* The program's identity is its file: the volume's id of the file this
     * cabin was started from, 0 for a program started from bytes held by
     * another (an image of no file). Written once, by the load of its image
     * (process_load_binary), before the program runs and before any other
     * core can see the cabin — never inherited from the spawner, never
     * changed after, and kept here, in memory only the kernel writes, so no
     * program can speak as another. Read it by CabinProgramFile. */
    uint64_t program_fid;
    /* Which volume that file is on — its Deed's uuid, written with
     * program_fid: a file id names a file of one volume, and a program
     * started from a medium since taken out must not be taken for whatever
     * file of another volume has the same id. Read it by CabinProgramVolume. */
    uint8_t  program_volume[16];
} cabin_t;

_Static_assert(sizeof(cabin_t) < 4096, "cabin_t must fit in one page");

/*
 * cabin_create — allocate and initialise a cabin_t.
 *
 * Calls vmm_create_cabin, initialises all IPC rings, applies MemTag to
 * ring pages, interns the initial tag string, generates ASLR offsets,
 * and seeds Bay/Brook/Touch heads.  Sets strand_count = 1.
 *
 * Returns NULL on any allocation failure (caller must abort process_create).
 */
cabin_t *cabin_create(uint32_t pid, const char *tags);

/*
 * cabin_hammock_take — the base VA of a hammock slot for a new strand: a slot
 * given back if there is one, else the next never-used one. Returns false when
 * the window is full of strands alive at once.
 *
 * cabin_hammock_give_back — a strand is gone and every page of its slot is
 * unmapped; the slot is the next spawn's. The list that holds it grows with
 * the most slots ever waiting at once. If it cannot grow, a slot at the top of
 * the cursor lowers the cursor instead, and any other is lost to this cabin
 * (its address space only — nothing is mapped there) and said so.
 */
bool cabin_hammock_take(cabin_t *cabin, uint64_t *slot_base);
void cabin_hammock_give_back(cabin_t *cabin, uint64_t slot_base);

/*
 * cabin_destroy — release all shared resources owned by this cabin.
 *
 * Calls vmm_destroy_context, frees tag_overflow_ids.
 * Takes NO global locks.  Must be called only when strand_count == 0.
 */
void cabin_destroy(cabin_t *cabin);

/*
 * CabinEnd — the program `named` belongs to ends: every strand of its cabin.
 *
 * A program is its cabin. A strand may end itself alone (strand_exit), but no
 * other end is the end of one strand: an end that lands while a strand holds a
 * lock of its program's (boxlib's heap lock above all) would leave that lock
 * held for ever over every strand still running. So a kill, an exit() from any
 * strand and a crash all come here.
 *
 * `named` is the strand the end came through and carries `disposition`
 * (proc_exit.h); the others share the program's end — the same exit code, the
 * same kill, and for any crash cause PROC_EXIT_CRASHED, since the cause was the
 * named strand's own. The workers are announced first, then `named`, and the
 * main strand LAST: it is the one the program's spawner knows it by and waits
 * on, so whoever finds it gone finds the program wholly gone.
 *
 * Every strand of the cabin also loses its reap-block: a joinable strand
 * waits to be joined by its program, and its program is gone.
 *
 * Allocates nothing, so it cannot fail halfway. The caller holds a reference
 * on `named` (which keeps the cabin alive) and holds no process_lock. Ends
 * strands that are ending already without harm: each end is the death commit
 * TouchCleanupProcess makes once per strand.
 */
struct process_t;
void CabinEnd(struct process_t *named, int32_t disposition);

/* The program this cabin is: the (pid, generation) of the strand that founded
 * it, as its CabinInfo says — the same words every strand of it reads at
 * CABIN_INFO_VADDR, in a page no strand of it can write. A server that keeps
 * something per program, and a Bay made for one, name the program by this.
 * False while the cabin has no program loaded into it yet. */
bool CabinProgram(const cabin_t *cabin, uint32_t *pid, uint32_t *generation);

/* The volume's id of the file the program of `cabin` was started from: who
 * the program is where a file's maker, or a file's own tags, are in question.
 * 0 for a program with no file (started from bytes), and for no cabin. */
uint64_t CabinProgramFile(const cabin_t *cabin);

/* The uuid of the volume that file is on — 16 bytes, all zero for a program
 * with no file — or NULL for no cabin. */
const uint8_t *CabinProgramVolume(const cabin_t *cabin);

/* Reference counting on strand_count. */
void cabin_ref_inc(cabin_t *cabin);
/* Decrements strand_count; calls cabin_destroy when it reaches 0. */
void cabin_ref_dec(cabin_t *cabin);

#endif /* CABIN_H */
