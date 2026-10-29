#include "escheat.h"
#include "binder_internal.h"
#include "settle.h"
#include "../mount/book_mount.h"
#include "../ink/fallow.h"
#include "../helm/engross.h"
#include "../integrity/gloss.h"
#include "../box_hash/box_hash.h"
#include "boardroom.h"

/*
 * Escheat (escheat.h; spec_v2 §16.5 п.20). What the act lets go is the
 * mount's: the rotten FILES nodes (g_binder.rot), what of the files locked by
 * name with them read (g_book.kept) and the ground held for what rotted.
 *
 * ‼ THE ACT LETS GO ONLY THE GROUND OF WHAT IT LETS GO. The census holds alike
 * a block a rotten range may own and a block a file locked for another reason
 * names — its Parcels do not cover its bytes, an item of it does not read
 * (tally_leaves.c BookFileLock) — and the act does not let that file go: every
 * Tally after it carries the file as it lay (binder_items.c KeptNext) and
 * digests its ground from the held list (binder_slice.c BinderGroundTake). So
 * the mount parts the held ground once its census is done (EscheatMounted):
 * what such a file's Parcels name stays held, and only the rest is the act's —
 * one rule with volume-read, whose oracle keeps the digests of a locked file
 * no range let go reaches (binder.py _LockedKept).
 *
 * The Settle's holder takes them out of those lists at the slice of the Tally
 * that owes the act, as DogEar's spans are taken — and puts them back if that
 * Tally is refused — so the Tally names none of them and no other reader of
 * the lists ever meets them half-moved. Asked from any core, the act is a
 * word published and a Tally demanded; what it answers is what the next Tally
 * lets go — the mount's counts while its ranges are still named
 * (g_binder.nrot), none of them once a Tally has let them go. An ask is
 * answered by the Tally that took it: one asked while the act's own Tally is
 * under way is owed to the next (s_asked, s_answered).
 *
 * ‼ A DIGESTS LEAF NO COPY OF WHICH READS IS LAID ANEW OF THE DATA, AND ONLY
 * HERE (§16.5 п.19, Р-e2 (а)). Without the act the Tally names it as it lies
 * and its blocks read unchecked; under it the whole Tally's walk of the rotten
 * leaves (TallyDigestsRottenNext) notes each one's range instead
 * (EscheatDigestsLaid), and the walk of every live file's ground takes the
 * digest of each block of it the Tally before cannot give out of the block
 * itself (EscheatDigestOf) — into the Gloss, so the walk that writes, the
 * self-check and every read until the DONE find it there. A block no living
 * file names is not walked: it has no digest after the act, and is free.
 */

static volatile uint64_t s_asked;     /* asks made this mount: published */
static volatile uint64_t s_answered;  /* of them, those a Tally answered */
static volatile uint64_t s_walked;    /* of them, those a walk of the files cut the tails for */
static volatile uint64_t s_reach;     /* the newest Ink an ask saw: no record made after it is the act's */
static uint64_t          s_cover;     /* the asks the Tally under way answers */
static volatile uint64_t s_tails;     /* tails the Helm's service let go under the act */
static bool              s_taken;     /* the Tally under way took it: the Settle's holder's */
static BinderKept*       s_rot;
static uint32_t          s_nrot;
static BookKeptFile*     s_kept;
static uint32_t          s_nkept;

/* The held ground as the mount parted it: what stays held, what the act lets
 * go; and the whole list the Tally under way took, given back if it is
 * refused. */
static BinderGround*     s_stay;
static uint64_t          s_nstay;
static BinderGround*     s_free;
static uint64_t          s_nfree;
static BinderGround*     s_all;
static uint64_t          s_nall;
static uint64_t          s_capall;
static bool              s_parted;

/* What the act lets go, counted at the mount — read from any core, written
 * before the volume opens. */
static uint64_t          s_ranges;
static uint64_t          s_files;
static uint64_t          s_blocks;

/* What a Tally let go: published once — g_binder.rot gains a range only at
 * the mount, so one Tally of a mount at most lets ranges go. */
static volatile uint8_t  s_gone;
static BinderKept*       s_lostrot;
static uint32_t          s_nlostrot;
static uint64_t*         s_lostfid;
static uint32_t          s_nlostfid;

