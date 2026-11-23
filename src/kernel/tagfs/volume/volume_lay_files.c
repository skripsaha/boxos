#include "volume_lay_files.h"
#include "tagfs_volume.h"
#include "tagfs_handle.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "klib.h"

/* The files the system keeps, looked up a page of the home volume's at a time. */
#define LAY_LIST_PAGE  64u

static const char* const s_reserved[] = {
#define X(id, key) key,
    TAGFS_RESERVED_KEYS(X)
#undef X
};

// ----------------------------------------------------------------------------
// Which files, and the programs they speak of
// ----------------------------------------------------------------------------

/* A file the laying opened, taken whole off the record its handle holds. */
static error_t LaidFileTaken(VolumeLaidFile* f, TagFSVolume* home, uint64_t fid)
{
    f->home = fid;
    f->src  = tagfs_open(home, fid, TAGFS_HANDLE_READ);
    if (!f->src)
        return ERR_FILE_NOT_FOUND;
    const FileRecord* rec = f->src->rec;
    f->name               = kmalloc(rec->name_len + 1u);
    if (!f->name)
        return ERR_NO_MEMORY;
    memcpy(f->name, rec->name, rec->name_len + 1u);
    f->name_len    = rec->name_len;
    f->flags       = TALLY_FILE_ACTIVE | (rec->flags & TAGFS_FILE_ROLE_MASK);
    f->size        = rec->size;
    f->created_ns  = rec->created_time;
    f->modified_ns = rec->modified_time;
    f->passport    = (rec->flags & TAGFS_FILE_PASSPORT) ? rec->passport : 0;
    f->head_len    = rec->head_len < TALLY_HEAD_MOST ? rec->head_len : TALLY_HEAD_MOST;
    memcpy(f->head, rec->head, f->head_len);
    f->blocks      = (f->size + VOLUME_BLOCK_BYTES - 1u) / VOLUME_BLOCK_BYTES;
    if (f->passport)
        f->flags |= TALLY_FILE_PASSPORT;
    if (f->head_len)
        f->flags |= TALLY_FILE_HEAD;
    return OK;
}

/* Whether a file the new volume holds carries `passport`. */
static bool LaidHoldsProgram(const VolumeLaying* l, uint64_t passport)
{
    for (uint32_t i = 0; i < l->count; i++)
        if (l->files[i].passport == passport)
            return true;
    return false;
}

/* Program `id` of the home volume — its file there, or its passport — as the
 * new volume speaks of it: its file there, else its passport when a file there
 * carries it; 0 when the new volume holds no file of it. Inside the home
 * volume's door. */
static uint64_t LaidWho(const VolumeLaying* l, TagFSVolume* home, uint64_t id)
{
    uint64_t passport = id;
    if (!(id & TAGFS_PASSPORT_BIT)) {
        for (uint32_t i = 0; i < l->count; i++)
            if (l->files[i].home == id)
                return (uint64_t)i + 1u;
        FileRecord* rec = id ? FileMapRecordHold(home->state.files, id, NULL) : NULL;
        passport        = rec ? rec->passport : 0;
        FileRecordRelease(rec);
    }
    return passport && LaidHoldsProgram(l, passport) ? passport : 0;
}

/* The id the new volume's book names tag `sp` of `owner_id` by (0 for the
 * person's): one already named, or a new one past the reserved words. */
