#include "escheat.h"
#include "binder_internal.h"
#include "settle.h"
#include "../mount/book_mount.h"
#include "../ink/fallow.h"
#include "../helm/engross.h"
#include "../integrity/gloss.h"
#include "../box_hash/box_hash.h"
#include "../retire/retire.h"
#include "boardroom.h"
#include "tagfs_volume.h"

/*
 * Escheat (escheat.h). What the act lets go is the mount's: the rotten FILES
 * nodes (vol->binder.rot), what of the files locked by name with them read
 * (vol->book.kept) and the ground held for what rotted.
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
 * (vol->binder.nrot), none of them once a Tally has let them go. An ask is
 * answered by the Tally that took it: one asked while the act's own Tally is
 * under way is owed to the next (escheat.asked, escheat.answered).
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





/* Whether the keys of file `fid` meet rotten range `r`. */
static bool FileMeets(uint64_t fid, const TallyPlace* r)
{
    bool below = !r->has_hi || fid < r->hi.k1 || (fid == r->hi.k1 && r->hi.k2 > 0);
    bool above = !r->has_lo || r->lo.k1 <= fid;
    return below && above;
}

/* Whether `fid` meets a range of the mount's rotten FILES (vol->book.rot). */
static bool FileInRot(TagFSVolume* vol, uint64_t fid)
{
    for (uint32_t i = 0; i < vol->book.nrot; i++) {
        TallyPlace p = { .lo = vol->book.rot[i].lo, .has_lo = true, .hi = vol->book.rot[i].hi,
                         .has_hi = vol->book.rot[i].has_hi };
        if (FileMeets(fid, &p))
            return true;
    }
    return false;
}

/* Whether `fid`'s header lies before the rotten range it meets — read, its
 * Parcels in the range: a file the parent's count of the range does not
 * count, locked by name instead. A file whose header lies in the range is
 * counted there, whatever of it lies past. */
static bool HeadBefore(TagFSVolume* vol, uint64_t fid)
{
    for (uint32_t i = 0; i < vol->book.nrot; i++)
        if (vol->book.rot[i].lo.k1 == fid && vol->book.rot[i].lo.k2 > 0)
            return true;
    return false;
}

// ----------------------------------------------------------------------------
// The held ground parted at the mount
// ----------------------------------------------------------------------------

/* The DATA runs of every file locked by name the act does not let go, as its
 * Parcels lay — what every Tally carries of it — in order, those that meet
 * joined. False with no memory. */
