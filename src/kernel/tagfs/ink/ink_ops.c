#include "ink.h"
#include "../tag_registry/tag_registry.h"

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

/* The tags a new file wears: its name's first, then the caller's, none twice,
 * none invalid, each on the medium before the file wears it. *out is the
 * caller's to kfree. */
static error_t CreateTags(uint16_t name_tag, const uint16_t* tag_ids, uint32_t tag_count,
                          uint16_t** out, uint32_t* out_count)
{
    uint16_t* tags = kmalloc(sizeof(uint16_t) * ((uint64_t)tag_count + 1u));
    uint32_t  n    = 0;
    if (!tags)
        return ERR_NO_MEMORY;
    if (name_tag != TAGFS_INVALID_TAG_ID)
        tags[n++] = name_tag;
    for (uint32_t i = 0; tag_ids && i < tag_count; i++) {
        uint16_t tid = tag_ids[i];
        bool     dup = tid == TAGFS_INVALID_TAG_ID;
        for (uint32_t j = 0; j < n && !dup; j++)
            dup = tags[j] == tid;
        if (dup)
            continue;
        error_t rc = TagRegistryFlushFor(OpsRegistry(), tid);
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

static error_t CreateInside(const char* filename, const uint16_t* tag_ids, uint32_t tag_count,
                            uint32_t* out_file_id)
{
    uint16_t name_tag = TAGFS_INVALID_TAG_ID;
    error_t  rc       = InkNameTagIntern(filename, &name_tag);
    if (rc != OK)
        return rc;

    uint16_t*  tags  = NULL;
    uint32_t   count = 0;
    InkReceipt receipt;
    rc = CreateTags(name_tag, tag_ids, tag_count, &tags, &count);
    if (rc == OK)
        rc = InkNew(filename, tags, count, out_file_id, &receipt);
    kfree(tags);
    return rc;
}

int tagfs_create_file(const char* filename, const uint16_t* tag_ids, uint32_t tag_count,
                      uint32_t* out_file_id)
{
    if (!filename || !out_file_id)
        return -(int)ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = CreateInside(filename, tag_ids, tag_count, out_file_id);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

int tagfs_delete_file(uint32_t file_id)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = InkQuitclaim(file_id, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

/* The tag follows the name: a rename that changed the string and left the tag
 * made the file visible under its old name and invisible under its new one —
 * by-name lookup and proc_exec ask the index, not the string. */
int tagfs_rename_file(uint32_t file_id, const char* new_filename)
{
    if (!new_filename)
        return -(int)ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    uint16_t   name_tag = TAGFS_INVALID_TAG_ID;
    InkReceipt receipt;
    error_t    rc = InkNameTagIntern(new_filename, &name_tag);
    if (rc == OK)
        rc = InkFields(file_id, new_filename, name_tag, TAGFS_INVALID_TAG_ID, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

int tagfs_add_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = tag_id == TAGFS_INVALID_TAG_ID ? ERR_TAGFS_INVALID_TAG
                                                   : TagRegistryFlushFor(OpsRegistry(), tag_id);
    if (rc == OK)
        rc = InkFields(file_id, NULL, tag_id, TAGFS_INVALID_TAG_ID, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

int tagfs_remove_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = InkFields(file_id, NULL, TAGFS_INVALID_TAG_ID, tag_id, &receipt);
    tagfs_leave();
    return rc == OK ? 0 : -(int)rc;
}

bool tagfs_has_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!tagfs_enter())
        return false;
    bool has = TagBitmapHas(tagfs_get_state()->bitmap_index, tag_id, file_id);
    tagfs_leave();
    return has;
}

error_t tagfs_add_tag_string(uint32_t file_id, const TagSpelling* tag)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    uint16_t tag_id;
    error_t  rc = TagRegistryIntern(OpsRegistry(), tag, &tag_id);
    tagfs_leave();
    if (rc != OK)
        return rc;
    int added = tagfs_add_tag(file_id, tag_id);
    return added == 0 ? OK : (error_t)-added;
}

/* Whether the file's tag `tag_id` is what `tag` names: that exact tag when it
 * has a value, and any tag of its key when it is bare. */
static bool TagNamedBy(uint16_t tag_id, const TagSpelling* tag)
{
    TagSpelling worn;
    if (!TagRegistrySpelling(OpsRegistry(), tag_id, &worn, NULL))
        return false;
    if (worn.key_len != tag->key_len || memcmp(worn.key, tag->key, tag->key_len) != 0)
        return false;
    return !tag->value || (worn.value && worn.value_len == tag->value_len &&
                           memcmp(worn.value, tag->value, tag->value_len) == 0);
}

/* Remove by what the text names. The registry keys tags by (key, value), so a
 * bare key names no one tag of its own: unset "color" drops "color:red" and a
 * bare "color" alike, and unset "color:red" drops that tag alone. The tags a
 * file wears are its record's. */
int tagfs_remove_tag_string(uint32_t file_id, const TagSpelling* tag)
{
    if (!tagfs_enter())
        return -(int)ERR_TAGFS_NOT_INITIALIZED;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    int answer = tagfs_get_metadata(file_id, &meta) == 0 ? -(int)ERR_TAGFS_TAG_NOT_FOUND
                                                         : -(int)ERR_FILE_NOT_FOUND;
    uint32_t removed = 0;
    for (uint32_t i = 0; i < meta.tag_count; i++) {
        if (!TagNamedBy(meta.tag_ids[i], tag))
            continue;
        int rc = tagfs_remove_tag(file_id, meta.tag_ids[i]);
        if (rc == 0)
            removed++;
        else if (answer == -(int)ERR_TAGFS_TAG_NOT_FOUND)
            answer = rc;
    }
    tagfs_metadata_free(&meta);
    tagfs_leave();
    return removed > 0 ? 0 : answer;
}