/* The DIGESTS leaves the Tally under way lays anew of the data, by their
 * ranges of blocks; the digests taken; the block read for one. */
typedef struct {
    uint64_t lo;
    uint64_t hi;
} EscheatBlocks;

static EscheatBlocks*   s_dig;
static uint32_t         s_ndig;
static uint32_t         s_capdig;
static uint64_t         s_digests;
static uint8_t*         s_block;

/* Whether the keys of file `fid` meet rotten range `r`. */
static bool FileMeets(uint64_t fid, const TallyPlace* r)
{
    bool below = !r->has_hi || fid < r->hi.k1 || (fid == r->hi.k1 && r->hi.k2 > 0);
    bool above = !r->has_lo || r->lo.k1 <= fid;
    return below && above;
}

/* Whether `fid` meets a range of the mount's rotten FILES (g_book.rot). */
static bool FileInRot(uint64_t fid)
{
    for (uint32_t i = 0; i < g_book.nrot; i++) {
        TallyPlace p = { .lo = g_book.rot[i].lo, .has_lo = true, .hi = g_book.rot[i].hi,
                         .has_hi = g_book.rot[i].has_hi };
        if (FileMeets(fid, &p))
            return true;
    }
    return false;
}

/* Whether `fid`'s header lies before the rotten range it meets — read, its
 * Parcels in the range: a file the parent's count of the range does not
 * count, locked by name instead. A file whose header lies in the range is
 * counted there, whatever of it lies past. */
static bool HeadBefore(uint64_t fid)
{
    for (uint32_t i = 0; i < g_book.nrot; i++)
        if (g_book.rot[i].lo.k1 == fid && g_book.rot[i].lo.k2 > 0)
            return true;
    return false;
}

// ----------------------------------------------------------------------------
// The held ground parted at the mount
// ----------------------------------------------------------------------------

/* The DATA runs of every file locked by name the act does not let go, as its
 * Parcels lay — what every Tally carries of it — in order, those that meet
 * joined. False with no memory. */
static bool KeptRuns(BinderGround** out, uint64_t* n)
{
    uint64_t cap = 0;
    for (uint32_t i = 0; i < g_book.nkept; i++) {
        const BookKeptFile* k = &g_book.kept[i];
        for (uint32_t p = 0; !FileInRot(k->fid) && p < k->nparcels; p++) {
            uint32_t in = 0;
            uint64_t runs = 0, from = 0;
            TallyTake(k->parcel_body[p], k->parcel_len[p], &in, &runs);
            for (uint64_t r = 0; r < runs; r++) {
                TallyRun run;
                if (!TallyRunGet(k->parcel_body[p], k->parcel_len[p], &in, &from, k->parcel_seq[p], &run))
                    break;
                if (run.hole || run.count == 0)
                    continue;
                if (!BinderListRoom((void**)out, *n, &cap, sizeof(BinderGround)))
                    return false;
                (*out)[(*n)++] = (BinderGround){ run.start, run.count };
            }
        }
    }
    *n = *n ? BinderGroundJoined(*out, *n) : 0;
    return true;
}

static bool RunPut(BinderGround** list, uint64_t* n, uint64_t* cap, uint64_t start, uint64_t count)
{
    if (!BinderListRoom((void**)list, *n, cap, sizeof(BinderGround)))
        return false;
    (*list)[(*n)++] = (BinderGround){ start, count };
    return true;
}

/* The held ground (in order) parted against the kept runs `k` (in order,
 * joined): what they name to s_stay, the rest to s_free. False with no
 * memory. */
static bool HeldParted(const BinderGround* k, uint64_t nk)
{
    uint64_t capstay = 0, capfree = 0, j = 0;
    for (uint64_t i = 0; i < g_binder.nheld; i++) {
        uint64_t at = g_binder.held[i].start, end = at + g_binder.held[i].count;
        while (at < end) {
            while (j < nk && k[j].start + k[j].count <= at)
                j++;
            bool     in = j < nk && k[j].start <= at;
            uint64_t to = j == nk ? end : in ? k[j].start + k[j].count : k[j].start;
            to          = to < end ? to : end;
            if (in ? !RunPut(&s_stay, &s_nstay, &capstay, at, to - at)
                   : !RunPut(&s_free, &s_nfree, &capfree, at, to - at))
                return false;
            s_blocks += in ? 0 : to - at;
            at = to;
        }
    }
    return true;
}