static bool KeptRuns(TagFSVolume* vol, BinderGround** out, uint64_t* n)
{
    uint64_t cap = 0;
    for (uint32_t i = 0; i < vol->book.nkept; i++) {
        const BookKeptFile* k = &vol->book.kept[i];
        for (uint32_t p = 0; !FileInRot(vol, k->fid) && p < k->nparcels; p++) {
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
 * joined): what they name to escheat.stay, the rest to escheat.free. False with no
 * memory. */
static bool HeldParted(TagFSVolume* vol, const BinderGround* k, uint64_t nk)
{
    uint64_t capstay = 0, capfree = 0, j = 0;
    for (uint64_t i = 0; i < vol->binder.nheld; i++) {
        uint64_t at = vol->binder.held[i].start, end = at + vol->binder.held[i].count;
        while (at < end) {
            while (j < nk && k[j].start + k[j].count <= at)
                j++;
            bool     in = j < nk && k[j].start <= at;
            uint64_t to = j == nk ? end : in ? k[j].start + k[j].count : k[j].start;
            to          = to < end ? to : end;
            if (in ? !RunPut(&vol->escheat.stay, &vol->escheat.nstay, &capstay, at, to - at)
                   : !RunPut(&vol->escheat.free, &vol->escheat.nfree, &capfree, at, to - at))
                return false;
            vol->escheat.blocks += in ? 0 : to - at;
            at = to;
        }
    }
    return true;
}

static void PartedLetGo(TagFSVolume* vol)
{
    kfree(vol->escheat.stay);
    kfree(vol->escheat.free);
    vol->escheat.stay   = NULL;
    vol->escheat.free   = NULL;
    vol->escheat.nstay  = vol->escheat.nfree = 0;
    vol->escheat.parted = false;
}

void EscheatMounted(TagFSVolume* vol)
{
    BinderGround* k  = NULL;
    uint64_t      nk = 0;
    vol->escheat.ranges = vol->binder.nrot;
    vol->escheat.files  = 0;
    vol->escheat.blocks = 0;
    for (uint32_t i = 0; i < vol->binder.nrot; i++)
        vol->escheat.files += vol->binder.rot[i].at.count;
    for (uint32_t i = 0; i < vol->book.nkept; i++)
        vol->escheat.files += FileInRot(vol, vol->book.kept[i].fid) && HeadBefore(vol, vol->book.kept[i].fid) ? 1u : 0u;
    if (vol->binder.nrot == 0) {
        vol->escheat.parted = true;    /* nothing rotted: every held block is a locked file's, and stays */
        return;
    }
    vol->binder.nheld = vol->binder.nheld ? BinderGroundJoined(vol->binder.held, vol->binder.nheld) : 0;
    vol->escheat.parted       = KeptRuns(vol, &k, &nk) && HeldParted(vol, k, nk);
    kfree(k);
    if (!vol->escheat.parted) {
        PartedLetGo(vol);
        vol->escheat.blocks = 0;
        kprintf("[TagFS] no memory to part the ground held for its rotten ranges from the ground of the files "
                "locked for another reason — Escheat is refused to this mount\n");
    }
}

// ----------------------------------------------------------------------------
// The ask
// ----------------------------------------------------------------------------

/* The act's reach grown to `gen`, never back: of two asks made at once, the
 * newer's stands. */
static void EscheatReachTo(TagFSVolume* vol, uint64_t gen)
{
    uint64_t was = __atomic_load_n(&vol->escheat.reach, __ATOMIC_ACQUIRE);
    while (gen > was && !__atomic_compare_exchange_n(&vol->escheat.reach, &was, gen, false, __ATOMIC_ACQ_REL,
                                                     __ATOMIC_ACQUIRE))
        ;
}

error_t TagFSEscheat(TagFSVolume* vol, InkWarrant warrant, EscheatSaid* out)
{
    memset(out, 0, sizeof(*out));
    if (warrant != INK_WARRANTED) {
        kprintf("[TagFS] Escheat refused: the cabin asking wears neither system nor god\n");
        return ERR_PERMISSION_DENIED;
    }
    if (!tagfs_enter(vol))
        return ERR_DEVICE_NOT_READY;
    uint64_t tails  = EngrossRefusedNow(vol);
    uint64_t leaves = TallyDigestsRotKnown(vol);
    bool     ranges = __atomic_load_n(&vol->binder.nrot, __ATOMIC_ACQUIRE) != 0;
    error_t  rc     = OK;
    if (!vol->book.stood)
        rc = ERR_UNSUPPORTED;
    else if (BinderNoWrite(vol))
        rc = ERR_TAGFS_READ_ONLY;
    else if (ranges && !vol->escheat.parted)
        rc = ERR_NO_MEMORY;
    else if (!ranges && tails == 0 && leaves == 0)
        rc = ERR_OBJECT_NOT_FOUND;
    if (rc == OK) {
        out->tails         = tails;
        out->digest_ranges = leaves;
        out->ranges        = ranges ? vol->escheat.ranges : 0;
        out->files         = ranges ? vol->escheat.files : 0;
        out->blocks        = ranges ? vol->escheat.blocks : 0;
        EscheatReachTo(vol, InkedGen(vol));     /* before the ask is counted: a walk that sees the ask sees its reach */
        __atomic_add_fetch(&vol->escheat.asked, 1u, __ATOMIC_ACQ_REL);
        BinderTallyAsked(vol);
        EngrossAsked(vol);
        kprintf("[TagFS] Escheat asked by a cabin wearing system or god: %llu rotten range(s) of FILES, %llu "
                "file(s) declared lost and %llu held block(s) let go by the next Tally, written whole, which lays "
                "%llu DIGESTS range(s) known to read in no copy anew of the data; %llu tail(s) that can never be laid "
                "in their blocks lost first\n", (unsigned long long)out->ranges, (unsigned long long)out->files,
                (unsigned long long)out->blocks, (unsigned long long)out->digest_ranges,
                (unsigned long long)out->tails);
    }
    tagfs_leave(vol);
    return rc;
}

bool EscheatOwed(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->escheat.asked, __ATOMIC_ACQUIRE) != __atomic_load_n(&vol->escheat.answered, __ATOMIC_ACQUIRE);
}

uint64_t EscheatTailsActing(TagFSVolume* vol)
{
    uint64_t asks = __atomic_load_n(&vol->escheat.asked, __ATOMIC_ACQUIRE);
    return asks > __atomic_load_n(&vol->escheat.walked, __ATOMIC_ACQUIRE) ? asks : 0;
}

uint64_t EscheatTailsReach(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->escheat.reach, __ATOMIC_ACQUIRE);
}

