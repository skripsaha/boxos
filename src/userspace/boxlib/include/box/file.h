#ifndef BOX_STORAGE_H
#define BOX_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

/*
 * Whose tags a call speaks — the first argument of every call below that
 * speaks tags:
 *
 *   MY_TAGS      the program's own: its words for its own files, which no
 *                other program meets — a game's `save` is never the person's
 *                `save`. Every program started from a file has them without
 *                asking; one started from bytes held in memory has none
 *                (-ERR_TAGFS_NO_OWN_TAGS). A signed program is one program in
 *                every file of it — its author and its name, as its box's
 *                signature proves them —, so a copy and a new version of it
 *                have its tags, which leave with the last file of it.
 *   PERSON_TAGS  the person's: the labels the person finds files by, and the
 *                words of the Use Context. A program sees them only when its
 *                box asks `see-tags`, and changes them only when it asks
 *                `change-tags` (-ERR_ACCESS_DENIED otherwise).
 *   PROGRAM_TAGS(program)
 *                another program's own, the program named by a file of it: seen
 *                when it shows them to this one (show_tags_to below) or when
 *                this one's box asks `see-tags` or `change-tags` — the person
 *                sees every tag on the person's files —, taken off a file
 *                only when it asks `change-tags` — the person's hands —, and
 *                put on by nobody but that program (-ERR_ACCESS_DENIED
 *                otherwise). This program named so is MY_TAGS.
 *   ALL_TAGS     every tag this program may see, each with whose it is (the
 *                owner beside it): its own, those of the programs that show it
 *                theirs, and with `see-tags` or `change-tags` the person's and
 *                every program's. For file_info, FileMetadataRead and tag_list
 *                alone: a question, a create or a change is of one owner's
 *                tags (-ERR_INVALID_ARGUMENT).
 *
 * A call that names no tag at all — a name looked up alone — asks nothing,
 * whichever it says. (boxos_decks.h, STORAGE_TAGS_*.)
 */
typedef struct {
    uint8_t   whose;      /* the deck's whose byte (boxos_decks.h, STORAGE_TAGS_*) */
    file_id_t program;    /* PROGRAM_TAGS' program; 0 for the rest */
} TagsOf;

#ifdef __cplusplus
#define TAGS_OF_(whose, program) (TagsOf{ (uint8_t)(whose), (file_id_t)(program) })
#else
#define TAGS_OF_(whose, program) ((TagsOf){ (uint8_t)(whose), (file_id_t)(program) })
#endif
#define MY_TAGS                TAGS_OF_(0, 0)
#define PERSON_TAGS            TAGS_OF_(1, 0)
#define PROGRAM_TAGS(program)  TAGS_OF_(2, (program))
#define ALL_TAGS               TAGS_OF_(3, 0)

/* Whose a tag is, in the answers that say it: 0 the person's, a program's
 * file id that program's own. */
#define PERSON_OWNS            ((file_id_t)0)

typedef struct {
    uint8_t   type;      // 0=user, 1=system
    char      key[11];
    char      value[12];
    file_id_t owner;     // whose it is: PERSON_OWNS, or the file of the program whose own it is
} tag_t;

/*
 * What a file is, as the system says it: what it keeps the file as (role), and
 * the file's first bytes (head) — whoever knows what they mean tells from them
 * what the file is: a box begins with its magic, a font with its tag, an
 * itinerary with its line. The system keeps them exact through every change of
 * what the file holds and knows no format; files_starting_with asks which files
 * begin so, and no file is read for it.
 */
#define FILE_HEAD_MOST      16u   /* the first bytes the system keeps of a file: all of a shorter one */
#define FILE_ROLE_ORDINARY  0u
#define FILE_ROLE_SYSTEM    1u
#define FILE_ROLE_BOOT      2u
#define FILE_ROLE_KERNEL    3u

typedef struct {
    file_id_t file_id;
    uint64_t size;
    uint8_t tag_count;
    char filename[32];
    tag_t tags[5];
    uint64_t deleted_until;   // the wall clock's nanoseconds a deleted file is kept until; 0 for one that is not
    file_id_t made_by;        // the file of the program that made this file; 0 for none (it may be gone since)
    uint8_t role;             // what the system keeps it as: FILE_ROLE_*
    uint8_t head_len;         // how many first bytes head holds: min(size, FILE_HEAD_MOST)
    uint8_t head[FILE_HEAD_MOST];
} file_info_t;

