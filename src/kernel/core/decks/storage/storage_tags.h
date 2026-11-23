#ifndef STORAGE_TAGS_H
#define STORAGE_TAGS_H

#include "ktypes.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"
#include "tagfs.h"

/* The storage deck's calls that speak tags (storage_tags.c), each in the
 * owner's tags its whose names — the program's own, the person's, another
 * program's, or every one its caller may see (boxos_decks.h, STORAGE_TAGS_*)
 * — and the ones that show a program's own to others (storage_show.c),
 * registered by StorageDeckRegister. */
int ObjQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjNameQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagList(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagSet(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagUnset(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagShow(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagShown(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);

/* One page of a list a deck answer gives: up to `room` ids from `from` on
 * into `out`, and how many there are when `want_total` (TagFSPage) — of the
 * list `ask` names. */
typedef error_t (*StoragePager)(const void *ask, uint64_t from, uint64_t *out, uint32_t room, bool want_total,
                                TagFSPage *page);

/* The list `pager` pages, from `from` on, written into `out` after its 8-byte
 * header, `piece_room` ids at a time through `piece`: hdr[0] how many went in,
 * hdr[1] how many there are — counted by the first page, which is asked
 * whatever the room, so a caller with room for none is told how many; never
 * less than what went in, a member come while the pages are taken delivered
 * too. The one way every list of the deck is answered. OK, or the pager's or
 * the crate's refusal. */
error_t StorageAnswerPages(StoragePager pager, const void *ask, uint64_t from, const Crate *out,
                           const OpContext *ctx, uint64_t *piece, uint32_t piece_room, uint32_t hdr[2]);

/* The caller's program on the volume mounted — who it is (TagOwner), while
 * it is on the volume (TagFSHoldsProgram) — or 0 for none. */
uint64_t StorageReader(const OpContext *ctx);

/* The program a program names by a file of it — the one that file holds
 * (TagFSProgramOf) — or the number itself for a file the volume has not; 0
 * stays 0. */
uint64_t StorageProgramNamed(uint64_t program);

/* Whether the caller's box asked `see-tags` or `change-tags` — with
 * `change`, `change-tags` — as its authority stands: the person's eyes, and
 * the person's hands. */
bool     StoragePersonSays(const OpContext *ctx, bool change);

/* What a call does with the tags it speaks. */
typedef enum {
    STORAGE_SEE,     /* asks by them, lists them, shows a file's */
    STORAGE_PUT,     /* puts one on a file, or makes a file wearing them */
    STORAGE_TAKE,    /* takes one off a file */
} StorageSpeech;

/* The one rule every call that speaks one owner's tags is judged by
 * (storage_tags.c): OK, ERR_TAGFS_NO_OWN_TAGS, or ERR_ACCESS_DENIED. */
error_t  StorageMaySpeak(const OpContext *ctx, TagOwner owner, StorageSpeech speech);

/* The view of every owner the caller may see (STORAGE_TAGS_ALL): *list holds
 * the programs that show it their tags, rising, the caller's to kfree. */
error_t  StorageViewAll(const OpContext *ctx, TagView *view, uint64_t **list);

#endif /* STORAGE_TAGS_H */
