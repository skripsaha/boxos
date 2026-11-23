#include "../tagfs.h"
#include "tagfs_volume.h"
#include "tagfs_handle.h"
#include "../file_map/file_map.h"
#include "klib.h"

/* The files a name may match at once, looked at a page at a time. */
#define KEPT_NAMED_PAGE  16u

/* The file of `vol` kept in a role and called `name` exactly: its id, 0 for
 * none. Inside the door. */
static uint64_t KeptFileFound(TagFSVolume* vol, const char* name)
{
    uint64_t ids[KEPT_NAMED_PAGE], from = 1, next = 0, found = 0;
    size_t   len = strlen(name);
    do {
        uint32_t n = FileMapNamed(vol->state.files, name, len, from, ids, KEPT_NAMED_PAGE, &next);
        for (uint32_t i = 0; i < n && !found; i++) {
            FileRecord* rec = FileMapRecordHold(vol->state.files, ids[i], NULL);
            if (rec && (rec->flags & TAGFS_FILE_ROLE_MASK) && rec->name_len == len &&
                memcmp(rec->name, name, len) == 0)
                found = ids[i];
            FileRecordRelease(rec);
        }
        from = next;
    } while (!found && from);
    return found;
}

error_t TagFSKeptRead(TagFSVolume* vol, const char* name, uint32_t most, uint8_t** out, uint32_t* bytes)
{
    *out   = NULL;
    *bytes = 0;
    if (!tagfs_enter(vol))
        return ERR_DEVICE_NOT_READY;
    uint64_t         fid = KeptFileFound(vol, name);
    TagFSFileHandle* h   = fid ? tagfs_open(vol, fid, TAGFS_HANDLE_READ) : NULL;
    error_t          rc  = !fid ? ERR_FILE_NOT_FOUND : h ? OK : ERR_NO_MEMORY;
    uint64_t         size = h ? h->rec->size : 0, got = 0;
    if (rc == OK && size > most)
        rc = ERR_OUT_OF_RANGE;
    uint8_t* buf = rc == OK ? kmalloc(size ? size : 1u) : NULL;
    if (rc == OK && !buf)
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        h->offset = 0;
        rc        = tagfs_read(vol, h, buf, size, &got);
        if (rc == OK && got != size)
            rc = ERR_METADATA_CORRUPTED;    /* the handle's view holds the file it opened: its size is there */
    }
    tagfs_close(vol, h);
    tagfs_leave(vol);
    if (rc != OK) {
        kfree(buf);
        return rc;
    }
    *out   = buf;
    *bytes = (uint32_t)size;
    return OK;
}