void EscheatTailsWalked(TagFSVolume* vol, uint64_t asks)
{
    uint64_t was = __atomic_load_n(&vol->escheat.walked, __ATOMIC_ACQUIRE);
    while (asks > was && !__atomic_compare_exchange_n(&vol->escheat.walked, &was, asks, false, __ATOMIC_ACQ_REL,
                                                      __ATOMIC_ACQUIRE))
        ;
}

void EscheatTailLost(TagFSVolume* vol)
{
    __atomic_add_fetch(&vol->escheat.tails, 1u, __ATOMIC_RELAXED);
}

// ----------------------------------------------------------------------------
// The Tally that owes it
// ----------------------------------------------------------------------------

/* The rotten FILES nodes and the files locked by name with them moved out of
 * the mount's lists — the lists keep their room, so a refusal puts them back
 * with no memory asked. False with no memory: nothing moved. */
static bool EscheatMoved(TagFSVolume* vol)
{
    uint32_t nkept = 0;
    for (uint32_t i = 0; i < vol->book.nkept; i++)
        nkept += FileInRot(vol, vol->book.kept[i].fid) ? 1u : 0u;
    vol->escheat.rot  = kmalloc(sizeof(BinderKept) * vol->binder.nrot);
    vol->escheat.kept = nkept ? kmalloc(sizeof(BookKeptFile) * nkept) : NULL;
    if (!vol->escheat.rot || (nkept && !vol->escheat.kept)) {
        kfree(vol->escheat.rot);
        kfree(vol->escheat.kept);
        vol->escheat.rot  = NULL;
        vol->escheat.kept = NULL;
        return false;
    }
    memcpy(vol->escheat.rot, vol->binder.rot, sizeof(BinderKept) * vol->binder.nrot);
    vol->escheat.nrot        = vol->binder.nrot;
    vol->binder.nrot = 0;
    vol->escheat.nkept       = 0;
    uint32_t left = 0;
    for (uint32_t i = 0; i < vol->book.nkept; i++) {
        if (FileInRot(vol, vol->book.kept[i].fid))
            vol->escheat.kept[vol->escheat.nkept++] = vol->book.kept[i];
        else
            vol->book.kept[left++] = vol->book.kept[i];
    }
    vol->book.nkept = left;
    return true;
}

/* Every key the Tally after the act lays otherwise than the one before:
 * each range let go, each file lost with it, the windows of DIGESTS over the
 * ground let go — DogEar's, so the self-check holds the rest to the Tally
 * before (§16.5 п.9; the Tally is whole, its Foliation kept: EscheatTaken).
 * The windows whole, not the ground's own keys: a live block past ground let
 * go in its window begins an element of its own (DogEarGroundChanged), a key
 * the Tally before never had — one the self-check, where it is built
 * (binder_internal.h, BINDER_CHECKS), would meet outside DogEar. */