static void PartedLetGo(void)
{
    kfree(s_stay);
    kfree(s_free);
    s_stay   = NULL;
    s_free   = NULL;
    s_nstay  = s_nfree = 0;
    s_parted = false;
}

void EscheatMounted(void)
{
    BinderGround* k  = NULL;
    uint64_t      nk = 0;
    s_ranges = g_binder.nrot;
    s_files  = 0;
    s_blocks = 0;
    for (uint32_t i = 0; i < g_binder.nrot; i++)
        s_files += g_binder.rot[i].at.count;
    for (uint32_t i = 0; i < g_book.nkept; i++)
        s_files += FileInRot(g_book.kept[i].fid) && HeadBefore(g_book.kept[i].fid) ? 1u : 0u;
    if (g_binder.nrot == 0) {
        s_parted = true;    /* nothing rotted: every held block is a locked file's, and stays */
        return;
    }
    g_binder.nheld = g_binder.nheld ? BinderGroundJoined(g_binder.held, g_binder.nheld) : 0;
    s_parted       = KeptRuns(&k, &nk) && HeldParted(k, nk);
    kfree(k);
    if (!s_parted) {
        PartedLetGo();
        s_blocks = 0;
        kprintf("[TagFS] no memory to part the ground held for its rotten ranges from the ground of the files "
                "locked for another reason — Escheat is refused to this mount\n");
    }
}

// ----------------------------------------------------------------------------
// The ask
// ----------------------------------------------------------------------------

/* The refusal of an ask with nothing to let go, said for the one tail the act
 * cannot cut: the loader's kernel's, which no call changes. */
static void NothingSaid(uint32_t boot)
{
    if (boot)
        kprintf("[TagFS] Escheat: nothing to let go — the tail of the file the loader reads the kernel by is "
                "changed by no call, warrant or none, and the Tally waits for it still\n");
}

/* The act's reach grown to `gen`, never back: of two asks made at once, the
 * newer's stands. */
static void EscheatReachTo(uint64_t gen)
{
    uint64_t was = __atomic_load_n(&s_reach, __ATOMIC_ACQUIRE);
    while (gen > was && !__atomic_compare_exchange_n(&s_reach, &was, gen, false, __ATOMIC_ACQ_REL,
                                                     __ATOMIC_ACQUIRE))
        ;
}

error_t TagFSEscheat(InkWarrant warrant, EscheatSaid* out)
{
    memset(out, 0, sizeof(*out));
    if (warrant != INK_WARRANTED) {
        kprintf("[TagFS] Escheat refused: the cabin asking wears neither system nor god\n");
        return ERR_PERMISSION_DENIED;
    }
    if (!tagfs_enter())
        return ERR_DEVICE_NOT_READY;
    uint32_t boot   = 0;
    uint64_t tails  = EngrossRefusedNow(&boot);
    uint64_t leaves = TallyDigestsRotKnown();
    bool     ranges = __atomic_load_n(&g_binder.nrot, __ATOMIC_ACQUIRE) != 0;
    error_t  rc     = OK;
    if (!g_book.stood)
        rc = ERR_UNSUPPORTED;
    else if (BinderNoWrite())
        rc = ERR_TAGFS_READ_ONLY;
    else if (ranges && !s_parted)
        rc = ERR_NO_MEMORY;
    else if (!ranges && tails == 0 && leaves == 0)
        rc = ERR_OBJECT_NOT_FOUND;
    if (rc == ERR_OBJECT_NOT_FOUND)
        NothingSaid(boot);
    if (rc == OK) {
        out->tails         = tails;
        out->digest_ranges = leaves;
        out->ranges        = ranges ? s_ranges : 0;
        out->files         = ranges ? s_files : 0;
        out->blocks        = ranges ? s_blocks : 0;
        EscheatReachTo(InkedGen());     /* before the ask is counted: a walk that sees the ask sees its reach */
        __atomic_add_fetch(&s_asked, 1u, __ATOMIC_ACQ_REL);
        BinderDemanded();
        EngrossAsked();
        kprintf("[TagFS] Escheat asked by a cabin wearing system or god: %llu rotten range(s) of FILES, %llu "
                "file(s) declared lost and %llu held block(s) let go by the next Tally, written whole, which lays "
                "%llu DIGESTS range(s) known to read in no copy anew of the data; %llu tail(s) that can never be laid "
                "in their blocks lost first%s\n", (unsigned long long)out->ranges, (unsigned long long)out->files,
                (unsigned long long)out->blocks, (unsigned long long)out->digest_ranges,
                (unsigned long long)out->tails,
                boot ? "; the tail of the file the loader reads the kernel by waits — no call changes it" : "");
    }
    tagfs_leave();
    return rc;
}

