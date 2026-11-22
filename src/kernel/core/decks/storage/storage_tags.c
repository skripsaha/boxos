/*
 * Storage Deck — the calls that speak tags: storage.query, storage.name.query,
 * storage.tag.list, storage.tag.set, storage.tag.unset, storage.create and
 * obj.getinfo (boxos_decks.h, where every layout is said once).
 *
 * ‼ EVERY ONE SPEAKS ONE OWNER'S TAGS — OR SHOWS EVERY ONE IT MAY SEE.
 *
 * A tag is its text and whose it is (tagfs.h, TagOwner): the caller names in
 * its whose byte its own tags — a program's words for its own files, which no
 * other program meets unless it shows them —, the person's, the labels the
 * person finds files by, or another program's; a file's tags, and the list of
 * them, may be shown of every owner the caller may see, each with whose it
 * is. Every program, the person's own tools among them, speaks through this
 * one door and is judged by the one rule, StorageMaySpeak: no program is let
 * in by its name, and no call of a program's own tags asks anything.
 */

#include "klib.h"
#include "crate_io.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "auth_tags.h"
#include "storage_deck.h"
#include "storage_tags.h"
#include "tagfs.h"
#include "use_context.h"   /* the person's tags — folded into a call in them */
#include "ink.h"           /* InkBookRefuses — a create a book refuses interns nothing */
#include "process.h"
#include "cabin.h"         /* CabinProgramFile — who the program is */
#include "settle_ops.h"    /* SettleOpForRoom — room on the ribbon waited for */

/* The caller's program as the volume mounted knows it: its file — or 0 for a
 * program started from bytes held in memory, from another volume, or whose
 * file is gone, which no other program can show its tags to. */
uint64_t StorageReader(const OpContext *ctx)
{
    const cabin_t *cabin = ctx && ctx->proc ? ctx->proc->cabin : NULL;
    uint64_t       fid   = CabinProgramFile(cabin);
    return fid && TagFSHoldsProgram(CabinProgramVolume(cabin), fid) ? fid : 0;
}

/* Whether the caller's box asked to see the person's tags — `change` to
 * change them — read off its authority as it stands when the call is taken:
 * the person's eyes see every mark on the person's files, and the person's
 * hands take one off. A call no cabin makes is the kernel's own — every
 * program's call comes through its cabin —, and the kernel speaks as the
 * system, the person's eyes and hands its own; it has no tags of its own,
 * being no program (StorageMaySpeak). */
bool StoragePersonSays(const OpContext *ctx, bool change)
{
    const cabin_t *cabin = ctx && ctx->proc ? ctx->proc->cabin : NULL;
    if (!cabin)
        return true;
    uint32_t auth = __atomic_load_n(&cabin->auth_bits, __ATOMIC_RELAXED);
    return (auth & (change ? AUTH_TAG_CHANGE_TAGS : (AUTH_TAG_SEE_TAGS | AUTH_TAG_CHANGE_TAGS))) != 0;
}

/*
 * Whose tags a call speaks — read from params[at] on (boxos_decks.h,
 * STORAGE_TAGS_*) — and judged apart: a call is judged only where it speaks
 * some — asks for tags, sets some aside, puts or takes one, shows a file's,
 * lists them, or folds in the Use Context — so a question of no tags, a name
 * looked up alone, asks nothing of anybody. *width the bytes the whose took:
 * its byte, and a program's file id after it. *all for STORAGE_TAGS_ALL,
 * which a call takes only when `all_ok` — it shows tags, it never asks by
 * them nor changes them.
 */
static error_t StorageWhose(const ManifestOp *op, size_t at, const OpContext *ctx, bool all_ok, TagOwner *out,
                            bool *all, size_t *width)
{
    *all   = false;
    *width = 1;
    if (op->param_size <= at)
        return ERR_INVALID_ARGUMENT;
    switch (op->params[at]) {
    case STORAGE_TAGS_MINE:
        *out = TagOwnerProgram(CabinProgramFile(ctx && ctx->proc ? ctx->proc->cabin : NULL));
        return OK;
    case STORAGE_TAGS_PERSON:
        *out = TagOwnerPerson();
        return OK;
    case STORAGE_TAGS_PROGRAM: {
        uint64_t program = op->param_size >= at + 9u ? param_u64(op, at + 1u) : 0;
        if (!program)
            return ERR_INVALID_ARGUMENT;
        *out   = TagOwnerProgram(program);
        *width = 9;
        return OK;
    }
    case STORAGE_TAGS_ALL:
        *out = TagOwnerPerson();
        *all = all_ok;
        return all_ok ? OK : ERR_INVALID_ARGUMENT;
    default:
        return ERR_INVALID_ARGUMENT;
    }
}

/*
 * Whether the caller may speak `owner`'s tags so. One rule for every
 * program, none let in by its name.
 *
 * Its own: named by the file it was started from (CabinProgramFile). To be
 * seen they ask nothing — a program with none, started from bytes held in
 * memory, is answered that it has none, which is so. To be put on or taken
 * off they ask that the file be on the volume the program was started from —
 * one started from memory has no file to name a tag by, and one whose file is
 * gone has no tags left to change (the volume retires them, tagfs/retire):
 * ERR_TAGFS_NO_OWN_TAGS. Another program's: seen by one it shows them to
 * (TagFSShows) and by the person's eyes, taken off a file by the person's
 * hands, put on by nobody but that program. The person's: seen by the
 * person's eyes, changed by the person's hands (StoragePersonSays):
 * ERR_ACCESS_DENIED otherwise. The kernel's own call — no cabin — is the
 * person's eyes and hands, and has no tags of its own.
 */