/* Full metadata for tools that must preserve a file exactly.  file_info_t is
 * deliberately a small display structure and may truncate names or tags;
 * this type owns complete copies returned by STORAGE_OBJ_GET_INFO.
 *
 * Filename and Tags are heap-owned by the value.  Tags is a comma-separated
 * list of complete tag spellings, or NULL when the file has no tags.  The
 * caller releases them with FileMetadataRelease(). */
typedef struct {
    file_id_t FileId;
    uint64_t  Size;
    uint32_t  TagCount;
    char     *Filename;
    char     *Tags;
    file_id_t *Owners;        /* whose each tag of Tags is, in its order (tag_t.owner); NULL with none */
    uint64_t  KeptUntil;      /* the wall clock's nanoseconds a deleted file is kept until; 0 for one that is not */
    file_id_t MadeBy;         /* the file of the program that made this file; 0 for none (it may be gone since) */
    uint8_t   Role;           /* what the system keeps it as: FILE_ROLE_* */
    uint8_t   HeadLength;     /* how many first bytes Head holds: min(Size, FILE_HEAD_MOST) */
    uint8_t   Head[FILE_HEAD_MOST];
} FileMetadata;

/* Read the complete self-describing metadata record — the tags `Whose` of
 * them the file wears.  The response buffer is grown until the kernel can
 * return every byte; no field is truncated. */
int FileMetadataRead(TagsOf Whose, file_id_t FileId, FileMetadata *Out);

/* Release all heap-owned members and reset Out to its empty state. */
void FileMetadataRelease(FileMetadata *Metadata);

/* Whether the file wears a tag whose key is `Key`, bare or with a value —
 * "note", "log:file" — among the tags of the owner `Metadata` was read with: a
 * program's own words on its own files (the notes finding notes), or the
 * person's labels for a program that works on them. What a file IS — a
 * program, a settings file, kept by the system — is never read from its tags:
 * it is told by its first bytes (Head). */
bool FileMetadataWears(const FileMetadata *Metadata, const char *Key);

/*
 * In the person's tags, inside the Use Context by default. When the user has
 * said `use code cpp`, query(PERSON_TAGS, "project", ...) asks for files that
 * are code AND cpp AND project, and create(PERSON_TAGS, ...) stamps the new
 * file code and cpp along with the tags it was given. The _everywhere
 * spellings ask the same of the whole volume, the user's context left out. A
 * call in MY_TAGS is the program's own and asked of the whole volume always:
 * the Use Context is the person's. See box/use.h.
 *
 * create answers the new file's id (> 0), or a negative -error_t — one int64
 * the way fread answers a count, so a whole 64-bit id is never cut to an int.
 */
int64_t create(TagsOf whose, const char* filename, const char* tags);
int64_t create_everywhere(TagsOf whose, const char* filename, const char* tags);
/* The same as create, on the volume numbered `volume` (VolumeListed) — create
 * makes its file on the home volume, number 0. */
int64_t create_on(uint32_t volume, TagsOf whose, const char* filename, const char* tags);

/*
 * The volumes of the house: every BoxOS volume the machine holds — the home
 * one, number 0, the volume it was booted from, and every other one met on any
 * medium since, numbered as it was first met. A query finds the files of all
 * of them; a file's id carries the number of the volume it lies on
 * (volume_of). volume_list answers how many there are and lays them out in
 * one block — *out, free() it whole — with their texts after them; volume_info
 * the one numbered `number`, in a block of its own. 0, or a negative -error_t.
 */
typedef struct {
    uint32_t    Number;
    bool        Home;       /* the volume the machine was booted from */
    bool        Mounted;    /* its files are found; the numbers below are its */
    bool        ReadOnly;   /* it takes no change */
    bool        Away;       /* its medium left, and the volume waits for it */
    bool        Removable;  /* on a removable medium */
    uint8_t     Uuid[16];
    uint64_t    Bytes;
    uint64_t    FreeBytes;
    uint64_t    Files;
    const char *Name;       /* the name a person gave it, "" when nobody has */
    const char *Medium;     /* what its medium calls itself */
} VolumeListed;
int volume_list(VolumeListed **out, size_t *count);
int volume_info(uint32_t number, VolumeListed **out);
/* The volume as a person calls it, into `out` (`room` bytes, NUL-ended): the
 * name they gave it, or — with none — what it is on and how big it is
 * ("QEMU HARDDISK, 64 MB"). size_shown: `bytes` said the way a medium's maker
 * counts them, in thousands ("32 GB"). The bytes written. */