bool EscheatOwed(void)
{
    return __atomic_load_n(&s_asked, __ATOMIC_ACQUIRE) != __atomic_load_n(&s_answered, __ATOMIC_ACQUIRE);
}

uint64_t EscheatTailsActing(void)
{
    uint64_t asks = __atomic_load_n(&s_asked, __ATOMIC_ACQUIRE);
    return asks > __atomic_load_n(&s_walked, __ATOMIC_ACQUIRE) ? asks : 0;
}

uint64_t EscheatTailsReach(void)
{
    return __atomic_load_n(&s_reach, __ATOMIC_ACQUIRE);
}

void EscheatTailsWalked(uint64_t asks)
{
    uint64_t was = __atomic_load_n(&s_walked, __ATOMIC_ACQUIRE);
    while (asks > was && !__atomic_compare_exchange_n(&s_walked, &was, asks, false, __ATOMIC_ACQ_REL,
                                                      __ATOMIC_ACQUIRE))
        ;
}

void EscheatTailLost(void)
{
    __atomic_add_fetch(&s_tails, 1u, __ATOMIC_RELAXED);
}

// ----------------------------------------------------------------------------
// The Tally that owes it
// ----------------------------------------------------------------------------

/* The rotten FILES nodes and the files locked by name with them moved out of
 * the mount's lists — the lists keep their room, so a refusal puts them back
 * with no memory asked. False with no memory: nothing moved. */
static bool EscheatMoved(void)
{
    uint32_t nkept = 0;
    for (uint32_t i = 0; i < g_book.nkept; i++)
        nkept += FileInRot(g_book.kept[i].fid) ? 1u : 0u;
    s_rot  = kmalloc(sizeof(BinderKept) * g_binder.nrot);
    s_kept = nkept ? kmalloc(sizeof(BookKeptFile) * nkept) : NULL;
    if (!s_rot || (nkept && !s_kept)) {
        kfree(s_rot);
        kfree(s_kept);
        s_rot  = NULL;
        s_kept = NULL;
        return false;
    }
    memcpy(s_rot, g_binder.rot, sizeof(BinderKept) * g_binder.nrot);
    s_nrot        = g_binder.nrot;
    g_binder.nrot = 0;
    s_nkept       = 0;
    uint32_t left = 0;
    for (uint32_t i = 0; i < g_book.nkept; i++) {
        if (FileInRot(g_book.kept[i].fid))
            s_kept[s_nkept++] = g_book.kept[i];
        else
            g_book.kept[left++] = g_book.kept[i];
    }
    g_book.nkept = left;
    return true;
}

/* Every key the Tally after the act lays otherwise than the one before:
 * each range let go, each file lost with it, the windows of DIGESTS over the
 * ground let go — DogEar's, so the self-check holds the rest to the Tally
 * before (§16.5 п.9; the Tally is whole, its Foliation kept: EscheatTaken). */
static void EscheatKeys(void)
{
    for (uint32_t i = 0; i < s_nrot; i++)
        DogEarPut(TALLY_FILES, s_rot[i].at.has_lo ? s_rot[i].at.lo : (TallyKey){ 0, 0 },
                  s_rot[i].at.has_hi ? s_rot[i].at.hi : DOG_EAR_END);
    for (uint32_t i = 0; i < s_nkept; i++)
        DogEarPut(TALLY_FILES, (TallyKey){ s_kept[i].fid, 0 }, (TallyKey){ s_kept[i].fid + 1u, 0 });
    for (uint64_t i = 0; i < s_nfree; i++) {
        uint64_t lo = s_free[i].start > DIGEST_ITEM_MOST ? s_free[i].start - DIGEST_ITEM_MOST : 0;
        DogEarPut(TALLY_DIGESTS, (TallyKey){ lo, 0 }, (TallyKey){ s_free[i].start + s_free[i].count, 0 });
    }
}

