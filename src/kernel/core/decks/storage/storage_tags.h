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

/* One page of a list a deck answer gives, of volume `vol`: up to `room` of
 * the volume's own ids from `from` on into `out`, and how many there are when
 * `want_total` (TagFSPage) — of the list `ask` names. */
typedef error_t (*StoragePager)(TagFSVolume *vol, const void *ask, uint64_t from, uint64_t *out, uint32_t room,
                                bool want_total, TagFSPage *page);

/* The list `pager` pages, from house id `from` on — of every mounted volume
 * of the house in the order of their numbers, or of `only` alone — written
 * into `out` after its 8-byte header as the house's ids (TagFSHouseId),
 * `piece_room` at a time through `piece`: hdr[0] how many went in, hdr[1] how
 * many there are — every volume's first page asked whatever the room, so a
 * caller with room for none is told how many; never less than what went in, a
 * member come while the pages are taken delivered too. The one way every list
 * of the deck is answered. OK, or the pager's or the crate's refusal. */
error_t StorageAnswerPages(StoragePager pager, const void *ask, TagFSVolume *only, uint64_t from, const Crate *out,
                           const OpContext *ctx, uint64_t *piece, uint32_t piece_room, uint32_t hdr[2]);

/* The volume of the house the caller's program was started from, mounted —
 * the home volume for the kernel's own call — or NULL (StorageReaderVolume).
 * The caller's program on volume `on` — who it is (TagOwner), while it is on
 * that volume (TagFSHoldsProgram) — or 0 for none (StorageReaderOn); on its
 * own volume (StorageReader). */
TagFSVolume *StorageReaderVolume(const OpContext *ctx);
uint64_t     StorageReaderOn(const OpContext *ctx, TagFSVolume *on);
uint64_t     StorageReader(const OpContext *ctx);

/* The program a program names by a file of it (its house id) — the one that
 * file holds (TagFSProgramOf) — or the number itself for a file the house has
 * not; 0 stays 0. *where the volume of that file — which a program named by
 * its file is that program on, alone —, NULL for a passport, the same program
 * on every volume. StorageOwnerOn: a program's owner made nobody — 0 — on a
 * volume `on` other than the one it is named on. */
uint64_t StorageProgramNamed(uint64_t program, TagFSVolume **where);

/* Whether any volume of the house is mounted. */
bool     StorageHouseUp(void);
void     StorageOwnerOn(TagOwner *owner, TagFSVolume *where, TagFSVolume *on);

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

/* The one rule every call that speaks one owner's tags on volume `on` is
 * judged by (storage_tags.c): OK, ERR_TAGFS_NO_OWN_TAGS, or
 * ERR_ACCESS_DENIED. */
error_t  StorageMaySpeak(const OpContext *ctx, TagFSVolume *on, TagOwner owner, StorageSpeech speech);

/* The view of every owner the caller may see on volume `on`
 * (STORAGE_TAGS_ALL): *list holds the programs that show it their tags
 * there, rising, the caller's to kfree. */
error_t  StorageViewAll(const OpContext *ctx, TagFSVolume *on, TagView *view, uint64_t **list);

#endif /* STORAGE_TAGS_H */
