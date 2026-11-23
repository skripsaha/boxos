#include "ribbon.h"
#include "book_mount.h"
#include "errata.h"
#include "../file_map/file_map.h"
#include "../file_map/tail_slip.h"
#include "../tag_registry/tag_registry.h"
#include "tagfs_volume.h"

/*
 * The rules a record of the ribbon is replayed by, one function a kind, for
 * pass 2 (ribbon_transcribe.c): each reads the file as the stitch leaves it so
 * far, holds the record to it, and puts what the record makes of it into the
 * stitch's stage — a refusal of the whole stitch, or the rule of rot for the
 * one file, said in bookmount.py's words (_Record, _File, _Label, _Slip).
 */

static const char* const g_kind_names[RIBBON_KINDS_KNOWN + 1u] = {
    "record", "StitchHead", "Clasp", "Undone", "Born", "Shaped", "Labeled", "Quitclaim", "Coined",
    "UseContext", "Frozen", "Thawed", "VolumeName",
};


/* A record's runs walked again — the cursor from *cursor — each DATA run not
 * SHARED handed its blocks' digests in order (Gloss), and every DATA run, when
 * `fallow`, let go: the ground of a record an Undone passes over (§2.8). */
static error_t RibbonRunsSeen(TagFSVolume* vol, RibbonStage* st, const uint8_t* runs, uint32_t bytes, uint64_t n, uint64_t* cursor,
                              const uint8_t* digests, uint64_t ndigests, uint64_t* k, bool fallow)
{
    uint32_t at = 0;
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        RibbonRunGet(runs, bytes, &at, cursor, st->u->seq, &r);
        if (r.hole)
            continue;
        if (fallow && RibbonStageFallow(vol, st, r.start, r.count, true) != OK)
            return ERR_NO_MEMORY;
        for (uint64_t b = 0; !fallow && RibbonRunFresh(&r) && b < r.count && *k < ndigests; b++) {
            if (!RibbonGrow((void**)&st->digests, st->ndigests, &st->capdigests, sizeof(RibbonDigest)))
                return ERR_NO_MEMORY;
            st->digests[st->ndigests++] = (RibbonDigest){ r.start + b, TallyDigestAt(digests, (*k)++) };
        }
    }
    return OK;
}

/* Whether a run of the record carries an extension this reader does not
 * carry on: its file is kept to be said (§8: CODEC, COPY, a SHARED it cannot
 * read as SHARED — RibbonRunCarried). */
static bool RibbonRunsExt(const uint8_t* runs, uint32_t bytes, uint64_t n, uint64_t cursor, uint64_t seq)
{
    uint32_t at = 0;
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        RibbonRunGet(runs, bytes, &at, &cursor, seq, &r);
        if (RibbonRunCarried(&r) == RUN_CARRIED_NOT)
            return true;
    }
    return false;
}

/* A record's runs as Strips — a run longer than a Strip counts laid as many —
 * into `out` when it is given: how many there are. */
static uint64_t RibbonStripsInto(const uint8_t* runs, uint32_t bytes, uint64_t n, uint64_t cursor, uint64_t seq,
                                 Strip* out)
{
    uint32_t at    = 0;
    uint64_t count = 0;
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        RibbonRunGet(runs, bytes, &at, &cursor, seq, &r);
        uint32_t kind = r.hole ? STRIP_HOLE : RibbonRunCarried(&r) == RUN_CARRIED_SHARED ? STRIP_SHARED : STRIP_DATA;
        for (uint64_t left = r.count, start = r.start; left;) {
            uint32_t k = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
            if (out)
                out[count] = (Strip){ .start = r.hole ? 0 : start, .count = k, .kind = kind, .birth = seq };
            count++;
            start += r.hole ? 0 : k;
            left  -= k;
        }
    }
    return count;
}

/* The same into a new array the caller frees (*out, *nout). ERR_NO_MEMORY. */
static error_t RibbonStrips(const uint8_t* runs, uint32_t bytes, uint64_t n, uint64_t cursor, uint64_t seq,
                            Strip** out, uint64_t* nout)
{
    *nout = RibbonStripsInto(runs, bytes, n, cursor, seq, NULL);
    *out  = kmalloc(sizeof(Strip) * (*nout ? *nout : 1u));
    if (!*out)
        return ERR_NO_MEMORY;
    RibbonStripsInto(runs, bytes, n, cursor, seq, *out);
    return OK;
}

// ----------------------------------------------------------------------------
// The rules of §3.7
// ----------------------------------------------------------------------------

/* A record that does not fit the file it names (§3.9, the rule of rot): said;
 * the file's records of the stitch passed over from here, and the stitch
 * gathered again without any of them (ribbon_transcribe.c, RibbonStitchApplied)
 * — the file where the stitch found it; and the roll-back a verdict the
 * session's first stitch lays down as an Undone of the stitch, why 2
 * (errata.h). */
static error_t RibbonMismatch(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, uint64_t kind, uint64_t fid, const char* fmt, ...)
{
    char    why[192];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    RibbonSay(vol, rp->w, RIBBON_CODE_BASE_MISMATCH, "stitch %llu's %s of fid %llu does not fit the file: %s — the file "
              "stays as it last matched (§3.9, the rule of rot)", (unsigned long long)st->u->seq,
              g_kind_names[kind], (unsigned long long)fid, why);
    st->rot = true;
    if (!RibbonListAdd(&rp->mismatched, &rp->nmismatched, &rp->capmismatched, fid) ||
        RibbonSkipPut(&rp->undone, st->u->seq, fid) != OK)
        return ERR_NO_MEMORY;
    return ErrataAdd(vol, st->u->seq, UNDONE_ROT, fid);
}

/* Whether a body read as its kind is read: refused, said, when it does not
 * read or is longer than this reader knows (§2.3, M-8). */