void EscheatTake(void)
{
    if (!EscheatOwed() || s_taken)
        return;
    s_cover = __atomic_load_n(&s_asked, __ATOMIC_ACQUIRE);
    EscheatTailsWalked(s_cover);    /* a Tally begins with no Slip left: no tail is the act's any more */
    if (g_binder.nrot == 0 && TallyDigestsRotKnown() == 0) {
        /* No range to let go: the tails were the act's whole — with no Slip
         * left this Tally could be begun, and its DONE says the act. */
        __atomic_store_n(&s_answered, s_cover, __ATOMIC_RELEASE);
        return;
    }
    if (g_binder.nrot && !EscheatMoved()) {
        kprintf("[TagFS] Escheat: no memory to take the rotten ranges out of the mount's lists — the Tally names "
                "them as they lie, and the next one owes the act\n");
        return;
    }
    if (s_nrot) {
        /* What stays held is the Tallies' from here: every one digests it. */
        s_all            = g_binder.held;
        s_nall           = g_binder.nheld;
        s_capall         = g_binder.capheld;
        g_binder.held    = s_stay;
        g_binder.nheld   = s_nstay;
        g_binder.capheld = s_nstay;
        s_stay           = NULL;
        s_nstay          = 0;
    }
    EscheatKeys();
    s_taken = true;
}

bool EscheatTaken(void)
{
    return s_taken;
}

/* The ground let go resting in Fallow for the lag's stitches
 * (FALLOW_BY_SPINE, as a slot's ground no slot names does, binder_slots.c):
 * its blocks. */
static uint64_t EscheatRest(void)
{
    uint64_t n = 0, blocks = 0;
    for (uint64_t i = 0; i < s_nfree; i++)
        n += (s_free[i].count + UINT32_MAX - 1u) / UINT32_MAX;
    Strip* runs = n && n <= UINT32_MAX ? kmalloc(sizeof(Strip) * n) : NULL;
    for (uint64_t i = 0, k = 0; runs && i < s_nfree; i++)
        for (uint64_t at = s_free[i].start, left = s_free[i].count; left;) {
            uint32_t c = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
            runs[k++]  = (Strip){ .start = at, .count = c, .kind = STRIP_DATA };
            at += c;
            left -= c;
            blocks += c;
        }
    FallowNode* node = runs ? FallowNodeMake(runs, (uint32_t)n, FALLOW_BY_SPINE, 0) : NULL;
    kfree(runs);
    if (n && !node) {
        kprintf("[TagFS] Escheat: no memory to rest the held ground in Fallow — it is held until the volume is "
                "mounted again, when no Tally names it and it is free\n");
        return 0;
    }
    if (node) {
        TagFSLockTake(&tagfs_get_state()->lock);
        FallowSpineAdd(node, SettledCycles());
        TagFSLockDrop(&tagfs_get_state()->lock);
    }
    return blocks;
}

bool EscheatDigestsLaid(const TallyPlace* at)
{
    char hi[24];
    if (!s_taken)
        return false;
    if (s_ndig == s_capdig) {
        uint32_t       cap  = s_capdig ? s_capdig * 2u : 8u;
        EscheatBlocks* more = kmalloc(sizeof(EscheatBlocks) * cap);
        if (!more) {
            kprintf("[TagFS] Escheat: no memory to note a DIGESTS leaf that reads in no copy — the Tally names it "
                    "as it lies, and its blocks read unchecked until the act is asked again\n");
            return false;
        }
        if (s_ndig)
            memcpy(more, s_dig, sizeof(EscheatBlocks) * s_ndig);
        kfree(s_dig);
        s_dig    = more;
        s_capdig = cap;
    }
    s_dig[s_ndig++] = (EscheatBlocks){ at->has_lo ? at->lo.k1 : 0, at->has_hi ? at->hi.k1 : UINT64_MAX };
    if (at->has_hi)
        ksnprintf(hi, sizeof(hi), "%llu", (unsigned long long)at->hi.k1);
    else
        ksnprintf(hi, sizeof(hi), "the end");
    kprintf("[TagFS] Escheat: the DIGESTS leaf of blocks [%llu, %s) reads in no copy — the Tally lays it anew, the "
            "digest of each block a living file names taken of its data\n",
            (unsigned long long)(at->has_lo ? at->lo.k1 : 0), hi);
    return true;
}

