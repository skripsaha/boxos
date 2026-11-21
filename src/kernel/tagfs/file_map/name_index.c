#include "name_index.h"
#include "klib.h"

#define NAME_INDEX_FIRST_BUCKETS 256u

/* FNV-1a, 32 bits, of the bytes of a name or a word. */
static uint32_t NameHash(const char* text, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ (uint8_t)text[i]) * 16777619u;
    return hash;
}

/* The record a link is one of. */
static FileRecord* LinkOwner(const NameLink* link)
{
    const NameLink* first = link - link->which;
    return (FileRecord*)((uint8_t*)first - offsetof(FileRecord, name_link));
}

/* Whether the record is in the index at all: a file kept deleted is not. */
static bool Indexed(const FileRecord* rec)
{
    return rec && !(rec->flags & TAGFS_FILE_DELETED);
}

uint32_t TagFSNameWords(const char* name, size_t length, size_t lengths[2])
{
    if (length == 0)
        return 0;
    lengths[0] = length;
    for (size_t at = length; at > 1; at--) {
        if (name[at - 1] == '.') {
            lengths[1] = at - 1;
            return 2;
        }
    }
    return 1;
}

/* The bytes of the record's name that link `which` stands for: the whole name
 * (0), or its stem (1). False when there is nothing to match. */
static bool KeyLength(const FileRecord* rec, uint32_t which, uint32_t* length)
{
    size_t lengths[2];
    if (which >= TagFSNameWords(rec->name, rec->name_len, lengths))
        return false;
    *length = (uint32_t)lengths[which];
    return true;
}

static void LinkIn(NameLink** bucket, uint32_t buckets, NameLink* link)
{
    NameLink** at = &bucket[link->hash & (buckets - 1u)];
    while (*at && (*at)->file_id < link->file_id)
        at = &(*at)->next;
    link->next = *at;
    *at        = link;
}

static void LinkOut(NameIndex* index, NameLink* link)
{
    NameLink** at = &index->bucket[link->hash & (index->buckets - 1u)];
    while (*at != link)
        at = &(*at)->next;
    *at          = link->next;
    link->next   = NULL;
    link->linked = false;
    index->links--;
}

/* `now`'s link takes the place of `was`'s: the same file's, the same key. */
static void LinkReplace(NameIndex* index, NameLink* was, NameLink* now)
{
    NameLink** at = &index->bucket[was->hash & (index->buckets - 1u)];
    while (*at != was)
        at = &(*at)->next;
    *now         = *was;
    *at          = now;
    was->next    = NULL;
    was->linked  = false;
}

static void LinkMake(NameIndex* index, FileRecord* rec, uint32_t which, uint64_t fid)
{
    uint32_t length;
    if (!KeyLength(rec, which, &length))
        return;
    NameLink* link = &rec->name_link[which];
    link->file_id  = fid;
    link->hash     = NameHash(rec->name, length);
    link->linked   = true;
    link->which    = (uint8_t)which;
    LinkIn(index->bucket, index->buckets, link);
    index->links++;
}

bool NameIndexOpen(NameIndex* index)
{
    memset(index, 0, sizeof(*index));
    index->bucket = NameIndexTableMake(NAME_INDEX_FIRST_BUCKETS);
    if (!index->bucket)
        return false;
    index->buckets = NAME_INDEX_FIRST_BUCKETS;
    return true;
}

void NameIndexClose(NameIndex* index)
{
    kfree(index->bucket);
    memset(index, 0, sizeof(*index));
}

uint32_t NameIndexRoom(const NameIndex* index, uint32_t more)
{
    uint32_t wanted = index->buckets;
    while ((uint64_t)index->links + 2u * (uint64_t)more > wanted && wanted <= UINT32_MAX / 4u)
        wanted *= 2u;
    return wanted > index->buckets ? wanted : 0;
}

NameLink** NameIndexTableMake(uint32_t buckets)
{
    NameLink** table = kmalloc(sizeof(NameLink*) * buckets);
    if (table)
        memset(table, 0, sizeof(NameLink*) * buckets);
    return table;
}

NameLink** NameIndexAdopt(NameIndex* index, NameLink** table, uint32_t buckets)
{
    if (buckets <= index->buckets)
        return table;
    NameLink** old = index->bucket;
    for (uint32_t b = 0; b < index->buckets; b++) {
        NameLink* link = old[b];
        while (link) {
            NameLink* next = link->next;
            LinkIn(table, buckets, link);
            link = next;
        }
    }
    index->bucket  = table;
    index->buckets = buckets;
    return old;
}

void NameIndexPut(NameIndex* index, uint64_t fid, FileRecord* was, FileRecord* now)
{
    bool before = Indexed(was);
    bool after  = Indexed(now);
    bool same   = before && after && was->name_len == now->name_len &&
                  memcmp(was->name, now->name, was->name_len) == 0;
    for (uint32_t which = 0; which < 2; which++) {
        bool linked = before && was->name_link[which].linked;
        if (same) {
            if (linked)
                LinkReplace(index, &was->name_link[which], &now->name_link[which]);
            continue;
        }
        if (linked)
            LinkOut(index, &was->name_link[which]);
        if (after)
            LinkMake(index, now, which, fid);
    }
}

/* Whether the link's key is `word`: the bytes of the record's name it stands
 * for, and no others. */
static bool LinkSays(const NameLink* link, const char* word, size_t length)
{
    const FileRecord* rec = LinkOwner(link);
    uint32_t          key;
    return KeyLength(rec, link->which, &key) && key == length && memcmp(rec->name, word, length) == 0;
}

uint32_t NameIndexFind(const NameIndex* index, const char* word, size_t length, uint64_t from,
                       uint64_t* out, uint32_t room, uint64_t* next)
{
    uint32_t hash  = NameHash(word, length);
    uint32_t given = 0;
    *next = 0;
    for (const NameLink* link = index->bucket[hash & (index->buckets - 1u)]; link; link = link->next) {
        if (link->hash != hash || link->file_id < from || !LinkSays(link, word, length))
            continue;
        if (given < room)
            out[given++] = link->file_id;
        else if (*next == 0)
            *next = link->file_id;
    }
    return given;
}
