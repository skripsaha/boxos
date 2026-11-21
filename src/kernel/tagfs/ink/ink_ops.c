#include "ink.h"
#include "../keep/keep.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../../core/use_context/use_context.h"

/*
 * The file operations every caller outside TagFS reaches — create, delete,
 * rename and tagging — each one Ink (ink.c), through the volume's door. What
 * an operation asks of the medium it asks before the Ink, with no TagFS lock
 * held: a tag a file is about to wear is written to the medium first, so no
 * record ever wears a tag the registry on the medium does not define.
 */

static TagRegistry* OpsRegistry(void)
{
    return tagfs_get_state()->registry;
}

// ----------------------------------------------------------------------------
// The name is a tag
//
// Every file carries the stem of its name — up to the last dot — as its first
// tag, so "who is called kernel" is a question for the index, not a walk of the
// volume. create puts the tag there and rename moves it, both through this one
// rule. The stem is a key, spelled by the one spelling (tag_spelling.h) but
// never split at ':', and it is read in place: a name is never cut to fit a
// buffer, so it never wears a shorter tag than itself.
// ----------------------------------------------------------------------------

/* The stem of a name as a key. OK with out->key NULL when the name has no
 * stem — ".profile", or nothing but blanks before the dot — and so no tag;
 * ERR_TAGFS_TAG_TOO_LONG for a stem longer than a key may be. */
error_t InkNameStem(const char* name, TagSpelling* out)
{
    size_t len  = strlen(name);
    size_t stem = len;
    for (size_t i = len; i > 0; i--) {
        if (name[i - 1] == '.') {
            stem = i - 1;
            break;
        }
    }
    error_t rc = TagSpellKey(name, stem, out);
    if (rc == ERR_TAGFS_INVALID_TAG) {
        out->key = NULL;
        return OK;
    }
    return rc;
}

error_t InkNameTagIntern(const char* name, uint32_t* out_tag)
{
    TagSpelling stem;
    *out_tag = TAGFS_INVALID_TAG_ID;
    error_t rc = InkNameStem(name, &stem);
    if (rc != OK || !stem.key)
        return rc;
    rc = TagRegistryIntern(OpsRegistry(), TagOwnerPerson(), &stem, out_tag);
    if (rc != OK)
        return rc;
    UseContextBindTag(&stem, *out_tag);
    return TagRegistryIdGiven(OpsRegistry(), *out_tag);
}

uint32_t InkNameTagLookup(const char* name)
{
    TagSpelling stem;
    if (!name || InkNameStem(name, &stem) != OK || !stem.key)
        return TAGFS_INVALID_TAG_ID;
    return TagRegistryLookup(OpsRegistry(), TagOwnerPerson(), &stem);
}

// ----------------------------------------------------------------------------
// The operations
// ----------------------------------------------------------------------------

/* The tags a new file wears: its name's first, then the caller's, none twice,
 * none invalid, each on the medium before the file wears it. *out is the
 * caller's to kfree. */
static error_t CreateTags(uint32_t name_tag, const uint32_t* tag_ids, uint32_t tag_count,
                          uint32_t** out, uint32_t* out_count)
{
    uint32_t* tags = kmalloc(sizeof(uint32_t) * ((uint64_t)tag_count + 1u));
    uint32_t  n    = 0;
    if (!tags)
        return ERR_NO_MEMORY;
    if (name_tag != TAGFS_INVALID_TAG_ID)
        tags[n++] = name_tag;
    for (uint32_t i = 0; tag_ids && i < tag_count; i++) {
        uint32_t tid = tag_ids[i];
        bool     dup = tid == TAGFS_INVALID_TAG_ID;
        for (uint32_t j = 0; j < n && !dup; j++)
            dup = tags[j] == tid;
        if (dup)
            continue;
        error_t rc = TagRegistryIdGiven(OpsRegistry(), tid);
        if (rc != OK) {
            kfree(tags);
            return rc;
        }
        tags[n++] = tid;
    }
    *out       = tags;
    *out_count = n;
    return OK;
}

