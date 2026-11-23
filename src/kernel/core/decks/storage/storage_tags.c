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
#include "cabin.h"         /* CabinProgramOwner — who the program is */
#include "settle_ops.h"    /* SettleOpForRoom — room on the ribbon waited for */

/* The volume of the house the caller's program was started from, mounted —
 * the home volume for the kernel's own call — or NULL. */
TagFSVolume *StorageReaderVolume(const OpContext *ctx)
{
    const cabin_t *cabin = ctx && ctx->proc ? ctx->proc->cabin : NULL;
    if (!cabin)
        return TagFSHome();
    TagFSVolume *vol = TagFSVolumeOfUuid(CabinProgramVolume(cabin));
    return vol && TagFSVolumeMounted(vol) ? vol : NULL;
}

/* Who the caller's program is on volume `on`: its passport — the same program
 * on every volume —, or the file it was started from, which names it on its
 * own volume alone; 0 when it is nobody there. */
static uint64_t StorageSelfOn(const OpContext *ctx, TagFSVolume *on)
{
    const cabin_t *cabin = ctx && ctx->proc ? ctx->proc->cabin : NULL;
    uint64_t       owner = CabinProgramOwner(cabin);
    if (!owner || !on)
        return 0;
    return (owner & TAGFS_PASSPORT_BIT) || on == StorageReaderVolume(ctx) ? owner : 0;
}

uint64_t StorageReaderOn(const OpContext *ctx, TagFSVolume *on)
{
    uint64_t owner = StorageSelfOn(ctx, on);
    return owner && TagFSHoldsProgram(on, owner) ? owner : 0;
}

uint64_t StorageReader(const OpContext *ctx)
{
    return StorageReaderOn(ctx, StorageReaderVolume(ctx));
}

uint64_t StorageProgramNamed(uint64_t program, TagFSVolume **where)
{
    uint64_t     own   = 0;
    TagFSVolume *vol   = program ? TagFSVolumeOfId(program, &own) : NULL;
    uint64_t     owner = vol ? TagFSProgramOf(vol, own) : 0;
    *where = owner & TAGFS_PASSPORT_BIT ? NULL : vol;
    return owner ? owner : program;
}

