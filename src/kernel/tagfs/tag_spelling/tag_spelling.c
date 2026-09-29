#include "tag_spelling.h"
#include "klib.h"

static bool TagBlank(char c)
{
    return c == ' ' || c == '\t';
}

error_t TagSpellingFits(size_t key_len, size_t value_len)
{
    if (key_len > TAG_KEY_WIDTH)
        return ERR_TAGFS_TAG_TOO_LONG;
    if (value_len > TAGFS_REGISTRY_DATA_SIZE - TAGFS_REGISTRY_RECORD_HEAD - key_len)
        return ERR_TAGFS_TAG_TOO_LONG;
    return OK;
}

/* One tag out of the n bytes at p, which hold no NUL: trimmed, split at the
 * first ':' when `split`, and judged. */
static error_t TagSpellSpan(const char *p, size_t n, bool split, TagSpelling *out)
{
    while (n > 0 && TagBlank(p[0])) {
        p++;
        n--;
    }
    while (n > 0 && TagBlank(p[n - 1]))
        n--;

    const char *colon     = split ? memchr(p, ':', n) : NULL;
    size_t      key_len   = colon ? (size_t)(colon - p) : n;
    size_t      value_len = colon ? n - key_len - 1 : 0;
    if (key_len == 0)
        return ERR_TAGFS_INVALID_TAG;
    error_t fits = TagSpellingFits(key_len, value_len);
    if (fits != OK)
        return fits;

    out->key       = p;
    out->key_len   = key_len;
    out->value     = value_len ? colon + 1 : NULL;
    out->value_len = value_len;
    return OK;
}

static error_t TagSpellText(const char *text, size_t len, bool split, TagSpelling *out)
{
    if (!text || !out)
        return ERR_TAGFS_INVALID_TAG;
    return TagSpellSpan(text, strnlen(text, len), split, out);
}

error_t TagSpell(const char *text, size_t len, TagSpelling *out)
{
    return TagSpellText(text, len, true, out);
}

error_t TagSpellKey(const char *text, size_t len, TagSpelling *out)
{
    return TagSpellText(text, len, false, out);
}

error_t TagListNext(const char *list, size_t len, size_t *at, TagSpelling *out)
{
    out->key       = NULL;
    out->value     = NULL;
    out->key_len   = 0;
    out->value_len = 0;
    if (!list)
        return OK;

    while (*at < len && list[*at] != '\0') {
        size_t start = *at;
        size_t stop  = start;
        while (stop < len && list[stop] != ',' && list[stop] != '\0')
            stop++;
        *at = (stop < len && list[stop] == ',') ? stop + 1 : stop;

        size_t blank = 0;
        while (start + blank < stop && TagBlank(list[start + blank]))
            blank++;
        if (start + blank == stop)
            continue;
        return TagSpellSpan(list + start, stop - start, true, out);
    }
    return OK;
}

error_t TagListCount(const char *list, size_t len, uint32_t *out_count)
{
    TagSpelling tag;
    size_t      at    = 0;
    uint32_t    count = 0;

    *out_count = 0;
    for (;;) {
        error_t rc = TagListNext(list, len, &at, &tag);
        if (rc != OK)
            return rc;
        if (!tag.key)
            break;
        count++;
    }
    *out_count = count;
    return OK;
}

bool TagSpellingIsWildcard(const TagSpelling *tag)
{
    return tag->value_len == 3 && memcmp(tag->value, "...", 3) == 0;
}

/* The first "..." of the n bytes at p, NULL when there is none. */
static const char *TagEllipsis(const char *p, size_t n)
{
    for (size_t i = 0; i + 3 <= n; i++)
        if (p[i] == '.' && p[i + 1] == '.' && p[i + 2] == '.')
            return p + i;
    return NULL;
}

/* One part of a tag, its key or its value, against the same part of a
 * pattern: equal, or — when the pattern's part holds "..." — the text begins
 * with what comes before it and ends with what comes after. */
static bool TagPartMatches(const char *pat, size_t plen, const char *text, size_t tlen)
{
    const char *wild = TagEllipsis(pat, plen);
    if (!wild)
        return plen == tlen && memcmp(pat, text, plen) == 0;

    size_t prefix = (size_t)(wild - pat);
    size_t suffix = plen - prefix - 3;
    return tlen >= prefix + suffix &&
           memcmp(pat, text, prefix) == 0 &&
           memcmp(wild + 3, text + tlen - suffix, suffix) == 0;
}

bool TagSpellingIsPattern(const TagSpelling *tag)
{
    return TagEllipsis(tag->key, tag->key_len) ||
           (tag->value && TagEllipsis(tag->value, tag->value_len));
}

bool TagPatternMatches(const TagSpelling *pattern, const TagSpelling *tag)
{
    if (!pattern->value != !tag->value)
        return false;
    if (!TagPartMatches(pattern->key, pattern->key_len, tag->key, tag->key_len))
        return false;
    return !pattern->value ||
           TagPartMatches(pattern->value, pattern->value_len, tag->value, tag->value_len);
}

uint32_t TagSpellingHash(const TagSpelling *tag)
{
    uint32_t hash = 5381;
    for (size_t i = 0; i < tag->key_len; i++)
        hash = ((hash << 5) + hash) + (uint8_t)tag->key[i];
    if (tag->value) {
        hash = ((hash << 5) + hash) + ':';
        for (size_t i = 0; i < tag->value_len; i++)
            hash = ((hash << 5) + hash) + (uint8_t)tag->value[i];
    }
    return hash;
}

bool TagSpanIs(const char *text, const char *span, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (text[i] != span[i])
            return false;
    return text[len] == '\0';
}

char *TagSpanDup(const char *span, size_t len)
{
    char *copy = kmalloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, span, len);
    copy[len] = '\0';
    return copy;
}

size_t TagSpellingTextLength(const TagSpelling *tag)
{
    return tag->key_len + (tag->value ? 1 + tag->value_len : 0);
}

void TagSpellingText(const TagSpelling *tag, char *out)
{
    memcpy(out, tag->key, tag->key_len);
    size_t at = tag->key_len;
    if (tag->value) {
        out[at++] = ':';
        memcpy(out + at, tag->value, tag->value_len);
        at += tag->value_len;
    }
    out[at] = '\0';
}

bool TagSpellingMatchesText(const TagSpelling *tag, const char *text)
{
    for (size_t i = 0; i < tag->key_len; i++)
        if (text[i] != tag->key[i])
            return false;
    if (!tag->value)
        return text[tag->key_len] == '\0';
    return text[tag->key_len] == ':' &&
           TagSpanIs(text + tag->key_len + 1, tag->value, tag->value_len);
}
