/*
 * Storage Deck — the volumes of the house: storage.volume.list,
 * storage.volume.info and storage.volume.name (boxos_decks.h, where the
 * layouts are said once). What a person sees of where a file lies: every
 * volume, its number — the one a file's id carries —, the name the person
 * gave it, whether it is the home volume, mounted, taking no change or away,
 * how big it is and how full, and what it is on.
 */

#include "klib.h"
#include "crate_io.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "storage_deck.h"
#include "storage_tags.h"
#include "tagfs.h"
#include "volume_name.h"
#include "volume_tally.h"
#include "settle_ops.h"

_Static_assert(STORAGE_VOLUME_NAME_MOST == TALLY_VOLUME_NAME_MOST, "a volume's name, as the deck and the book bound it");

/* What a record says of one volume: the house's facts and the name. */
typedef struct {
    TagFSVolumeFacts f;
    uint16_t         name_len;
    char             name[TALLY_VOLUME_NAME_MOST];
} VolumeSaid;

static void VolumeSaidOf(TagFSVolume *vol, VolumeSaid *v)
{
    TagFSVolumeFactsOf(vol, &v->f);
    size_t n    = VolumeNameOf(vol, v->name, sizeof(v->name));
    v->name_len = (uint16_t)(n < sizeof(v->name) ? n : sizeof(v->name));
}

/* One volume's record ([u32 number][u32 flags][u8 uuid[16]][u64 bytes]
 * [u64 free][u64 files][u16 name_len][u16 medium_len][name][medium]) laid at
 * kp + pos, or only measured with kp NULL: the position past it. */
static uint64_t VolumeRecord(uint8_t *kp, uint64_t pos, const VolumeSaid *v)
{
    const TagFSVolumeFacts *f = &v->f;
    uint16_t name_len   = v->name_len;
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
        memcpy(kp + pos + STORAGE_VOLUME_LISTED_HEAD, v->name, name_len);
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
        VolumeSaid v;
        VolumeSaidOf(vol, &v);
        uint64_t past = VolumeRecord(NULL, need, &v);
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
        VolumeSaid v;
        VolumeSaidOf(vol, &v);
        if (VolumeRecord(NULL, pos, &v) > need)
            break;               /* a volume taken in, renamed or grown since the room was measured: the next call sees it */
        pos = VolumeRecord(kp, pos, &v);
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

    VolumeSaid v;
    VolumeSaidOf(vol, &v);
    Crate   *out  = &crates[op->out_crate];
    uint64_t need = VolumeRecord(NULL, 0, &v);
    if (need > out->capacity) return ERR_BUFFER_TOO_SMALL;
    uint8_t *kp = crate_out_alloc(out, need);
    if (!kp) return ERR_INVALID_ADDRESS;
    (void)VolumeRecord(kp, 0, &v);
    int crc = crate_out_commit(out, ctx, kp, need);
    crate_buf_free(kp);
    if (crc != OK) return crc;
    out->size = need;
    return OK;
}

/* STORAGE_VOLUME_NAME  params: [u32 number][u16 len][name]. */
int ObjVolumeName(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 6) return ERR_INVALID_ARGUMENT;
    uint16_t len = param_u16(op, 4);
    if (op->param_size < 6u + (uint32_t)len) return ERR_INVALID_ARGUMENT;
    if (!StoragePersonSays(ctx, true)) return ERR_ACCESS_DENIED;
    TagFSVolume *vol = TagFSVolumeNumbered(param_u32(op, 0));
    if (!vol) return ERR_OBJECT_NOT_FOUND;
    return SettleVolumeNamed(vol, (const char *)op->params + 6, len, ctx, crates);
}