error_t StorageMaySpeak(const OpContext *ctx, TagOwner owner, StorageSpeech speech)
{
    const cabin_t *cabin = ctx && ctx->proc ? ctx->proc->cabin : NULL;
    if (owner.kind == TAG_OWNER_PERSON)
        return StoragePersonSays(ctx, speech != STORAGE_SEE) ? OK : ERR_ACCESS_DENIED;
    uint64_t self = CabinProgramFile(cabin);
    if (owner.id == 0 || owner.id == self)
        return speech == STORAGE_SEE || (owner.id && TagFSHoldsProgram(CabinProgramVolume(cabin), owner.id))
                   ? OK
                   : ERR_TAGFS_NO_OWN_TAGS;
    switch (speech) {
    case STORAGE_SEE: {
        uint64_t reader = StorageReader(ctx);
        return StoragePersonSays(ctx, false) || (reader && TagFSShows(owner.id, reader)) ? OK : ERR_ACCESS_DENIED;
    }
    case STORAGE_TAKE:
        return StoragePersonSays(ctx, true) ? OK : ERR_ACCESS_DENIED;
    default:
        return ERR_ACCESS_DENIED;
    }
}

/* Whose tags a call speaks, judged at once: the calls that always speak one
 * owner's. */
static error_t StorageTagsOf(const ManifestOp *op, size_t at, const OpContext *ctx, StorageSpeech speech,
                             TagOwner *out)
{
    bool    all = false;
    size_t  width;
    error_t rc = StorageWhose(op, at, ctx, false, out, &all, &width);
    return rc == OK ? StorageMaySpeak(ctx, *out, speech) : rc;
}

/* The programs that show their tags to the caller's grow a page at a time. */
#define STORAGE_SHOWN_PIECE 32u

/*
 * The view of every owner the caller may see (STORAGE_TAGS_ALL): its own,
 * those of the programs that show it theirs, and — the person's eyes — every
 * one. *list holds the programs shown it, rising, the caller's to kfree.
 */
error_t StorageViewAll(const OpContext *ctx, TagView *view, uint64_t **list)
{
    *list = NULL;
    *view = (TagView){ .all = true, .everyone = StoragePersonSays(ctx, false), .owner = TagOwnerPerson(),
                       .reader = StorageReader(ctx) };
    if (view->everyone || !view->reader)
        return OK;
    uint64_t  piece[STORAGE_SHOWN_PIECE];
    uint64_t *ids = NULL, from = 1;
    uint32_t  n = 0, room = 0;
    for (;;) {
        TagFSPage page;
        error_t   rc = TagFSShownToPage(view->reader, from, piece, STORAGE_SHOWN_PIECE, false, &page);
        if (rc == OK && n + page.delivered > room) {
            uint32_t  more  = room ? 2u * room : STORAGE_SHOWN_PIECE;
            uint64_t *grown = kmalloc(sizeof(uint64_t) * more);
            if (grown && n)
                memcpy(grown, ids, sizeof(uint64_t) * n);
            kfree(ids);
            ids  = grown;
            room = grown ? more : 0;
            rc   = grown ? OK : ERR_NO_MEMORY;
        }
        if (rc != OK) {
            kfree(ids);
            return rc;
        }
        if (page.delivered)
            memcpy(ids + n, piece, sizeof(uint64_t) * page.delivered);
        n += page.delivered;
        if (!page.next)
            break;
        from = page.next;
    }
    view->shown  = ids;
    view->nshown = n;
    *list        = ids;
    return OK;
}

/* -------------------------------------------------------------------------
 * OBJ_GET_INFO — serialize file metadata into out_crate.
 *
 * Every tag of the caller's view goes out whole, however long: its key and
 * value are copied straight out of the volume's registry into the answer.
 * The view is one owner's — the caller's own tags, the person's, another
 * program's — or every owner's the caller may see (TagView), and a tag the
 * view does not let in is not the caller's to read. What a file is — a note,
 * anything — is the tags it wears; the answer has no flags.
 *
 * params: [u64 file_id][whose]. Layout written into out_crate (little-endian):
 *   [u64 file_id]
 *   [u64 size]
 *   [u32 tag_count]      every tag of the view the file wears: the book caps a file's tags at none
 *   [u16 filename_len]
 *   [char filename[filename_len]]
 *   for each tag:
 *     [u16 key_len][u16 val_len][u8 type][u64 owner][char key[key_len]][char val[val_len]]
 *       type: 1 = system (reserved-vocabulary tag), 0 = user;
 *       owner: 0 the person's, a program's file id a program's.
 *
 * out_crate.size is set to the total bytes written. No truncation: if the
 * crate is too small, we return ERR_BUFFER_TOO_SMALL with size unchanged.
 * ------------------------------------------------------------------------- */