static void EscheatKeys(TagFSVolume* vol)
{
    for (uint32_t i = 0; i < vol->escheat.nrot; i++)
        DogEarPut(vol, TALLY_FILES, vol->escheat.rot[i].at.has_lo ? vol->escheat.rot[i].at.lo : (TallyKey){ 0, 0 },
                  vol->escheat.rot[i].at.has_hi ? vol->escheat.rot[i].at.hi : DOG_EAR_END);
    for (uint32_t i = 0; i < vol->escheat.nkept; i++)
        DogEarPut(vol, TALLY_FILES, (TallyKey){ vol->escheat.kept[i].fid, 0 }, (TallyKey){ vol->escheat.kept[i].fid + 1u, 0 });
    for (uint64_t i = 0; i < vol->escheat.nfree; i++)
        DogEarGroundChanged(vol, vol->escheat.free[i].start, vol->escheat.free[i].count);
}

void EscheatTake(TagFSVolume* vol)
{
    if (!EscheatOwed(vol) || vol->escheat.taken)
        return;
    vol->escheat.cover = __atomic_load_n(&vol->escheat.asked, __ATOMIC_ACQUIRE);
    EscheatTailsWalked(vol, vol->escheat.cover);    /* a Tally begins with no Slip left: no tail is the act's any more */
    if (vol->binder.nrot == 0 && TallyDigestsRotKnown(vol) == 0) {
        /* No range to let go: the tails were the act's whole — with no Slip
         * left this Tally could be begun, and its DONE says the act. */
        __atomic_store_n(&vol->escheat.answered, vol->escheat.cover, __ATOMIC_RELEASE);
        return;
    }
    if (vol->binder.nrot && !EscheatMoved(vol)) {
        kprintf("[TagFS] Escheat: no memory to take the rotten ranges out of the mount's lists — the Tally names "
                "them as they lie, and the next one owes the act\n");
        return;
    }
    if (vol->escheat.nrot) {
        /* What stays held is the Tallies' from here: every one digests it. */
        vol->escheat.all            = vol->binder.held;
        vol->escheat.nall           = vol->binder.nheld;
        vol->escheat.capall         = vol->binder.capheld;
        vol->binder.held    = vol->escheat.stay;
        vol->binder.nheld   = vol->escheat.nstay;
        vol->binder.capheld = vol->escheat.nstay;
        vol->escheat.stay           = NULL;
        vol->escheat.nstay          = 0;
    }
    EscheatKeys(vol);
    vol->escheat.taken = true;
}

bool EscheatTaken(TagFSVolume* vol)
{
    return vol->escheat.taken;
}

/* The ground let go resting in Fallow for the lag's stitches
 * (FALLOW_BY_SPINE, as a slot's ground no slot names does, binder_slots.c):
 * its blocks. */
static uint64_t EscheatRest(TagFSVolume* vol)
{
    uint64_t n = 0, blocks = 0;
    for (uint64_t i = 0; i < vol->escheat.nfree; i++)
        n += (vol->escheat.free[i].count + UINT32_MAX - 1u) / UINT32_MAX;
    Strip* runs = n && n <= UINT32_MAX ? kmalloc(sizeof(Strip) * n) : NULL;
    for (uint64_t i = 0, k = 0; runs && i < vol->escheat.nfree; i++)
        for (uint64_t at = vol->escheat.free[i].start, left = vol->escheat.free[i].count; left;) {
            uint32_t c = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
            runs[k++]  = (Strip){ .start = at, .count = c, .kind = STRIP_DATA };
            at += c;
            left -= c;
            blocks += c;
        }
    FallowNode* node = runs ? FallowNodeMake(vol, runs, (uint32_t)n, FALLOW_BY_SPINE, 0) : NULL;
    kfree(runs);
    if (n && !node) {
        kprintf("[TagFS] Escheat: no memory to rest the held ground in Fallow — it is held until the volume is "
                "mounted again, when no Tally names it and it is free\n");
        return 0;
    }
    if (node) {
        TagFSLockTake(&vol->state.lock);
        FallowSpineAdd(vol, node, SettledCycles(vol));
        TagFSLockDrop(&vol->state.lock);
    }
    return blocks;
}

