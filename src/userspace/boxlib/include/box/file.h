#ifndef BOX_STORAGE_H
#define BOX_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

typedef struct {
    uint8_t type;        // 0=user, 1=system
    char key[11];
    char value[12];
} tag_t;

typedef struct {
    file_id_t file_id;
    uint64_t size;
    uint8_t tag_count;
    char filename[32];
    tag_t tags[5];
} file_info_t;

/* Full metadata for tools that must preserve a file exactly.  file_info_t is
 * deliberately a small display structure and may truncate names or tags;
 * this type owns complete copies returned by STORAGE_OBJ_GET_INFO.
 *
 * Filename and Tags are heap-owned by the value.  Tags is a comma-separated
 * list of complete tag spellings, or NULL when the file has no tags.  The
 * caller releases both with FileMetadataRelease(). */
typedef struct {
    file_id_t FileId;
    uint64_t  Size;
    uint16_t  TagCount;
    char     *Filename;
    char     *Tags;
} FileMetadata;

/* Read the complete self-describing metadata record.  The response buffer is
 * grown until the kernel can return every byte; no field is truncated. */
int FileMetadataRead(file_id_t FileId, FileMetadata *Out);

/* Release all heap-owned members and reset Out to its empty state. */
void FileMetadataRelease(FileMetadata *Metadata);

/* Whether the file wears a tag whose key is `Key`, bare or with a value:
 * "trashed", "settings", "log:file". What a file is, the tags it wears say. */
bool FileMetadataWears(const FileMetadata *Metadata, const char *Key);

/*
 * Inside the Use Context by default. When the user has said `use code cpp`,
 * query("project") asks for files that are code AND cpp AND project, and
 * create() stamps the new file code and cpp along with the tags it was given.
 * The _everywhere spellings ask the same of the whole volume, the user's
 * context left out. See box/use.h.
 *
 * create answers the new file's id (> 0), or a negative -error_t — one int64
 * the way fread answers a count, so a whole 64-bit id is never cut to an int.
 */
int64_t create(const char* filename, const char* tags);
int64_t create_everywhere(const char* filename, const char* tags);
/* The files that wear every tag of `tags` (every file, for NULL or "") and
 * none of `without` (a comma list too; NULL or "" sets nothing aside). The
 * volume sets aside no file of its own accord — trashed and hidden are tags
 * like any other — so each asker says what it leaves out: a listing for the
 * person "trashed,hidden" until the person names them, a name looked up to
 * act on "trashed".
 *
 * query writes up to max_files ids and returns how many it wrote. A caller
 * that wants every match asks query_all: it learns the count from the kernel,
 * hands back a malloc'd list of that many ids (NULL when there are none) and
 * returns the count; the caller frees the list. */
int query(const char* tags, const char* without, file_id_t* file_ids, size_t max_files);
int query_everywhere(const char* tags, const char* without, file_id_t* file_ids, size_t max_files);
int query_all(const char* tags, const char* without, file_id_t** out_ids);
int query_all_everywhere(const char* tags, const char* without, file_id_t** out_ids);
int file_info(file_id_t file_id, file_info_t* info);
/* Return the byte count transferred (0..4 GiB, short-transfer: a request > 4 GiB
 * is capped to one syscall — loop for more), or a negative -error_t on failure.
 * fwrite writes at `offset` no further than the file's size: past it the
 * write is refused -ERR_TAGFS_BEYOND_SIZE (file_span makes a gap). A write
 * the volume refused part way answers the bytes the file holds of it — a
 * short count; the refusal is the answer of the next call, from there. */
int64_t fread(file_id_t file_id, uint64_t offset, void* buffer, size_t size);
int64_t fwrite(file_id_t file_id, uint64_t offset, const void* buffer, size_t size);
/* fwrite at the file's end as it is when the write reaches the file: writers
 * adding to one file never lay their bytes over each other's. The same answer
 * as fwrite. */
int64_t file_append(file_id_t file_id, const void* buffer, size_t size);
/* `delete` is a C++ keyword — C++ callers use file_delete(), bound to the
 * same ELF symbol via asm label. C keeps the original name unchanged. */
#ifdef __cplusplus
int file_delete(file_id_t file_id) asm("delete");
#else
int delete(file_id_t file_id);
#endif
int file_rename(file_id_t file_id, const char* new_filename);

/* The file made exactly `size` bytes long. Shorter: the tail is cut and its
 * blocks given back. Longer: the bytes from the old size on read as zeros —
 * never another file's. A write grows a file only at its end, and one that
 * begins past it is refused -ERR_TAGFS_BEYOND_SIZE: a gap is made by span.
 * flags STORAGE_SPAN_GROW (box/types.h): grown to `size` only if it is
 * shorter, never cut — the way to make room for a write past the end without
 * cutting back what another writer added meanwhile; 0 for exactly `size`.
 * Refused on "system"/"boot" files (-ERR_PERMISSION_DENIED), like delete; a
 * growth past every block the volume could give, -ERR_TAGFS_NO_SPACE.
 * Returns 0 or a negative -error_t.
 *
 * Publishes the change on every tag of the file — the same 40-byte payload
 * shape as a write, with op = 3, offset = where the file changed (the smaller
 * of the old and the new size), bytes = how many were cut or added. */
