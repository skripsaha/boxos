#include "dedup_internal.h"
#include "../file_map/file_map.h"
#include "../file_map/tail_slip.h"
#include "../ink/ink.h"
#include "../mount/book_mount.h"
#include "../tag_registry/tag_registry.h"

/*
 * Who names a block (dedup_internal.h): the live files walked in id order, a
 * few a turn, every run of each record held to the blocks asked — sorted, so
 * a run finds the first of them it covers in log n. No map of block to owner
 * is kept: a book derives one only at its mount (spec_v2 §4.6), so the pass
 * that needs one asks the mirror for the blocks it matched alone, and lets
 * the answer go with the pass.
 *
 * ‼ ANY NAME OF A BLOCK BARS IT AS ITS FILE IS BARRED (§16.7 п.8). A file
 * wearing dedup:off, a file the system keeps, the kernel the Deed points the
 * loader at, a file the mount locked: every block one of them names is
 * neither laid nor laid on, whoever else names it too — nothing is laid on a
 * block in common with a file the system keeps, nor on a block of the kernel's
 * BOOT run. A copy is laid from its plain ground alone: a block under a Slip,
 * or of a file whose runs touch BOOT, is laid on nothing (Conjugate refuses
 * both, conjugate.c ConjugateGuards) — and passed over here, before any act.
 */

#define DEDUP_OWNERS_LOOK  32u     /* file ids one look at the map takes */

static int OwnerByBlock(const void* a, const void* b, void* ctx)
{
    const DedupOwner* x = a;
    const DedupOwner* y = b;
    (void)ctx;
    return x->block < y->block ? -1 : x->block > y->block;
}

error_t DedupOwnersBegin(DedupOwners* w, const DedupMember* m, uint64_t n)
{
    TagSpelling off = { .key = "dedup", .key_len = 5, .value = "off", .value_len = 3 };
    uint64_t    k   = 0;
    memset(w, 0, sizeof(*w));
    if (n > UINT64_MAX / sizeof(DedupOwner))
        return ERR_NO_MEMORY;
    w->owner = n ? kmalloc(sizeof(DedupOwner) * n) : NULL;
    if (n && !w->owner)
        return ERR_NO_MEMORY;
    for (uint64_t i = 0; i < n; i++)
        w->owner[i] = (DedupOwner){ .block = m[i].block, .kind = DEDUP_NO_OWNER };
    KSort(w->owner, n, sizeof(DedupOwner), OwnerByBlock, NULL);
    for (uint64_t i = 0; i < n; i++)
        if (k == 0 || w->owner[k - 1u].block != w->owner[i].block)
            w->owner[k++] = w->owner[i];
    w->n       = k;
    w->from    = k ? 0 : UINT64_MAX;
    w->off_tag = TagRegistryLookup(tagfs_get_state()->registry, &off);
    return OK;
}

/* What bars every block the file names, whatever its runs. */
static uint32_t FileBars(const DedupOwners* w, const FileRecord* rec)
{
    if (InkWearsGuarded(rec->tags, rec->tag_count) || rec->file_id == g_book.boot_fid ||
        BookFileLocked(rec->file_id))
        return DEDUP_BAR_ONTO | DEDUP_BAR_COPY;
    for (uint32_t i = 0; w->off_tag != TAGFS_INVALID_TAG_ID && i < rec->tag_count; i++)
        if (rec->tags[i] == w->off_tag)
            return DEDUP_BAR_ONTO | DEDUP_BAR_COPY;
    return 0;
}

/* Run `s` of the file, its block `k` of the file the first of the run, held
 * to the blocks asked: each it names barred as the file is — and a block
 * under a Slip, or of a file whose runs touch BOOT, laid on nothing; a block
 * of the BOOT run itself, neither — and named the file's when it has no owner
 * yet, or only a run SHARED where this is the DATA run. *touches whether the
 * file's runs touch BOOT, asked the first time a block of it is met (-1
 * before). */
static void OwnersOfRun(DedupOwners* w, const FileRecord* rec, const Strip* s, uint64_t k, uint32_t bars,
                        int* touches)
{
    uint64_t lo = 0, hi = w->n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (w->owner[mid].block < s->start)
            lo = mid + 1u;
        else
            hi = mid;
    }
    for (uint64_t i = lo; i < w->n && w->owner[i].block < s->start + s->count; i++) {
        DedupOwner* o  = &w->owner[i];
        uint64_t    at = k + (o->block - s->start);
        uint32_t    b  = bars;
        if (*touches < 0)
            *touches = InkRecordTouchesBoot(rec) ? 1 : 0;
        if (*touches || (s->kind == STRIP_DATA && TailSlipOn(rec, at)))
            b |= DEDUP_BAR_COPY;
        if (InkTouchesBoot(s, 1))
            b |= DEDUP_BAR_ONTO | DEDUP_BAR_COPY;
        o->bars |= b;
        if (o->kind == DEDUP_NO_OWNER || (o->kind == STRIP_SHARED && s->kind == STRIP_DATA)) {
            o->fid   = rec->file_id;
            o->at    = at;
            o->birth = s->birth;
            o->kind  = s->kind;
        }
    }
}

/* One live file's runs held to the blocks asked. */
static void OwnersOfFile(DedupOwners* w, uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    StripWalk   walk;
    Strip       s;
    uint64_t    k       = 0;
    int         touches = -1;
    if (!rec)
        return;
    if (rec->flags & TAGFS_FILE_ACTIVE) {
        uint32_t bars = FileBars(w, rec);
        StripWalkBegin(rec, &walk);
        while (StripWalkNext(&walk, &s)) {
            if (StripOnGround(s.kind))
                OwnersOfRun(w, rec, &s, k, bars, &touches);
            k += s.count;
        }
    }
    FileRecordRelease(rec);
}

bool DedupOwnersStep(DedupOwners* w, uint32_t files)
{
    uint64_t ids[DEDUP_OWNERS_LOOK];
    while (files > 0 && w->from != UINT64_MAX) {
        uint32_t n = FileMapRows(tagfs_get_state()->files, w->from, ids,
                                 files < DEDUP_OWNERS_LOOK ? files : DEDUP_OWNERS_LOOK);
        if (n == 0) {
            w->from = UINT64_MAX;
            break;
        }
        for (uint32_t i = 0; i < n; i++)
            OwnersOfFile(w, ids[i]);
        files  -= n;
        w->from = ids[n - 1u] == UINT64_MAX ? UINT64_MAX : ids[n - 1u] + 1u;
    }
    return w->from == UINT64_MAX;
}

const DedupOwner* DedupOwnerOf(const DedupOwners* w, uint64_t block)
{
    uint64_t lo = 0, hi = w->n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (w->owner[mid].block < block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < w->n && w->owner[lo].block == block ? &w->owner[lo] : NULL;
}

void DedupOwnersLetGo(DedupOwners* w)
{
    kfree(w->owner);
    memset(w, 0, sizeof(*w));
    w->from = UINT64_MAX;
}
