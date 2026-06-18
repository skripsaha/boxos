#ifndef BAY_H
#define BAY_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"

/*
 * Bay — cross-cabin shared memory, tag-driven discovery, refcounted lifetime.
 *
 * Both producer and consumer call bay_open("video:frame:0", size, flags)
 * with the SAME tag string; the kernel resolves the tag via the TagFS
 * registry and returns a user-VA mapping into THE SAME physical pages in
 * each cabin. No explicit handle exchange via Pocket/Touch is required.
 *
 * Lifetime is refcounted on the per-cabin "claim". Each bay_open in a
 * cabin adds one claim; bay_release drops it. When the last claim across
 * all cabins drops, the physical pages return to PMM and the BayObject
 * is freed. process_destroy auto-releases every Bay claim the cabin
 * holds (BayCleanupProcess) so a crashing app never leaks the backing
 * pages.
 *
 * Page-class selection is implicit: ≥ 2 MB allocations try 2 MB pages
 * via PMM buddy order-9; smaller allocations use 4 KB pages. 1 GB pages
 * are not supported in v1 (BUDDY_MAX_ORDER=14 caps contiguous chunks at
 * 64 MB; a buddy upgrade in a future session would enable 1 GB).
 *
 * Bay sits at the same architectural tier as Touch — distinct semantic
 * primitive, not a deck. Manifest ops live in System Deck (bay_ops.c)
 * to keep the deck count small. Userspace boxlib exposes:
 *
 *   void   *bay_open(const char *tag, uint64_t size, uint32_t flags);
 *   int     bay_release(void *ptr);
 *   uint64_t bay_size(void *ptr);
 *
 * which is the full surface — no separate map/unmap step. Internally the
 * kernel binds the user-VA to BayClaim at open() and resolves user-VA to
 * BayClaim on release()/size().
 */

/* Open flags. The default is "open existing, read+write mapping". */
#define BAY_OPEN        0x00u   /* default — fail if tag doesn't exist yet */
#define BAY_CREATE      0x01u   /* create if missing; size MUST be non-zero */
#define BAY_RO          0x02u   /* read-only mapping (PTE without WRITABLE) */

/* TME-MK Per-Bay encryption.
 *
 * When BAY_CREATE | BAY_ENCRYPTED is passed, the kernel:
 *   1. Reserves a unique KeyID from the TME pool (tme_keyid_alloc).
 *   2. Allocates the backing pages and tags them tme:keyid:N (PMM).
 *   3. Maps each chunk into the creator's cabin VA via
 *      vmm_map_page_with_keyid so the user-VA PTE encodes that KeyID
 *      in PTE.phys upper bits. The CPU transparently encrypts every
 *      store with key N and decrypts every load with key N — DRAM
 *      contents become opaque to anyone reading without the key.
 *   4. Stores keyid in the BayObject so subsequent bay_open()s from
 *      OTHER cabins receive a mapping with the SAME KeyID (otherwise
 *      cross-cabin reads would decrypt with the wrong key and return
 *      garbage).
 *
 * Subsequent bay_open() WITHOUT BAY_ENCRYPTED on a created-encrypted
 * Bay still binds via the stored KeyID — the flag is a creation-time
 * declaration. Trying to open an encrypted Bay with BAY_ENCRYPTED on
 * a non-encrypted existing Bay (or vice versa) returns ERR_INVALID_ARGUMENT.
 *
 * On the LAST bay_release of an encrypted Bay, the kernel calls
 * tme_keyid_free which re-keys the slot (PCONFIG SET_KEY_RANDOM) before
 * returning it to the pool. A future Bay that reuses the slot cannot
 * decrypt this Bay's old ciphertext.
 *
 * Behavior on TME-MK-inactive hosts (QEMU TCG, BIOS without TME setup):
 *   BAY_CREATE | BAY_ENCRYPTED returns ERR_UNSUPPORTED. Userspace can
 *   detect via the `hw tme` shell command or by attempting the create
 *   and falling back. The current TME baseline encryption (KeyID 0,
 *   platform key) is still in effect for ALL RAM in any case — this
 *   flag is about adding per-Bay cryptographic isolation on top.
 */
#define BAY_ENCRYPTED   0x04u

/* Per-process quota for BAY_CREATE | BAY_ENCRYPTED.
 *
 * The global TME-MK KeyID pool has N slots (typical 16-64 on shipping
 * Intel silicon). Without a quota, a single misbehaving cabin could
 * exhaust the pool and DoS every other cabin's encryption requests.
 * 8 per cabin is enough for typical use (model weights, secret
 * config, framebuffer, IPC scratch) while leaving headroom for
 * concurrent processes. Counter lives on process_t.tme_keyids_held
 * (process.h) and is enforced inside BayOpenInternal. */
#define TME_QUOTA_PER_PROC  8u

/* Mask of all defined flag bits — used by BayOpenInternal to reject
 * unknown bits before allocating anything. Keep in lock-step with the
 * BAY_* definitions above. */
#define BAY_FLAGS_MASK   (BAY_CREATE | BAY_RO | BAY_ENCRYPTED)

/* Per-cabin allocation size cap. The kernel handler clamps requested
 * Bay sizes at this value so a runaway userspace can't exhaust PMM
 * with one call. 16 GiB is enough for any realistic dataset
 * (3 GiB video frame, ML weights, page-cache blobs) and still leaves
 * room for the rest of the cabin's working set on a 32 GiB host. */
#define BAY_MAX_OPEN_SIZE   (16ULL * 1024 * 1024 * 1024)

