#include "book_mount.h"
#include "../settle/binder.h"
#include "../settle/foliation.h"
#include "../settle/dog_ear.h"
#include "under_glass.h"
#include "ground_census.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../tag_spelling/tag_spelling.h"
#include "../use_memory/use_memory.h"
#include "facsimile.h"

/*
 * The leaves of the Tally the mount stands on, into the memory a mount fills
 * (book_mount.h): FILES into the map of live files, TAGS into the
 * registry, USE into the Use Context. BOOKMARKS and LIEN carry what this
 * kernel cannot hold — a tree of them that is not empty puts the volume
 * under glass (book_mount.c) — and their leaves are read for nothing but
 * their shape.
 *
 * Read the way volume-read reads them (tools/volumeread/tally.py, Tally._Files
 * and _Rest), so the two readers lock the same files and give the same
 * reasons the volume may only be read: a body longer than this reader knows,
 * a tag RETIRED, past the last id this kernel gives or wider than it holds a
 * tag, a run carrying an extension but SHARED alone (RibbonRunCarried).
 */

/* What a body this reader read to its known end leaves over, said: written by
 * a kernel that knows more (§2.3, M-8). */
static void LeavesLonger(const char* what, uint32_t left)
{
    char words[192];
    if (left == 0)
        return;
    ksnprintf(words, sizeof(words), "%s is %u byte(s) longer than this reader knows — written by a "
              "kernel that knows more", what, left);
    TagFSUnderGlass(words);
}

// ----------------------------------------------------------------------------
// FILES: a header, then the Parcels that cover its blocks in a row
// ----------------------------------------------------------------------------

/* The Parcels kept for the gathered file, let go. */
static void BookParcelsForget(BookFile* f)
{
    for (uint32_t i = 0; i < f->nparcels; i++)
        kfree(f->parcel_body[i]);
    kfree(f->parcel_b);
    kfree(f->parcel_body);
    kfree(f->parcel_len);
    kfree(f->parcel_seq);
    f->parcel_b    = NULL;
    f->parcel_body = NULL;
    f->parcel_len  = NULL;
    f->parcel_seq  = NULL;
    f->nparcels = f->capparcels = 0;
}

void BookFileForget(BookFile* f)
{
    kfree(f->meta.filename);
    kfree(f->meta.tag_ids);
    kfree(f->strips);
    kfree(f->cut);
    BookParcelsForget(f);
    memset(f, 0, sizeof(*f));
}

/* Why the file's items do not read, kept: the first reason is the one said. */
static void BookFileBroken(BookFile* f, const char* why)
{
    if (!f->why[0])
        ksnprintf(f->why, sizeof(f->why), "%s", why);
}

/* The header's bytes as this reader knows them: what is past them is a newer
 * writer's (TallyFileHeadGet answered LONGER). */
static uint32_t BookHeadKnown(const TallyFileHead* h)
{
    return TallyVarintBytes(h->gen) + TallyVarintBytes(h->flags) + TallyVarintBytes(h->size) +
           TallyVarintBytes(h->created_ns) +
           TallyVarintBytes(BookZigzag((int64_t)(h->modified_ns - h->created_ns))) +
           TallyVarintBytes(h->name_len) + (uint32_t)h->name_len + TallyVarintBytes(h->ntags) +
           h->tags_bytes + ((h->flags & TALLY_FILE_DELETED) ? TallyVarintBytes(h->keep_ns) : 0u) +
           ((h->flags & TALLY_FILE_MADE_BY) ? TallyVarintBytes(h->made_by) : 0u);
}

/* The header of `f->fid` into the gathering: its name and its tags copied out
 * of the node, which is let go of after its leaf. */
static error_t BookFileHead(BookFile* f, const uint8_t* body, uint32_t len)
{
    TallyFileHead h;
    char          what[64];
    int           got = TallyFileHeadGet(body, len, &h);
    if (got == TALLY_BODY_BROKEN) {
        BookFileBroken(f, "its header does not read, or is not ACTIVE");
        return OK;
    }
    ksnprintf(what, sizeof(what), "the FILES header of fid %llu", (unsigned long long)f->fid);
    if (got == TALLY_BODY_LONGER)
        LeavesLonger(what, len - BookHeadKnown(&h));

    f->meta.filename = kmalloc(h.name_len + 1u);
    f->meta.tag_ids  = kmalloc(sizeof(uint32_t) * (h.ntags ? h.ntags : 1u));
    if (!f->meta.filename || !f->meta.tag_ids)
        return ERR_NO_MEMORY;
    memcpy(f->meta.filename, h.name, h.name_len);
    f->meta.filename[h.name_len] = '\0';

    uint32_t at = 0;
    uint64_t id = 0;
    for (uint64_t k = 0; k < h.ntags; k++) {
        TallyIdNext(h.tags, h.tags_bytes, &at, &id, k == 0);
        if (id > TAGFS_MAX_TAG_ID) {
            char words[160];
            ksnprintf(words, sizeof(words), "fid %llu wears tag id %llu, past the last this kernel gives",
                      (unsigned long long)f->fid, (unsigned long long)id);
            TagFSUnderGlass(words);
            continue;
        }
        f->meta.tag_ids[f->meta.tag_count++] = (uint32_t)id;
    }
    f->meta.file_id       = f->fid;
    f->gen                = h.gen;
    f->meta.flags         = (uint32_t)h.flags;
    f->wide               = h.flags > UINT32_MAX;
    f->meta.size          = h.size;
    f->meta.created_time  = h.created_ns;
    f->meta.modified_time = h.modified_ns;
    f->meta.keep_until    = h.keep_ns;
    f->meta.made_by       = h.made_by;
    f->head = true;
    return OK;
}