static bool RibbonBodyTaken(RibbonStage* st, uint64_t kind, int got, uint32_t len, uint32_t used)
{
    if (got == TALLY_BODY_BROKEN)
        RibbonRefuse(st, "its %s does not read: a field is cut short, or written as the book never writes one",
                     g_kind_names[kind]);
    else if (got == TALLY_BODY_LONGER)
        RibbonRefuse(st, "its %s is %u byte(s) longer than this reader knows — written by a kernel that knows "
                     "more", g_kind_names[kind], len - used);
    return got == TALLY_BODY_KNOWN;
}

/* Whether `fid` is locked by name, by the Tally or the mount. */
static bool RibbonLockedByName(TagFSVolume* vol, uint64_t fid)
{
    uint32_t lo = 0, hi = vol->book.nlocked;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (vol->book.locked[mid] == fid)
            return true;
        if (vol->book.locked[mid] < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return false;
}

/* Is `fid` locked for the ribbon: by name, or as a file of a rotten FILES
 * range no Born gave birth to anew (§4.5 п.3 (б)) — its records stay with the
 * range (bookmount.py _Locked). */
static bool RibbonLocked(TagFSVolume* vol, RibbonStage* st, uint64_t fid)
{
    bool        held;
    FileRecord* now  = RibbonFileNow(vol, st, fid, &held);
    bool        live = now != NULL;
    if (held)
        FileRecordRelease(now);
    return RibbonLockedByName(vol, fid) || (!live && BookRotLocks(vol, fid));
}

/* Whether tag `id` is one a record may wear: coined earlier in the stitch,
 * held by the registry, or named by the book and set aside — a tag the
 * registry cannot hold is a tag all the same (bookmount.py keeps every one
 * TAGS and a Coined name, BookTagAsideOf) — and not retired, which is an id
 * and no tag. */
static bool RibbonTagKnown(TagFSVolume* vol, RibbonStage* st, uint64_t id)
{
    TagSpelling sp;
    for (uint32_t i = 0; i < st->ncoins; i++)
        if (st->coins[i].id == id)
            return !(st->coins[i].tag.flags & TALLY_TAG_RETIRED);
    return (id <= TAGFS_MAX_TAG_ID &&
            TagRegistrySpelling(vol->state.registry, (uint32_t)id, &sp, NULL, NULL)) ||
           BookTagAsideOf(vol, id) != NULL;
}

/* The tags of a rising list that are not known, as the census writes them —
 * empty when every one is. */
static void RibbonTagsUnknown(TagFSVolume* vol, RibbonStage* st, const uint8_t* ids, uint32_t bytes, uint64_t n, char* out,
                              size_t room)
{
    uint32_t at = 0;
    uint64_t id = 0;
    size_t   w  = 0;
    out[0] = '\0';
    for (uint64_t k = 0; k < n; k++) {
        TallyIdNext(ids, bytes, &at, &id, k == 0);
        if (!RibbonTagKnown(vol, st, id) && w + 24u < room)
            w += (size_t)ksnprintf(out + w, room - w, w ? ", %llu" : "[%llu", (unsigned long long)id);
    }
    if (w && w + 2u < room)
        ksnprintf(out + w, room - w, "]");
}

/* A Slip laid on the record's last block — held to its rules: inside the
 * block and not past the file's size (§2.8). OK; *fits false when it does
 * not; ERR_NO_MEMORY. */
static error_t RibbonSlipLaid(FileRecord* rec, const RibbonSlip* slip, bool* fits)
{
    *fits = true;
    if (!slip->len)
        return OK;
    uint64_t last = rec->size ? (rec->size - 1u) / TAGFS_BLOCK_SIZE : 0;
    if (!rec->size || slip->off > TAGFS_BLOCK_SIZE || slip->len > TAGFS_BLOCK_SIZE - slip->off ||
        last * TAGFS_BLOCK_SIZE + slip->off + slip->len > rec->size) {
        *fits = false;
        return OK;
    }
    TailSlip* laid = TailSlipLaid(rec->slip, (uint32_t)slip->off, slip->bytes, (uint32_t)slip->len);
    if (!laid)
        return ERR_NO_MEMORY;
    TailSlipLetGo(rec->slip);
    rec->slip = laid;
    return OK;
}

/* The blocks `size` bytes need. */
static uint64_t RibbonBlocksOf(uint64_t size)
{
    return size / TAGFS_BLOCK_SIZE + (size % TAGFS_BLOCK_SIZE != 0);
}

/* What a new record of a file must hold before it stands — its blocks held to
 * its size before it was built (RibbonBornApplied, RibbonSplices): its Slip
 * inside its last block. *fits false — said — when not. */
static error_t RibbonRecordFits(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, uint64_t kind, FileRecord* rec,
                                const RibbonSlip* slip, bool* fits)
{
    error_t rc = RibbonSlipLaid(rec, slip, fits);
    if (rc == OK && !*fits)
        rc = RibbonMismatch(vol, rp, st, kind, rec->file_id, "its Slip of %llu bytes at %llu does not lie inside the "
                            "last block of %llu bytes", (unsigned long long)slip->len,
                            (unsigned long long)slip->off, (unsigned long long)rec->size);
    return rc;
}


// ----------------------------------------------------------------------------
// Born, Shaped, Labeled, Quitclaim
// ----------------------------------------------------------------------------

/* A Born's file built: its name, the tags this kernel can wear, its runs, and
 * the stitch's seq for its gen. NULL with no memory. */
static FileRecord* RibbonBornRecord(RibbonStage* st, const RibbonBorn* b, uint64_t seq)
{
    TagFSMetadata meta;
    uint32_t      at = 0;
    uint64_t      id = 0, nstrips = 0;
    FileRecord*   rec = NULL;
    memset(&meta, 0, sizeof(meta));
    meta.filename = kmalloc(b->name_len + 1u);
    meta.tag_ids  = kmalloc(sizeof(uint32_t) * (b->ntags ? b->ntags : 1u));
    meta.shown_to = b->nshown ? kmalloc(sizeof(uint64_t) * b->nshown) : NULL;
    if (meta.filename && meta.tag_ids && (!b->nshown || meta.shown_to) &&
        RibbonStrips(b->runs, b->runs_bytes, b->nruns, 0, seq, &meta.extents, &nstrips) == OK &&
        nstrips <= UINT32_MAX) {
        memcpy(meta.filename, b->name, b->name_len);
        meta.filename[b->name_len] = '\0';
        for (uint64_t k = 0; k < b->ntags; k++) {
            TallyIdNext(b->tags, b->tags_bytes, &at, &id, k == 0);
            if (id <= TAGFS_MAX_TAG_ID)
                meta.tag_ids[meta.tag_count++] = (uint32_t)id;
        }
        meta.file_id       = b->fid;
        meta.flags         = (uint32_t)b->flags;
        meta.size          = b->size;
        meta.created_time  = b->created_ns;
        meta.modified_time = b->modified_ns;
        meta.made_by       = b->made_by;
        meta.passport      = b->passport;
        meta.shown_count   = (uint32_t)b->nshown;
        meta.head_len      = (uint8_t)b->head_len;
        meta.extent_count  = (uint32_t)nstrips;
        if (b->head_len)
            memcpy(meta.head, b->head, b->head_len);
        if (b->nshown)
            TallyFidsOut(b->shown, b->shown_bytes, b->nshown, meta.shown_to);
        if ((rec = FileRecordBuild(&meta)) != NULL && RibbonHighSet(st, b->fid, (uint32_t)(b->flags >> 32)) != OK) {
            FileRecordRelease(rec);
            rec = NULL;
        }
    }
    kfree(meta.filename);
    kfree(meta.tag_ids);
    kfree(meta.shown_to);
    kfree(meta.extents);
    if (rec)
        rec->gen = seq;
    return rec;
}

/* The Born's digests staged for its blocks, its ext noted, and the file put
 * into the stage — when it holds its size and its Slip. */
static error_t RibbonBornStands(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonBorn* b, FileRecord* rec)
{
    bool     fits = false;
    uint64_t cursor = 0, k = 0;
    error_t  rc = RibbonRecordFits(vol, rp, st, RIBBON_BORN, rec, &b->slip, &fits);
    if (rc == OK && fits)
        rc = RibbonRunsSeen(vol, st, b->runs, b->runs_bytes, b->nruns, &cursor, b->digests, b->ndigests, &k, false);
    if (rc == OK && fits && RibbonRunsExt(b->runs, b->runs_bytes, b->nruns, 0, st->u->seq) &&
        !RibbonListAdd(&rp->ext, &rp->next, &rp->capext, b->fid))
        rc = ERR_NO_MEMORY;
    if (rc != OK || !fits) {
        FileRecordRelease(rec);
        return rc;
    }
    return RibbonStagePut(st, b->fid, rec);
}

/* Born (§2.8, §3.7): a file that does not exist, ACTIVE, wearing tags the
 * volume knows. */
static error_t RibbonBornApplied(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonRecord* r)
{
    RibbonBorn  b;
    uint32_t    used = 0;
    uint64_t    seq  = st->u->seq, cursor = 0, k = 0;
    bool        held;
    char        unknown[160];
    if (!RibbonBodyTaken(st, RIBBON_BORN, RibbonBornGet(st->u->stream + r->body, r->len, seq, &b, &used), r->len,
                         used))
        return OK;
    if (RibbonSkipHas(&rp->undone, seq, b.fid))
        return RibbonRunsSeen(vol, st, b.runs, b.runs_bytes, b.nruns, &cursor, NULL, 0, &k, true);
    FileRecord* now = RibbonFileNow(vol, st, b.fid, &held);
    if (held)
        FileRecordRelease(now);
    if (now || RibbonLockedByName(vol, b.fid)) {
        RibbonRefuse(st, "its Born gives birth to fid %llu, which exists", (unsigned long long)b.fid);
        return OK;
    }
    if (!(b.flags & TALLY_FILE_ACTIVE)) {
        RibbonRefuse(st, "its Born of fid %llu is not ACTIVE — bit 0 of a file's flags is always 1 (§2.8)",
                     (unsigned long long)b.fid);
        return OK;
    }
    error_t rc = BookBornNoted(vol, b.fid);
    if (rc != OK)
        return rc;
    RibbonTagsUnknown(vol, st, b.tags, b.tags_bytes, b.ntags, unknown, sizeof(unknown));
    if (unknown[0])
        return RibbonMismatch(vol, rp, st, RIBBON_BORN, b.fid, "it wears tag id(s) %s that no Coined or TAGS names or that are "
                              "RETIRED", unknown);
    /* Before a Strip is counted or an array of them asked for: a hole of
     * 2^62 blocks is a record that does not fit, not a want of memory. */
    if (b.blocks != RibbonBlocksOf(b.size))
        return RibbonMismatch(vol, rp, st, RIBBON_BORN, b.fid, "its runs cover %llu blocks and its %llu bytes need %llu",
                              (unsigned long long)b.blocks, (unsigned long long)b.size,
                              (unsigned long long)RibbonBlocksOf(b.size));
    FileRecord* rec = RibbonBornRecord(st, &b, seq);
    return rec ? RibbonBornStands(vol, rp, st, &b, rec) : ERR_NO_MEMORY;
}

/* Whether a Shaped's splices fit a file of `blocks`: each takes out blocks
 * the file has as the splices before it leave it, and the file they leave
 * holds what its size needs — held before a Strip of them is counted (a hole
 * of 2^62 blocks is a record that does not fit, not a want of memory). Why
 * not into `why`. */
static bool RibbonSplicesFit(const RibbonShaped* d, uint64_t seq, uint64_t blocks, char* why, size_t room)
{
    uint32_t     at = 0;
    uint64_t     cursor = 0, digests = 0, end = 0;
    RibbonSplice s;
    for (uint64_t i = 0; i < d->nsplice; i++) {
        RibbonSpliceNext(d->splices, d->splices_bytes, &at, &cursor, seq, &digests, &s);
        if (s.at > blocks - end || s.remove > blocks - end - s.at) {
            uint64_t from = s.at > UINT64_MAX - end ? UINT64_MAX : end + s.at;
            ksnprintf(why, room, "a splice takes out blocks %llu..%llu of a file of %llu", (unsigned long long)from,
                      (unsigned long long)(s.remove > UINT64_MAX - from ? UINT64_MAX : from + s.remove - 1u),
                      (unsigned long long)blocks);
            return false;
        }
        uint64_t kept = blocks - s.remove;
        if (s.blocks > UINT64_MAX - kept) {
            ksnprintf(why, room, "its runs cover more blocks than 2^64 - 1 and its %llu bytes need %llu",
                      (unsigned long long)d->size, (unsigned long long)RibbonBlocksOf(d->size));
            return false;
        }
        end    = end + s.at + s.blocks;
        blocks = kept + s.blocks;
    }
    if (blocks == RibbonBlocksOf(d->size))
        return true;
    ksnprintf(why, room, "its runs cover %llu blocks and its %llu bytes need %llu", (unsigned long long)blocks,
              (unsigned long long)d->size, (unsigned long long)RibbonBlocksOf(d->size));
    return false;
}

/* A Shaped's splices as the mirror's (file_record.h BookSplice): `at` counted
 * in the file as the splices before it leave it, the inserted runs as Strips
 * in one array (*strips, the caller's to free). *fits false — with why — for
 * splices that do not fit the file (RibbonSplicesFit). */
static error_t RibbonSplices(const RibbonShaped* d, uint64_t seq, uint64_t blocks, BookSplice* sp, Strip** strips,
                             bool* fits, char* why, size_t room)
{
    uint32_t     at = 0;
    uint64_t     cursor = 0, digests = 0, end = 0, total = 0;
    RibbonSplice s;
    *strips = NULL;
    *fits   = RibbonSplicesFit(d, seq, blocks, why, room);
    if (!*fits)
        return OK;
    for (uint64_t i = 0; i < d->nsplice; i++) {
        RibbonSpliceNext(d->splices, d->splices_bytes, &at, &cursor, seq, &digests, &s);
        sp[i] = (BookSplice){ .at = end + s.at, .remove = s.remove,
                              .ninsert = RibbonStripsInto(s.runs, s.runs_bytes, s.ninsert, s.cursor, seq, NULL) };
        total += sp[i].ninsert;
        end    = end + s.at + s.blocks;
    }
    Strip* all = kmalloc(sizeof(Strip) * (total ? total : 1u));
    if (!all)
        return ERR_NO_MEMORY;
    at = 0, cursor = 0, total = 0;
    for (uint64_t i = 0; i < d->nsplice; i++) {
        RibbonSpliceNext(d->splices, d->splices_bytes, &at, &cursor, seq, &digests, &s);
        RibbonStripsInto(s.runs, s.runs_bytes, s.ninsert, s.cursor, seq, all + total);
        sp[i].insert = all + total;
        total       += sp[i].ninsert;
    }
    *strips = all;
    return OK;
}

/* The Shaped applied to `now`: the new record, the ground it lets go to the
 * stage's Fallow — NULL with *fits false when a splice does not fit. */
static error_t RibbonShapedRecord(TagFSVolume* vol, RibbonStage* st, const RibbonShaped* d, const FileRecord* now, FileRecord** out,
                                  bool* fits, char* why, size_t room)
{
    BookSplice* sp     = kmalloc(sizeof(BookSplice) * (d->nsplice ? d->nsplice : 1u));
    Strip*      strips = NULL;
    Strip*      freed  = NULL;
    uint64_t    nfreed = 0;
    *out = NULL;
    if (!sp)
        return ERR_NO_MEMORY;
    error_t rc = RibbonSplices(d, st->u->seq, now->blocks, sp, &strips, fits, why, room);
    if (rc == OK && *fits)
        rc = FileRecordSplice(vol, now, sp, d->nsplice, d->size, d->mtime_ns, out, &freed, &nfreed);
    for (uint64_t i = 0; rc == OK && i < nfreed; i++)
        rc = RibbonStageFallow(vol, st, freed[i].start, freed[i].count, false);
    kfree(freed);
    kfree(strips);
    kfree(sp);
    if (rc != OK && *out) {
        FileRecordRelease(*out);
        *out = NULL;
    }
    return rc;
}

/* The file `fid` as the stitch leaves it so far, held to the record's base:
 * *now the file (a reference the caller lets go), or NULL — said — when the
 * file does not stand at the gen the base names. */
static error_t RibbonBased(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, uint64_t kind, uint64_t fid, uint64_t base,
                           FileRecord** now)
{
    bool        held;
    FileRecord* rec = RibbonFileNow(vol, st, fid, &held);
    uint64_t    seq = st->u->seq;
    *now = NULL;
    if (rec && base <= seq && rec->gen == seq - base) {
        if (!held)
            __atomic_add_fetch(&rec->refs, 1, __ATOMIC_ACQ_REL);
        *now = rec;
        return OK;
    }
    char stands[32];
    if (rec)
        ksnprintf(stands, sizeof(stands), "%llu", (unsigned long long)rec->gen);
    else
        ksnprintf(stands, sizeof(stands), "nothing");
    if (held)
        FileRecordRelease(rec);
    return RibbonMismatch(vol, rp, st, kind, fid, "its base names the file at %lld and the file stands at %s",
                          (long long)(seq - base), stands);
}

/* The Shaped's digests staged for the blocks its splices put in, its ext
 * noted, and the file put into the stage. */
static error_t RibbonShapedStands(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonShaped* d, FileRecord* rec)
{
    RibbonSplice s;
    uint32_t     at = 0;
    uint64_t     cursor = 0, digests = 0, k = 0, seq = st->u->seq;
    bool         ext = false;
    for (uint64_t i = 0; i < d->nsplice; i++) {
        RibbonSpliceNext(d->splices, d->splices_bytes, &at, &cursor, seq, &digests, &s);
        uint64_t c = s.cursor;
        ext |= RibbonRunsExt(s.runs, s.runs_bytes, s.ninsert, s.cursor, seq);
        if (RibbonRunsSeen(vol, st, s.runs, s.runs_bytes, s.ninsert, &c, d->digests, d->ndigests, &k, false) != OK) {
            FileRecordRelease(rec);
            return ERR_NO_MEMORY;
        }
    }
    if (ext && !RibbonListAdd(&rp->ext, &rp->next, &rp->capext, d->fid)) {
        FileRecordRelease(rec);
        return ERR_NO_MEMORY;
    }
    return RibbonStagePut(st, d->fid, rec);
}

/* Shaped (§2.8, §3.7): the file at its base, its splices fitting its blocks,
 * its blocks its size, its Slip in its last block — the Slips before it kept
 * by their one rule, the Ink's (FileRecordSplice, TailSlipKept): a splice that
 * changes the last block lets them go (§16.3 п.7), the size cuts them (§16.5
 * п.13). */
static error_t RibbonShapedApplied(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonRecord* r)
{
    RibbonShaped d;
    FileRecord * now = NULL, *rec = NULL;
    uint32_t     used = 0, at = 0;
    uint64_t     seq = st->u->seq, cursor = 0, k = 0;
    bool         fits = true;
    char         why[96];
    if (!RibbonBodyTaken(st, RIBBON_SHAPED, RibbonShapedGet(st->u->stream + r->body, r->len, seq, &d, &used),
                         r->len, used))
        return OK;
    if (RibbonSkipHas(&rp->undone, seq, d.fid)) {
        RibbonSplice s;
        uint64_t     digests = 0;
        for (uint64_t i = 0; i < d.nsplice; i++) {
            RibbonSpliceNext(d.splices, d.splices_bytes, &at, &cursor, seq, &digests, &s);
            uint64_t c = s.cursor;
            if (RibbonRunsSeen(vol, st, s.runs, s.runs_bytes, s.ninsert, &c, NULL, 0, &k, true) != OK)
                return ERR_NO_MEMORY;
        }
        return OK;
    }
    if (RibbonLocked(vol, st, d.fid))
        return OK;
    error_t rc = RibbonBased(vol, rp, st, RIBBON_SHAPED, d.fid, d.base, &now);
    if (rc != OK || !now)
        return rc;
    rc = RibbonShapedRecord(vol, st, &d, now, &rec, &fits, why, sizeof(why));
    FileRecordRelease(now);
    if (rc != OK)
        return rc;
    if (!fits)
        return RibbonMismatch(vol, rp, st, RIBBON_SHAPED, d.fid, "%s", why);
    rec->gen = seq;
    rc = RibbonRecordFits(vol, rp, st, RIBBON_SHAPED, rec, &d.slip, &fits);
    if (rc != OK || !fits) {
        FileRecordRelease(rec);
        return rc;
    }
    return RibbonShapedStands(vol, rp, st, &d, rec);
}

/* The tags a Labeled leaves on `now` into `out` (room for now's and every one
 * it adds): rising, those taken off gone. *fits false — said — when it puts
 * on a tag worn, takes off one not worn, or puts on one not known. */
static error_t RibbonLabelTags(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonLabeled* d, const FileRecord* now,
                               uint32_t* out, uint32_t* n, bool* fits)
{
    uint32_t at = 0;
    uint64_t id = 0;
    char     unknown[160];
    *n = 0;
    for (uint32_t i = 0; i < now->tag_count; i++)
        out[(*n)++] = now->tags[i];
    for (uint64_t k = 0; k < d->nadd; k++) {
        TallyIdNext(d->add, d->add_bytes, &at, &id, k == 0);
        bool worn = false;
        for (uint32_t i = 0; i < now->tag_count; i++)
            worn |= now->tags[i] == id;
        if (worn) {
            *fits = false;
            break;
        }
        if (id > TAGFS_MAX_TAG_ID)
            continue;           /* not worn in memory: past what it holds, said where the tag was read */
        uint32_t w = (*n)++;
        while (w > 0 && out[w - 1u] > id) {
            out[w] = out[w - 1u];
            w--;
        }
        out[w] = (uint32_t)id;
    }
    at = 0;
    for (uint64_t k = 0; *fits && k < d->ndrop; k++) {
        uint32_t w = 0, kept = 0;
        TallyIdNext(d->drop, d->drop_bytes, &at, &id, k == 0);
        for (uint32_t i = 0; i < now->tag_count; i++)
            w += now->tags[i] == id;
        for (uint32_t i = 0; i < *n; i++)
            if (out[i] != id)
                out[kept++] = out[i];
        *fits = w != 0;
        *n    = kept;
    }
    if (!*fits)
        return RibbonMismatch(vol, rp, st, RIBBON_LABELED, d->fid, "it puts on a tag already worn or takes off one "
                              "not worn");
    RibbonTagsUnknown(vol, st, d->add, d->add_bytes, d->nadd, unknown, sizeof(unknown));
    *fits = !unknown[0];
    return *fits ? OK : RibbonMismatch(vol, rp, st, RIBBON_LABELED, d->fid, "it wears tag id(s) %s that no Coined or "
                                       "TAGS names or that are RETIRED", unknown);
}

/* The Labeled applied to `now`: its name, flags, tags, term, the programs it
 * shows its tags to and its first bytes. */
static error_t RibbonLabelRecord(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonLabeled* d, const FileRecord* now)
{
    uint64_t  flags  = RibbonFlagsOf(rp, st, now);
    uint64_t  keep   = now->keep_until;
    uint32_t  n      = 0;
    bool      fits   = true;
    char*     name   = NULL;
    uint32_t* tags   = NULL;
    uint64_t* shown  = NULL;
    uint32_t  nshown = (d->what & LABEL_SHOWN) ? (uint32_t)d->nshown : now->shown_count;
    if (d->what & LABEL_FLAGS) {
        flags = (flags | d->set) & ~d->clear;
        if (!(flags & TALLY_FILE_ACTIVE)) {
            RibbonRefuse(st, "its Labeled takes ACTIVE off fid %llu — a file leaves by Quitclaim (§2.8)",
                         (unsigned long long)d->fid);
            return OK;
        }
    }
    if (d->what & LABEL_KEEP) {
        keep  = d->keep_ns;
        flags = keep ? flags | TALLY_FILE_DELETED : flags & ~(uint64_t)TALLY_FILE_DELETED;
    }
    if (d->what & LABEL_SHOWN)
        flags = nshown ? flags | TALLY_FILE_SHOWN : flags & ~(uint64_t)TALLY_FILE_SHOWN;
    tags  = kmalloc(sizeof(uint32_t) * (now->tag_count + d->nadd + 1u));
    name  = kmalloc((d->what & LABEL_NAME ? d->name_len : now->name_len) + 1u);
    shown = kmalloc(sizeof(uint64_t) * (nshown + 1u));
    error_t rc = tags && name && shown ? OK : ERR_NO_MEMORY;
    if (rc == OK && (d->what & LABEL_SHOWN))
        TallyFidsOut(d->shown, d->shown_bytes, d->nshown, shown);
    else if (rc == OK && nshown)
        memcpy(shown, now->shown_to, sizeof(uint64_t) * nshown);
    if (rc == OK && (d->what & LABEL_TAGS))
        rc = RibbonLabelTags(vol, rp, st, d, now, tags, &n, &fits);
    else if (rc == OK)
        for (uint32_t i = 0; i < now->tag_count; i++)
            tags[n++] = now->tags[i];
    if (rc == OK && fits) {
        if (d->what & LABEL_NAME)
            memcpy(name, d->name, d->name_len);
        else
            memcpy(name, now->name, now->name_len);
        name[d->what & LABEL_NAME ? d->name_len : now->name_len] = '\0';
        FileRecord* rec = FileRecordRelabel(now, name, tags, n, shown, nshown, (uint32_t)flags, keep);
        if (rec && (d->what & LABEL_HEAD))
            FileRecordHeadSet(rec, d->head, (uint32_t)d->head_len);
        if (rec && (d->what & LABEL_PASSPORT))
            FileRecordPassportSet(rec, d->passport);
        if (!rec || RibbonHighSet(st, d->fid, (uint32_t)(flags >> 32)) != OK) {
            FileRecordRelease(rec);
            rc = ERR_NO_MEMORY;
        } else {
            rec->gen = st->u->seq;
            rc       = RibbonStagePut(st, d->fid, rec);
        }
    }
    kfree(tags);
    kfree(name);
    kfree(shown);
    return rc;
}

/* Labeled (§2.8, §3.7): the file at its base; the tags it puts on known and
 * not worn, those it takes off worn; ACTIVE kept. Its mtime does not move. */
static error_t RibbonLabeledApplied(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonRecord* r)
{
    RibbonLabeled d;
    FileRecord*   now  = NULL;
    uint32_t      used = 0;
    if (!RibbonBodyTaken(st, RIBBON_LABELED, RibbonLabeledGet(st->u->stream + r->body, r->len, &d, &used), r->len,
                         used))
        return OK;
    if (RibbonSkipHas(&rp->undone, st->u->seq, d.fid) || RibbonLocked(vol, st, d.fid))
        return OK;
    error_t rc = RibbonBased(vol, rp, st, RIBBON_LABELED, d.fid, d.base, &now);
    if (rc != OK || !now)
        return rc;
    rc = RibbonLabelRecord(vol, rp, st, &d, now);
    FileRecordRelease(now);
    return rc;
}

/* Quitclaim (§2.8, §3.7): the file at its base, gone; every block of its
 * ground let go to the Fallow of the mount (M-1) — a block SHARED there too,
 * which the census keeps for any owner left naming it (ground_census.c,
 * RunsOwnerLess), so it lies fallow once, when the last has gone (§8.2). Its
 * id is never given again. */
static error_t RibbonQuitclaimApplied(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonRecord* r)
{
    uint64_t    fid = 0, base = 0;
    uint32_t    used = 0;
    FileRecord* now  = NULL;
    StripWalk   walk;
    Strip       run;
    if (!RibbonBodyTaken(st, RIBBON_QUITCLAIM, RibbonQuitclaimGet(st->u->stream + r->body, r->len, &fid, &base,
                                                                   &used), r->len, used))
        return OK;
    if (RibbonSkipHas(&rp->undone, st->u->seq, fid) || RibbonLocked(vol, st, fid))
        return OK;
    error_t rc = RibbonBased(vol, rp, st, RIBBON_QUITCLAIM, fid, base, &now);
    if (rc != OK || !now)
        return rc;
    StripWalkBegin(now, &walk);
    while (rc == OK && StripWalkNextGround(&walk, &run))
        rc = RibbonStageFallow(vol, st, run.start, run.count, false);
    FileRecordRelease(now);
    return rc == OK ? RibbonStagePut(st, fid, NULL) : rc;
}

// ----------------------------------------------------------------------------
// Coined, UseContext, the bookmarks, and a record of each kind
// ----------------------------------------------------------------------------

/* Whether `a` and `b` are one tag: one owner, one text. */
static bool RibbonTagSame(const TallyTag* a, const TallyTag* b)
{
    bool a_value = (a->flags & TALLY_TAG_HAS_VALUE) != 0, b_value = (b->flags & TALLY_TAG_HAS_VALUE) != 0;
    return TagOwnerSame(TagRegistryOwnerOf(a), TagRegistryOwnerOf(b)) && a->key_len == b->key_len &&
           !memcmp(a->key, b->key, a->key_len) && a_value == b_value &&
           (!a_value || (a->value_len == b->value_len && !memcmp(a->value, b->value, a->value_len)));
}

/* The coin of `id` the stitch holds, or NULL. */
static RibbonCoin* RibbonCoinStaged(RibbonStage* st, uint64_t id)
{
    for (uint32_t i = 0; i < st->ncoins; i++)
        if (st->coins[i].id == id)
            return &st->coins[i];
    return NULL;
}

/* A Coined of a tag the stitch, the registry and the book's tags set aside do
 * not know: held for the registry, or — a Coined RETIRED taking the id the
 * stitch coined before — in place of what was coined. */
static error_t RibbonCoinStage(RibbonStage* st, uint64_t id, const TallyTag* t, bool held)
{
    RibbonCoin* staged = RibbonCoinStaged(st, id);
    if (staged) {
        staged->tag = *t;
        return OK;
    }
    uint64_t cap = st->capcoins;
    if (!RibbonGrow((void**)&st->coins, st->ncoins, &cap, sizeof(RibbonCoin)))
        return ERR_NO_MEMORY;
    st->capcoins = (uint32_t)cap;
    st->coins[st->ncoins++] = (RibbonCoin){ id, *t, held };
    return OK;
}

/* Coined (§2.8, §3.7): the id not known, or known as the same tag — owner and
 * text — by the stitch, the registry, or the book's tags set aside. What of it this kernel
 * cannot carry on is said whatever the stitch comes to; a tag the registry
 * cannot hold is coined all the same, aside (BookTagAsideNoted), and known to
 * the records after it. A Coined RETIRED retires the id, whatever it was —
 * and an id retired is given no more: a Coined naming it a tag refuses the
 * stitch. */
static error_t RibbonCoinedApplied(TagFSVolume* vol, RibbonStage* st, const RibbonRecord* r)
{
    TallyTag        t, registered;
    const TallyTag* have = NULL;
    TagSpelling     sp;
    TagOwner        owner;
    uint64_t        id = 0;
    uint32_t        used = 0;
    if (!RibbonBodyTaken(st, RIBBON_COINED, RibbonCoinedGet(st->u->stream + r->body, r->len, &id, &t, &used),
                         r->len, used))
        return OK;
    bool         held   = BookTagHeld(vol, id, &t);
    RibbonCoin*  staged = RibbonCoinStaged(st, id);
    if (t.flags & TALLY_TAG_RETIRED)
        return RibbonCoinStage(st, id, &t, held);
    bool retired = staged ? (staged->tag.flags & TALLY_TAG_RETIRED) != 0
                          : id <= TAGFS_MAX_TAG_ID && TagRegistryRetiredId(vol->state.registry, (uint32_t)id);
    if (retired) {
        RibbonRefuse(st, "its Coined gives id %llu to '%.*s' of %s, which the volume has RETIRED — an id is given "
                     "no more", (unsigned long long)id, (int)t.key_len, (const char*)t.key,
                     (t.flags & TALLY_TAG_OWNED) ? "a program" : "the person");
        return OK;
    }
    if (staged)
        have = &staged->tag;
    bool known = !have && id <= TAGFS_MAX_TAG_ID &&
                 TagRegistrySpelling(vol->state.registry, (uint32_t)id, &sp, NULL, &owner);
    if (known) {
        registered = TagRegistryBookForm(&sp, owner);
        have       = &registered;
    }
    if (!have)
        have = BookTagAsideOf(vol, id);
    if (have && !RibbonTagSame(have, &t)) {
        RibbonRefuse(st, "its Coined gives id %llu to '%.*s' of %s, which the volume knows as another tag",
                     (unsigned long long)id, (int)t.key_len, (const char*)t.key,
                     (t.flags & TALLY_TAG_OWNED) ? "a program" : "the person");
        return OK;
    }
    return known || staged ? OK : RibbonCoinStage(st, id, &t, held);
}

/* Whether a record is of a kind this reader does not know as a stitch's
 * record: kind 0 is none — a tag of 0 is PAD, and a tag of 1 a kind no writer
 * was given (§2.3) —, a kind past the last it knows is a newer writer's, and
 * a StitchHead or a Clasp is the stitch's frame, never a record of it
 * (bookmount.py _Record: a kind not in KIND_NAMES, or STITCH_HEAD, CLASP). */
static bool RibbonKindUnknown(uint64_t kind)
{
    return kind < RIBBON_UNDONE || kind > RIBBON_KINDS_KNOWN;
}

/* A record of the stitch through the rule of its kind (bookmount.py _Record):
 * a kind this reader does not know stepped over when it may be, and the
 * stitch refused when it may not. */
error_t RibbonRecordApplied(TagFSVolume* vol, RibbonReplay* rp, RibbonStage* st, const RibbonRecord* r)
{
    uint64_t       id = 0, n = 0;
    uint32_t       used = 0;
    const uint8_t* text = NULL;
    char           why[128];
    if (RibbonKindUnknown(r->kind)) {
        if (!r->ignorable)
            RibbonRefuse(st, "it carries a record of kind %llu this reader does not know and may not step over "
                         "— written by a kernel that knows more", (unsigned long long)r->kind);
        return OK;
    }
    const uint8_t* b = st->u->stream + r->body;
    switch (r->kind) {
    case RIBBON_BORN:      return RibbonBornApplied(vol, rp, st, r);
    case RIBBON_SHAPED:    return RibbonShapedApplied(vol, rp, st, r);
    case RIBBON_LABELED:   return RibbonLabeledApplied(vol, rp, st, r);
    case RIBBON_QUITCLAIM: return RibbonQuitclaimApplied(vol, rp, st, r);
    case RIBBON_COINED:    return RibbonCoinedApplied(vol, st, r);
    case RIBBON_UNDONE: {
        RibbonUndone d;
        RibbonBodyTaken(st, RIBBON_UNDONE, RibbonUndoneGet(b, r->len, &d, &used), r->len, used);
        return OK;
    }
    case RIBBON_USE_CONTEXT: {
        int got = TallyUseGet(b, r->len, &text, &n);
        if (RibbonBodyTaken(st, r->kind, got, r->len, got == TALLY_BODY_BROKEN ? 0 : TallyVarintBytes(n) + (uint32_t)n)) {
            st->use_set = true;
            st->use     = text;
            st->use_len = n;
        }
        return OK;
    }
    case RIBBON_VOLUME_NAME: {
        int got = TallyUseGet(b, r->len, &text, &n);
        if (RibbonBodyTaken(st, r->kind, got, r->len, got == TALLY_BODY_BROKEN ? 0 : TallyVarintBytes(n) + (uint32_t)n)) {
            st->name_set = true;
            st->name     = text;
            st->name_len = n;
        }
        return OK;
    }
    default: {
        int got = r->kind == RIBBON_FROZEN ? RibbonFrozenGet(b, r->len, &id, &used)
                                           : RibbonThawedGet(b, r->len, &id, &used);
        if (!RibbonBodyTaken(st, r->kind, got, r->len, used))
            return OK;
        ksnprintf(why, sizeof(why), "stitch %llu carries a %s — a bookmark this kernel cannot hold (§5)",
                  (unsigned long long)st->u->seq, g_kind_names[r->kind]);
        TagFSUnderGlass(vol, why);
        return OK;
    }
    }
}

/* The fresh blocks a record's runs name (Clasp.fresh, §2.7), added to *sum:
 * false when it passes 2^64 - 1. */
static bool RibbonFreshAdded(const uint8_t* runs, uint32_t bytes, uint64_t n, uint64_t cursor, uint64_t seq,
                             uint64_t* sum)
{
    uint32_t at = 0;
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        RibbonRunGet(runs, bytes, &at, &cursor, seq, &r);
        uint64_t fresh = RibbonRunFresh(&r);
        if (fresh > UINT64_MAX - *sum)
            return false;
        *sum += fresh;
    }
    return true;
}