/* One tag's record: [u16 key_len][u16 val_len][u8 type][u64 owner][key][val]. */
static uint64_t info_tag_record(uint8_t *kp, uint64_t pos, uint32_t tag_id)
{
    TagSpelling tag       = { 0 };
    TagOwner    owner     = TagOwnerPerson();
    bool        is_system = false;
    /* A tag the volume no longer knows still takes its place in the list:
     * the count in the header says how many records follow, so skipping one
     * would leave the reader parsing the next tag's bytes as this one's. */
    (void)TagFSTagSpelling(tag_id, &tag, &is_system, &owner);
    uint16_t kl    = (uint16_t)tag.key_len;
    uint16_t vl    = (uint16_t)tag.value_len;
    uint64_t whose = TagOwnerWord(owner);
    if (kp) {
        memcpy(kp + pos, &kl, 2);
        memcpy(kp + pos + 2, &vl, 2);
        kp[pos + 4] = is_system ? 1 : 0;
        memcpy(kp + pos + 5, &whose, 8);
        if (kl) memcpy(kp + pos + 13, tag.key, kl);
        if (vl) memcpy(kp + pos + 13 + kl, tag.value, vl);
    }
    return pos + 13u + kl + vl;
}

/* Whether the file's tag `tag_id` is in `view`. Inside the door. */
static bool InfoTagOf(uint32_t tag_id, const TagView *view)
{
    TagSpelling tag;
    TagOwner    owner;
    return TagFSTagSpelling(tag_id, &tag, NULL, &owner) && TagViewLets(view, owner);
}

int ObjGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 9)                return ERR_INVALID_ARGUMENT;

    TagOwner  owner;
    TagView   view;
    uint64_t *shown = NULL;
    bool      all   = false;
    size_t    width = 0;
    error_t   whose = StorageWhose(op, 8, ctx, true, &owner, &all, &width);
    if (whose == OK && all)
        whose = StorageViewAll(ctx, &view, &shown);
    else if (whose == OK)
        whose = StorageMaySpeak(ctx, owner, STORAGE_SEE);
    if (whose != OK) return whose;
    if (!all)
        view = TagViewOf(owner);
    uint64_t file_id = param_u64(op, 0);
    Crate   *out     = &crates[op->out_crate];

    TagFSMetadata md;
    if (tagfs_get_metadata(file_id, &md) != 0 || !(md.flags & TAGFS_FILE_ACTIVE)) {
        kfree(shown);
        return ERR_FILE_NOT_FOUND;
    }

    uint16_t filename_len = 0;
    if (md.filename) {
        size_t l = strlen(md.filename);
        if (l > 0xFFFFu) l = 0xFFFFu;
        filename_len = (uint16_t)l;
    }

    /* Both passes read the tags as spans into the volume's registry, so both
     * are inside the door: nothing is copied into a buffer of this deck's
     * size, and the registry the second pass writes from is the one the first
     * measured — a tag's text never changes while it is mounted. */
    if (!tagfs_enter()) {
        tagfs_metadata_free(&md);
        kfree(shown);
        return ERR_FILE_NOT_FOUND;
    }

    uint32_t tag_count = 0;
    uint64_t need = 8u + 8u + 4u + 2u + filename_len + 8u + 8u + 2u + md.head_len;
    for (uint32_t i = 0; i < md.tag_count; i++) {
        if (!InfoTagOf(md.tag_ids[i], &view))
            continue;
        need = info_tag_record(NULL, need, md.tag_ids[i]);
        tag_count++;
    }

    uint8_t *kp = NULL;
    int      rc = OK;
    if (need > out->capacity)
        rc = ERR_BUFFER_TOO_SMALL;
    else if (!(kp = crate_out_alloc(out, need)))
        rc = ERR_INVALID_ADDRESS;

    uint64_t pos = 0;
    if (rc == OK) {
        /* What a file is — a note, anything else — is the tags it
         * wears, and only they say it: the answer carries no bits beside them
         * but the volume's own words: the time a deleted file is kept until,
         * and the file of the program that made it. */
        memcpy(kp + pos, &file_id,    8); pos += 8;   /* the id asked: the record's own */
        memcpy(kp + pos, &md.size,    8); pos += 8;
        memcpy(kp + pos, &tag_count,  4); pos += 4;
        memcpy(kp + pos, &filename_len, 2); pos += 2;
        if (filename_len) { memcpy(kp + pos, md.filename, filename_len); pos += filename_len; }
        for (uint32_t i = 0; i < md.tag_count; i++)
            if (InfoTagOf(md.tag_ids[i], &view))
                pos = info_tag_record(kp, pos, md.tag_ids[i]);
        /* The last eight bytes: when a deleted file is let go, 0 for one that is not. */
        const uint64_t kept_until = (md.flags & TAGFS_FILE_DELETED) ? md.keep_until : 0;
        memcpy(kp + pos, &kept_until, 8); pos += 8;
        /* The program that made the file, 0 when none did. */
        memcpy(kp + pos, &md.made_by, 8); pos += 8;
        /* What the system keeps the file as, and its first bytes: what a
         * file is, told by the volume and by the file itself. */
        kp[pos++] = (uint8_t)((md.flags & TAGFS_FILE_ROLE_MASK) >> 2);
        kp[pos++] = md.head_len;
        if (md.head_len) { memcpy(kp + pos, md.head, md.head_len); pos += md.head_len; }
    }
    tagfs_leave();
    tagfs_metadata_free(&md);
    kfree(shown);
    if (rc != OK) return rc;

    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = pos;
    return OK;
}