size_t volume_shown(const VolumeListed *v, char *out, size_t room);
size_t size_shown(uint64_t bytes, char *out, size_t room);
/* The name a person gives the volume numbered `number` — UTF-8 of any
 * language, at most STORAGE_VOLUME_NAME_MOST bytes, no control character;
 * NULL or "" takes it away. Kept in the volume's book, it goes with the
 * volume; with none, the volume is shown by its medium. Asked by the person's
 * hands (change-tags in the box), answered once the volume holds it: 0, or a
 * negative -error_t — -ERR_TAGFS_NO_SPACE for a name the volume had no room
 * for, the old one kept. */
int volume_rename(uint32_t number, const char *name);
/* The `count` files of `ids` carried to the volume numbered `volume` whole —
 * their bytes, name, the person's tags and the programs' own, the programs
 * they show their tags to, a deleted one's term, the program that made them,
 * the passport of the program they hold, their times. files_copy_to leaves
 * them where they were too; files_move_to lets each go there once its copy
 * is the other volume's for good, and leaves one that changed meanwhile
 * where it was, beside its copy. `ids` go in their order — a program before
 * the files that name it. `copies` (room for `count`, or NULL) takes the
 * copies' ids, *carried how many arrived (the first refused ends the call),
 * *dropped the programs' words left behind where the other volume does not
 * know the program; either NULL when not wanted. Asked by the person's hands
 * (change-tags in the box). 0 when every file arrived, or a negative -error_t:
 * the first refusal. */
int files_copy_to(uint32_t volume, const file_id_t *ids, size_t count, file_id_t *copies, size_t *carried,
                  uint32_t *dropped);
int files_move_to(uint32_t volume, const file_id_t *ids, size_t count, file_id_t *copies, size_t *carried,
                  uint32_t *dropped);
static inline uint32_t volume_of(file_id_t file)
{
    return (uint32_t)((file >> STORAGE_VOLUME_FILE_SHIFT) & 0x7FFFu);
}
/* Of `count` files, the ones on the nearest volume among them — the home
 * volume when any lies there, else the one numbered lowest — kept at the
 * front in their order: how many. A name a program of the machine's own
 * volume answers to is that program's, whatever a stick carries of the same
 * name; only files of one volume are ever a choice to ask about. */
size_t files_home_first(file_id_t *files, size_t count);
/* The files that wear every tag of `tags` (every file, for NULL or "") and
 * none of `without` (a comma list too; NULL or "" sets nothing aside). The
 * volume sets aside no file of its own accord but the deleted ones — which
 * these never meet; query_deleted is the one question that does — so each
 * asker says what else, if anything, it leaves out.
 *
 * query writes up to max_files ids and returns how many it wrote. A caller
 * that wants every match asks query_all: it learns the count from the kernel,
 * hands back a malloc'd list of that many ids (NULL when there are none) and
 * returns the count; the caller frees the list. */
int query(TagsOf whose, const char* tags, const char* without, file_id_t* file_ids, size_t max_files);
int query_everywhere(TagsOf whose, const char* tags, const char* without, file_id_t* file_ids, size_t max_files);
int query_all(TagsOf whose, const char* tags, const char* without, file_id_t** out_ids);
int query_all_everywhere(TagsOf whose, const char* tags, const char* without, file_id_t** out_ids);
/* The same question asked of the files kept deleted (file_keep) alone, over the
 * whole volume — the Use Context left out. */
int query_deleted(TagsOf whose, const char* tags, const char* without, file_id_t* file_ids, size_t max_files);
int query_all_deleted(TagsOf whose, const char* tags, const char* without, file_id_t** out_ids);
/* The files called `word` — a file's name is a field of its card and no tag, so
 * the tag questions never find one by it. A word matches a file when it is the
 * whole of the file's name or its stem, the name up to its last dot (`slot3`
 * finds slot3.sav; `slot3.sav` finds it too), byte for byte: the word is not
 * spelled as a tag, so a ':' in it is a byte like any other. As query_all,
 * only those wearing every tag of `tags` (NULL or "" for none), inside the Use
 * Context or everywhere, those wearing a tag of `without` left out, the
 * deleted never met; the count, and a malloc'd list the caller frees (NULL for
 * none), or a negative -error_t — -ERR_INVALID_ARGUMENT for an empty word. */