bool RibbonFreshNamed(const RibbonUnit* u, uint64_t* named)
{
    bool fits = true;
    *named = 0;
    for (uint32_t i = 0; fits && i < u->nrecords; i++) {
        const RibbonRecord* r = &u->records[i];
        const uint8_t*      b = u->stream + r->body;
        RibbonBorn          born;
        RibbonShaped        d;
        RibbonSplice        s;
        uint32_t            used = 0, at = 0;
        uint64_t            cursor = 0, digests = 0;
        if (r->kind == RIBBON_BORN && RibbonBornGet(b, r->len, u->seq, &born, &used) != TALLY_BODY_BROKEN) {
            fits = RibbonFreshAdded(born.runs, born.runs_bytes, born.nruns, 0, u->seq, named);
        } else if (r->kind == RIBBON_SHAPED && RibbonShapedGet(b, r->len, u->seq, &d, &used) != TALLY_BODY_BROKEN) {
            for (uint64_t k = 0; fits && k < d.nsplice; k++) {
                RibbonSpliceNext(d.splices, d.splices_bytes, &at, &cursor, u->seq, &digests, &s);
                fits = RibbonFreshAdded(s.runs, s.runs_bytes, s.ninsert, s.cursor, u->seq, named);
            }
        }
    }
    return fits;
}