bool EscheatDigestsLaid(TagFSVolume* vol, const TallyPlace* at)
{
    char hi[24];
    if (!vol->escheat.taken)
        return false;
    if (vol->escheat.ndig == vol->escheat.capdig) {
        uint32_t       cap  = vol->escheat.capdig ? vol->escheat.capdig * 2u : 8u;
        EscheatBlocks* more = kmalloc(sizeof(EscheatBlocks) * cap);
        if (!more) {
            kprintf("[TagFS] Escheat: no memory to note a DIGESTS leaf that reads in no copy — the Tally names it "
                    "as it lies, and its blocks read unchecked until the act is asked again\n");
            return false;
        }
        if (vol->escheat.ndig)
            memcpy(more, vol->escheat.dig, sizeof(EscheatBlocks) * vol->escheat.ndig);
        kfree(vol->escheat.dig);
        vol->escheat.dig    = more;
        vol->escheat.capdig = cap;
    }
    vol->escheat.dig[vol->escheat.ndig++] = (EscheatBlocks){ at->has_lo ? at->lo.k1 : 0, at->has_hi ? at->hi.k1 : UINT64_MAX };
    if (at->has_hi)
        ksnprintf(hi, sizeof(hi), "%llu", (unsigned long long)at->hi.k1);
    else
        ksnprintf(hi, sizeof(hi), "the end");
    kprintf("[TagFS] Escheat: the DIGESTS leaf of blocks [%llu, %s) reads in no copy — the Tally lays it anew, the "
            "digest of each block a living file names taken of its data\n",
            (unsigned long long)(at->has_lo ? at->lo.k1 : 0), hi);
    return true;
}

error_t EscheatDigestOf(TagFSVolume* vol, uint64_t block, uint64_t* digest, bool* have)
{
    BoxHashContext ctx;
    bool           in = false;
    *have = false;
    for (uint32_t i = 0; vol->escheat.taken && !in && i < vol->escheat.ndig; i++)
        in = vol->escheat.dig[i].lo <= block && block < vol->escheat.dig[i].hi;
    if (!in)
        return OK;
    if (!vol->escheat.block && !(vol->escheat.block = kmalloc(TAGFS_BLOCK_SIZE)))
        return ERR_NO_MEMORY;
    int rc = BoardroomRead(tagfs_get_seat(vol), tagfs_get_volume_base(vol) + block * TAGFS_BLOCK_SECTORS,
                           TAGFS_BLOCK_SECTORS, vol->escheat.block);
    if (rc == (int)ERR_BAD_SECTOR) {
        kprintf("[TagFS] Escheat: block %llu, whose DIGESTS leaf is laid anew, does not read — the Tally names no "
                "digest of it, and a read of it is refused\n", (unsigned long long)block);
        return OK;
    }
    if (rc != 0)
        return ERR_READ_FAILED;
    BoxHashInit(&ctx, vol->state.uuid, 16);
    *digest   = BoxHashIntegrity(vol->escheat.block, TAGFS_BLOCK_SIZE, &ctx);
    error_t g = GlossNote(vol, block, *digest);
    if (g != OK)
        return g;
    vol->escheat.digests++;
    *have = true;
    return OK;
}

// ----------------------------------------------------------------------------
// Its DONE, its refusal, the mount let go
// ----------------------------------------------------------------------------

/* The DIGESTS leaves laid anew let go of. */
static void EscheatDigestsLetGo(TagFSVolume* vol)
{
    kfree(vol->escheat.dig);
    kfree(vol->escheat.block);
    vol->escheat.dig     = NULL;
    vol->escheat.block   = NULL;
    vol->escheat.ndig    = vol->escheat.capdig = 0;
    vol->escheat.digests = 0;
}