/* =========================================================================
 *  Tag / Query / Context / Create
 *
 *  A tag's text is read by the one spelling (tag_spelling.h), in place in the
 *  crate's copy, and a tag that does not spell refuses the call. The Use
 *  Context's tags are folded in for a call in the person's tags when the
 *  scope is USE (StorageLens).
 * ========================================================================= */

/*
 * The scope byte of storage.query / storage.create (boxos_decks.h): inside the
 * Use Context unless the caller said everywhere.
 */
static error_t storage_scope_of(const ManifestOp *op, size_t at, bool deleted_too, uint8_t *out_scope)
{
    if (op->param_size <= at) return ERR_INVALID_ARGUMENT;
    uint8_t scope = op->params[at];
    if (scope != STORAGE_SCOPE_USE && scope != STORAGE_SCOPE_EVERYWHERE &&
        !(deleted_too && scope == STORAGE_SCOPE_DELETED))
        return ERR_INVALID_ARGUMENT;
    *out_scope = scope;
    return OK;
}

/* The scope a call is asked in: the Use Context is the person's lens, and a
 * call in a program's own tags is asked of the whole volume whatever its
 * scope says — a game finds its saves wherever the person is working. */
static uint8_t StorageLens(uint8_t scope, TagOwner owner)
{
    return owner.kind == TAG_OWNER_PERSON || scope == STORAGE_SCOPE_DELETED ? scope : STORAGE_SCOPE_EVERYWHERE;
}

/* Whether a call speaks tags at all: it asks or puts `tags` of them, sets
 * `without` aside, or is asked through the person's lens — a call in the
 * person's tags inside the Use Context speaks them whether the context says
 * anything this moment or not, so whether it is let in never turns on what
 * the person happens to be working on. */
static bool StorageSpeaks(uint8_t scope, TagOwner owner, uint32_t tags, uint32_t without)
{
    return tags || without || StorageLens(scope, owner) == STORAGE_SCOPE_USE;
}

/*
 * The tags a query asks for or a create stamps: the caller's, spelled in place
 * out of the `len` bytes at `text`, followed — when the scope is USE — by the
 * Use Context's. The caller's list is judged whole before anything is
 * allocated for it, and a tag that does not spell refuses the call: a query
 * never drops a tag it cannot read and answers for the rest, and a create
 * never makes a file without a tag it was asked for. So is a text that names
 * no tag at all — asking for every file is asking with no text.
 *
 * The Use Context's spellings point into the context as it stood at the one
 * look this takes — held in *out_use, which the caller gives back with
 * UseListDrop once the spellings are done with; NULL outside the USE scope or
 * with no context. *out_tags is the caller's to kfree; the caller's own tags
 * are the first *out_own of *out_count. All NULL and 0 when there is nothing
 * to ask.
 */
static error_t storage_tags_spelled(uint8_t scope, const char *text, size_t len,
                                    TagSpelling **out_tags, UseList **out_use,
                                    uint32_t *out_own, uint32_t *out_count)
{
    *out_tags  = NULL;
    *out_use   = NULL;
    *out_own   = 0;
    *out_count = 0;

    uint32_t own = 0;
    error_t  rc  = TagListCount(text, len, &own);
    if (rc != OK)
        return rc;
    if (len > 0 && own == 0)
        return ERR_TAGFS_INVALID_TAG;

    UseList *use   = scope == STORAGE_SCOPE_USE ? UseContextHold() : NULL;
    uint32_t n_use = UseListCount(use);
    if (own + n_use == 0) {
        UseListDrop(use);
        return OK;
    }

    TagSpelling *tags = kmalloc(sizeof(TagSpelling) * ((size_t)own + n_use));
    if (!tags) {
        UseListDrop(use);
        return ERR_NO_MEMORY;
    }
    size_t at = 0;
    for (uint32_t i = 0; i < own; i++)
        (void)TagListNext(text, len, &at, &tags[i]);   /* judged whole above */
    for (uint32_t i = 0; i < n_use; i++)
        tags[own + i] = UseListTag(use, i);

    *out_tags  = tags;
    *out_use   = use;
    *out_own   = own;
    *out_count = own + n_use;
    return OK;
}

/*
 * The tags a question sets aside: the comma list in the op's params from byte
 * `at` to their end, spelled in place (the params are the kernel's copy of the
 * manifest). *out is the caller's to kfree; NULL and 0 when the question sets
 * none aside. A tag that does not spell refuses the question, as one of its
 * own tags would.
 */
static error_t storage_without_spelled(const ManifestOp *op, size_t at, TagSpelling **out,
                                       uint32_t *out_count)
{
    *out       = NULL;
    *out_count = 0;
    if (op->param_size <= at)
        return OK;
    const char  *text = (const char *)op->params + at;
    const size_t len  = op->param_size - at;
    uint32_t     n    = 0;
    error_t      rc   = TagListCount(text, len, &n);
    if (rc != OK || n == 0)
        return rc;
    TagSpelling *without = kmalloc(sizeof(TagSpelling) * n);
    if (!without)
        return ERR_NO_MEMORY;
    size_t from = 0;
    for (uint32_t i = 0; i < n; i++)
        (void)TagListNext(text, len, &from, &without[i]);   /* judged whole above */
    *out       = without;
    *out_count = n;
    return OK;
}