/* Clasp.fresh held to the fresh blocks the stitch's Borns and Shapeds name
 * (§2.7): the bound a mount sees before it reads a block of data, so a Clasp
 * that says other than its records is a stitch that may not be applied — nor
 * is its data read (RibbonVerify). */
void RibbonFreshHeld(RibbonStage* st)
{
    const RibbonUnit* u     = st->u;
    uint64_t          named = 0;
    if (!u->sound)
        return;
    if (!RibbonFreshNamed(u, &named))
        RibbonRefuse(st, "its Clasp names %llu fresh block(s) and its records more than 2^64 - 1 (§2.7)",
                     (unsigned long long)u->clasp.fresh);
    else if (named != u->clasp.fresh)
        RibbonRefuse(st, "its Clasp names %llu fresh block(s) and its records %llu (§2.7)",
                     (unsigned long long)u->clasp.fresh, (unsigned long long)named);
}

bool RibbonRecordWithin(const RibbonUnit* u, const RibbonRecord* r, uint64_t tree, uint64_t lo, uint64_t hi)
{
    const uint8_t* b    = u->stream + r->body;
    uint32_t       used = 0;
    uint64_t       key  = 0, base = 0;
    RibbonBorn     born;
    RibbonShaped   d;
    RibbonLabeled  l;
    TallyTag       t;
    int            got  = TALLY_BODY_BROKEN;
    if (RibbonKindUnknown(r->kind))
        return !r->ignorable;                   /* its keys unknown: applied, it refuses the stitch */
    if (tree == TALLY_TAGS && r->kind == RIBBON_COINED)
        got = RibbonCoinedGet(b, r->len, &key, &t, &used);
    else if (tree != TALLY_FILES)
        return false;
    else if (r->kind == RIBBON_BORN && (got = RibbonBornGet(b, r->len, u->seq, &born, &used)) != TALLY_BODY_BROKEN)
        key = born.fid;
    else if (r->kind == RIBBON_SHAPED && (got = RibbonShapedGet(b, r->len, u->seq, &d, &used)) != TALLY_BODY_BROKEN)
        key = d.fid;
    else if (r->kind == RIBBON_LABELED && (got = RibbonLabeledGet(b, r->len, &l, &used)) != TALLY_BODY_BROKEN)
        key = l.fid;
    else if (r->kind == RIBBON_QUITCLAIM)
        got = RibbonQuitclaimGet(b, r->len, &key, &base, &used);
    else if (r->kind != RIBBON_BORN && r->kind != RIBBON_SHAPED && r->kind != RIBBON_LABELED)
        return false;                           /* a kind that names no file */
    return got == TALLY_BODY_BROKEN || (lo <= key && key < hi);    /* a key that does not read may be one */
}