/* One Strip more for the gathering: false with no memory. */
static bool BookFileStrip(BookFile* f, const Strip* s)
{
    if (f->nstrips == f->cap) {
        uint32_t cap  = f->cap ? f->cap * 2u : 16u;
        Strip*   more = kmalloc(sizeof(Strip) * cap);
        if (!more)
            return false;
        if (f->nstrips)
            memcpy(more, f->strips, sizeof(Strip) * f->nstrips);
        kfree(f->strips);
        f->strips = more;
        f->cap    = cap;
    }
    f->strips[f->nstrips++] = *s;
    return true;
}

/* The Strips a Parcel laid, `n` of them, as one Parcel of memory: false with
 * no memory. */
static bool BookFileCut(BookFile* f, uint32_t n)
{
    if (f->ncut == f->capcut) {
        uint32_t  cap  = f->capcut ? f->capcut * 2u : 8u;
        uint32_t* more = kmalloc(sizeof(uint32_t) * cap);
        if (!more)
            return false;
        if (f->ncut)
            memcpy(more, f->cut, sizeof(uint32_t) * f->ncut);
        kfree(f->cut);
        f->cut    = more;
        f->capcut = cap;
    }
    f->cut[f->ncut++] = n;
    return true;
}

/* A run of the book into Strips: a run longer than a Strip counts is laid in
 * as many as it takes. What of a DATA run lies outside the data run is not
 * the file's to name — the file is locked for it, and only what lies inside
 * is laid, the part a lock holds (BookGroundHoldRun); the run as it is named
 * goes to the census's reach, so ground past the far Deed a locked file names
 * is never taken as no one's (mount/book_grow.c); a HOLE is laid only
 * while the file's Parcels have covered no more than its bytes need — of a
 * file locked, a hole names no ground and is not kept. So what is laid is
 * bounded by the volume and by the file, never by a number a run carries. */
static bool BookFileRun(BookFile* f, const TallyRun* r)
{
    uint64_t start = r->start, end = r->start + r->count;
    if (r->hole && f->why[0])
        return true;
    if (!r->hole) {
        uint64_t lo = g_book.frame.data_block, hi = g_book.data_end;
        if (start < lo || end > hi) {
            BookFileBroken(f, "it names ground outside the data run");
            GroundCensusNamed(r->start, r->count);
        }
        start = start < lo ? lo : start;
        end   = end > hi ? hi : end;
        if (start >= end)
            return true;
    }
    uint32_t kind = r->hole ? STRIP_HOLE : RibbonRunCarried(r) == RUN_CARRIED_SHARED ? STRIP_SHARED : STRIP_DATA;
    for (uint64_t left = end - start; left;) {
        uint32_t n = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        Strip    s = { r->hole ? 0 : start, n, kind, r->birth };
        if (!BookFileStrip(f, &s))
            return false;
        start += r->hole ? 0 : n;
        left  -= n;
    }
    return true;
}

/* Room for one Parcel more in the gathering's lists: false with no memory. */
static bool BookParcelRoom(BookFile* f)
{
    if (f->nparcels < f->capparcels)
        return true;
    uint32_t  cap  = f->capparcels ? f->capparcels * 2u : 8u;
    uint64_t* pb   = kmalloc(sizeof(uint64_t) * cap);
    uint8_t** body = kmalloc(sizeof(uint8_t*) * cap);
    uint32_t* len  = kmalloc(sizeof(uint32_t) * cap);
    uint64_t* seq  = kmalloc(sizeof(uint64_t) * cap);
    if (!pb || !body || !len || !seq) {
        kfree(pb);
        kfree(body);
        kfree(len);
        kfree(seq);
        return false;
    }
    if (f->nparcels) {
        memcpy(pb, f->parcel_b, sizeof(uint64_t) * f->nparcels);
        memcpy(body, f->parcel_body, sizeof(uint8_t*) * f->nparcels);
        memcpy(len, f->parcel_len, sizeof(uint32_t) * f->nparcels);
        memcpy(seq, f->parcel_seq, sizeof(uint64_t) * f->nparcels);
    }
    kfree(f->parcel_b);
    kfree(f->parcel_body);
    kfree(f->parcel_len);
    kfree(f->parcel_seq);
    f->parcel_b    = pb;
    f->parcel_body = body;
    f->parcel_len  = len;
    f->parcel_seq  = seq;
    f->capparcels  = cap;
    return true;
}

/* A Parcel that read, kept as it lay — its file block, its body, the seq of
 * its node — for a file the mount may lock by name (BookKeptFile): let go at
 * the file's close when it stands. False with no memory. */
