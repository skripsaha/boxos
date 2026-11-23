/*
 * Storage Deck — the volumes of the house: storage.volume.list and
 * storage.volume.info (boxos_decks.h, where both layouts are said once).
 * What a person sees of where a file lies: every volume, its number — the one
 * a file's id carries —, whether it is the home volume, mounted, taking no
 * change or away, how big it is and how full, and what it is on.
 */

#include "klib.h"
#include "crate_io.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "storage_deck.h"
#include "storage_tags.h"
#include "tagfs.h"

/* One volume's record ([u32 number][u32 flags][u8 uuid[16]][u64 bytes]
 * [u64 free][u64 files][u16 name_len][u16 medium_len][name][medium]) laid at
 * kp + pos, or only measured with kp NULL: the position past it. */
static uint64_t VolumeRecord(uint8_t *kp, uint64_t pos, const TagFSVolumeFacts *f)
{
    uint16_t name_len   = 0;
    uint16_t medium_len = (uint16_t)strnlen(f->medium, sizeof(f->medium));
    if (kp) {
        uint32_t flags = (f->home ? STORAGE_VOLUME_HOME : 0u) | (f->mounted ? STORAGE_VOLUME_MOUNTED : 0u) |
                         (f->read_only ? STORAGE_VOLUME_READ_ONLY : 0u) | (f->away ? STORAGE_VOLUME_AWAY : 0u) |
                         (f->removable ? STORAGE_VOLUME_REMOVABLE : 0u);
        memcpy(kp + pos, &f->number, 4);
        memcpy(kp + pos + 4, &flags, 4);
        memcpy(kp + pos + 8, f->uuid, 16);
        memcpy(kp + pos + 24, &f->bytes, 8);
        memcpy(kp + pos + 32, &f->free_bytes, 8);
        memcpy(kp + pos + 40, &f->files, 8);
        memcpy(kp + pos + 48, &name_len, 2);
        memcpy(kp + pos + 50, &medium_len, 2);
        memcpy(kp + pos + STORAGE_VOLUME_LISTED_HEAD + name_len, f->medium, medium_len);
    }
    return pos + STORAGE_VOLUME_LISTED_HEAD + name_len + medium_len;
}

/* STORAGE_VOLUME_LIST  out_crate: [u32 delivered][u32 total] and the records
 * of every volume of the house, in the order of their numbers, while they
 * fit. */
int ObjVolumeList(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint32_t total = 0;
    uint64_t need  = 8;
    bool     full  = false;
    for (TagFSVolume *vol = TagFSVolumeNext(NULL); vol; vol = TagFSVolumeNext(vol)) {
        TagFSVolumeFacts f;
        TagFSVolumeFactsOf(vol, &f);
        uint64_t past = VolumeRecord(NULL, need, &f);
        full = full || past > out->capacity;
        if (!full)
            need = past;
        total++;
    }
    uint8_t *kp = crate_out_alloc(out, need);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint32_t delivered = 0;
    uint64_t pos       = 8;
    for (TagFSVolume *vol = TagFSVolumeNext(NULL); vol && delivered < total; vol = TagFSVolumeNext(vol)) {
        TagFSVolumeFacts f;
        TagFSVolumeFactsOf(vol, &f);
        if (VolumeRecord(NULL, pos, &f) > need)
            break;               /* a volume taken in, or grown, since the room was measured: the next call sees it */
        pos = VolumeRecord(kp, pos, &f);
        delivered++;
    }
    memcpy(kp, &delivered, 4);
    memcpy(kp + 4, &total, 4);
    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    if (crc != OK) return crc;
    out->size = pos;
    return OK;
}

/* STORAGE_VOLUME_INFO  params: [u32 number]; out_crate: one record. */
int ObjVolumeInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4 || op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    TagFSVolume *vol = TagFSVolumeNumbered(param_u32(op, 0));
    if (!vol) return ERR_OBJECT_NOT_FOUND;

    TagFSVolumeFacts f;
    TagFSVolumeFactsOf(vol, &f);
    Crate   *out  = &crates[op->out_crate];
    uint64_t need = VolumeRecord(NULL, 0, &f);
    if (need > out->capacity) return ERR_BUFFER_TOO_SMALL;
    uint8_t *kp = crate_out_alloc(out, need);
    if (!kp) return ERR_INVALID_ADDRESS;
    (void)VolumeRecord(kp, 0, &f);
    int crc = crate_out_commit(out, ctx, kp, need);
    crate_buf_free(kp);
    if (crc != OK) return crc;
    out->size = need;
    return OK;
}
