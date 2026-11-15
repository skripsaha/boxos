#ifndef TAG_SPELLING_H
#define TAG_SPELLING_H

#include "ktypes.h"
#include "error.h"
#include "../tagfs_constants.h"

/*
 * ‼ ONE SPELLING OF A TAG.
 *
 * A tag is text, "key" or "key:value", and every station that was handed one
 * used to read it its own way, into its own fixed buffer: 31 bytes in a query,
 * 63 in `tag add`, 127, 255, 511 elsewhere, each cutting silently where its
 * buffer ended. A query of 256 bytes was dropped unread and answered with every
 * file, a `tag add` of 70 bytes put a different, shorter tag on the process,
 * and a create whose tags did not fit made the file without them.
 *
 * There is one reading now, and it reads in place: a spelling is two spans
 * into the text it was given, nothing is copied, and a tag the volume could
 * not hold is refused out loud before anything is allocated for it.
 *
 * The rules, the same for every station:
 *   - the text ends at its length or at a NUL, whichever comes first;
 *   - blanks (space, tab) around a tag are not part of it;
 *   - the first ':' splits the key from the value, and "key:" is "key";
 *   - an empty key is no tag: ERR_TAGFS_INVALID_TAG;
 *   - the key is at most TAG_KEY_WIDTH bytes (the book's u8 key_len), and the
 *     key and the value together at most TAG_KEY_VALUE_WIDTH: otherwise
 *     ERR_TAGFS_TAG_TOO_LONG;
 *   - in a list, tags are separated by ',' and a blank element is skipped.
 */

#define TAG_KEY_WIDTH        255u

/* The bytes of one tag's key and value together — what every station, the
 * book's TAGS and a stitch's Coined hold a tag to (mount/tally_leaves.c,
 * settle/endpaper_fact.h). */
#define TAG_KEY_VALUE_WIDTH  4074u

/* The longest text of one tag: key, ':' and value. A station that must copy
 * one tag out of a program's memory judges the text's whole length against it
 * before it allocates, so blanks sent around the tag count there. */
#define TAG_TEXT_WIDTH       (TAG_KEY_VALUE_WIDTH + 1u)

typedef struct TagSpelling {
    const char *key;
    const char *value;      /* NULL: a tag without one */
    size_t      key_len;
    size_t      value_len;
} TagSpelling;

/* A tag's text. */
error_t TagSpell(const char *text, size_t len, TagSpelling *out);

/* A bare key: the text whole, never split at ':' — the stem of a file's name,
 * which is a key and may carry any byte a name may. */
error_t TagSpellKey(const char *text, size_t len, TagSpelling *out);

/* The next tag of a comma list, from *at on; *at moves past it. At the end of
 * the list OK with out->key NULL. A refusal names the first tag that does not
 * spell. */
error_t TagListNext(const char *list, size_t len, size_t *at, TagSpelling *out);

/* Every tag of a list spelled, and counted — the whole list judged before a
 * caller sizes anything by it. */
error_t TagListCount(const char *list, size_t len, uint32_t *out_count);

/* Whether a key and a value of these lengths make a tag the volume can hold. */
error_t TagSpellingFits(size_t key_len, size_t value_len);

/* "key:...": every tag of the key, not a tag of its own. */
bool    TagSpellingIsWildcard(const TagSpelling *tag);

/* A pattern of tags: "..." in its key or its value stands for any run of
 * bytes between what comes before it and what comes after. Whether a spelling
 * holds one, and whether `tag` is one the pattern names — a pattern with a
 * value names only tags with one, and a bare pattern only bare tags. */
bool    TagSpellingIsPattern(const TagSpelling *tag);
bool    TagPatternMatches(const TagSpelling *pattern, const TagSpelling *tag);

/* The djb2 of the key, and of ':' and the value when there is one — the one
 * hash both books of names, the volume's registry and the Logbook, keep. */
uint32_t TagSpellingHash(const TagSpelling *tag);

/* One order of all tags, for a list kept sorted and searched by halves: by
 * key, then a bare tag before its key's tags with a value, then by value;
 * bytewise, a shorter span before a longer one it begins. Zero exactly when
 * the two are one tag. */
int     TagSpellingCompare(const TagSpelling *a, const TagSpelling *b);

/* Whether NUL-terminated `text` is exactly the `len` bytes at `span`. */
bool    TagSpanIs(const char *text, const char *span, size_t len);

/* The span copied into the heap with a NUL after it; NULL when there was no
 * memory. */
char   *TagSpanDup(const char *span, size_t len);

/* The canonical text of a spelling, "key" or "key:value": its length, and the
 * text written into `out` (room for the length and a NUL). */
size_t  TagSpellingTextLength(const TagSpelling *tag);
void    TagSpellingText(const TagSpelling *tag, char *out);

/* The spelling of a key the kernel names itself, from a C string literal. The
 * widths are still the reader's to judge. */
static inline TagSpelling TagSpellingOfKey(const char *key)
{
    TagSpelling tag = { key, NULL, __builtin_strlen(key), 0 };
    return tag;
}

#endif /* TAG_SPELLING_H */