static bool BookParcelMarked(BookFile* f, uint64_t b, const uint8_t* body, uint32_t len, uint64_t seq)
{
    uint8_t* copy = kmalloc(len ? len : 1u);
    if (!copy || !BookParcelRoom(f)) {
        kfree(copy);
        return false;
    }
    if (len)
        memcpy(copy, body, len);
    f->parcel_b[f->nparcels]    = b;
    f->parcel_body[f->nparcels] = copy;
    f->parcel_len[f->nparcels]  = len;
    f->parcel_seq[f->nparcels]  = seq;
    f->nparcels++;
    return true;
}

/* The blocks a file's bytes take, its last one part filled. */
static uint64_t BookFileNeed(const BookFile* f)
{
    return f->meta.size / TAGFS_BLOCK_SIZE + (f->meta.size % TAGFS_BLOCK_SIZE != 0);
}

/* A Parcel's runs decoded with nothing kept: whether every one reads as the
 * codec reads it (TallyRunGet) — volume-read's RunsGet refuses the same, the
 * whole Parcel with it — and the blocks they cover, in *blocks, held at
 * 2^64 - 1 rather than wrapped. */
static bool BookParcelReads(const uint8_t* body, uint32_t len, uint64_t seq, uint64_t* blocks)
{
    uint32_t at = 0;
    uint64_t n = 0, cursor = 0;
    *blocks = 0;
    if (!TallyTake(body, len, &at, &n))
        return false;
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        if (!TallyRunGet(body, len, &at, &cursor, seq, &r))
            return false;
        *blocks = r.count > UINT64_MAX - *blocks ? UINT64_MAX : *blocks + r.count;
    }
    return true;
}

/* The Parcel that covers the file's blocks from `first` — the next in a row,
 * or the gathering is a file whose Parcels leave a hole or overlap (§2.9).
 * Read whole before a run of it is laid: a Parcel any run of which does not
 * read is kept not at all, and the file locked for it. One that reads is
 * kept whatever else is wrong with the file: a file locked for its Parcels
 * still names the ground they name, which is held (tally.py _Files, `known`).
 * A Parcel that takes the file past the blocks its bytes need is said so the
 * moment it does, before its runs are laid (BookFileRun). */
static error_t BookFileParcel(BookFile* f, uint64_t first, const uint8_t* body, uint32_t len, uint64_t seq)
{
    uint32_t at = 0;
    uint64_t n = 0, cursor = 0, blocks = 0;
    char     words[160];

    if (!f->head) {
        BookFileBroken(f, "it has Parcels and no header");
    } else if (first != f->covered) {
        ksnprintf(words, sizeof(words), "its Parcel from block %llu follows its blocks up to %llu — %s",
                  (unsigned long long)first, (unsigned long long)f->covered,
                  first > f->covered ? "a hole" : "an overlap");
        BookFileBroken(f, words);
    }
    if (!BookParcelReads(body, len, seq, &blocks)) {
        BookFileBroken(f, "a Parcel of it does not read");
        return OK;
    }
    uint64_t need    = BookFileNeed(f);
    uint64_t covered = blocks > UINT64_MAX - f->covered ? UINT64_MAX : f->covered + blocks;
    if (f->head && covered > need) {
        ksnprintf(words, sizeof(words), "its Parcels cover %llu blocks and its %llu bytes need %llu",
                  (unsigned long long)covered, (unsigned long long)f->meta.size, (unsigned long long)need);
        BookFileBroken(f, words);
    }
    TallyTake(body, len, &at, &n);
    g_book.laid.runs += n;
    FoliationLeafHolds(TALLY_FILES, n, 0);
    uint32_t from = f->nstrips;
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        TallyRunGet(body, len, &at, &cursor, seq, &r);
        f->ext |= RibbonRunCarried(&r) == RUN_CARRIED_NOT;
        if (!BookFileRun(f, &r))
            return ERR_NO_MEMORY;
    }
    if (f->nstrips > from && !BookFileCut(f, f->nstrips - from))
        return ERR_NO_MEMORY;
    if (!f->parcels_lost && !BookParcelMarked(f, first, body, len, seq)) {
        BookParcelsForget(f);
        f->parcels_lost = true;     /* a matter only for a file the mount locks (BookFileKept) */
    }
    f->covered = covered;
    ksnprintf(words, sizeof(words), "a Parcel of fid %llu", (unsigned long long)f->fid);
    LeavesLonger(words, len - at);
    return OK;
}

/* Whether the last rotten range said reaches into `fid`'s keys from before
 * them — it ends inside them. */
static bool BookRotReaches(uint64_t fid)
{
    if (g_book.nrot == 0)
        return false;
    const BookRotten* r = &g_book.rot[g_book.nrot - 1u];
    return !r->has_hi || (r->hi.k1 == fid && r->hi.k2 > 0);
}

/* Why a gathered file does not stand, or NULL: a rotten range, items that do
 * not read, ground outside the data run (each said as it was met), Parcels
 * that do not cover its bytes. */
