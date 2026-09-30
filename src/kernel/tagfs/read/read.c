#include "read.h"
#include "../plot/plot.h"
#include "../integrity/integrity.h"

error_t ReadSinkPush(const ReadSink *sink, uint64_t at, const void *src, uint32_t bytes)
{
    if (at > sink->bytes || bytes > sink->bytes - at)
        return ERR_INVALID_ARGUMENT;
    if (bytes == 0)
        return OK;
    if (sink->kernel) {
        memcpy(sink->kernel + at, src, bytes);
        return OK;
    }
    if (!sink->vmm)
        return ERR_INVALID_ADDRESS;
    return vmm_user_buf_commit_out(sink->vmm, sink->user + (uintptr_t)at, src, bytes);
}

error_t ReadLocate(const TagFSFileHandle *h, uint64_t k, uint32_t *block, uint32_t *left)
{
    if (PlotFind(h->extents, h->extent_count, k, block, left))
        return OK;
    kprintf("[TagFS] file %u: no extent covers block %llu of its %llu bytes — the record "
            "describes more file than there are blocks, and the read stops there\n",
            h->file_id, (unsigned long long)k, (unsigned long long)h->file_size);
    return ERR_METADATA_CORRUPTED;
}

/* The block where the read is, out of the handle's window. A block the window
 * does not hold is fetched with the `run` blocks from it on the read still
 * needs, one command for all of them; a read of one block reads that one into
 * `*buf`, made at the first such read. Its bytes are checked against the
 * digest recorded when it was written either way: a block out of the window
 * came off the medium in a run nothing checked (CW-D8). */
static error_t ReadStreamBlock(TagFSFileHandle *h, uint32_t block, uint32_t run, uint8_t **buf,
                               const uint8_t **out)
{
    const uint8_t *held = ReadAheadServe(h, block);
    if (!held && run > 1) {
        ReadAheadFetch(h, block, run);
        held = ReadAheadServe(h, block);
    }
    if (held) {
        if (!IntegrityVerify(block, held))
            return ERR_CHECKSUM;
        *out = held;
        return OK;
    }
    if (!*buf && !(*buf = kmalloc(TAGFS_BLOCK_SIZE)))
        return ERR_NO_MEMORY;
    error_t rc = tagfs_read_block(block, *buf);
    if (rc == OK)
        *out = *buf;
    return rc;
}

/*
 * ‼ A REFUSAL IS THE READ'S ANSWER, NEVER A SHORTER COUNT. A caller that took a
 * short read of zeros for data once started a process image of nothing (the
 * program loader, a flash drive pulled while running). The answer carries the
 * reason as it is — the medium's, the digest's, the record's, the sink's —
 * and how many bytes landed before it; the handle moves by those.
 */
error_t TagFSReadStream(TagFSFileHandle *h, const ReadSink *sink, uint64_t *out_read)
{
    uint64_t size = sink->bytes;
    *out_read = 0;
    if (size == 0 || h->offset >= h->file_size)
        return OK;
    if (size > h->file_size - h->offset)
        size = h->file_size - h->offset;

    uint8_t *buf  = NULL;
    uint64_t done = 0;
    uint32_t block = 0, left = 0;
    error_t  rc   = OK;

    while (done < size) {
        uint64_t pos    = h->offset + done;
        uint32_t offset = (uint32_t)(pos % TAGFS_BLOCK_SIZE);
        uint32_t chunk  = TAGFS_BLOCK_SIZE - offset;
        if (chunk > size - done)
            chunk = (uint32_t)(size - done);

        if (done > 0 && left > 0) {
            block++;
            left--;
        } else if ((rc = ReadLocate(h, pos / TAGFS_BLOCK_SIZE, &block, &left)) != OK) {
            break;
        }

        /* The blocks of this run the read still needs, from this one on. */
        uint64_t need = (offset + (size - done) + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
        uint32_t run  = need > (uint64_t)left + 1 ? left + 1 : (uint32_t)need;

        const uint8_t *from = NULL;
        rc = ReadStreamBlock(h, block, run, &buf, &from);
        if (rc == OK)
            rc = ReadSinkPush(sink, done, from + offset, chunk);
        if (rc != OK) {
            kprintf("[TagFS] file %u: block %u answered %u — %llu of %llu bytes had landed "
                    "before it\n", h->file_id, block, (unsigned)rc, (unsigned long long)done,
                    (unsigned long long)size);
            break;
        }
        done += chunk;
    }

    kfree(buf);
    h->offset += done;
    *out_read = done;
    return rc;
}