static error_t LaidTagNamed(VolumeLaying* l, uint64_t owner_id, const TagSpelling* sp, uint32_t* id)
{
    for (uint32_t i = 0; i < l->tag_count; i++) {
        const VolumeLaidTag* t = &l->tags[i];
        if (t->owner_id == owner_id && t->key_len == sp->key_len && t->has_value == (sp->value != NULL) &&
            t->value_len == (sp->value ? sp->value_len : 0) && memcmp(t->text, sp->key, sp->key_len) == 0 &&
            (!t->value_len || memcmp(t->text + t->key_len, sp->value, t->value_len) == 0)) {
            *id = TAGFS_RESERVED_COUNT + i;
            return OK;
        }
    }
    if (l->tag_count == l->tag_room) {
        uint32_t       room  = l->tag_room ? l->tag_room * 2u : 32u;
        VolumeLaidTag* grown = krealloc(l->tags, room * sizeof(VolumeLaidTag));
        if (!grown)
            return ERR_NO_MEMORY;
        l->tags     = grown;
        l->tag_room = room;
    }
    VolumeLaidTag* t = &l->tags[l->tag_count];
    t->owner_id      = owner_id;
    t->key_len       = (uint32_t)sp->key_len;
    t->has_value     = sp->value != NULL;
    t->value_len     = sp->value ? (uint32_t)sp->value_len : 0;
    t->text          = kmalloc(t->key_len + t->value_len + 1u);
    if (!t->text)
        return ERR_NO_MEMORY;
    memcpy(t->text, sp->key, t->key_len);
    if (t->value_len)
        memcpy(t->text + t->key_len, sp->value, t->value_len);
    *id = TAGFS_RESERVED_COUNT + l->tag_count++;
    return OK;
}

/* One id into a rising list of `*n`, once. */
static void LaidRising32(uint32_t* ids, uint32_t* n, uint32_t id)
{
    uint32_t at = 0;
    while (at < *n && ids[at] < id)
        at++;
    if (at < *n && ids[at] == id)
        return;
    memmove(ids + at + 1u, ids + at, sizeof(uint32_t) * (*n - at));
    ids[at] = id;
    (*n)++;
}

static void LaidRising64(uint64_t* ids, uint32_t* n, uint64_t id)
{
    uint32_t at = 0;
    while (at < *n && ids[at] < id)
        at++;
    if (at < *n && ids[at] == id)
        return;
    memmove(ids + at + 1u, ids + at, sizeof(uint64_t) * (*n - at));
    ids[at] = id;
    (*n)++;
}

/* The file's tags, the programs it shows them to and the one that made it, as
 * the new volume speaks of them — what it cannot speak left behind, and how
 * many in *dropped. Inside the home volume's door. */
static error_t LaidFileSpoken(VolumeLaying* l, TagFSVolume* home, VolumeLaidFile* f, uint32_t* dropped)
{
    const FileRecord* rec = f->src->rec;
    f->tags  = rec->tag_count ? kmalloc(sizeof(uint32_t) * rec->tag_count) : NULL;
    f->shown = rec->shown_count ? kmalloc(sizeof(uint64_t) * rec->shown_count) : NULL;
    if ((rec->tag_count && !f->tags) || (rec->shown_count && !f->shown))
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < rec->tag_count; i++) {
        TagSpelling sp;
        TagOwner    owner;
        bool        system = false;
        uint32_t    id     = rec->tags[i];
        if (!TagRegistrySpelling(home->state.registry, id, &sp, &system, &owner))
            continue;
        if (id >= TAGFS_RESERVED_COUNT) {
            uint64_t who = owner.kind == TAG_OWNER_PERSON ? 0 : LaidWho(l, home, owner.id);
            if (owner.kind != TAG_OWNER_PERSON && !who) {
                (*dropped)++;
                continue;
            }
            error_t rc = LaidTagNamed(l, who, &sp, &id);
            if (rc != OK)
                return rc;
        }
        LaidRising32(f->tags, &f->ntags, id);
    }
    for (uint32_t i = 0; i < rec->shown_count; i++) {
        uint64_t who = rec->shown_to[i] ? LaidWho(l, home, rec->shown_to[i]) : 0;
        if (rec->shown_to[i] && !who)
            (*dropped)++;
        else
            LaidRising64(f->shown, &f->nshown, who);
    }
    if (f->nshown)
        f->flags |= TALLY_FILE_SHOWN;
    f->made_by = (rec->flags & TAGFS_FILE_MADE_BY) && rec->made_by ? LaidWho(l, home, rec->made_by) : 0;
    if (f->made_by)
        f->flags |= TALLY_FILE_MADE_BY;
    else if (rec->flags & TAGFS_FILE_MADE_BY)
        (*dropped)++;
    return OK;
}