static error_t CreateInside(const char* filename, const uint32_t* tag_ids, uint32_t tag_count,
                            uint64_t* out_file_id)
{
    /* A volume under glass makes no file, and no name of one is interned for
     * it: the refusal comes before anything is given out (tagfs.h) — and so
     * on a book whose ribbon has no room for the name's Coined
     * (InkBookRefuses). */
    if (TagFSIsUnderGlass())
        return ERR_TAGFS_READ_ONLY;
    uint32_t    name_tag = TAGFS_INVALID_TAG_ID;
    uint64_t    held     = 0;
    TagSpelling stem;
    error_t     rc = InkNameStem(filename, &stem);
    if (rc == OK)
        rc = InkBookRefuses(TagOwnerPerson(), &stem, stem.key ? 1u : 0u, &held);
    if (rc == OK)
        rc = InkNameTagIntern(filename, &name_tag);
    InkBookHeldLetGo(held);
    if (rc != OK)
        return rc;

    uint32_t*  tags  = NULL;
    uint32_t   count = 0;
    InkReceipt receipt;
    rc = CreateTags(name_tag, tag_ids, tag_count, &tags, &count);
    if (rc == OK)
        rc = InkNew(filename, tags, count, out_file_id, &receipt);
    kfree(tags);
    return rc;
}