void StorageOwnerOn(TagOwner *owner, TagFSVolume *where, TagFSVolume *on)
{
    if (owner->kind == TAG_OWNER_PROGRAM && where && where != on)
        owner->id = 0;
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
 * its byte, and a program's file id after it — the program it names that
 * file holds (StorageProgramNamed). *all for STORAGE_TAGS_ALL,
 * which a call takes only when `all_ok` — it shows tags, it never asks by
 * them nor changes them. *where the volume a program named by a file is that
 * program on — its own, alone (StorageOwnerOn) —, NULL for a passport and for
 * the person.
 */
static error_t StorageWhose(const ManifestOp *op, size_t at, const OpContext *ctx, bool all_ok, TagOwner *out,
                            bool *all, size_t *width, TagFSVolume **where)
{
    *all   = false;
    *width = 1;
    *where = NULL;
    if (op->param_size <= at)
        return ERR_INVALID_ARGUMENT;
    switch (op->params[at]) {
    case STORAGE_TAGS_MINE: {
        uint64_t self = CabinProgramOwner(ctx && ctx->proc ? ctx->proc->cabin : NULL);
        *out   = TagOwnerProgram(self);
        *where = self & TAGFS_PASSPORT_BIT ? NULL : StorageReaderVolume(ctx);
        return OK;
    }
    case STORAGE_TAGS_PERSON:
        *out = TagOwnerPerson();
        return OK;
    case STORAGE_TAGS_PROGRAM: {
        uint64_t program = op->param_size >= at + 9u ? param_u64(op, at + 1u) : 0;
        if (!program)
            return ERR_INVALID_ARGUMENT;
        *out   = TagOwnerProgram(StorageProgramNamed(program, where));
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
 * Whether the caller may speak `owner`'s tags so on volume `on`. One rule for
 * every program, none let in by its name.
 *
 * Its own: who it is there (StorageSelfOn) — its passport, or the file it was
 * started from on that file's volume. To be seen they ask nothing — a program
 * with none, started from bytes held in memory or from another volume and
 * signed by none, is answered that it has none, which is so. To be put on or
 * taken off they ask that a file of it be on the volume (TagFSHoldsProgram) —
 * one with no file there has no file to name a tag by, and one whose files
 * are gone has no tags left to change (the volume retires them, tagfs/retire):
 * ERR_TAGFS_NO_OWN_TAGS. Another program's: seen by one it shows them to on
 * that volume (TagFSShows) and by the person's eyes, taken off a file by the
 * person's hands, put on by nobody but that program. The person's: seen by
 * the person's eyes, changed by the person's hands (StoragePersonSays):
 * ERR_ACCESS_DENIED otherwise. The kernel's own call — no cabin — is the
 * person's eyes and hands, and has no tags of its own.
 */
error_t StorageMaySpeak(const OpContext *ctx, TagFSVolume *on, TagOwner owner, StorageSpeech speech)
{
    if (owner.kind == TAG_OWNER_PERSON)
        return StoragePersonSays(ctx, speech != STORAGE_SEE) ? OK : ERR_ACCESS_DENIED;
    uint64_t self = StorageSelfOn(ctx, on);
    if (owner.id == 0 || owner.id == self)
        return speech == STORAGE_SEE || (owner.id && TagFSHoldsProgram(on, owner.id)) ? OK : ERR_TAGFS_NO_OWN_TAGS;
    switch (speech) {
    case STORAGE_SEE: {
        uint64_t reader = StorageReaderOn(ctx, on);
        return StoragePersonSays(ctx, false) || (reader && TagFSShows(on, owner.id, reader)) ? OK : ERR_ACCESS_DENIED;
    }
    case STORAGE_TAKE:
        return StoragePersonSays(ctx, true) ? OK : ERR_ACCESS_DENIED;
    default:
        return ERR_ACCESS_DENIED;
    }
}

/* Whose tags a call on volume `on` speaks, judged at once: the calls that
 * always speak one owner's. */
static error_t StorageTagsOf(const ManifestOp *op, size_t at, const OpContext *ctx, StorageSpeech speech,
                             TagFSVolume *on, TagOwner *out)
{
    bool         all = false;
    size_t       width;
    TagFSVolume *where;
    error_t      rc = StorageWhose(op, at, ctx, false, out, &all, &width, &where);
    if (rc != OK)
        return rc;
    StorageOwnerOn(out, where, on);
    return StorageMaySpeak(ctx, on, *out, speech);
}

/* The programs that show their tags to the caller's grow a page at a time. */
#define STORAGE_SHOWN_PIECE 32u

/*
 * The view of every owner the caller may see on volume `on`
 * (STORAGE_TAGS_ALL): its own, those of the programs that show it theirs
 * there, and — the person's eyes — every one. *list holds the programs shown
 * it, rising, the caller's to kfree.
 */
error_t StorageViewAll(const OpContext *ctx, TagFSVolume *on, TagView *view, uint64_t **list)
{
    *list = NULL;
    *view = (TagView){ .all = true, .everyone = StoragePersonSays(ctx, false), .owner = TagOwnerPerson(),
                       .reader = StorageReaderOn(ctx, on) };
    if (view->everyone || !view->reader)
        return OK;
    uint64_t  piece[STORAGE_SHOWN_PIECE];
    uint64_t *ids = NULL, from = 1;
    uint32_t  n = 0, room = 0;
    for (;;) {
        TagFSPage page;
        error_t   rc = TagFSShownToPage(on, view->reader, from, piece, STORAGE_SHOWN_PIECE, false, &page);
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

/* A program as a list names it: by the house's id of a file of it on `vol`
 * (TagOwnerWord), a passport kept no file of it there named as it is; 0 the
 * person. */
static uint64_t StorageOwnerWord(TagFSVolume *vol, TagOwner owner)
{
    uint64_t word = TagOwnerWord(vol, owner);
    return word & TAGFS_PASSPORT_BIT ? word : TagFSHouseId(vol, word);
}

/* One tag's record: [u16 key_len][u16 val_len][u8 type][u64 owner][key][val]. */
static uint64_t info_tag_record(TagFSVolume *vol, uint8_t *kp, uint64_t pos, uint32_t tag_id)
{
    TagSpelling tag       = { 0 };
    TagOwner    owner     = TagOwnerPerson();
    bool        is_system = false;
    /* A tag the volume no longer knows still takes its place in the list:
     * the count in the header says how many records follow, so skipping one
     * would leave the reader parsing the next tag's bytes as this one's. */
    (void)TagFSTagSpelling(vol, tag_id, &tag, &is_system, &owner);
    uint16_t kl    = (uint16_t)tag.key_len;
    uint16_t vl    = (uint16_t)tag.value_len;
    uint64_t whose = StorageOwnerWord(vol, owner);
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
static bool InfoTagOf(TagFSVolume *vol, uint32_t tag_id, const TagView *view)
{
    TagSpelling tag;
    TagOwner    owner;
    return TagFSTagSpelling(vol, tag_id, &tag, NULL, &owner) && TagViewLets(view, owner);
}

int ObjGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 9)                return ERR_INVALID_ARGUMENT;

    uint64_t     file_id = param_u64(op, 0);
    uint64_t     own     = 0;
    TagFSVolume *vol     = TagFSVolumeOfId(file_id, &own);
    if (!vol) return ERR_FILE_NOT_FOUND;
    error_t ready = TagFSVolumeReady(vol);
    if (ready != OK) return ready;

    TagOwner     owner;
    TagView      view;
    uint64_t    *shown = NULL;
    bool         all   = false;
    size_t       width = 0;
    TagFSVolume *where = NULL;
    error_t      whose = StorageWhose(op, 8, ctx, true, &owner, &all, &width, &where);
    StorageOwnerOn(&owner, where, vol);
    if (whose == OK && all)
        whose = StorageViewAll(ctx, vol, &view, &shown);
    else if (whose == OK)
        whose = StorageMaySpeak(ctx, vol, owner, STORAGE_SEE);
    if (whose != OK) return whose;
    if (!all)
        view = TagViewOf(owner);
    Crate *out = &crates[op->out_crate];

    TagFSMetadata md;
    if (tagfs_get_metadata(vol, own, &md) != 0 || !(md.flags & TAGFS_FILE_ACTIVE)) {
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
    if (!tagfs_enter(vol)) {
        tagfs_metadata_free(&md);
        kfree(shown);
        return ERR_FILE_NOT_FOUND;
    }

    uint32_t tag_count = 0;
    uint64_t need = 8u + 8u + 4u + 2u + filename_len + 8u + 8u + 2u + md.head_len;
    for (uint32_t i = 0; i < md.tag_count; i++) {
        if (!InfoTagOf(vol, md.tag_ids[i], &view))
            continue;
        need = info_tag_record(vol, NULL, need, md.tag_ids[i]);
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
            if (InfoTagOf(vol, md.tag_ids[i], &view))
                pos = info_tag_record(vol, kp, pos, md.tag_ids[i]);
        /* The last eight bytes: when a deleted file is let go, 0 for one that is not. */
        const uint64_t kept_until = (md.flags & TAGFS_FILE_DELETED) ? md.keep_until : 0;
        memcpy(kp + pos, &kept_until, 8); pos += 8;
        /* The program that made the file, 0 when none did: a file of it on
         * the file's volume, as the house names it. */
        const uint64_t made_by = TagFSHouseId(vol, md.made_by);
        memcpy(kp + pos, &made_by, 8); pos += 8;
        /* What the system keeps the file as, and its first bytes: what a
         * file is, told by the volume and by the file itself. */
        kp[pos++] = (uint8_t)((md.flags & TAGFS_FILE_ROLE_MASK) >> 2);
        kp[pos++] = md.head_len;
        if (md.head_len) { memcpy(kp + pos, md.head, md.head_len); pos += md.head_len; }
    }
    tagfs_leave(vol);
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
 * Use Context unless the caller said everywhere — and, for a question that
 * takes it (`one_volume_too`), the bit that asks it of one volume taken off
 * (storage_question_volume reads that volume).
 */
static error_t storage_scope_of(const ManifestOp *op, size_t at, bool deleted_too, bool one_volume_too,
                                uint8_t *out_scope)
{
    if (op->param_size <= at) return ERR_INVALID_ARGUMENT;
    uint8_t scope = op->params[at] & (uint8_t)~(one_volume_too ? STORAGE_SCOPE_ONE_VOLUME : 0u);
    if (scope != STORAGE_SCOPE_USE && scope != STORAGE_SCOPE_EVERYWHERE &&
        !(deleted_too && scope == STORAGE_SCOPE_DELETED))
        return ERR_INVALID_ARGUMENT;
    *out_scope = scope;
    return OK;
}

/* The volume a question is asked of (STORAGE_SCOPE_ONE_VOLUME): its number at
 * params[at], past the whose's bytes, *width grown by it; NULL — every
 * volume of the house — when the scope at params[0] does not carry the bit. */
static error_t storage_question_volume(const ManifestOp *op, size_t at, size_t *width, TagFSVolume **only)
{
    *only = NULL;
    if (!(op->params[0] & STORAGE_SCOPE_ONE_VOLUME))
        return OK;
    if (op->param_size < at + 4u)
        return ERR_INVALID_ARGUMENT;
    *only   = TagFSVolumeNumbered(param_u32(op, at));
    *width += 4u;
    return *only ? OK : ERR_OBJECT_NOT_FOUND;
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

/* One volume's share of a list (StorageAnswerPages): its pages from its own
 * id `from` on while the crate has room — the first asked whatever the room,
 * for how many there are — each id written as the house's. */
static error_t StorageVolumePages(StoragePager pager, const void *ask, TagFSVolume *vol, uint64_t from,
                                  const Crate *out, const OpContext *ctx, uint64_t *piece, uint32_t piece_room,
                                  uint32_t most, uint32_t hdr[2])
{
    error_t rc = OK;
    for (bool first = true; rc == OK; first = false) {
        uint32_t  room = most - hdr[0] < piece_room ? most - hdr[0] : piece_room;
        TagFSPage page;
        rc = pager(vol, ask, from, piece, room, first, &page);
        if (rc != OK)
            break;
        if (first)
            hdr[1] += page.total;
        for (uint32_t i = 0; i < page.delivered; i++)
            piece[i] = TagFSHouseId(vol, piece[i]);
        if (page.delivered)
            rc = (error_t)crate_out_commit_at(out, ctx, 8 + (uint64_t)hdr[0] * sizeof(uint64_t), piece,
                                              (uint64_t)page.delivered * sizeof(uint64_t));
        hdr[0] += page.delivered;
        if (!page.next || hdr[0] == most)
            break;
        from = page.next;
    }
    return rc;
}

error_t StorageAnswerPages(StoragePager pager, const void *ask, TagFSVolume *only, uint64_t from, const Crate *out,
                           const OpContext *ctx, uint64_t *piece, uint32_t piece_room, uint32_t hdr[2])
{
    uint64_t room_ids = (out->capacity - 8) / sizeof(uint64_t);
    uint32_t most     = room_ids > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)room_ids;
    uint64_t own      = from & TAGFS_VOLUME_FILES_MOST;
    uint32_t start    = (uint32_t)(from >> TAGFS_VOLUME_FILE_BITS);
    error_t  rc       = only && TagFSVolumeReady(only) == ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY : OK;
    for (TagFSVolume *vol = only ? only : TagFSVolumeNext(NULL); vol && rc == OK;
         vol = only ? NULL : TagFSVolumeNext(vol)) {
        uint32_t number = TagFSVolumeNumber(vol);
        if (number < start || TagFSVolumeReady(vol) != OK)
            continue;
        rc = StorageVolumePages(pager, ask, vol, number == start && own ? own : 1, out, ctx, piece, piece_room,
                                most, hdr);
    }
    if (hdr[1] < hdr[0])
        hdr[1] = hdr[0];
    return rc;
}

/* Whether any volume of the house is mounted: a question of files has
 * somewhere to be asked. */
bool StorageHouseUp(void)
{
    for (TagFSVolume *vol = TagFSVolumeNext(NULL); vol; vol = TagFSVolumeNext(vol))
        if (TagFSVolumeMounted(vol))
            return true;
    return false;
}

/* The ids a query answer moves from the index to the caller's crate at a time. */
#define STORAGE_QUERY_PIECE 512u

/* A question of storage.query or storage.name.query, as one page of it is
 * asked of the index (StorageQueryPager). */
typedef struct {
    TagOwner           owner;
    TagFSVolume       *where;   /* the volume a program named by a file is that program on (StorageOwnerOn) */
    const TagSpelling *tags;
    uint32_t           count;
    const TagSpelling *without;
    uint32_t           without_count;
    bool               kept;
    const char        *word;
    size_t             word_len;
} StorageQueryAsk;

static error_t StorageQueryPager(TagFSVolume *vol, const void *ask, uint64_t from, uint64_t *out, uint32_t room,
                                 bool want_total, TagFSPage *page)
{
    const StorageQueryAsk *q = ask;
    if (q->owner.kind == TAG_OWNER_PROGRAM && q->where && q->where != vol) {
        *page = (TagFSPage){ 0 };    /* that program is nobody on this volume: none of its tags is here */
        return OK;
    }
    if (q->word)
        return TagFSNamedPage(vol, q->owner, q->word, q->word_len, q->tags, q->count, q->without, q->without_count, from,
                              out, room, want_total, page);
    if (q->kept)
        return TagFSQueryKeptPage(vol, q->owner, q->tags, q->count, q->without, q->without_count, from, out, room,
                                  want_total, page);
    return TagFSQueryPage(vol, q->owner, q->tags, q->count, q->without, q->without_count, from, out, room, want_total,
                          page);
}

/*
 * The files `tags` match (every file, for none) — or, with a `word`, those of
 * them that are called it (TagFSNamedPage) — written into the out crate
 * after its 8-byte header a piece at a time, straight from the index's pages
 * (StorageAnswerPages): the kernel holds one piece and never the whole
 * answer, and the crate is given as many ids as it has room for, each the u64
 * the index holds.
 */
static error_t storage_query_into(TagOwner owner, TagFSVolume *where, TagFSVolume *only, const TagSpelling *tags,
                                  uint32_t count, const TagSpelling *without, uint32_t without_count, bool kept,
                                  const char *word, size_t word_len, const Crate *out, const OpContext *ctx,
                                  uint32_t hdr[2])
{
    const StorageQueryAsk ask   = { owner, where, tags, count, without, without_count, kept, word, word_len };
    uint64_t             *piece = kmalloc(STORAGE_QUERY_PIECE * sizeof(uint64_t));
    if (!piece) return ERR_NO_MEMORY;
    error_t rc = StorageAnswerPages(StorageQueryPager, &ask, only, 1, out, ctx, piece, STORAGE_QUERY_PIECE, hdr);
    kfree(piece);
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

    uint8_t      scope;
    TagOwner     owner;
    bool         all    = false;
    size_t       width  = 0;
    TagFSVolume *where  = NULL;
    TagFSVolume *only   = NULL;
    error_t      src_rc = storage_scope_of(op, 0, true, true, &scope);
    if (src_rc == OK)
        src_rc = StorageWhose(op, 1, ctx, false, &owner, &all, &width, &where);
    if (src_rc == OK)
        src_rc = storage_question_volume(op, 1 + width, &width, &only);
    if (src_rc != OK) return src_rc;
    if (!StorageHouseUp()) return ERR_NOT_INITIALIZED;

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
        rc = StorageMaySpeak(ctx, where ? where : StorageReaderVolume(ctx), owner, STORAGE_SEE);
    if (rc != OK) {
        kfree(tags);
        UseListDrop(use);
        kfree(without);
        crate_buf_free(text);
        return rc;
    }

    uint32_t hdr[2] = { 0, 0 };   /* delivered, total */
    rc = storage_query_into(owner, where, only, tags, total, without, without_count, scope == STORAGE_SCOPE_DELETED,
                            NULL, 0, out, ctx, hdr);
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
 *                     out_crate: as STORAGE_TAG_QUERY's. A word no volume
 *                     that answers has a file called, and a volume whose
 *                     medium left has, is ERR_DEVICE_NOT_READY: it is
 *                     somebody's — on a medium out of the machine. */
int ObjNameQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint8_t      scope;
    TagOwner     owner;
    bool         all   = false;
    size_t       width = 0;
    TagFSVolume *where = NULL;
    TagFSVolume *only  = NULL;
    error_t      rc    = storage_scope_of(op, 0, false, true, &scope);
    if (rc == OK)
        rc = StorageWhose(op, 1, ctx, false, &owner, &all, &width, &where);
    if (rc == OK)
        rc = storage_question_volume(op, 1 + width, &width, &only);
    if (rc != OK) return rc;
    if (!StorageHouseUp()) return ERR_NOT_INITIALIZED;

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
        rc = StorageMaySpeak(ctx, where ? where : StorageReaderVolume(ctx), owner, STORAGE_SEE);
    if (rc != OK) {
        kfree(tags);
        UseListDrop(use);
        kfree(without);
        crate_buf_free(text);
        return rc;
    }

    uint32_t hdr[2] = { 0, 0 };   /* delivered, total */
    rc = storage_query_into(owner, where, only, tags, total, without, without_count, false, text, word_len, out, ctx,
                            hdr);
    if (rc == OK && hdr[1] == 0 && !only && TagFSAwayCalled(text, word_len))
        rc = ERR_DEVICE_NOT_READY;
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

/* STORAGE_TAG_LIST  params: [u64 from][u8 scope][whose][tags set aside]
 *                   (the comma list optional, as a query's) — out_crate as
 *                   boxos_decks.h says: `whose`'s tags the files wear, inside
 *                   the Use Context counted among its files, the files set
 *                   aside not counted; every owner's the caller may see, of
 *                   every volume and none set aside, for STORAGE_TAGS_ALL.
 *                   The records go into the crate a piece at a time, straight
 *                   from each volume's pages in the house's order, until the
 *                   crate is full or the tags run out; `next` says where the
 *                   rest begin. */
/* The owners of the records of a tag.list page laid in `piece` (`used`
 * bytes): a program's, a file of it on `vol`, named as the house names it. */
static void StorageListedOwners(TagFSVolume *vol, uint8_t *piece, uint32_t used)
{
    for (uint32_t at = 0; at + STORAGE_TAG_LISTED_HEAD <= used;) {
        uint64_t owner;
        uint16_t value_len;
        memcpy(&owner, piece + at + 8, 8);
        memcpy(&value_len, piece + at + 6, 2);
        if (owner && !(owner & TAGFS_PASSPORT_BIT)) {
            owner = TagFSHouseId(vol, owner);
            memcpy(piece + at + 8, &owner, 8);
        }
        at += STORAGE_TAG_LISTED_HEAD + piece[at + 5] + (uint32_t)value_len;
    }
}

int ObjTagList(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 8 || op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < STORAGE_TAG_LIST_HEAD) return ERR_BUFFER_TOO_SMALL;

    uint8_t      scope;
    TagOwner     owner;
    bool         all   = false;
    size_t       width = 0;
    TagFSVolume *where = NULL;
    error_t      rc    = storage_scope_of(op, 8, false, false, &scope);
    if (rc == OK)
        rc = StorageWhose(op, 9, ctx, true, &owner, &all, &width, &where);
    if (rc == OK && !all)
        rc = StorageMaySpeak(ctx, where ? where : StorageReaderVolume(ctx), owner, STORAGE_SEE);
    if (rc != OK) return rc;
    if (!StorageHouseUp()) return ERR_NOT_INITIALIZED;

    TagSpelling *without       = NULL;
    uint32_t     without_count = 0;
    rc = storage_without_spelled(op, 9 + width, &without, &without_count);
    if (rc == OK && all && without_count)
        rc = ERR_INVALID_ARGUMENT;      /* whose tags would they be */
    if (rc != OK) {
        kfree(without);
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
        return rc;
    }

    uint8_t *piece = kmalloc(STORAGE_TAG_LIST_PIECE);
    if (!piece) {
        kfree(within);
        kfree(without);
        UseListDrop(use);
        return ERR_NO_MEMORY;
    }

    /* Every volume of the house in the order of its number, each's tags in the
     * order of its numbers for them: `from` and `next` carry the volume's
     * number above its number for the tag. */
    uint32_t delivered_all = 0;
    uint64_t next_all      = 0;
    uint64_t written       = STORAGE_TAG_LIST_HEAD;
    uint64_t from          = param_u64(op, 0);
    uint32_t start         = (uint32_t)(from >> 32);
    bool     full          = false;
    for (TagFSVolume *vol = TagFSVolumeNext(NULL); vol && rc == OK && !full; vol = TagFSVolumeNext(vol)) {
        uint32_t number = TagFSVolumeNumber(vol);
        if (number < start || TagFSVolumeReady(vol) != OK)
            continue;
        TagView   view;
        uint64_t *shown    = NULL;
        uint32_t  tag_from = number == start ? (uint32_t)from : 0;
        if (all) {
            rc = StorageViewAll(ctx, vol, &view, &shown);
        } else {
            TagOwner here = owner;
            StorageOwnerOn(&here, where, vol);
            if (here.kind == TAG_OWNER_PROGRAM && owner.id && !here.id)
                continue;               /* that program is nobody on this volume */
            view = TagViewOf(here);
        }
        while (rc == OK) {
            const uint64_t left = out->capacity - written;
            const uint32_t room = left < STORAGE_TAG_LIST_PIECE ? (uint32_t)left : STORAGE_TAG_LIST_PIECE;
            uint32_t used = 0, delivered = 0, next = 0;
            rc = TagFSTagPage(vol, &view, within, count, without, without_count, tag_from, piece, room, &used,
                              &delivered, &next);
            if (rc == ERR_BUFFER_TOO_SMALL && delivered_all > 0) {
                next_all = ((uint64_t)number << 32) | tag_from;   /* the crate is full: the rest from here */
                rc       = OK;
                full     = true;
                break;
            }
            if (rc != OK) break;
            if (used > 0) {
                StorageListedOwners(vol, piece, used);
                int wrc = crate_out_commit_at(out, ctx, written, piece, used);
                if (wrc != OK) {
                    rc = (error_t)wrc;
                    break;
                }
                written       += used;
                delivered_all += delivered;
            }
            if (next == 0)
                break;
            if (room == left) {
                next_all = ((uint64_t)number << 32) | next;
                full     = true;
                break;
            }
            tag_from = next;
        }
        kfree(shown);
    }
    kfree(piece);
    kfree(within);
    kfree(without);
    UseListDrop(use);
    if (rc != OK) return rc;

    /* The header last: a crate whose records did not all go in says nothing. */
    uint8_t head[STORAGE_TAG_LIST_HEAD] = { 0 };
    memcpy(head, &delivered_all, 4);
    memcpy(head + 8, &next_all, 8);
    int crc = crate_out_commit_at(out, ctx, 0, head, sizeof(head));
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

    uint64_t     own = 0;
    TagFSVolume *vol = TagFSVolumeOfId(param_u64(op, 0), &own);
    if (!vol) return ERR_FILE_NOT_FOUND;
    TagOwner owner;
    error_t  rc = StorageTagsOf(op, 8, ctx, STORAGE_PUT, vol, &owner);
    if (rc != OK) return rc;
    char       *text = NULL;
    TagSpelling tag;
    rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    rc = tagfs_add_tag_string(vol, owner, own, &tag);
    crate_buf_free(text);
    if (rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, vol, param_u64(op, 0));
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

    uint64_t     own = 0;
    TagFSVolume *vol = TagFSVolumeOfId(param_u64(op, 0), &own);
    if (!vol) return ERR_FILE_NOT_FOUND;
    TagOwner owner;
    error_t  rc = StorageTagsOf(op, 8, ctx, STORAGE_TAKE, vol, &owner);
    if (rc != OK) return rc;
    char       *text = NULL;
    TagSpelling tag;
    rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    int removed = tagfs_remove_tag_string(vol, owner, own, &tag);
    crate_buf_free(text);
    if (-removed == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, vol, param_u64(op, 0));
    return removed == 0 ? OK : -removed;
}

/* The ids of `owner`'s tags a file is created with, each interned; the first
 * refusal ends it. *out_ids is the caller's to kfree. */
static error_t storage_intern_all(TagFSVolume *vol, TagOwner owner, const TagSpelling *tags, uint32_t count,
                                  uint32_t **out_ids)
{
    *out_ids = NULL;
    if (count == 0)
        return OK;
    uint32_t *ids = kmalloc(sizeof(uint32_t) * count);
    if (!ids)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        error_t rc = TagFSInternOwned(vol, owner, &tags[i], &ids[i]);
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
static error_t StorageCreateInterned(TagFSVolume *vol, TagOwner owner, const TagSpelling *tags, uint32_t count,
                                     uint32_t **out_ids)
{
    uint64_t held = 0;
    *out_ids = NULL;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = InkBookRefuses(vol, owner, tags, count, &held);
    if (rc == OK)
        rc = storage_intern_all(vol, owner, tags, count, out_ids);
    InkBookHeldLetGo(vol, held);
    tagfs_leave(vol);
    return rc;
}

/* The volume a create names after its `whose` (params[at] on, a u32): the
 * home volume when it names none. ERR_INVALID_ARGUMENT for a number the house
 * gave no volume, ERR_DEVICE_NOT_READY for one not mounted — its medium away. */
static error_t StorageCreateVolume(const ManifestOp *op, size_t at, TagFSVolume **out)
{
    uint32_t number = op->param_size >= at + 4u ? param_u32(op, at) : 0;
    *out = TagFSVolumeNumbered(number);
    if (!*out)
        return ERR_INVALID_ARGUMENT;
    return TagFSVolumeMounted(*out) ? OK : ERR_DEVICE_NOT_READY;
}

/* STORAGE_OBJ_CREATE  params:[char filename[32]] (NUL-padded)
 *                            [u8 scope] — inside the Use Context
 *                            (STORAGE_SCOPE_USE) or EVERYWHERE
 *                            [u8 whose] — the tags' owner (STORAGE_TAGS_*)
 *                            [u32 volume] (optional) — the volume of the
 *                            house it is made on, the home volume (0) when
 *                            none is named
 *                     in_crate (optional): tag string, `whose`'s tags
 *                     out_crate (optional): u64 file_id, as the house names it
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

    uint8_t      scope;
    TagOwner     owner;
    bool         all   = false;
    size_t       width = 0;
    TagFSVolume *where = NULL;
    TagFSVolume *vol   = NULL;
    error_t      rc    = storage_scope_of(op, 32, false, false, &scope);
    if (rc == OK)
        rc = StorageWhose(op, 33, ctx, false, &owner, &all, &width, &where);
    if (rc == OK)
        rc = StorageCreateVolume(op, 33 + width, &vol);
    if (rc != OK) return rc;
    StorageOwnerOn(&owner, where, vol);

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
        rc = StorageMaySpeak(ctx, vol, owner, STORAGE_PUT);
    if (rc == OK)
        rc = StorageCreateInterned(vol, owner, tags, total, &tag_ids);
    kfree(tags);
    UseListDrop(use);
    crate_buf_free(text);
    if (rc != OK) {
        kfree(tag_ids);
        kfree(filename);
        if (rc == ERR_WOULD_BLOCK)
            return SettleOpForRoom(op, crates, crate_count, ctx, vol, 0);
        return rc;
    }

    /* The program that makes it, by its file — on its own volume alone: a
     * file of another volume is no program of this one. */
    uint64_t made_by = CabinProgramFile(ctx && ctx->proc ? ctx->proc->cabin : NULL);
    if (vol != StorageReaderVolume(ctx))
        made_by = 0;
    uint64_t made_id = 0;
    int made = tagfs_create_file(vol, filename, tag_ids, total, made_by, &made_id);
    kfree(filename);
    kfree(tag_ids);
    if (-made == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, vol, 0);
    if (made != 0) return (int)(error_t)(-made);
    made_id = TagFSHouseId(vol, made_id);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(made_id))
            (void)crate_write(out, ctx, &made_id, sizeof(made_id));
    }
    return OK;
}