/* The in_crate's bytes copied in, or NULL with *out_len 0 when there is no
 * in_crate or it is empty. ERR_INVALID_ADDRESS when the copy failed. */
static error_t storage_crate_text(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                                  char **out_text, size_t *out_len)
{
    *out_text = NULL;
    *out_len  = 0;
    if (op->in_crate == CRATE_INDEX_NONE || crates[op->in_crate].size == 0)
        return OK;
    *out_text = crate_in_buf(&crates[op->in_crate], ctx);
    if (!*out_text)
        return ERR_INVALID_ADDRESS;
    *out_len = crates[op->in_crate].size;
    return OK;
}

/* The ids a query answer moves from the index to the caller's crate at a time. */
#define STORAGE_QUERY_PIECE 512u

/*
 * The files `tags` match (every file, for none) — or, with a `word`, those of
 * them that are called it (TagFSNamedPage) — written into the out crate
 * after its 8-byte header a piece at a time, straight from the index's pages:
 * the kernel holds one piece and never the whole answer, and the crate is
 * given as many ids as it has room for, each the u64 the index holds. hdr[0]
 * is how many went in, hdr[1] how many match — counted by the first page, and
 * never less than what went in, since a file made while the pages are taken
 * is delivered too.
 */
static error_t storage_query_into(TagOwner owner, const TagSpelling *tags, uint32_t count,
                                  const TagSpelling *without, uint32_t without_count, bool kept, const char *word,
                                  size_t word_len, const Crate *out, const OpContext *ctx, uint32_t hdr[2])
{
    uint64_t room_ids = (out->capacity - 8) / sizeof(uint64_t);
    uint32_t most     = room_ids > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)room_ids;
    uint64_t *piece   = kmalloc(STORAGE_QUERY_PIECE * sizeof(uint64_t));
    if (!piece) return ERR_NO_MEMORY;

    error_t  rc    = OK;
    uint64_t from  = 1;
    bool     first = true;
    for (;;) {
        uint32_t  room = most - hdr[0] < STORAGE_QUERY_PIECE ? most - hdr[0] : STORAGE_QUERY_PIECE;
        TagFSPage page;
        if (word)
            rc = TagFSNamedPage(owner, word, word_len, tags, count, without, without_count, from, piece, room, first,
                                &page);
        else if (kept)
            rc = TagFSQueryKeptPage(owner, tags, count, without, without_count, from, piece, room, first, &page);
        else
            rc = TagFSQueryPage(owner, tags, count, without, without_count, from, piece, room, first, &page);
        if (rc != OK) break;
        if (first) hdr[1] = page.total;
        first = false;
        if (page.delivered > 0) {
            int wrc = crate_out_commit_at(out, ctx, 8 + (uint64_t)hdr[0] * sizeof(uint64_t),
                                          piece, (uint64_t)page.delivered * sizeof(uint64_t));
            if (wrc != OK) {
                rc = (error_t)wrc;
                break;
            }
            hdr[0] += page.delivered;
        }
        if (page.next == 0 || hdr[0] == most) break;
        from = page.next;
    }
    kfree(piece);
    if (hdr[1] < hdr[0]) hdr[1] = hdr[0];
    return rc;
}

/* STORAGE_TAG_QUERY  params: [u8 scope][u8 whose][tags set aside] — inside
 *                    the Use Context (STORAGE_SCOPE_USE, for the person's
 *                    tags) or STORAGE_SCOPE_EVERYWHERE; in `whose`'s tags
 *                    (STORAGE_TAGS_*); after them, a comma list of tags no
 *                    file of the answer wears ("key:..." any tag of the key). The volume sets aside nothing of its own
 *                    accord but the files it keeps deleted (obj.keep), which
 *                    no scope but STORAGE_SCOPE_DELETED meets — that scope
 *                    asks for them alone, over the whole volume; every tag is
 *                    left out when the asker names it, and only then.
 *                    in_crate (optional): comma-separated tag string
 *                    out_crate: [u32 delivered][u32 total][u64 file_ids[delivered]]
 *                    `total` is how many files match; `delivered` is how many
 *                    of them the crate had room for. A caller whose crate was
 *                    too small is told the true count and can ask again with
 *                    room for it. It used to be told the smaller number as if
 *                    that were the whole answer, and nothing said otherwise.
 *                    Inside the context the user's tags are ANDed with the
 *                    caller's: `use code cpp` then "project" asks for files
 *                    that are code AND cpp AND project. */
int ObjQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint8_t  scope;
    TagOwner owner;
    bool     all    = false;
    size_t   width  = 0;
    error_t  src_rc = storage_scope_of(op, 0, true, &scope);
    if (src_rc == OK)
        src_rc = StorageWhose(op, 1, ctx, false, &owner, &all, &width);
    if (src_rc != OK) return src_rc;

    TagFSState *state = tagfs_get_state();
    if (!state || !state->initialized) return ERR_NOT_INITIALIZED;

    /* The query's tags, read BEFORE the output bounce buffer is allocated so
     * an early return here cannot leak the kbuf. */
    char  *text = NULL;
    size_t tlen = 0;
    error_t rc = storage_crate_text(op, crates, ctx, &text, &tlen);
    if (rc != OK) return rc;

    TagSpelling *without       = NULL;
    uint32_t     without_count = 0;
    rc = storage_without_spelled(op, 1 + width, &without, &without_count);
    if (rc != OK) {
        crate_buf_free(text);
        return rc;
    }

    TagSpelling *tags  = NULL;
    UseList     *use   = NULL;
    uint32_t     own   = 0;
    uint32_t     total = 0;
    rc = storage_tags_spelled(StorageLens(scope, owner), text, tlen, &tags, &use, &own, &total);
    if (rc == OK && StorageSpeaks(scope, owner, total, without_count))
        rc = StorageMaySpeak(ctx, owner, STORAGE_SEE);
    if (rc != OK) {
        kfree(tags);
        UseListDrop(use);
        kfree(without);
        crate_buf_free(text);
        return rc;
    }

    uint32_t hdr[2] = { 0, 0 };   /* delivered, total */
    rc = storage_query_into(owner, tags, total, without, without_count, scope == STORAGE_SCOPE_DELETED, NULL, 0, out,
                            ctx, hdr);
    kfree(tags);
    kfree(without);
    UseListDrop(use);
    crate_buf_free(text);
    if (rc != OK) return rc;

    /* The header last: a crate whose ids did not all go in says nothing. */
    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = sizeof(hdr) + (uint64_t)hdr[0] * sizeof(uint64_t);
    return OK;
}

/* STORAGE_NAME_QUERY  params: [u8 scope][u8 whose][tags set aside] — as
 *                     STORAGE_TAG_QUERY's, but for the one scope it does not
 *                     take, DELETED; a name is no one's, `whose` the owner of
 *                     the tags asked beside it. in_crate: the word, its bytes whole, and
 *                     after a NUL the tags the files must wear too.
 *                     out_crate: as STORAGE_TAG_QUERY's. */
int ObjNameQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint8_t  scope;
    TagOwner owner;
    bool     all   = false;
    size_t   width = 0;
    error_t  rc    = storage_scope_of(op, 0, false, &scope);
    if (rc == OK)
        rc = StorageWhose(op, 1, ctx, false, &owner, &all, &width);
    if (rc != OK) return rc;

    TagFSState *state = tagfs_get_state();
    if (!state || !state->initialized) return ERR_NOT_INITIALIZED;

    char  *text     = NULL;
    size_t text_len = 0;
    rc = storage_crate_text(op, crates, ctx, &text, &text_len);
    if (rc != OK) return rc;

    size_t word_len = 0;
    while (word_len < text_len && text[word_len] != '\0') word_len++;
    if (word_len == 0) {
        crate_buf_free(text);
        return ERR_INVALID_ARGUMENT;
    }
    const bool   tagged   = word_len < text_len;
    const char  *tag_text = tagged ? text + word_len + 1 : NULL;
    const size_t tag_len  = tagged ? text_len - word_len - 1 : 0;

    TagSpelling *without       = NULL;
    uint32_t     without_count = 0;
    rc = storage_without_spelled(op, 1 + width, &without, &without_count);
    if (rc != OK) {
        crate_buf_free(text);
        return rc;
    }

    TagSpelling *tags  = NULL;
    UseList     *use   = NULL;
    uint32_t     own   = 0;
    uint32_t     total = 0;
    rc = storage_tags_spelled(StorageLens(scope, owner), tag_text, tag_len, &tags, &use, &own, &total);
    if (rc == OK && StorageSpeaks(scope, owner, total, without_count))
        rc = StorageMaySpeak(ctx, owner, STORAGE_SEE);
    if (rc != OK) {
        kfree(tags);
        UseListDrop(use);
        kfree(without);
        crate_buf_free(text);
        return rc;
    }

    uint32_t hdr[2] = { 0, 0 };   /* delivered, total */
    rc = storage_query_into(owner, tags, total, without, without_count, false, text, word_len, out, ctx, hdr);
    kfree(tags);
    kfree(without);
    UseListDrop(use);
    crate_buf_free(text);
    if (rc != OK) return rc;

    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;
    out->size = sizeof(hdr) + (uint64_t)hdr[0] * sizeof(uint64_t);
    return OK;
}

/* The bytes of records the kernel holds at a time on the way to the caller's
 * crate: any one record fits, a tag's text being narrower (TAG_TEXT_WIDTH). */
#define STORAGE_TAG_LIST_PIECE 4096u

/* STORAGE_TAG_LIST  params: [u32 from][u8 scope][whose][tags set aside]
 *                   (the comma list optional, as a query's) — out_crate as
 *                   boxos_decks.h says: `whose`'s tags the files wear, inside
 *                   the Use Context counted among its files, the files set
 *                   aside not counted; every owner's the caller may see, of
 *                   the whole volume and none set aside, for STORAGE_TAGS_ALL.
 *                   The records go into the crate a piece at a time, straight
 *                   from the volume's pages, until the crate is full or the
 *                   tags run out; `next` says where the rest begin. */
