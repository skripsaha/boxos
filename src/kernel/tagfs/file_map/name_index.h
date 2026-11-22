#ifndef TAGFS_NAME_INDEX_H
#define TAGFS_NAME_INDEX_H

#include "file_record.h"

/*
 * Which files are called a word, answered from memory.
 *
 * A file's name is a field of its card, never a tag, so "who is called kernel"
 * is not a question the tag index can answer — and walking the volume's cards
 * for it is what the name index is there to spare. It is derived: built as
 * the map takes the records of a mount and kept by every record the map takes
 * or lets go after it, so it is exact for every change that makes a file,
 * renames it, deletes it, keeps it deleted, brings it back, lets it go
 * (escheat) or undoes it — each of them puts or drops a record. It is never
 * written to the medium.
 *
 * A word matches a file when it is the whole of the file's name or its stem —
 * the name up to its last dot, so `slot3` matches `slot3.sav` — byte for byte,
 * as tags are compared. A file kept deleted is left out, as every query leaves
 * it out; a name with no stem (`.profile`) is matched whole only.
 *
 * A hash table of links that live in the records themselves (NameLink), so
 * that putting a record never allocates: an Ink puts its record under the
 * volume's lock, with nothing in hand to allocate with. Each chain is kept in
 * file id order, which is what makes a page of answers a walk of one chain, and
 * is doubly linked with its tail known: files are not called uniquely — a
 * volume may hold tens of thousands of one name, all in one chain — so a file
 * made (the next id, at the tail), a record put again and a file let go cost
 * the same however long the chain is.
 * The table grows by taking a larger one made with no lock held (NameIndexRoom)
 * and relinking into it; a table that could not grow only has longer chains.
 *
 * Every function but the table's making is under the lock of the map that
 * holds the index (file_map.h); this file takes none.
 */

typedef struct {
    NameLink* head;
    NameLink* tail;
} NameBucket;

typedef struct {
    NameBucket* bucket;
    uint32_t    buckets;         /* a power of two */
    uint32_t    links;
} NameIndex;

/* The first table, made; false for no memory. */
bool       NameIndexOpen(NameIndex* index);
void       NameIndexClose(NameIndex* index);

/* The bucket count the table should have now to hold the links of `more`
 * more files, or 0 when it is large enough. */
uint32_t   NameIndexRoom(const NameIndex* index, uint32_t more);
/* A table of `buckets` empty buckets, made with no lock held; NULL for no
 * memory. */
NameBucket* NameIndexTableMake(uint32_t buckets);
/* The table is taken in place of the one the index has — every link moved
 * into it — when it has more buckets; the table now unused, to be freed with
 * no lock held, comes back (the one given, if it was not taken). */
NameBucket* NameIndexAdopt(NameIndex* index, NameBucket* table, uint32_t buckets);

/* File `fid`'s record `was` (NULL for none) becomes `now` (NULL for none) in
 * the index: the links of `was` taken out, those of `now` put in — a record
 * that carries the same name in the same state takes `was`'s place in its
 * chains. Never allocates, never fails. */
void       NameIndexPut(NameIndex* index, uint64_t fid, FileRecord* was, FileRecord* now);

/* The files called `word` (`length` bytes, not NUL-terminated) from file id
 * `from` on, in file id order: up to `room` ids into `out`, how many were
 * written. *next is the id of the first match past `room`, 0 when there is
 * none. */
uint32_t   NameIndexFind(const NameIndex* index, const char* word, size_t length, uint64_t from,
                         uint64_t* out, uint32_t room, uint64_t* next);

#endif /* TAGFS_NAME_INDEX_H */