static const char* BookFileJudged(BookFile* f, char* words, size_t room)
{
    if (f->rotten)
        return "it lies in a rotten range of FILES";
    if (f->why[0])
        return f->why;
    uint64_t need = BookFileNeed(f);
    if (f->covered != need) {
        ksnprintf(words, room, "its Parcels cover %llu blocks and its %llu bytes need %llu",
                  (unsigned long long)f->covered, (unsigned long long)f->meta.size,
                  (unsigned long long)need);
        return words;
    }
    return NULL;
}

/* `fid` kept by name among the locked, in their order, and said: no open of
 * it is given, its id is not given again. ERR_NO_MEMORY. */
static error_t BookLockedByName(uint64_t fid, const char* why)
{
    if (g_book.nlocked == g_book.caplocked) {
        uint32_t  cap  = g_book.caplocked ? g_book.caplocked * 2u : 16u;
        uint64_t* more = kmalloc(sizeof(uint64_t) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (g_book.nlocked)
            memcpy(more, g_book.locked, sizeof(uint64_t) * g_book.nlocked);
        kfree(g_book.locked);
        g_book.locked    = more;
        g_book.caplocked = cap;
    }
    uint32_t at = g_book.nlocked++;
    while (at > 0 && g_book.locked[at - 1u] > fid) {
        g_book.locked[at] = g_book.locked[at - 1u];
        at--;
    }
    g_book.locked[at] = fid;
    g_book.said.locked++;
    kprintf("[TagFS] fid %llu is locked: %s — no open of it is given, its id is not given again, and "
            "the blocks it names are held\n", (unsigned long long)fid, why);
    return OK;
}

/* What of a gathered file read, kept for the next Tally (BookKeptFile): the
 * gathering's own header and Parcels moved into the list, nothing copied.
 * Where memory would not keep them — its Parcels as they lay, or a place in the
 * list — the mount goes on, and no Tally is written by it: a Tally names a
 * file locked by name as it lay or not at all. Said. */
static void BookFileKept(BookFile* f)
{
    static const char lost[] = "memory would not keep what of a file locked by name read, which a Tally carries "
                               "as it lay";
    BookKeptFile*     more   = NULL;
    uint32_t          cap    = g_book.capkept ? g_book.capkept * 2u : 8u;
    if (!f->parcels_lost && g_book.nkept == g_book.capkept && !(more = kmalloc(sizeof(BookKeptFile) * cap)))
        f->parcels_lost = true;
    if (f->parcels_lost) {
        if (!BinderNoTallies())
            kprintf("[TagFS] fid %llu: %s — no Tally is written by this mount\n", (unsigned long long)f->fid, lost);
        BinderNoTalliesFrom(lost);
        return;
    }
    if (more) {
        if (g_book.nkept)
            memcpy(more, g_book.kept, sizeof(BookKeptFile) * g_book.nkept);
        kfree(g_book.kept);
        g_book.kept    = more;
        g_book.capkept = cap;
    }
    g_book.kept[g_book.nkept++] = (BookKeptFile){ .fid = f->fid, .head = f->head, .meta = f->meta, .gen = f->gen,
                                                  .parcel_b = f->parcel_b, .parcel_body = f->parcel_body,
                                                  .parcel_len = f->parcel_len, .parcel_seq = f->parcel_seq,
                                                  .nparcels = f->nparcels };
    f->meta.filename = NULL;
    f->meta.tag_ids  = NULL;
    f->parcel_b      = NULL;
    f->parcel_body   = NULL;
    f->parcel_len    = NULL;
    f->parcel_seq    = NULL;
    f->nparcels = f->capparcels = 0;
}

/* A file the book holds and cannot read: said, kept by name for every open of
 * it to be refused, and what of its ground it is known to name held. */
static error_t BookFileLock(BookFile* f, const char* why)
{
    error_t rc = BookLockedByName(f->fid, why);
    for (uint32_t i = 0; rc == OK && i < f->nstrips; i++)
        if (StripOnGround(f->strips[i].kind))
            BookGroundHoldRun(f->strips[i].start, f->strips[i].count);
    if (rc == OK)
        BookFileKept(f);
    return rc;
}

error_t BookFileLockLive(uint64_t fid, const char* why)
{
    TagFSState* fs  = tagfs_get_state();
    RecordWhere was;
    FileRecord* rec = FileMapRecordHold(fs->files, fid, NULL);
    StripWalk   walk;
    Strip       run;
    if (!rec)
        return OK;
    char    glass[192];
    error_t rc = BookLockedByName(fid, why);
    ksnprintf(glass, sizeof(glass), "fid %llu is locked by the replay of the ribbon — no Tally names a file it "
              "cannot name as it lay, and a book no Tally is written of cannot go on", (unsigned long long)fid);
    TagFSUnderGlass(glass);
    StripWalkBegin(rec, &walk);
    while (rc == OK && StripWalkNextGround(&walk, &run))
        BookGroundHoldRun(run.start, run.count);
    for (uint32_t t = 0; rc == OK && t < rec->tag_count; t++)
        TagBitmapLeave(fs->bitmap_index, rec->tags[t], fid);
    if (rec->flags & TAGFS_FILE_DELETED)
        TagBitmapLeave(fs->bitmap_index, TAGFS_DELETED_MEMBERS, fid);
    FileRecordRelease(rec);
    if (rc == OK) {
        FileMapDrop(fs->files, fid, &was);
        g_book.files--;
    }
    return rc;
}

/* A file that stands, into the map of live files, its tags into the index,
 * its ground into the census. */
static error_t BookFileLive(BookFile* f)
{
    TagFSState* fs = tagfs_get_state();
    bool recut = false;
    f->meta.extents      = f->strips;
    f->meta.extent_count = f->nstrips;
    FileRecord* rec = FileRecordBuildCut(&f->meta, f->cut, f->ncut, &recut);
    if (!rec)
        return ERR_NO_MEMORY;
    if (recut)   /* memory holds it otherwise than the Tally: the next Tally lays its Parcels anew */
        DogEarPut(TALLY_FILES, (TallyKey){ f->fid, 1 }, (TallyKey){ f->fid + 1u, 0 });
    rec->gen = f->gen;
    RecordWhere  where, was;
    FileMapStake stake = { .count = 0 };
    memset(&where, 0, sizeof(where));
    error_t rc = FileMapRecordPut(fs->files, f->fid, rec, &where, &stake, &was);
    FileMapStakeLetGo(&stake);
    if (rc != OK)
        return rc;
    for (uint32_t t = 0; t < f->meta.tag_count; t++)
        if (tag_bitmap_set(fs->bitmap_index, f->meta.tag_ids[t], f->fid) != 0)
            return ERR_NO_MEMORY;
    if ((f->meta.flags & TAGFS_FILE_DELETED) && tag_bitmap_set(fs->bitmap_index, TAGFS_DELETED_MEMBERS, f->fid) != 0)
        return ERR_NO_MEMORY;
    char words[160];
    if (f->ext) {
        ksnprintf(words, sizeof(words), "a run of fid %llu carries an extension this kernel cannot carry "
                  "on (CODEC, SHARED or COPY, §8)", (unsigned long long)f->fid);
        TagFSUnderGlass(words);
    }
    if (f->wide) {
        /* Its flags' unknown bits are a newer writer's, carried byte for
         * byte by any writer (§2.3) — and memory holds 32 of them. */
        ksnprintf(words, sizeof(words), "fid %llu carries flags past the 32 bits this kernel holds — "
                  "written by a kernel that knows more", (unsigned long long)f->fid);
        TagFSUnderGlass(words);
    }
    g_book.files++;
    return OK;
}

static error_t BookFileAside(BookFile* f);
static error_t LeavesTag(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq);

error_t BookFileClose(void)
{
    BookFile* f = &g_book.file;
    char      words[160];
    error_t   rc = OK;
    if (!f->open)
        return OK;
    if (f->fid + 1u > g_book.next_fid)
        g_book.next_fid = f->fid + 1u;
    if (f->rotten)
        return BookFileAside(f);
    const char* why = BookFileJudged(f, words, sizeof(words));
    rc = why ? BookFileLock(f, why) : BookFileLive(f);
    BookFileForget(f);
    return rc;
}

// ----------------------------------------------------------------------------
// A rotten range's files, judged once every range is (mount/facsimile.c)
// ----------------------------------------------------------------------------

/*
 * ‼ A FILE A ROTTEN RANGE REACHES IS LOCKED ONLY ONCE NO OLDER SLOT STANDS IN
 * FOR THE RANGE. What of it read — its header before the range, its Parcels
 * past it — is set aside whole as the walk closes it; Facsimile (facsimile.c)
 * weighs every range once the walk is done, and then each file set aside is
 * either taken with a range an older slot gives back — the older slot's file,
 * replayed, stands in its place — or locked as it always was (BookFileLock):
 * the lock, its holds and what a Tally carries of it as it lay.
 */
static BookFile* s_aside;
static uint32_t  s_naside;
static uint32_t  s_capaside;

/* Why the older slot does not stand in for the range taken: a file of it
 * that does not stand there either — the first said. */
static char s_unfit[192];

static error_t BookFileAside(BookFile* f)
{
    if (s_naside == s_capaside) {
        uint32_t  cap  = s_capaside ? s_capaside * 2u : 4u;
        BookFile* more = kmalloc(sizeof(BookFile) * cap);
        if (!more) {
            BookFileForget(f);
            return ERR_NO_MEMORY;
        }
        if (s_naside)
            memcpy(more, s_aside, sizeof(BookFile) * s_naside);
        kfree(s_aside);
        s_aside    = more;
        s_capaside = cap;
    }
    s_aside[s_naside++] = *f;
    memset(f, 0, sizeof(*f));
    return OK;
}

error_t BookAsideSettled(bool (*taken)(uint64_t fid))
{
    error_t rc = OK;
    for (uint32_t i = 0; i < s_naside; i++) {
        BookFile* f = &s_aside[i];
        if (rc == OK && !taken(f->fid))
            rc = BookFileLock(f, "it lies in a rotten range of FILES");
        BookFileForget(f);
    }
    BookAsideLetGo();
    return rc;
}

void BookAsideLetGo(void)
{
    for (uint32_t i = 0; i < s_naside; i++)
        BookFileForget(&s_aside[i]);
    kfree(s_aside);
    s_aside    = NULL;
    s_naside   = s_capaside = 0;
    s_unfit[0] = '\0';
}

// ----------------------------------------------------------------------------
// The leaves of an older slot's Tally a range is taken from (Facsimile)
// ----------------------------------------------------------------------------

error_t BookFacsimileClose(void)
{
    BookFile* f = &g_book.file;
    char      words[160];
    error_t   rc = OK;
    if (!f->open)
        return OK;
    const char* why = BookFileJudged(f, words, sizeof(words));
    if (why && !s_unfit[0])
        ksnprintf(s_unfit, sizeof(s_unfit), "fid %llu does not stand in its Tally either (%s)",
                  (unsigned long long)f->fid, why);
    else if (!why)
        rc = BookFileLive(f);
    BookFileForget(f);
    return rc;
}

bool BookFacsimileUnfit(char* why, size_t room)
{
    bool unfit = s_unfit[0] != '\0';
    if (unfit)
        ksnprintf(why, room, "%s", s_unfit);
    s_unfit[0] = '\0';
    return unfit;
}

/*
 * The files a range's take laid in let go: every live file of [lo, hi) — the
 * walk set aside each file the range reaches and laid none of them in
 * (BookFileClose), so each is the take's, its items' or its replay's. Nothing
 * else of them stands yet: their ground goes to the census only once every
 * range is weighed and the ribbon replayed (BookFilesLaid), and a Born of the
 * range noted no fid while the range was out of g_book.rot (BookBornNoted).
 */
void BookFacsimileUndone(uint64_t lo, uint64_t hi)
{
    TagFSState* fs = tagfs_get_state();
    uint64_t    ids[32];
    uint32_t    n;
    while (lo < hi && (n = FileMapFiles(fs->files, lo, ids, 32u)) > 0) {
        for (uint32_t i = 0; i < n && ids[i] < hi; i++) {
            RecordWhere was;
            FileRecord* rec = FileMapRecordHold(fs->files, ids[i], NULL);
            if (!rec)
                continue;
            for (uint32_t t = 0; t < rec->tag_count; t++)
                TagBitmapLeave(fs->bitmap_index, rec->tags[t], ids[i]);
            if (rec->flags & TAGFS_FILE_DELETED)
                TagBitmapLeave(fs->bitmap_index, TAGFS_DELETED_MEMBERS, ids[i]);
            FileRecordRelease(rec);
            FileMapDrop(fs->files, ids[i], &was);
            g_book.files--;
        }
        lo = ids[n - 1u] < hi ? ids[n - 1u] + 1u : hi;
    }
}

error_t BookFacsimileItem(uint64_t tree, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    BookFile* f = &g_book.file;
    if (tree == TALLY_TAGS)
        return LeavesTag(NULL, key, body, len, seq);
    if (f->open && key.k1 != f->fid) {
        error_t rc = BookFacsimileClose();
        if (rc != OK)
            return rc;
    }
    if (!f->open) {
        f->open = true;
        f->fid  = key.k1;
    }
    return key.k2 == 0 ? BookFileHead(f, body, len) : BookFileParcel(f, key.k2 - 1u, body, len, seq);
}

/* A FILES item: (fid, 0) the header, (fid, 1 + b) the Parcel from block b. */
static error_t LeavesFile(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    BookFile* f = &g_book.file;
    (void)ctx;
    if (f->open && key.k1 != f->fid) {
        error_t rc = BookFileClose();
        if (rc != OK)
            return rc;
    }
    if (!f->open) {
        f->open   = true;
        f->fid    = key.k1;
        f->rotten = BookRotReaches(key.k1);
    }
    return key.k2 == 0 ? BookFileHead(f, body, len) : BookFileParcel(f, key.k2 - 1u, body, len, seq);
}

/* The keys of a FILES range, as its line says them — fid/part, which the
 * census scenario reads between the brackets. */
static void BookKeySay(const TallyKey* k, bool has, char* out, size_t room)
{
    if (has)
        ksnprintf(out, room, "%llu/%llu", (unsigned long long)k->k1, (unsigned long long)k->k2);
    else
        ksnprintf(out, room, "the end");
}

error_t BookRangeRots(const TallyPlace* at, const char* why)
{
    if (g_book.nrot == g_book.caprot) {
        uint32_t    cap  = g_book.caprot ? g_book.caprot * 2u : 8u;
        BookRotten* more = kmalloc(sizeof(BookRotten) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (g_book.nrot)
            memcpy(more, g_book.rot, sizeof(BookRotten) * g_book.nrot);
        kfree(g_book.rot);
        g_book.rot    = more;
        g_book.caprot = cap;
    }
    g_book.rot[g_book.nrot++] = (BookRotten){ at->lo, at->hi, at->has_hi, at->count };
    if (g_book.file.open && at->has_lo && at->lo.k1 == g_book.file.fid)
        g_book.file.rotten = true;
    return FacsimileRotNoted(TALLY_FILES, at, why);
}

void BookRangeTaken(const TallyPlace* at)
{
    for (uint32_t i = 0; i < g_book.nrot; i++) {
        const BookRotten* r = &g_book.rot[i];
        if (r->lo.k1 == at->lo.k1 && r->lo.k2 == at->lo.k2 && r->has_hi == at->has_hi &&
            (!r->has_hi || (r->hi.k1 == at->hi.k1 && r->hi.k2 == at->hi.k2))) {
            memmove(&g_book.rot[i], &g_book.rot[i + 1u], sizeof(BookRotten) * (g_book.nrot - i - 1u));
            g_book.nrot--;
            return;
        }
    }
}

void BookRangeBack(const TallyPlace* at)
{
    uint32_t i = g_book.nrot++;       /* BookRangeTaken kept the room */
    while (i > 0 && (g_book.rot[i - 1u].lo.k1 > at->lo.k1 ||
                     (g_book.rot[i - 1u].lo.k1 == at->lo.k1 && g_book.rot[i - 1u].lo.k2 > at->lo.k2))) {
        g_book.rot[i] = g_book.rot[i - 1u];
        i--;
    }
    g_book.rot[i] = (BookRotten){ at->lo, at->hi, at->has_hi, at->count };
}

error_t BookRangeRotsLaid(const TallyPlace* at, const char* why)
{
    char lo[48], hi[48];
    if (BinderRotten(TALLY_FILES, at) != OK)
        return ERR_NO_MEMORY;
    g_book.files_rot = true;
    g_book.said.rotten++;
    BookKeySay(&at->lo, at->has_lo, lo, sizeof(lo));
    BookKeySay(&at->hi, at->has_hi, hi, sizeof(hi));
    kprintf("[TagFS] rotten range fid [%s, %s): %llu file(s) locked — the FILES range rots in every copy "
            "(%s): none of them opens, their ids are not given again, their blocks are held\n", lo, hi,
            (unsigned long long)at->count, why);
    return OK;
}

// ----------------------------------------------------------------------------
// TAGS, USE, and the trees this kernel only looks at
// ----------------------------------------------------------------------------

/* A walk of one tree: which, and whether its USE element was met. */
typedef struct {
    uint64_t tree;
    bool     use_seen;
} LeavesWalk;

bool BookTagHeld(uint64_t tid, const TallyTag* t)
{
    char words[192];
    bool held = true;
    if (tid > TAGFS_MAX_TAG_ID) {
        ksnprintf(words, sizeof(words), "tag id %llu is past 2^31 - 2, the last this kernel gives",
                  (unsigned long long)tid);
        TagFSUnderGlass(words);
        held = false;
    }
    if (t->key_len > TAG_KEY_WIDTH || t->key_len + t->value_len > TAG_TEXT_WIDTH - 1u) {
        ksnprintf(words, sizeof(words), "tag id %llu's text (%llu + %llu bytes) is wider than this kernel "
                  "holds a tag (tag_spelling.h)", (unsigned long long)tid, (unsigned long long)t->key_len,
                  (unsigned long long)t->value_len);
        TagFSUnderGlass(words);
        held = false;
    }
    return held;
}

/* Where tag `tid` lies among those set aside, or would. */
static uint32_t BookTagAsideAt(uint64_t tid)
{
    uint32_t lo = 0, hi = g_book.naside;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g_book.aside[mid].id < tid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

const TallyTag* BookTagAsideOf(uint64_t tid)
{
    uint32_t at = BookTagAsideAt(tid);
    return at < g_book.naside && g_book.aside[at].id == tid ? &g_book.aside[at].tag : NULL;
}

error_t BookTagAsideNoted(uint64_t tid, const TallyTag* t)
{
    uint32_t at   = BookTagAsideAt(tid);
    bool     has  = at < g_book.naside && g_book.aside[at].id == tid;
    uint64_t cap  = g_book.capaside;
    uint8_t* text = kmalloc(t->key_len + t->value_len + 1u);
    if (!text || (!has && !RibbonGrow((void**)&g_book.aside, g_book.naside, &cap, sizeof(BookTagAside)))) {
        kfree(text);
        return ERR_NO_MEMORY;
    }
    g_book.capaside = (uint32_t)cap;
    if (has) {
        kfree(g_book.aside[at].text);
    } else {
        memmove(&g_book.aside[at + 1u], &g_book.aside[at], sizeof(BookTagAside) * (g_book.naside - at));
        g_book.naside++;
    }
    if (t->key_len)
        memcpy(text, t->key, t->key_len);
    if (t->value_len)
        memcpy(text + t->key_len, t->value, t->value_len);
    g_book.aside[at] = (BookTagAside){ .id = tid, .tag = *t, .text = text };
    g_book.aside[at].tag.key   = text;
    g_book.aside[at].tag.value = text + t->key_len;
    return OK;
}

static error_t LeavesTag(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    TallyTag t;
    char     words[160];
    (void)ctx;
    (void)seq;
    int got = TallyTagGet(body, len, &t);
    if (got == TALLY_BODY_BROKEN) {
        ksnprintf(words, sizeof(words), "the TAGS element of id %llu does not read", (unsigned long long)key.k1);
        TagFSUnderGlass(words);
        return OK;
    }
    ksnprintf(words, sizeof(words), "the TAGS element of id %llu", (unsigned long long)key.k1);
    if (got == TALLY_BODY_LONGER)
        LeavesLonger(words, len - TallyTagBytes(&t));
    if (key.k1 + 1u > g_book.next_tag)
        g_book.next_tag = key.k1 + 1u;
    if (!BookTagHeld(key.k1, &t))
        return BookTagAsideNoted(key.k1, &t);
    /* A retired id is an element of the tree as a tag is — one item, its text
     * none — and is counted as the Tally's writer counted it (binder_items.c,
     * TagsNext): the measures of the tree the mount stands on are the ones
     * its writer laid. */
    error_t rc = (t.flags & TALLY_TAG_RETIRED)
                     ? RegistryCoinRetired(tagfs_get_state()->registry, (uint32_t)key.k1)
                     : RegistryCoin(tagfs_get_state()->registry, (uint32_t)key.k1,
                                    (t.flags & TALLY_TAG_HAS_VALUE) ? TAGFS_TAG_FLAG_HAS_VALUE : 0,
                                    TagRegistryOwnerOf(&t), t.key, (uint8_t)t.key_len, t.value,
                                    (uint16_t)t.value_len);
    if (rc == ERR_ALREADY_EXISTS) {
        ksnprintf(words, sizeof(words), "tag id %llu's owner and text are another id's too, or its id "
                  "another tag's — the registry cannot hold the two at once", (unsigned long long)key.k1);
        TagFSUnderGlass(words);
        return OK;
    }
    if (rc == OK) {
        EndpaperLaidTag(&g_book.laid, TallyTagCarried(&t));
        FoliationLeafHolds(TALLY_TAGS, 1u, TallyTagCarried(&t));
    }
    return rc;
}

/* The Use Context: the tree's one element, key 0. */
static error_t LeavesUse(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    const uint8_t* text = NULL;
    uint64_t       n    = 0;
    LeavesWalk*    walk = (LeavesWalk*)ctx;
    (void)key;
    (void)seq;
    if (walk->use_seen)
        return OK;
    walk->use_seen = true;
    int got = TallyUseGet(body, len, &text, &n);
    if (got == TALLY_BODY_BROKEN) {
        TagFSUnderGlass("the USE element does not read");
        return OK;
    }
    if (got == TALLY_BODY_LONGER)
        LeavesLonger("the USE element", len - (TallyVarintBytes(n) + (uint32_t)n));
    return BookUseRecalled(text, n);
}

static error_t LeavesNone(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    (void)ctx;
    (void)key;
    (void)body;
    (void)len;
    (void)seq;
    return OK;
}

/* A rotten range of a tree whose leaves hold no file: said as the census says
 * it. */
static error_t LeavesRot(void* ctx, const TallyPlace* at, const char* why)
{
    uint64_t tree = ((const LeavesWalk*)ctx)->tree;
    if (at->level > 0)
        EndpaperLaidNode(&g_book.laid, tree, at->level, 0, 1u, at->has_lo ? at->lo.k1 : 0);
    if (tree == TALLY_FILES)
        return BookRangeRots(at, why);
    if (tree == TALLY_TAGS)
        return FacsimileRotNoted(TALLY_TAGS, at, why);
    return BookTagsRotLaid(tree, at, why);
}

error_t BookTagsRotLaid(uint64_t tree, const TallyPlace* at, const char* why)
{
    char lo[48], hi[48];
    g_book.said.rotten++;
    ksnprintf(lo, sizeof(lo), "%llu", (unsigned long long)(at->has_lo ? at->lo.k1 : 0));
    if (at->has_hi)
        ksnprintf(hi, sizeof(hi), "%llu", (unsigned long long)at->hi.k1);
    else
        ksnprintf(hi, sizeof(hi), "the end");
    if (tree == TALLY_TAGS)
        kprintf("[TagFS] the TAGS range [%s, %s) rots in every copy (%s): the names of %llu tag id(s) are "
                "lost — the files keep the ids, nameless\n", lo, hi, why, (unsigned long long)at->count);
    else
        kprintf("[TagFS] the %s range [%s, %s) rots in every copy (%s)\n", TallyTreeName(tree), lo, hi, why);
    return OK;
}

error_t TallyLeaves(uint64_t tree, const SpineRoot* root)
{
    LeavesWalk walk = { tree, false };
    TallySink  sink = {
        .ctx    = &walk,
        .item   = tree == TALLY_FILES ? LeavesFile
                : tree == TALLY_TAGS  ? LeavesTag
                : tree == TALLY_USE   ? LeavesUse : LeavesNone,
        .rotten = LeavesRot,
        .node   = BookGroundNode,
        .said   = true,
        .mend   = BookGroundMended,
        .shapes = &g_book.said.shapes,
    };
    error_t rc = TallyRead(tree, root, true, &sink);
    if (rc == OK && tree == TALLY_FILES)
        rc = BookFileClose();
    if (rc == OK && tree == TALLY_USE && !walk.use_seen)
        rc = BookUseRecalled(NULL, 0);
    return rc;
}
