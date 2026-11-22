/*
 * Storage Deck — a program's own tags shown to other programs: storage.tag.show
 * and storage.tag.shown (boxos_decks.h, where both layouts are said once).
 *
 * The caller is its program — who it is, while a file of it is on the volume
 * (StorageReader) — and a program changes, asks for and is asked by nobody
 * but as itself: a program with no file of it there has no tags of its own to
 * show nor a name to be shown by, ERR_TAGFS_NO_OWN_TAGS. Programs name the
 * others by a file of them (StorageProgramNamed), and every list names them
 * by the one each is named by. Whom a program shows its tags to is read by
 * itself and by the person's eyes (StoragePersonSays) alone.
 */

#include "klib.h"
#include "crate_io.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "storage_deck.h"
#include "storage_tags.h"
#include "tagfs.h"
#include "process.h"
#include "touch.h"

/* The word a program asked to show its tags hears on (boxos_decks.h,
 * STORAGE_ASK_TO_SEE): its payload a file of the asked program, nothing of
 * who asked — every program listening hears it, and the one asked knows
 * itself by the file it was started from. */
static const char s_tags_asked[] = "tags-asked";

/* A note was left for `owner`: said on the word once for every file of it,
 * whichever of them a running copy of it was started from — none said when
 * the heap gives no room to list them, the note standing all the same. */
static void StorageAskSaid(uint64_t owner, const OpContext *ctx)
{
    TouchTag full, bare;
    if (TouchTagResolve(s_tags_asked, sizeof(s_tags_asked) - 1u, &full, &bare) != OK)
        return;
    uint32_t  want  = TagFSProgramFiles(owner, NULL, 0);
    uint64_t *files = want ? kmalloc(sizeof(uint64_t) * want) : NULL;
    uint32_t  n     = files ? TagFSProgramFiles(owner, files, want) : 0;
    for (uint32_t i = 0; i < n && i < want; i++)
        TouchPublishPair(full, bare, &files[i], sizeof(files[i]), ctx && ctx->proc ? ctx->proc->pid : 0,
                         TOUCH_FLAG_TAGFS);
    kfree(files);
}

/* STORAGE_TAG_SHOW  params: [u8 act][u64 program] — the caller's program
 *                   shows its tags to `program` (0: every program), hides
 *                   them from it, or asks it to show it its own. */
int ObjTagShow(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crates;
    (void)crate_count;
    if (op->param_size < 9) return ERR_INVALID_ARGUMENT;
    uint8_t  act     = op->params[0];
    uint64_t program = StorageProgramNamed(param_u64(op, 1));
    uint64_t self    = StorageReader(ctx);
    if (act != STORAGE_SHOW_TO && act != STORAGE_HIDE_FROM && act != STORAGE_ASK_TO_SEE)
        return ERR_INVALID_ARGUMENT;
    if (!self)
        return ERR_TAGFS_NO_OWN_TAGS;
    if (act != STORAGE_ASK_TO_SEE)
        return TagFSShowTags(self, program, act == STORAGE_SHOW_TO);
    bool    noted = false;
    error_t rc    = TagFSAskToSee(self, program, &noted);
    if (rc == OK && noted)
        StorageAskSaid(program, ctx);
    return rc;
}

/* The ids a list answer moves to the caller's crate at a time. */
#define STORAGE_SHOWN_ANSWER_PIECE 64u

/* STORAGE_TAG_SHOWN  params: [u8 which][u64 program][u64 from] — out_crate
 *                    [u32 delivered][u32 total][u64 ids[delivered]]. */
int ObjTagShown(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 17 || op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint8_t  which   = op->params[0];
    uint64_t program = StorageProgramNamed(param_u64(op, 1));
    uint64_t from    = op->param_size >= 25 ? param_u64(op, 9) : 0;
    uint64_t self    = StorageReader(ctx);
    if (which != STORAGE_SHOWN_TO_ME && which != STORAGE_SHOWN_BY && which != STORAGE_ASKS_TO_SEE)
        return ERR_INVALID_ARGUMENT;
    if (which == STORAGE_SHOWN_BY && program != self && !StoragePersonSays(ctx, false))
        return ERR_ACCESS_DENIED;
    if (which != STORAGE_SHOWN_BY && !self)
        return ERR_TAGFS_NO_OWN_TAGS;

    uint64_t room_ids = (out->capacity - 8) / sizeof(uint64_t);
    uint32_t most     = room_ids > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)room_ids;
    uint64_t piece[STORAGE_SHOWN_ANSWER_PIECE];
    uint32_t hdr[2]   = { 0, 0 };   /* delivered, total */
    error_t  rc       = OK;
    if (which == STORAGE_SHOWN_BY) {
        /* A program's own list, whole: from `from` on, as the others page. */
        uint64_t *all   = NULL;
        uint32_t  count = 0;
        rc = TagFSShownBy(program, &all, &count);
        uint32_t at = 0;
        while (rc == OK && at < count && all[at] < from)
            at++;
        hdr[1] = count - at;
        hdr[0] = hdr[1] < most ? hdr[1] : most;
        if (rc == OK && hdr[0])
            rc = (error_t)crate_out_commit_at(out, ctx, 8, all + at, (uint64_t)hdr[0] * sizeof(uint64_t));
        kfree(all);
    } else {
        for (bool first = true; rc == OK && hdr[0] < most; first = false) {
            uint32_t  room = most - hdr[0] < STORAGE_SHOWN_ANSWER_PIECE ? most - hdr[0] : STORAGE_SHOWN_ANSWER_PIECE;
            TagFSPage page;
            rc = which == STORAGE_SHOWN_TO_ME ? TagFSShownToPage(self, from, piece, room, first, &page)
                                              : TagFSAsksPage(self, from, piece, room, first, &page);
            if (rc != OK)
                break;
            if (first)
                hdr[1] = page.total;
            if (page.delivered)
                rc = (error_t)crate_out_commit_at(out, ctx, 8 + (uint64_t)hdr[0] * sizeof(uint64_t), piece,
                                                  (uint64_t)page.delivered * sizeof(uint64_t));
            hdr[0] += page.delivered;
            if (!page.next)
                break;
            from = page.next;
        }
        if (hdr[1] < hdr[0])
            hdr[1] = hdr[0];
    }
    if (rc != OK) return rc;

    /* The header last: a crate whose ids did not all go in says nothing. */
    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;
    out->size = sizeof(hdr) + (uint64_t)hdr[0] * sizeof(uint64_t);
    return OK;
}