/* What the act let go, kept for the rest of the mount: an open of a file it
 * lost answers that no such file is, as it does after the next mount — not
 * that the file is locked (BookFileLocked). */
static void EscheatLostKept(TagFSVolume* vol)
{
    uint64_t* fids = vol->escheat.nkept ? kmalloc(sizeof(uint64_t) * vol->escheat.nkept) : NULL;
    if (vol->escheat.nkept && !fids) {
        kprintf("[TagFS] Escheat: no memory to keep the ids it lost — an open of one is refused as a locked "
                "file's until the volume is mounted again\n");
        return;
    }
    for (uint32_t i = 0; i < vol->escheat.nkept; i++)
        fids[i] = vol->escheat.kept[i].fid;
    vol->escheat.lostfid  = fids;
    vol->escheat.nlostfid = vol->escheat.nkept;
    vol->escheat.lostrot  = vol->escheat.rot;
    vol->escheat.nlostrot = vol->escheat.nrot;
    vol->escheat.rot      = NULL;
    vol->escheat.nrot     = 0;
    __atomic_store_n(&vol->escheat.gone, 1u, __ATOMIC_RELEASE);
    RetireOwed(vol);       /* a program it lost leaves its tags owed (retire/retire.h) */
}

/* What the Tally under way took let go — the files lost and the whole held
 * list it replaced; what is kept of them is EscheatLostKept's. */
static void EscheatSpent(TagFSVolume* vol)
{
    EscheatDigestsLetGo(vol);
    for (uint32_t i = 0; i < vol->escheat.nkept; i++)
        BookKeptLetGo(&vol->escheat.kept[i]);
    kfree(vol->escheat.rot);
    kfree(vol->escheat.kept);
    kfree(vol->escheat.all);
    kfree(vol->escheat.free);
    vol->escheat.rot   = NULL;
    vol->escheat.kept  = NULL;
    vol->escheat.all   = NULL;
    vol->escheat.free  = NULL;
    vol->escheat.nrot  = vol->escheat.nkept = 0;
    vol->escheat.nall  = vol->escheat.capall = vol->escheat.nfree = 0;
    vol->escheat.taken = false;
}

void EscheatDone(TagFSVolume* vol, uint64_t seq)
{
    uint64_t files = 0;
    if (!vol->escheat.taken) {
        uint64_t tails = __atomic_exchange_n(&vol->escheat.tails, 0u, __ATOMIC_ACQ_REL);
        if (tails)
            kprintf("[TagFS] Escheat laid by Tally %llu: 0 range(s) no Tally names, 0 file(s) lost, 0 held "
                    "block(s) rest in Fallow for the lag; %llu tail(s) lost\n", (unsigned long long)seq,
                    (unsigned long long)tails);
        return;
    }
    for (uint32_t i = 0; i < vol->escheat.nrot; i++) {
        char hi[24];
        files += vol->escheat.rot[i].at.count;
        if (vol->escheat.rot[i].at.has_hi)
            ksnprintf(hi, sizeof(hi), "%llu", (unsigned long long)vol->escheat.rot[i].at.hi.k1);
        else
            ksnprintf(hi, sizeof(hi), "the end");
        kprintf("[TagFS] Escheat: the FILES range fid [%llu, %s) no Tally names any more — its %llu file(s) are "
                "lost, their ids never given again\n", (unsigned long long)(vol->escheat.rot[i].at.has_lo ? vol->escheat.rot[i].at.lo.k1 : 0),
                hi, (unsigned long long)vol->escheat.rot[i].at.count);
    }
    for (uint32_t i = 0; i < vol->escheat.nkept; i++) {
        kprintf("[TagFS] Escheat: fid %llu, locked by name with a range let go, is lost\n",
                (unsigned long long)vol->escheat.kept[i].fid);
        files += HeadBefore(vol, vol->escheat.kept[i].fid) ? 1u : 0u;
    }
    uint64_t blocks = EscheatRest(vol);
    kprintf("[TagFS] Escheat laid by Tally %llu: %u range(s) no Tally names, %llu file(s) lost, %llu held "
            "block(s) rest in Fallow for the lag; %u DIGESTS leaf(s) laid anew of the data, %llu digest(s) taken; "
            "%llu tail(s) lost\n", (unsigned long long)seq, vol->escheat.nrot, (unsigned long long)files,
            (unsigned long long)blocks, vol->escheat.ndig, (unsigned long long)vol->escheat.digests,
            (unsigned long long)__atomic_exchange_n(&vol->escheat.tails, 0u, __ATOMIC_ACQ_REL));
    if (vol->escheat.nrot || vol->escheat.nkept)
        EscheatLostKept(vol);
    EscheatSpent(vol);
    __atomic_store_n(&vol->escheat.answered, vol->escheat.cover, __ATOMIC_RELEASE);
}