int query_name_all(TagsOf whose, const char* word, const char* tags, const char* without, file_id_t** out_ids);
int query_name_all_everywhere(TagsOf whose, const char* word, const char* tags, const char* without,
                              file_id_t** out_ids);
/* The files a typed word finds: those that wear it as a tag of `whose`, and
 * those that are called it — a search looks at both the labels and the name,
 * so `slot3` finds the file tagged slot3 and slot3.sav alike. Inside the Use
 * Context; each file once, in file id order; the count and a malloc'd list the
 * caller frees (NULL for none), or a negative -error_t. */
int query_word_all(TagsOf whose, const char* word, file_id_t** out_ids);
/* The files that begin with `length` bytes at `bytes` (1 to FILE_HEAD_MOST) —
 * the box magic, a font's tag, "itinerary 1\n" — each in file id order, the
 * deleted never met; no file is read, and nothing is asked of anybody. The
 * count and a malloc'd list the caller frees (NULL for none), or a negative
 * -error_t — -ERR_INVALID_ARGUMENT for no bytes or more than FILE_HEAD_MOST. */
int files_starting_with(const void* bytes, size_t length, file_id_t** out_ids);
/* The same questions asked of the volume numbered `volume` (VolumeListed)
 * alone — a search narrowed to one volume of the house; the ones above ask
 * every volume. -ERR_OBJECT_NOT_FOUND for a number the house gave none. */
int query_all_on(uint32_t volume, TagsOf whose, const char* tags, const char* without, file_id_t** out_ids);
int query_word_all_on(uint32_t volume, TagsOf whose, const char* word, file_id_t** out_ids);
int files_starting_with_on(uint32_t volume, const void* bytes, size_t length, file_id_t** out_ids);
/* A file's card: its name, size and the tags `whose` of them it wears. */
int file_info(TagsOf whose, file_id_t file_id, file_info_t* info);
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
/* A file deleted for a term: no query meets it, it can be brought back by
 * file_restore until the term ends, and the volume lets it go by itself then.
 * The term is the caller's — in seconds, from now — and the volume has none of
 * its own; 0 gives the file up at once, as `delete` does. Guarded as `delete`
 * is: a file the system keeps is refused (-ERR_PERMISSION_DENIED). Whoever
 * deletes warns the person first, if he is to be warned. file_restore: the file found again, its name
 * and tags as they were (-ERR_FILE_NOT_FOUND once its term has ended). */
int file_keep(file_id_t file_id, uint64_t term_seconds);
int file_restore(file_id_t file_id);
int file_rename(file_id_t file_id, const char* new_filename);

/* The file made exactly `size` bytes long. Shorter: the tail is cut and its
 * blocks given back. Longer: the bytes from the old size on read as zeros —
 * never another file's. A write grows a file only at its end, and one that
 * begins past it is refused -ERR_TAGFS_BEYOND_SIZE: a gap is made by span.
 * flags STORAGE_SPAN_GROW (box/types.h): grown to `size` only if it is
 * shorter, never cut — the way to make room for a write past the end without
 * cutting back what another writer added meanwhile; 0 for exactly `size`.
 * Refused on a file the system keeps in a role (-ERR_PERMISSION_DENIED), like delete; a
 * growth past every block the volume could give, -ERR_TAGFS_NO_SPACE.
 * Returns 0 or a negative -error_t.
 *
 * Publishes the change on every tag of the file — the same 40-byte payload
 * shape as a write, with op = 3, offset = where the file changed (the smaller
 * of the old and the new size), bytes = how many were cut or added. */
int file_span(file_id_t file_id, uint64_t size, uint32_t flags);

/* A tag of `whose` put on a file, or taken off — "key" every one of the key,
 * "key:value" that one; never another owner's of the same text. */
int tag_add(TagsOf whose, file_id_t file_id, const char* tag);
int tag_remove(TagsOf whose, file_id_t file_id, const char* key);

/* A tag files wear, and how many of them wear it: one entry of the house's
 * list of its tags — each volume's own (storage.tag.list), a tag of one owner
 * worn on several volumes once, their files summed — not a walk of files. */
typedef struct {
    char     *Tag;      /* "key" or "key:value", as the volume spells it */
    uint32_t  Files;    /* how many files wear it — inside the Use Context, of its files */
    bool      System;   /* a tag the system keeps */
    file_id_t Owner;    /* whose it is: PERSON_OWNS, or the file of the program whose own it is */
} TagListed;