error_t EscheatDigestOf(uint64_t block, uint64_t* digest, bool* have)
{
    BoxHashContext ctx;
    bool           in = false;
    *have = false;
    for (uint32_t i = 0; s_taken && !in && i < s_ndig; i++)
        in = s_dig[i].lo <= block && block < s_dig[i].hi;
    if (!in)
        return OK;
    if (!s_block && !(s_block = kmalloc(TAGFS_BLOCK_SIZE)))
        return ERR_NO_MEMORY;
    int rc = BoardroomRead(tagfs_get_seat(), tagfs_get_volume_base() + block * TAGFS_BLOCK_SECTORS,
                           TAGFS_BLOCK_SECTORS, s_block);
    if (rc == (int)ERR_BAD_SECTOR) {
        kprintf("[TagFS] Escheat: block %llu, whose DIGESTS leaf is laid anew, does not read — the Tally names no "
                "digest of it, and a read of it is refused\n", (unsigned long long)block);
        return OK;
    }
    if (rc != 0)
        return ERR_READ_FAILED;
    BoxHashInit(&ctx, tagfs_get_state()->uuid, 16);
    *digest   = BoxHashIntegrity(s_block, TAGFS_BLOCK_SIZE, &ctx);
    error_t g = GlossNote(block, *digest);
    if (g != OK)
        return g;
    s_digests++;
    *have = true;
    return OK;
}

// ----------------------------------------------------------------------------
// Its DONE, its refusal, the mount let go
// ----------------------------------------------------------------------------

/* The DIGESTS leaves laid anew let go of. */
static void EscheatDigestsLetGo(void)
{
    kfree(s_dig);
    kfree(s_block);
    s_dig     = NULL;
    s_block   = NULL;
    s_ndig    = s_capdig = 0;
    s_digests = 0;
}

/* What the act let go, kept for the rest of the mount: an open of a file it
 * lost answers that no such file is, as it does after the next mount — not
 * that the file is locked (BookFileLocked). */
static void EscheatLostKept(void)
{
    uint64_t* fids = s_nkept ? kmalloc(sizeof(uint64_t) * s_nkept) : NULL;
    if (s_nkept && !fids) {
        kprintf("[TagFS] Escheat: no memory to keep the ids it lost — an open of one is refused as a locked "
                "file's until the volume is mounted again\n");
        return;
    }
    for (uint32_t i = 0; i < s_nkept; i++)
        fids[i] = s_kept[i].fid;
    s_lostfid  = fids;
    s_nlostfid = s_nkept;
    s_lostrot  = s_rot;
    s_nlostrot = s_nrot;
    s_rot      = NULL;
    s_nrot     = 0;
    __atomic_store_n(&s_gone, 1u, __ATOMIC_RELEASE);
}

/* What the Tally under way took let go — the files lost and the whole held
 * list it replaced; what is kept of them is EscheatLostKept's. */
static void EscheatSpent(void)
{
    EscheatDigestsLetGo();
    for (uint32_t i = 0; i < s_nkept; i++)
        BookKeptLetGo(&s_kept[i]);
    kfree(s_rot);
    kfree(s_kept);
    kfree(s_all);
    kfree(s_free);
    s_rot   = NULL;
    s_kept  = NULL;
    s_all   = NULL;
    s_free  = NULL;
    s_nrot  = s_nkept = 0;
    s_nall  = s_capall = s_nfree = 0;
    s_taken = false;
}