/* The ids of the files the home volume keeps in a role, the kernel's first,
 * into a kmalloc'd *ids. ERR_FILE_NOT_FOUND for none in the kernel's. */
static error_t LaidRoleIds(TagFSVolume* home, uint64_t** ids, uint32_t* count)
{
    uint64_t  page_ids[LAY_LIST_PAGE];
    uint32_t  room = 0, n = 0, kernels = 0;
    TagFSPage page;
    *ids   = NULL;
    *count = 0;
    for (uint32_t pass = 0; pass < 2u; pass++) {
        uint64_t from = 1;
        do {
            if (TagFSListPage(home, from, page_ids, LAY_LIST_PAGE, false, &page) != OK)
                return ERR_DEVICE_NOT_READY;
            for (uint32_t i = 0; i < page.delivered; i++) {
                FileRecord* rec  = FileMapRecordHold(home->state.files, page_ids[i], NULL);
                uint32_t    role = rec ? rec->flags & TAGFS_FILE_ROLE_MASK : 0u;
                FileRecordRelease(rec);
                if (!role || (pass == 0) != (role == TAGFS_FILE_ROLE_KERNEL) || (pass == 0 && kernels))
                    continue;
                if (n == room) {
                    room            = room ? room * 2u : 32u;
                    uint64_t* grown = krealloc(*ids, room * sizeof(uint64_t));
                    if (!grown)
                        return ERR_NO_MEMORY;
                    *ids = grown;
                }
                (*ids)[n++] = page_ids[i];
                kernels += pass == 0;
            }
            from = page.next;
        } while (from);
    }
    *count = n;
    return kernels ? OK : ERR_FILE_NOT_FOUND;
}

error_t VolumeLayFilesTaken(VolumeLaying* l, TagFSVolume* home)
{
    uint64_t* ids     = NULL;
    uint32_t  count   = 0, dropped = 0;
    error_t   rc      = LaidRoleIds(home, &ids, &count);
    if (rc == ERR_FILE_NOT_FOUND)
        kprintf("[TagFS] the home volume keeps no kernel — there is no system to lay\n");
    if (rc == OK) {
        l->files = kmalloc(sizeof(VolumeLaidFile) * count);
        rc       = l->files ? OK : ERR_NO_MEMORY;
    }
    if (rc == OK)
        memset(l->files, 0, sizeof(VolumeLaidFile) * count);
    for (uint32_t i = 0; rc == OK && i < count; i++) {
        rc = LaidFileTaken(&l->files[i], home, ids[i]);
        l->count = i + 1u;
    }
    for (uint32_t i = 0; rc == OK && i < l->count; i++)
        rc = LaidFileSpoken(l, home, &l->files[i], &dropped);
    kfree(ids);
    if (rc != OK)
        return rc;
    if (dropped)
        kprintf("[TagFS] the files the system keeps go to the new volume without %u word(s) of programs it will "
                "not hold\n", dropped);

    uint64_t next = l->frame.data_block;
    for (uint32_t i = 0; i < l->count; i++) {
        l->files[i].start = next;
        next += l->files[i].blocks;
    }
    l->end = next;
    const VolumeLaidFile* k = &l->files[0];
    if (l->end >= l->blocks - 1u || k->start + k->blocks > UINT32_MAX || k->size > UINT32_MAX) {
        kprintf("[TagFS] seat %u: entry %u is %llu block(s) — the system takes them to block %llu, its kernel %llu "
                "byte(s)\n", l->seat, l->entry, (unsigned long long)l->blocks, (unsigned long long)l->end,
                (unsigned long long)k->size);
        return ERR_TAGFS_NO_SPACE;
    }
    l->digests = kmalloc(sizeof(uint64_t) * (l->end - l->frame.data_block + 1u));
    if (!l->digests)
        return ERR_NO_MEMORY;
    memset(l->digests, 0, sizeof(uint64_t) * (l->end - l->frame.data_block + 1u));
    return OK;
}