/* Every tag of `whose` a file wears, each with how many files wear it, in the
 * order the volume first knew them: inside the Use Context, counted among the files that
 * wear its tags, or on the whole volume — the files wearing a tag of `without`
 * left uncounted, as a query leaves them out. A tag no file wears — a
 * process's, a Touch's — is not listed. The list and the text of every Tag are
 * one heap block the caller frees (NULL when nothing is listed). 0, or a
 * negative -error_t. */
int tag_list(TagsOf whose, const char *without, TagListed **out, size_t *count);
int tag_list_everywhere(TagsOf whose, const char *without, TagListed **out, size_t *count);

/*
 * A program's own tags shown to other programs. This program shows every tag
 * of its own — those to come too — to a program named by its file, or to
 * EVERY_PROGRAM; one shown them sees them on files, finds files by them and
 * lists them (PROGRAM_TAGS), and never puts them on nor takes them off.
 * hide_tags_from takes a program — or EVERY_PROGRAM, the ones named staying —
 * out of the list. ask_to_see_tags asks a program to show this one its tags:
 * a note the system holds until that program answers — shows or hides them
 * —, either leaves, or the volume is mounted again; when a note is left, the
 * Touch word "tags-asked" says so, its payload the file_id_t of the program
 * asked and nothing of who asked, so a program that wants to hear its asks
 * claims the word (touch_claim) and reads asks_to_see_tags when its own id is
 * said: the word is said once for every file of the program asked, so each
 * running copy hears the file it was started from (cabin_info()->
 * program_file). Every list names a program by one file of it — a signed
 * program in many files by the same one. The list is the program's, kept on
 * the cards of its files: one started from bytes has none
 * (-ERR_TAGFS_NO_OWN_TAGS); a program the volume has not is
 * -ERR_FILE_NOT_FOUND, one shown or asked to show to itself
 * -ERR_INVALID_ARGUMENT. 0, or a negative -error_t. A program that left is
 * taken off every list by the volume.
 */
#define EVERY_PROGRAM          ((file_id_t)0)
int show_tags_to(file_id_t program);
int hide_tags_from(file_id_t program);
int ask_to_see_tags(file_id_t program);

/* The programs that show this one their tags (by name or to every program);
 * the programs `program` shows its tags to, EVERY_PROGRAM first — asked by
 * that program itself or by one whose box asks `see-tags` or `change-tags`;
 * the programs that asked this one and are not answered. The count and a
 * malloc'd list the caller frees (NULL for none), or a negative -error_t. */
int tags_shown_to_me(file_id_t **out_programs);
int tags_shown_by(file_id_t program, file_id_t **out_programs);
int asks_to_see_tags(file_id_t **out_programs);

/* Every file named `filename` whole, but those wearing a tag of `whose` of
 * `without` (as query) — inside the Use Context for the person's. Returns how many there are; the first `max` of
 * them are written to file_ids (and out_infos, when given). Asked with max ==
 * 0 it only counts. */
int find_file_by_name(TagsOf whose, const char* filename, const char* without, file_id_t* file_ids,
                      file_info_t* out_infos, size_t max);

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
 *                         every tag of the anchored file and on the
 *                         words its name answers to. So a
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
 * escheat() — Escheat: what a book can never give back let go for
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

/*
 * conjugate() — Conjugate: `count` blocks of file `fid` from its block `at`
 * laid on the blocks of file `onto` from its block `onto_at` that hold them
 * byte for byte (storage.conjugate) — named by a run SHARED on that file's
 * ground, `fid`'s own blocks let go: the volume holds the bytes once, and both
 * files read what they read before. Every block alike or none laid. Only a
 * program wearing system or god may ask (ERR_ACCESS_DENIED,
 * ERR_PERMISSION_DENIED); a file the system keeps is never laid, and the
 * kernel's file neither laid nor laid on.
 * The verdict comes back in *said whatever the answer — `why` as
 * boxos_decks.h names it for obj.conjugate.
 */
typedef struct {
    uint64_t blocks;     /* blocks of `fid` laid: `count`, or 0 */
    uint64_t at;         /* the block of `fid` a refusal met */
    uint64_t why;        /* 0 laid, or why not (boxos_decks.h, STORAGE_OBJ_CONJUGATE) */
} ConjugateAnswer;
int conjugate(file_id_t fid, uint64_t at, file_id_t onto, uint64_t onto_at, uint64_t count, ConjugateAnswer* said);

#ifdef __cplusplus
}
#endif

#endif // BOX_STORAGE_H