void EscheatDone(uint64_t seq)
{
    uint64_t files = 0;
    if (!s_taken) {
        uint64_t tails = __atomic_exchange_n(&s_tails, 0u, __ATOMIC_ACQ_REL);
        if (tails)
            kprintf("[TagFS] Escheat laid by Tally %llu: 0 range(s) no Tally names, 0 file(s) lost, 0 held "
                    "block(s) rest in Fallow for the lag; %llu tail(s) lost\n", (unsigned long long)seq,
                    (unsigned long long)tails);
        return;
    }
    for (uint32_t i = 0; i < s_nrot; i++) {
        char hi[24];
        files += s_rot[i].at.count;
        if (s_rot[i].at.has_hi)
            ksnprintf(hi, sizeof(hi), "%llu", (unsigned long long)s_rot[i].at.hi.k1);
        else
            ksnprintf(hi, sizeof(hi), "the end");
        kprintf("[TagFS] Escheat: the FILES range fid [%llu, %s) no Tally names any more — its %llu file(s) are "
                "lost, their ids never given again\n", (unsigned long long)(s_rot[i].at.has_lo ? s_rot[i].at.lo.k1 : 0),
                hi, (unsigned long long)s_rot[i].at.count);
    }
    for (uint32_t i = 0; i < s_nkept; i++) {
        kprintf("[TagFS] Escheat: fid %llu, locked by name with a range let go, is lost\n",
                (unsigned long long)s_kept[i].fid);
        files += HeadBefore(s_kept[i].fid) ? 1u : 0u;
    }
    uint64_t blocks = EscheatRest();
    kprintf("[TagFS] Escheat laid by Tally %llu: %u range(s) no Tally names, %llu file(s) lost, %llu held "
            "block(s) rest in Fallow for the lag; %u DIGESTS leaf(s) laid anew of the data, %llu digest(s) taken; "
            "%llu tail(s) lost\n", (unsigned long long)seq, s_nrot, (unsigned long long)files,
            (unsigned long long)blocks, s_ndig, (unsigned long long)s_digests,
            (unsigned long long)__atomic_exchange_n(&s_tails, 0u, __ATOMIC_ACQ_REL));
    if (s_nrot || s_nkept)
        EscheatLostKept();
    EscheatSpent();
    __atomic_store_n(&s_answered, s_cover, __ATOMIC_RELEASE);
}

bool EscheatLost(uint64_t fid)
{
    if (!__atomic_load_n(&s_gone, __ATOMIC_ACQUIRE))
        return false;
    for (uint32_t i = 0; i < s_nlostrot; i++)
        if (FileMeets(fid, &s_lostrot[i].at))
            return true;
    for (uint32_t i = 0; i < s_nlostfid; i++)
        if (s_lostfid[i] == fid)
            return true;
    return false;
}

/* Kept file `k` back among the mount's, in order of fid — the list kept its
 * room. */
static void KeptBack(const BookKeptFile* k)
{
    uint32_t i = g_book.nkept++;
    while (i > 0 && g_book.kept[i - 1u].fid > k->fid) {
        g_book.kept[i] = g_book.kept[i - 1u];
        i--;
    }
    g_book.kept[i] = *k;
}

void EscheatTakenBack(void)
{
    if (!s_taken)
        return;
    for (uint32_t i = 0; i < s_nrot; i++)
        (void)BinderRotten(s_rot[i].tree, &s_rot[i].at);   /* into the room it kept: no memory asked */
    for (uint32_t i = 0; i < s_nkept; i++)
        KeptBack(&s_kept[i]);
    if (s_nrot) {
        s_stay           = g_binder.held;
        s_nstay          = g_binder.nheld;
        g_binder.held    = s_all;
        g_binder.nheld   = s_nall;
        g_binder.capheld = s_capall;
        s_all            = NULL;
        s_nall           = s_capall = 0;
    }
    EscheatDigestsLetGo();
    kfree(s_rot);
    kfree(s_kept);
    s_rot   = NULL;
    s_kept  = NULL;
    s_nrot  = s_nkept = 0;
    s_taken = false;
}

void EscheatLetGo(void)
{
    __atomic_store_n(&s_gone, 0u, __ATOMIC_RELEASE);
    EscheatSpent();
    PartedLetGo();
    kfree(s_lostrot);
    kfree(s_lostfid);
    s_lostrot  = NULL;
    s_lostfid  = NULL;
    s_nlostrot = s_nlostfid = 0;
    s_ranges   = s_files = s_blocks = 0;
    __atomic_store_n(&s_tails, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_asked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_answered, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_walked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_reach, 0u, __ATOMIC_RELEASE);
    s_cover = 0;
}