int ObjTagList(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4 || op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < STORAGE_TAG_LIST_HEAD) return ERR_BUFFER_TOO_SMALL;

    uint8_t   scope;
    TagOwner  owner;
    TagView   view;
    uint64_t *shown = NULL;
    bool      all   = false;
    size_t    width = 0;
    error_t   rc    = storage_scope_of(op, 4, false, &scope);
    if (rc == OK)
        rc = StorageWhose(op, 5, ctx, true, &owner, &all, &width);
    if (rc == OK && all)
        rc = StorageViewAll(ctx, &view, &shown);
    else if (rc == OK)
        rc = StorageMaySpeak(ctx, owner, STORAGE_SEE);
    if (rc != OK) return rc;
    if (!all)
        view = TagViewOf(owner);

    TagFSState *state = tagfs_get_state();
    if (!state || !state->initialized) {
        kfree(shown);
        return ERR_NOT_INITIALIZED;
    }

    TagSpelling *without       = NULL;
    uint32_t     without_count = 0;
    rc = storage_without_spelled(op, 5 + width, &without, &without_count);
    if (rc == OK && all && without_count)
        rc = ERR_INVALID_ARGUMENT;      /* whose tags would they be */
    if (rc != OK) {
        kfree(without);
        kfree(shown);
        return rc;
    }

    TagSpelling *within = NULL;
    UseList     *use    = NULL;
    uint32_t     own    = 0;
    uint32_t     count  = 0;
    rc = storage_tags_spelled(all ? STORAGE_SCOPE_EVERYWHERE : StorageLens(scope, owner), NULL, 0, &within, &use,
                              &own, &count);
    if (rc != OK) {
        kfree(without);
        kfree(shown);
        return rc;
    }

    uint8_t *piece = kmalloc(STORAGE_TAG_LIST_PIECE);
    if (!piece) {
        kfree(within);
        kfree(without);
        kfree(shown);
        UseListDrop(use);
        return ERR_NO_MEMORY;
    }

    uint32_t hdr[2]  = { 0, 0 };   /* delivered, next */
    uint64_t written = STORAGE_TAG_LIST_HEAD;
    uint32_t from    = param_u32(op, 0);
    for (;;) {
        const uint64_t left = out->capacity - written;
        const uint32_t room = left < STORAGE_TAG_LIST_PIECE ? (uint32_t)left : STORAGE_TAG_LIST_PIECE;
        uint32_t used = 0, delivered = 0, next = 0;
        rc = TagFSTagPage(&view, within, count, without, without_count, from, piece, room, &used, &delivered, &next);
        if (rc == ERR_BUFFER_TOO_SMALL && hdr[0] > 0) {
            hdr[1] = next;              /* the crate is full: the rest from here */
            rc     = OK;
            break;
        }
        if (rc != OK) break;
        if (used > 0) {
            int wrc = crate_out_commit_at(out, ctx, written, piece, used);
            if (wrc != OK) {
                rc = (error_t)wrc;
                break;
            }
            written += used;
            hdr[0]  += delivered;
        }
        if (next == 0 || room == left) {
            hdr[1] = next;
            break;
        }
        from = next;
    }
    kfree(piece);
    kfree(within);
    kfree(without);
    kfree(shown);
    UseListDrop(use);
    if (rc != OK) return rc;

    /* The header last: a crate whose records did not all go in says nothing. */
    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = written;
    return OK;
}

/* One tag's text out of the in_crate, spelled. The width is judged on the
 * crate's size before anything is copied: a text longer than any tag can be
 * is refused as the too-long tag it is. *out_text is the caller's to free. */
static error_t storage_one_tag(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                               char **out_text, TagSpelling *out)
{
    *out_text = NULL;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    const Crate *src = &crates[op->in_crate];
    if (src->size == 0)              return ERR_TAGFS_INVALID_TAG;
    if (src->size > TAG_TEXT_WIDTH)  return ERR_TAGFS_TAG_TOO_LONG;

    char  *text = NULL;
    size_t len  = 0;
    error_t rc = storage_crate_text(op, crates, ctx, &text, &len);
    if (rc != OK) return rc;
    rc = TagSpell(text, len, out);
    if (rc != OK) {
        crate_buf_free(text);
        return rc;
    }
    *out_text = text;
    return OK;
}

/* STORAGE_TAG_SET  params:[u64 file_id][whose]  in_crate: "key" or
 * "key:value" — the file wears exactly that tag of `whose`'s, or the call is
 * refused: a program's own tags are put on by that program alone. Any tag goes on any file, whatever its key: a file's tags are
 * labels, and none makes a file one the system keeps. Room on its way — room
 * on the ribbon for the tag's stitch — is waited for (SettleOpForRoom). */
