#ifndef STORAGE_TAGS_H
#define STORAGE_TAGS_H

#include "ktypes.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"

/* The storage deck's calls that speak tags (storage_tags.c), each in the
 * owner's tags its whose byte names — the program's own or the person's
 * (boxos_decks.h, STORAGE_TAGS_*) — registered by StorageDeckRegister. */
int ObjQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjNameQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagList(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagSet(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjTagUnset(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);

#endif /* STORAGE_TAGS_H */