bool EscheatLost(TagFSVolume* vol, uint64_t fid)
{
    if (!__atomic_load_n(&vol->escheat.gone, __ATOMIC_ACQUIRE))
        return false;
    for (uint32_t i = 0; i < vol->escheat.nlostrot; i++)
        if (FileMeets(fid, &vol->escheat.lostrot[i].at))
            return true;
    for (uint32_t i = 0; i < vol->escheat.nlostfid; i++)
        if (vol->escheat.lostfid[i] == fid)
            return true;
    return false;
}

/* Kept file `k` back among the mount's, in order of fid — the list kept its
 * room. */
static void KeptBack(TagFSVolume* vol, const BookKeptFile* k)
{
    uint32_t i = vol->book.nkept++;
    while (i > 0 && vol->book.kept[i - 1u].fid > k->fid) {
        vol->book.kept[i] = vol->book.kept[i - 1u];
        i--;
    }
    vol->book.kept[i] = *k;
}

void EscheatTakenBack(TagFSVolume* vol)
{
    if (!vol->escheat.taken)
        return;
    for (uint32_t i = 0; i < vol->escheat.nrot; i++)
        (void)BinderRotten(vol, vol->escheat.rot[i].tree, &vol->escheat.rot[i].at);   /* into the room it kept: no memory asked */
    for (uint32_t i = 0; i < vol->escheat.nkept; i++)
        KeptBack(vol, &vol->escheat.kept[i]);
    if (vol->escheat.nrot) {
        vol->escheat.stay           = vol->binder.held;
        vol->escheat.nstay          = vol->binder.nheld;
        vol->binder.held    = vol->escheat.all;
        vol->binder.nheld   = vol->escheat.nall;
        vol->binder.capheld = vol->escheat.capall;
        vol->escheat.all            = NULL;
        vol->escheat.nall           = vol->escheat.capall = 0;
    }
    EscheatDigestsLetGo(vol);
    kfree(vol->escheat.rot);
    kfree(vol->escheat.kept);
    vol->escheat.rot   = NULL;
    vol->escheat.kept  = NULL;
    vol->escheat.nrot  = vol->escheat.nkept = 0;
    vol->escheat.taken = false;
}

void EscheatLetGo(TagFSVolume* vol)
{
    __atomic_store_n(&vol->escheat.gone, 0u, __ATOMIC_RELEASE);
    EscheatSpent(vol);
    PartedLetGo(vol);
    kfree(vol->escheat.lostrot);
    kfree(vol->escheat.lostfid);
    vol->escheat.lostrot  = NULL;
    vol->escheat.lostfid  = NULL;
    vol->escheat.nlostrot = vol->escheat.nlostfid = 0;
    vol->escheat.ranges   = vol->escheat.files = vol->escheat.blocks = 0;
    __atomic_store_n(&vol->escheat.tails, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->escheat.asked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->escheat.answered, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->escheat.walked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->escheat.reach, 0u, __ATOMIC_RELEASE);
    vol->escheat.cover = 0;
}