void VolumeLayFilesLetGo(VolumeLaying* l, TagFSVolume* home)
{
    for (uint32_t i = 0; i < l->count; i++) {
        VolumeLaidFile* f = &l->files[i];
        if (f->src)
            tagfs_close(home, f->src);
        kfree(f->name);
        kfree(f->tags);
        kfree(f->shown);
    }
    kfree(l->files);
    for (uint32_t i = 0; i < l->tag_count; i++)
        kfree(l->tags[i].text);
    kfree(l->tags);
    kfree(l->digests);
    l->files     = NULL;
    l->count     = 0;
    l->tags      = NULL;
    l->tag_count = 0;
    l->tag_room  = 0;
    l->digests   = NULL;
}

// ----------------------------------------------------------------------------
// The book in memory
// ----------------------------------------------------------------------------

/* FILES: each file's header — gen 1, ACTIVE, its role and what it carries —
 * and its one run as its Parcels (§2.9). */
static int LaidFileLeaves(VolumeLaying* l)
{
    BindLeaves* leaves = &l->leaves;
    for (uint32_t i = 0; i < l->count; i++) {
        const VolumeLaidFile* f    = &l->files[i];
        uint64_t              fid  = (uint64_t)i + 1u;
        uint8_t*              ids  = BindBody(leaves, 10u * ((uint64_t)f->ntags + 1u));
        uint8_t*              seen = BindBody(leaves, 10u * ((uint64_t)f->nshown + 1u));
        uint8_t*              body = BindBody(leaves, 180u + f->name_len + 10u * ((uint64_t)f->ntags + f->nshown));
        if (!ids || !seen || !body)
            return -1;
        TallyFileHead h = { .gen = LAY_TALLY_SEQ, .flags = f->flags, .size = f->size, .created_ns = f->created_ns,
                            .modified_ns = f->modified_ns, .name = (const uint8_t*)f->name, .name_len = f->name_len,
                            .ntags = f->ntags, .tags = ids, .made_by = f->made_by, .nshown = f->nshown,
                            .shown = seen, .head_len = f->head_len, .head = f->head, .passport = f->passport };
        h.tags_bytes  = TallyIdsPut(ids, f->tags, f->ntags);
        h.shown_bytes = TallyFidsPut(seen, f->shown, f->nshown);
        if (BindAdd(leaves, TALLY_FILES, (TallyKey){ fid, 0 }, body, TallyFileHeadPut(body, &h), 1) != 0)
            return -1;
        TallyRun run = { .start = f->start, .count = f->blocks, .birth = LAY_TALLY_SEQ };
        if (BindParcels(leaves, fid, &run, f->blocks ? 1u : 0u) != 0)
            return -1;
    }
    return 0;
}

/* One tag into TAGS at `id`. */
static int LaidTagLeaf(BindLeaves* leaves, uint32_t id, const TallyTag* t)
{
    uint8_t* body = BindBody(leaves, 50u + t->key_len + t->value_len);
    return body ? BindAdd(leaves, TALLY_TAGS, (TallyKey){ id, 0 }, body, TallyTagPut(body, t), 1) : -1;
}

/* TAGS: the reserved words at their ids, as every maker lays them, and every
 * tag a file wears past them — a program's whose it is. */