typedef struct BayClaim BayClaim;
typedef struct BayObject BayObject;
struct process_t;

/* BayObject — one per-tag physical-memory region.
 *
 * Identified by tag_id (TagFS-interned uint16_t). Hash table keys
 * BayObject by tag_id so cross-cabin open is O(1) modulo bucket-chain
 * length. All cabins claiming the same tag share this single record.
 *
 * The `chunks` array holds physical addresses of each backing chunk;
 * each chunk is either 4 KB (page_class==12) or 2 MB (page_class==21).
 * The chunk count is page_count_total / pages_per_chunk; the field is
 * elided since chunk_size+total_size derives it.
 *
 * ref_count == # active claims across all cabins. When it drops to 0
 * under the bucket lock, the BayObject is unlinked, chunks returned to
 * PMM, and the struct kfree'd.
 */
struct BayObject {
    BayObject  *bucket_next;        /* hash-bucket collision chain */
    uint16_t    tag_id;             /* TagFS-interned id */
    uint16_t    page_class;         /* 12 = 4 KiB, 21 = 2 MiB */
    /* TME-MK KeyID. Zero when not encrypted — every cabin opening this
     * Bay maps with KeyID 0 (platform-default encryption, identical to
     * non-encrypted Bays semantically because no other consumer sees a
     * different KeyID). Non-zero when BAY_CREATE | BAY_ENCRYPTED was
     * specified at creation; every cross-cabin mapping uses this same
     * KeyID so reads decrypt correctly. */
    uint16_t    tme_keyid;
    uint16_t    _pad0;              /* keep `total_size` 8-byte-aligned */
    uint64_t    total_size;         /* PHYSICAL mapped bytes = chunk_count*chunk_size (VA reserve + unmap) */
    uint64_t    user_size;          /* LOGICAL bytes the creator requested — what bay_size() reports */
    uint64_t    chunk_size;         /* PMM_PAGE_SIZE or BAY_HUGE_SIZE */
    uint32_t    chunk_count;        /* ceil(total_size / chunk_size) */
    uint32_t    flags;              /* sticky creation flags (RW + ENCRYPTED capability) */
    uint64_t   *chunks;             /* phys addresses, chunk_count entries (NO KeyID bits) */
    /* ref_count is always read/written under the owning BayBucket's
     * spinlock — see bay.c. Declared as plain uint32_t (no atomic
     * qualifier) because the lock provides the ordering guarantees;
     * mixing the two would invite "is this protected or not?" confusion
     * at every call site. */
    uint32_t    ref_count;          /* total claims across cabins */
    uint32_t    _pad1;
    uint64_t    create_pid;         /* diagnostic */
    uint64_t    create_tsc;         /* diagnostic */
};

/* BayClaim — one per cabin-open of a Bay. Lives in two places:
 *   - bucket-side: hangs off BayObject only via the ref_count
 *   - process-side: linked into proc->bay_claims_head (singly-linked)
 *
 * Records the user-VA window inside the cabin and the per-claim flags
 * (RO vs RW). bay_release(user_va) walks the process list to find the
 * claim, unmaps its pages from the cabin, drops ref_count, and free()s.
 */
struct BayClaim {
    BayObject  *bay;                /* upstream BayObject */
    BayClaim   *proc_next;          /* process claim list link */
    uint64_t    user_va_base;       /* per-cabin VA window start */
    uint32_t    flags;              /* claim-specific flags (BAY_RO) */
    uint32_t    _pad;
    struct process_t *proc;         /* back-pointer for sanity check */
};

void BayInit(void);

/* bay_open semantics.
 *   - flags == BAY_OPEN: open existing tag; if missing, ERR_NOT_FOUND.
 *   - flags == BAY_CREATE: create if missing; size must be > 0. If
 *     already exists, this acts like BAY_OPEN (existing size wins,
 *     requested size ignored unless caller passes 0).
 *   - flags & BAY_RO: claim mapped without VMM_FLAG_WRITABLE.
 *
 * On success, *out_user_va receives the user-VA window base and
 * *out_actual_size the actual size (chunk-rounded). The caller is in
 * the kernel; the user-VA is valid inside ctx->proc->cabin. */
error_t BayOpenInternal(struct process_t *proc,
                        const char *tag,
                        uint64_t requested_size,
                        uint32_t flags,
                        uint64_t *out_user_va,
                        uint64_t *out_actual_size);

/* bay_release semantics: walk proc->bay_claims_head, find claim where
 * user_va_base == user_va, unmap the pages from the cabin, drop
 * ref_count, free claim. If ref_count drops to 0, the underlying
 * BayObject is destroyed and its chunks return to PMM. */
error_t BayReleaseInternal(struct process_t *proc, uint64_t user_va);

/* bay_size: O(N_claims_in_proc) lookup of the BayClaim by user_va. */
uint64_t BaySizeInternal(struct process_t *proc, uint64_t user_va);

/* Process teardown hook. MUST be called from process_destroy BEFORE
 * vmm_destroy_context — otherwise the destroy walk frees Bay-owned
 * pages as if they were exclusive process pages (leading to refcount
 * underflow + PMM double-free on the next bay_release). Idempotent;
 * safe to call multiple times. */
void BayCleanupProcess(struct process_t *proc);

/* Diagnostic snapshot — used by perf dump / future bay_info ops.
 *   out[0] = live BayObject count
 *   out[1] = total claims across all cabins
 *   out[2] = pages held (in 4 KB units)
 *   out[3] = aggregate ref drops since boot
 */
void BayStatsSnapshot(uint64_t out[4]);

#endif /* BAY_H */