int file_span(file_id_t file_id, uint64_t size, uint32_t flags);

int tag_add(file_id_t file_id, const char* tag);
int tag_remove(file_id_t file_id, const char* key);

/* A tag files wear, and how many of them wear it: one entry of the volume's
 * own list of its tags (storage.tag.list), not a walk of its files. */
typedef struct {
    char    *Tag;       /* "key" or "key:value", as the volume spells it */
    uint32_t Files;     /* how many files wear it — inside the Use Context, of its files */
    bool     System;    /* a tag the system keeps */
} TagListed;

/* Every tag a file wears, each with how many files wear it, in the order the
 * volume first knew them: inside the Use Context, counted among the files that
 * wear its tags, or on the whole volume — the files wearing a tag of `without`
 * left uncounted, as a query leaves them out. A tag no file wears — a
 * process's, a Touch's — is not listed. The list and the text of every Tag are
 * one heap block the caller frees (NULL when nothing is listed). 0, or a
 * negative -error_t. */
int tag_list(const char *without, TagListed **out, size_t *count);
int tag_list_everywhere(const char *without, TagListed **out, size_t *count);

/* Every file called `filename`, inside the Use Context, but those wearing a
 * tag of `without` (as query). Returns how many there are; the first `max` of
 * them are written to file_ids (and out_infos, when given). Asked with max ==
 * 0 it only counts. */
int find_file_by_name(const char* filename, const char* without, file_id_t* file_ids,
                      file_info_t* out_infos, size_t max);

/* Snapshots. On a format-v1 volume every one of these four answers
 * -ERR_UNSUPPORTED and fills in nothing: a snapshot there could be taken and
 * never read, and snapshots arrive with format v2. The kernel says so once a
 * boot, naming the request. */
int snap_create(const char *name, file_id_t file_id, uint32_t *out_snap_id);
int snap_delete(uint32_t snap_id);
int snap_list(uint32_t *out_ids, uint32_t max_ids, uint32_t *out_count);

/* A snapshot's record, as snap_info would fill it. */
typedef struct {
    uint32_t id;
    file_id_t parent_file_id;
    uint64_t created_time;
    uint32_t file_count;
    uint64_t total_size;
    uint8_t  flags;
    char     name[32];
} snap_info_t;

int snap_info(uint32_t snap_id, snap_info_t *out);

/*
 * anchor() — durability primitive. Returns once every change made before it
 * is on the medium, in a place the volume never writes again, behind a kept
 * cache flush. The volume settles changes on its own within five seconds
 * (the mount says so); anchor is the word that makes it now and for good.
 *
 * Two ways to use it:
 *   anchor(fid)        — explicit blocking call. POSIX-shaped.
 *   touch_claim("anchor", REST, 0, 0); ... fwrite(...); anchor(fid);
 *                       — observers subscribed on "anchor" wake up
 *                         with payload {file_id, op=2, ...}, plus on
 *                         every tag of the anchored file. So a
 *                         monitor app can confirm durability without
 *                         polling.
 *
 * file_id == 0 anchors the whole filesystem and publishes only the
 * generic "anchor" event (no per-tag fan-out).
 */
int anchor(file_id_t file_id);

/*
 * write_settled() — waits, without asking for anything to be published, until
 * the file's changes so far are settled on the medium in a place the volume
 * never writes again (file_id == 0: every change of the volume so far). At
 * once when they already are. A refusal of the medium comes back as its code:
 * ERR_TAGFS_READ_ONLY for a medium that takes no write, ERR_DEVICE_NOT_READY
 * for one that left.
 */
int write_settled(file_id_t file_id);

/*
 * escheat() — Escheat: what a format-v2 book can never give back let go for
 * good (storage.escheat). Every FILES range of the Tally that no copy of its
 * node reads: its files declared lost, their ids never given again, the blocks
 * held for them freed once the next Tally — written whole — names none of it.
 * Only a program wearing system or god may ask: any other is refused
 * (ERR_ACCESS_DENIED, ERR_PERMISSION_DENIED). Nothing to let go:
 * ERR_OBJECT_NOT_FOUND. What it lets go comes back in *said.
 */
typedef struct {
    uint64_t ranges;     /* rotten ranges of FILES let go */
    uint64_t files;      /* files declared lost */
    uint64_t blocks;     /* blocks held for them, let go */
    uint64_t tails;          /* tails of Slips lost */
    uint64_t digest_ranges;  /* DIGESTS ranges — a leaf, or an inner node's whole range — known to read in
                              * no copy, laid anew of the data */
} EscheatAnswer;
int escheat(EscheatAnswer* said);

#ifdef __cplusplus
}
#endif

#endif // BOX_STORAGE_H