int ObjTagSet(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    if (op->param_size < 9) return ERR_INVALID_ARGUMENT;

    TagOwner owner;
    error_t  rc = StorageTagsOf(op, 8, ctx, STORAGE_PUT, &owner);
    if (rc != OK) return rc;
    char       *text = NULL;
    TagSpelling tag;
    rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    rc = tagfs_add_tag_string(owner, param_u64(op, 0), &tag);
    crate_buf_free(text);
    if (rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return rc;
}

/* STORAGE_TAG_UNSET  params:[u64 file_id][whose]  in_crate: "key" — every
 * tag of the key — or "key:value" — that tag alone, `whose`'s and never
 * another owner's of the same text: another program's taken off by the
 * person's hands alone. The answer is the volume's own: a tag the
 * file does not wear (ERR_TAGFS_TAG_NOT_FOUND), a file that is not there, a
 * medium that left — never folded into one code a program cannot tell its own
 * mistake by. */
int ObjTagUnset(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    if (op->param_size < 9) return ERR_INVALID_ARGUMENT;

    TagOwner owner;
    error_t  rc = StorageTagsOf(op, 8, ctx, STORAGE_TAKE, &owner);
    if (rc != OK) return rc;
    char       *text = NULL;
    TagSpelling tag;
    rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    int removed = tagfs_remove_tag_string(owner, param_u64(op, 0), &tag);
    crate_buf_free(text);
    if (-removed == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return removed == 0 ? OK : -removed;
}

/* The ids of `owner`'s tags a file is created with, each interned; the first
 * refusal ends it. *out_ids is the caller's to kfree. */
static error_t storage_intern_all(TagOwner owner, const TagSpelling *tags, uint32_t count, uint32_t **out_ids)
{
    *out_ids = NULL;
    if (count == 0)
        return OK;
    uint32_t *ids = kmalloc(sizeof(uint32_t) * count);
    if (!ids)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        error_t rc = TagFSInternOwned(owner, &tags[i], &ids[i]);
        if (rc != OK) {
            kfree(ids);
            return rc;
        }
    }
    *out_ids = ids;
    return OK;
}

/* A create a book refuses interns none of its tags (InkBookRefuses): the
 * next stitch would lay them down for a file that never was. The room their
 * Coineds take is held while they are interned, in one stay behind the door —
 * the mount the hold was taken on is the one it is let go on. Room on the
 * ribbon on its way is waited for like any room (SettleOpForRoom). */
static error_t StorageCreateInterned(TagOwner owner, const TagSpelling *tags, uint32_t count, uint32_t **out_ids)
{
    uint64_t held = 0;
    *out_ids = NULL;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = InkBookRefuses(owner, tags, count, &held);
    if (rc == OK)
        rc = storage_intern_all(owner, tags, count, out_ids);
    InkBookHeldLetGo(held);
    tagfs_leave();
    return rc;
}

/* STORAGE_OBJ_CREATE  params:[char filename[32]] (NUL-padded)
 *                            [u8 scope] — inside the Use Context
 *                            (STORAGE_SCOPE_USE) or EVERYWHERE
 *                            [u8 whose] — the tags' owner (STORAGE_TAGS_*)
 *                     in_crate (optional): tag string, `whose`'s tags
 *                     out_crate (optional): u64 file_id
 *                     Inside the context the new file is stamped with the
 *                     user's tags as well as the ones it was given; its name
 *                     is no tag. Every tag is spelled before any is
 *                     interned; a tag that does not spell or intern refuses
 *                     the create: no file is made without a tag it was asked
 *                     to wear, least of all its context's. Room on its way —
 *                     room on the ribbon for the create's stitch — is waited
 *                     for (SettleOpForRoom). */
int ObjCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    if (op->param_size < 34) return ERR_INVALID_ARGUMENT;

    uint8_t  scope;
    TagOwner owner;
    bool     all   = false;
    size_t   width = 0;
    error_t  rc    = storage_scope_of(op, 32, false, &scope);
    if (rc == OK)
        rc = StorageWhose(op, 33, ctx, false, &owner, &all, &width);
    if (rc != OK) return rc;

    /* The name runs to its NUL inside the 32-byte field, or fills it. */
    size_t name_len = strnlen((const char *)op->params, 32);
    if (name_len == 0) return ERR_INVALID_ARGUMENT;

    char *filename = kmalloc(name_len + 1);
    if (!filename) return ERR_NO_MEMORY;
    memcpy(filename, op->params, name_len);
    filename[name_len] = '\0';

    char  *text = NULL;
    size_t tlen = 0;
    rc = storage_crate_text(op, crates, ctx, &text, &tlen);
    if (rc != OK) {
        kfree(filename);
        return rc;
    }

    TagSpelling *tags    = NULL;
    UseList     *use     = NULL;
    uint32_t     own     = 0;
    uint32_t     total   = 0;
    uint32_t    *tag_ids = NULL;
    rc = storage_tags_spelled(StorageLens(scope, owner), text, tlen, &tags, &use, &own, &total);
    if (rc == OK && StorageSpeaks(scope, owner, total, 0))
        rc = StorageMaySpeak(ctx, owner, STORAGE_PUT);
    if (rc == OK)
        rc = StorageCreateInterned(owner, tags, total, &tag_ids);
    kfree(tags);
    UseListDrop(use);
    crate_buf_free(text);
    if (rc != OK) {
        kfree(tag_ids);
        kfree(filename);
        if (rc == ERR_WOULD_BLOCK)
            return SettleOpForRoom(op, crates, crate_count, ctx, 0);
        return rc;
    }

    uint64_t made_id = 0;
    int made = tagfs_create_file(filename, tag_ids, total,
                                 CabinProgramFile(ctx && ctx->proc ? ctx->proc->cabin : NULL), &made_id);
    kfree(filename);
    kfree(tag_ids);
    if (-made == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, 0);
    if (made != 0) return (int)(error_t)(-made);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(made_id))
            (void)crate_write(out, ctx, &made_id, sizeof(made_id));
    }
    return OK;
}