static int LaidTagLeaves(VolumeLaying* l)
{
    for (uint32_t id = 0; id < TAGFS_RESERVED_COUNT; id++) {
        TallyTag t = { .key = (const uint8_t*)s_reserved[id], .key_len = strlen(s_reserved[id]) };
        if (LaidTagLeaf(&l->leaves, id, &t) != 0)
            return -1;
    }
    for (uint32_t i = 0; i < l->tag_count; i++) {
        const VolumeLaidTag* v = &l->tags[i];
        TallyTag t = { .flags = (v->has_value ? TALLY_TAG_HAS_VALUE : 0u) | (v->owner_id ? TALLY_TAG_OWNED : 0u),
                       .owner_kind = v->owner_id ? TALLY_OWNER_PROGRAM : 0u, .owner_id = v->owner_id,
                       .key = (const uint8_t*)v->text, .key_len = v->key_len,
                       .value = v->has_value ? (const uint8_t*)v->text + v->key_len : NULL,
                       .value_len = v->value_len };
        if (LaidTagLeaf(&l->leaves, TAGFS_RESERVED_COUNT + i, &t) != 0)
            return -1;
    }
    return 0;
}

/* USE: the name a person gave the volume, at key (1, 0) (§1.4). */
static int LaidUseLeaves(VolumeLaying* l)
{
    if (!l->name_len)
        return 0;
    uint8_t* body = BindBody(&l->leaves, 10u + l->name_len);
    return body ? BindAdd(&l->leaves, TALLY_USE, (TallyKey){ 1, 0 }, body, TallyUsePut(body, l->name, l->name_len),
                          1)
                : -1;
}

/* DIGESTS: the files' blocks are one row from the data run's first block, cut
 * at every window of DIGEST_ITEM_MOST blocks (§16.3 п.1). */
static int LaidDigestLeaves(VolumeLaying* l)
{
    for (uint64_t b = l->frame.data_block; b < l->end;) {
        uint64_t stop = (b / DIGEST_ITEM_MOST + 1u) * DIGEST_ITEM_MOST;
        uint32_t n    = (uint32_t)((stop < l->end ? stop : l->end) - b);
        uint8_t* body = BindBody(&l->leaves, 10u + 8u * (uint64_t)n);
        if (!body ||
            BindAdd(&l->leaves, TALLY_DIGESTS, (TallyKey){ b, 0 }, body,
                    TallyDigestsPut(body, l->digests + (b - l->frame.data_block), n), 1) != 0)
            return -1;
        b += n;
    }
    return 0;
}

error_t VolumeLayBookPlanned(VolumeLaying* l)
{
    VolumeGeometry geo = VolumeGeometryLaid();
    BindPlanFree(&l->plan);
    BindLeavesFree(&l->leaves);
    memset(&l->leaves, 0, sizeof(l->leaves));
    memset(&l->plan, 0, sizeof(l->plan));
    l->leaves.grain = geo.grain_bytes / VOLUME_BLOCK_BYTES;
    l->leaves.grid  = geo.physical_bytes > VOLUME_BLOCK_BYTES ? geo.physical_bytes / VOLUME_BLOCK_BYTES : 1u;
    l->leaves.seq   = LAY_TALLY_SEQ;
    if (LaidFileLeaves(l) != 0 || LaidTagLeaves(l) != 0 || LaidUseLeaves(l) != 0 || LaidDigestLeaves(l) != 0)
        return ERR_NO_MEMORY;
    int laid = QuireLay(&l->leaves, l->end, l->blocks - 1u, &l->plan);
    if (laid == BIND_NO_ROOM)
        kprintf("[TagFS] seat %u: entry %u is %llu block(s) — the system's files run to block %llu and their book "
                "to %llu\n", l->seat, l->entry, (unsigned long long)l->blocks, (unsigned long long)l->end,
                (unsigned long long)(l->plan.next + l->leaves.grain));
    return laid == BIND_OK ? OK : laid == BIND_NO_ROOM ? ERR_TAGFS_NO_SPACE : ERR_NO_MEMORY;
}

SpineCounts VolumeLayCounts(const VolumeLaying* l)
{
    return (SpineCounts){ (uint64_t)l->count + 1u, TAGFS_RESERVED_COUNT + (uint64_t)l->tag_count, 1u };
}