int tagfs_create_file(const char* filename, const uint32_t* tag_ids, uint32_t tag_count,
                      uint64_t* out_file_id)
{
    if (!filename || !out_file_id)
        return -(int)ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = CreateInside(filename, tag_ids, tag_count, out_file_id);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

int tagfs_delete_file(uint64_t file_id, InkWarrant warrant)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = InkQuitclaim(file_id, warrant, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

int tagfs_keep_file(uint64_t file_id, uint64_t term_seconds, InkWarrant warrant)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = term_seconds ? InkKeep(file_id, KeepUntil(term_seconds), warrant, &receipt)
                                 : InkQuitclaim(file_id, warrant, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

int tagfs_restore_file(uint64_t file_id, InkWarrant warrant)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt  receipt;
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, file_id, NULL);
    error_t     rc  = !rec ? ERR_FILE_NOT_FOUND : OK;
    if (rc == OK && (rec->flags & TAGFS_FILE_DELETED) && rec->keep_until <= KeepNowNs())
        rc = ERR_FILE_NOT_FOUND;
    if (rec)
        FileRecordRelease(rec);
    if (rc == OK)
        rc = InkKeep(file_id, 0, warrant, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

/* The tag follows the name: a rename that changed the string and left the tag
 * made the file visible under its old name and invisible under its new one —
 * by-name lookup and proc_exec ask the index, not the string. */
int tagfs_rename_file(uint64_t file_id, const char* new_filename)
{
    if (!new_filename)
        return -(int)ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    uint32_t    name_tag = TAGFS_INVALID_TAG_ID;
    uint64_t    held     = 0;
    InkReceipt  receipt;
    TagSpelling stem;
    error_t     rc = InkNameStem(new_filename, &stem);
    if (rc == OK)
        rc = InkBookRefuses(TagOwnerPerson(), &stem, stem.key ? 1u : 0u, &held);
    if (rc == OK)
        rc = InkNameTagIntern(new_filename, &name_tag);
    InkBookHeldLetGo(held);
    if (rc == OK)
        rc = InkFields(file_id, new_filename, name_tag, TAGFS_INVALID_TAG_ID, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

/* A tag put on takes none off. */
int tagfs_add_tag(uint64_t file_id, uint32_t tag_id)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = tag_id == TAGFS_INVALID_TAG_ID ? ERR_TAGFS_INVALID_TAG
                                                   : TagRegistryIdGiven(OpsRegistry(), tag_id);
    if (rc == OK)
        rc = InkFields(file_id, NULL, tag_id, TAGFS_INVALID_TAG_ID, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

int tagfs_remove_tag(uint64_t file_id, uint32_t tag_id)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = InkFields(file_id, NULL, TAGFS_INVALID_TAG_ID, tag_id, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

bool tagfs_has_tag(uint64_t file_id, uint32_t tag_id)
{
    if (!tagfs_enter())
        return false;
    bool has = TagBitmapHas(tagfs_get_state()->bitmap_index, tag_id, file_id);
    tagfs_leave();
    return has;
}

/* The id is the registry's of this mounting, and is worn inside the same stay
 * behind the door: a volume read again between the two would number another
 * tag with it. Only the person's tags are words of the Use Context, so only
 * they are bound to it. */
error_t tagfs_add_tag_string(TagOwner owner, uint64_t file_id, const TagSpelling* tag)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    uint32_t tag_id;
    uint64_t held = 0;
    error_t  rc   = InkBookRefuses(owner, tag, 1, &held);
    if (rc == OK)
        rc = TagRegistryIntern(OpsRegistry(), owner, tag, &tag_id);
    InkBookHeldLetGo(held);
    if (rc == OK) {
        if (owner.kind == TAG_OWNER_PERSON)
            UseContextBindTag(tag, tag_id);
        int added = tagfs_add_tag(file_id, tag_id);
        rc        = added == 0 ? OK : (error_t)-added;
    }
    tagfs_leave();
    return rc;
}

/* Whether the file's tag `tag_id` is what `owner`'s `tag` names: that exact
 * tag when it has a value, and any of `owner`'s tags of its key when it is
 * bare — never another owner's, whatever its text. */
static bool TagNamedBy(uint32_t tag_id, TagOwner owner, const TagSpelling* tag)
{
    TagSpelling worn;
    TagOwner    whose;
    if (!TagRegistrySpelling(OpsRegistry(), tag_id, &worn, NULL, &whose) || !TagOwnerSame(whose, owner))
        return false;
    if (worn.key_len != tag->key_len || memcmp(worn.key, tag->key, tag->key_len) != 0)
        return false;
    return !tag->value || (worn.value && worn.value_len == tag->value_len &&
                           memcmp(worn.value, tag->value, tag->value_len) == 0);
}

/* Remove by what the text names. The registry keys tags by (key, value), so a
 * bare key names no one tag of its own: unset "color" drops "color:red" and a
 * bare "color" alike, and unset "color:red" drops that tag alone. The tags a
 * file wears are its record's. A tag whose Ink meets room on its way stops the
 * walk with -ERR_WOULD_BLOCK, whatever came off before it — the first, or one
 * after the tail turned beneath the walk (another core's Ink, a cycle's close)
 * with no Spare to be had: the caller asks again once a cycle gave ground
 * back, and what the text names is then what is still on. */
int tagfs_remove_tag_string(TagOwner owner, uint64_t file_id, const TagSpelling* tag)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    int answer = tagfs_get_metadata(file_id, &meta) == 0 ? -(int)ERR_TAGFS_TAG_NOT_FOUND
                                                         : -(int)ERR_FILE_NOT_FOUND;
    uint32_t removed = 0;
    bool     room    = false;
    for (uint32_t i = 0; i < meta.tag_count && !room; i++) {
        if (!TagNamedBy(meta.tag_ids[i], owner, tag))
            continue;
        int rc = tagfs_remove_tag(file_id, meta.tag_ids[i]);
        if (rc == 0)
            removed++;
        else if (rc == -(int)ERR_WOULD_BLOCK)
            room = true;
        else if (answer == -(int)ERR_TAGFS_TAG_NOT_FOUND)
            answer = rc;
    }
    tagfs_metadata_free(&meta);
    tagfs_leave();
    if (room)
        return -(int)ERR_WOULD_BLOCK;
    return removed > 0 ? 0 : answer;
}
