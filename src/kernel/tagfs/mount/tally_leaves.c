#include "book_mount.h"
#include "ground_census.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../tag_spelling/tag_spelling.h"
#include "../use_memory/use_memory.h"

/*
 * The leaves of the Tally the mount stands on, into the memory every format
 * shares (book_mount.h): FILES into the map of live files, TAGS into the
 * registry, USE into the Use Context. BOOKMARKS and LIEN carry what this
 * kernel cannot hold — a tree of them that is not empty puts the volume
 * under glass (book_mount.c) — and their leaves are read for nothing but
 * their shape.
 *
 * Read the way volume-read reads them (tools/volumeread/tally.py, Tally._Files
 * and _Rest), so the two readers lock the same files and give the same
 * reasons the volume may only be read: a body longer than this reader knows,
 * a tag RETIRED, past the last id this kernel gives or wider than it holds a
 * tag, a run carrying an extension.
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

/* The gathering's memory let go, and the gathering emptied. */
static void BookFileForget(BookFile* f)
{
    kfree(f->meta.filename);
    kfree(f->meta.tag_ids);
    kfree(f->strips);
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
           h->tags_bytes;
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

/* A run of the book into Strips: a run longer than a Strip counts is laid in
 * as many as it takes. What of a DATA run lies outside the data run is not
 * the file's to name — the file is locked for it, and only what lies inside
 * is laid, the part a lock holds (BookGroundHoldRun); a HOLE is laid only
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
        if (start < lo || end > hi)
            BookFileBroken(f, "it names ground outside the data run");
        start = start < lo ? lo : start;
        end   = end > hi ? hi : end;
        if (start >= end)
            return true;
    }
    for (uint64_t left = end - start; left;) {
        uint32_t n = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        Strip    s = { r->hole ? 0 : start, n, r->hole ? STRIP_HOLE : STRIP_DATA, r->birth };
        if (!BookFileStrip(f, &s))
            return false;
        start += r->hole ? 0 : n;
        left  -= n;
    }
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
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        TallyRunGet(body, len, &at, &cursor, seq, &r);
        f->ext |= r.ext != NULL;
        if (!BookFileRun(f, &r))
            return ERR_NO_MEMORY;
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

/* A file the book holds and cannot read: said, kept by name for every open of
 * it to be refused, and what of its ground it is known to name held. */
static error_t BookFileLock(BookFile* f, const char* why)
{
    error_t rc = BookLockedByName(f->fid, why);
    for (uint32_t i = 0; rc == OK && i < f->nstrips; i++)
        if (f->strips[i].kind == STRIP_DATA)
            BookGroundHoldRun(f->strips[i].start, f->strips[i].count);
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
    error_t rc = BookLockedByName(fid, why);
    StripWalkBegin(rec, &walk);
    while (rc == OK && StripWalkNextGround(&walk, &run))
        BookGroundHoldRun(run.start, run.count);
    for (uint32_t t = 0; rc == OK && t < rec->tag_count; t++)
        TagBitmapLeave(fs->bitmap_index, rec->tags[t], fid);
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
    f->meta.extents      = f->strips;
    f->meta.extent_count = f->nstrips;
    FileRecord* rec = FileRecordBuild(&f->meta);
    if (!rec)
        return ERR_NO_MEMORY;
    rec->gen = f->gen;
    RecordWhere  where, was;
    FileMapStake stake = { .count = 0 };
    memset(&where, 0, sizeof(where));
    error_t rc = FileMapRecordPut(fs->files, f->fid, rec, &where, 0, &stake, &was);
    FileMapStakeLetGo(&stake);
    if (rc != OK)
        return rc;
    for (uint32_t t = 0; t < f->meta.tag_count; t++)
        if (tag_bitmap_set(fs->bitmap_index, f->meta.tag_ids[t], f->fid) != 0)
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

error_t BookFileClose(void)
{
    BookFile* f = &g_book.file;
    char      words[160];
    error_t   rc = OK;
    if (!f->open)
        return OK;
    if (f->fid + 1u > g_book.next_fid)
        g_book.next_fid = f->fid + 1u;
    const char* why = BookFileJudged(f, words, sizeof(words));
    rc = why ? BookFileLock(f, why) : BookFileLive(f);
    BookFileForget(f);
    return rc;
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
    char lo[48], hi[48];
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
    if (t->flags & TALLY_TAG_RETIRED) {
        ksnprintf(words, sizeof(words), "tag id %llu is RETIRED — this kernel gives no id twice",
                  (unsigned long long)tid);
        TagFSUnderGlass(words);
        held = false;
    }
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

/* The bytes of a TAGS body this reader knows. */
static uint32_t LeavesTagKnown(const TallyTag* t)
{
    uint32_t n = TallyVarintBytes(t->flags);
    if (t->flags & TALLY_TAG_RETIRED)
        return n;
    n += TallyVarintBytes(t->key_len) + (uint32_t)t->key_len;
    if (t->flags & TALLY_TAG_HAS_VALUE)
        n += TallyVarintBytes(t->value_len) + (uint32_t)t->value_len;
    return n;
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
        LeavesLonger(words, len - LeavesTagKnown(&t));
    if (key.k1 + 1u > g_book.next_tag)
        g_book.next_tag = key.k1 + 1u;
    if (!BookTagHeld(key.k1, &t))
        return BookTagAsideNoted(key.k1, &t);
    error_t rc = RegistryCoin(tagfs_get_state()->registry, (uint32_t)key.k1,
                              (t.flags & TALLY_TAG_HAS_VALUE) ? TAGFS_TAG_FLAG_HAS_VALUE : 0, t.key,
                              (uint8_t)t.key_len, t.value, (uint16_t)t.value_len);
    if (rc == ERR_ALREADY_EXISTS) {
        ksnprintf(words, sizeof(words), "tag id %llu's text is another id's too, or its id another "
                  "text's — the registry cannot hold the two at once", (unsigned long long)key.k1);
        TagFSUnderGlass(words);
        return OK;
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
    char     lo[48], hi[48];
    if (tree == TALLY_FILES)
        return BookRangeRots(at, why);
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
        .mended = &g_book.said.mended,
        .shapes = &g_book.said.shapes,
    };
    error_t rc = TallyRead(tree, root, true, &sink);
    if (rc == OK && tree == TALLY_FILES)
        rc = BookFileClose();
    if (rc == OK && tree == TALLY_USE && !walk.use_seen)
        rc = BookUseRecalled(NULL, 0);
    return rc;
}
