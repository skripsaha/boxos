/*
 * quilltest_throwaway.c — the cases of quilltest a throwaway image alone may
 * be asked.
 *
 * Each of these harms the volume past files of its own: sequential and
 * interleave leak blocks on the synchronous path of HEAD, fidhistory spends
 * fifty thousand file ids for good. quilltest is on every volume and wears `system`, so these
 * are compiled only into a quilltest built with VOLUME_TESTS=on — the key
 * that already means a throwaway image — and in any other build the table in
 * quilltest.c answers each with one FAIL that says so. See quilltest.c for
 * the rest.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"
#include "box/error.h"
#include "quilltest.h"

#ifdef CONFIG_VOLUME_TESTS

/* logcheck's quillmut edits these two lines by exact text — keep them verbatim */
#define QUILL_WEAVE_BYTES    (64u << 20)
#define QUILL_SEQ_BYTES      (3u << 20)
/* ...and the delete line in QuillFidHistory. */

#define QUILL_FID_CYCLES     50000u
#define QUILL_FID_PROGRESS   10000u
#define QUILL_SEED_A         0xAu
#define QUILL_SEED_B         0xBu
#define QUILL_SEED_S         0x5u

static uint64_t g_chunk[QUILL_CHUNK / 8];

/* One QUILL_CHUNK of the stream at `off`, written; fwrite's answer. */
static int64_t QuillWriteChunk(file_id_t fid, uint32_t seed, uint64_t off)
{
    QuillFill(g_chunk, seed, off, QUILL_CHUNK);
    return fwrite(fid, off, g_chunk, QUILL_CHUNK);
}

/* The byte at which a file holding one extent per block is refused: its
 * record — 42 bytes, 2 per tag, 6 per extent and the name — fills the 4080
 * bytes of a pool block's data (plot/plot_width.h RECORD_HEADER_SIZE and
 * TAGFS_MPOOL_DATA_SIZE, PlotFits). 2748416, 671 extents, for
 * a 10-byte name wearing its stem alone. 0 when the file will not say. */
static uint64_t QuillWidthLimit(file_id_t fid)
{
    file_info_t info;
    if (file_info(fid, &info) != 0)
        return 0;
    uint32_t fixed = 42u + 2u * info.tag_count + (uint32_t)strlen(info.filename);
    return fixed < 4080u ? (uint64_t)((4080u - fixed) / 6u) * QUILL_CHUNK : 0;
}

/* =========================================================================
 *  sequential — one writer, 4 KiB at a time, one write after another
 * ========================================================================= */

/* The design's 1.3 test "3 MB in a row". A lone writer is the case where
 * every block could lie next to the one before it: a record that merges
 * neighbouring blocks into one extent holds the whole file in a handful of
 * extents, and a record that takes one extent per write runs out of room at
 * its 4080 bytes long before the volume runs out of space. The DEFECT is that
 * refusal and nothing else: ERR_IO at exactly the byte the record's width
 * puts it — any other refusal, a medium's among them, is a FAIL. The host
 * counts the extents (logcheck `quill`, volume-read.py list). */
QuillVerdict QuillSequential(const char *const *args)
{
    (void)args;
    int64_t fid = QuillCreate("quillS.bin");
    if (fid <= 0) {
        int arc = anchor(0);
        printf("[QUILL] sequential: FAIL — quillS.bin could not be created (answer %ld, anchor %d)\n",
               fid, arc);
        return QUILL_FAIL;
    }

    const uint32_t writes = QUILL_SEQ_BYTES / QUILL_CHUNK;
    uint32_t done    = 0;
    int64_t  refusal = QUILL_CHUNK;
    for (; done < writes; done++) {
        refusal = QuillWriteChunk((file_id_t)fid, QUILL_SEED_S, (uint64_t)done * QUILL_CHUNK);
        if (refusal != (int64_t)QUILL_CHUNK)
            break;
    }
    uint64_t acked = (uint64_t)done * QUILL_CHUNK;
    uint64_t size  = QuillSize((file_id_t)fid);
    uint64_t limit = QuillWidthLimit((file_id_t)fid);
    int64_t  rerr  = 0;
    uint64_t good  = QuillReadBack((file_id_t)fid, QUILL_SEED_S, size, &rerr);
    int      arc   = anchor(0);
    bool     whole = arc == 0 && size == acked && good == size;

    if (whole && done == writes) {
        printf("[QUILL] sequential: PASS — quillS.bin took %u bytes in %u writes of %u, one "
               "after another, and read back exactly; anchored for the extent count\n",
               QUILL_SEQ_BYTES, writes, QUILL_CHUNK);
        return QUILL_PASS;
    }
    if (whole && refusal == -(int64_t)ERR_IO && limit && acked == limit) {
        printf("[QUILL] sequential: DEFECT — quillS.bin, with no other writer, refused its "
               "write at byte %llu of %u (error %u) after %u writes of %u in a row, the byte "
               "its record's width puts the refusal at (%llu); the %llu bytes it acknowledged "
               "read back exactly — no extent merge, and the record width runs out\n",
               (unsigned long long)acked, QUILL_SEQ_BYTES, box_errno_of((int)refusal), done,
               QUILL_CHUNK, (unsigned long long)limit, (unsigned long long)size);
        return QUILL_DEFECT;
    }
    printf("[QUILL] sequential: FAIL — quillS.bin took %u of %u writes of %u (the next "
           "answered %lld, and the record's width refuses at byte %llu), holds %llu bytes and "
           "reads back good to byte %llu (read answer %lld, anchor %d)\n", done, writes,
           QUILL_CHUNK, (long long)refusal, (unsigned long long)limit, (unsigned long long)size,
           (unsigned long long)good, (long long)rerr, arc);
    return QUILL_FAIL;
}

/* =========================================================================
 *  interleave — two files, 4 KiB each in turn
 * ========================================================================= */

/* One program alternating rather than two strands: the allocator sees the
 * same interleaving, the order is the same on every run, and what makes two
 * files' blocks land apart — Allotment — is per file id either way. On HEAD
 * this case is refused at the same byte as `sequential`, by quillA.bin, which
 * writes first in every round: the record width, not the interleaving; once
 * 1.3 lands it asks about Allotment alone. */
QuillVerdict QuillInterleave(const char *const *args)
{
    (void)args;
    static const char    *names[2] = { "quillA.bin", "quillB.bin" };
    static const uint32_t seeds[2] = { QUILL_SEED_A, QUILL_SEED_B };
    int64_t fids[2];
    for (int f = 0; f < 2; f++) {
        fids[f] = QuillCreate(names[f]);
        if (fids[f] <= 0) {
            int drc = f == 1 ? delete((file_id_t)fids[0]) : 0;
            int arc = anchor(0);
            printf("[QUILL] interleave: FAIL — %s could not be created (answer %ld)%s (anchor %d)\n",
                   names[f], fids[f], f == 1 ? (drc == 0 ? "; quillA.bin is deleted again"
                                                         : "; quillA.bin would not be deleted") : "",
                   arc);
            return QUILL_FAIL;
        }
    }

    const uint32_t rounds = QUILL_WEAVE_BYTES / QUILL_CHUNK;
    uint32_t done = 0;
    int      stopped = -1;             /* which file refused, -1 for neither */
    int64_t  refusal = 0;
    uint64_t refused_at = 0;
    for (uint32_t r = 0; r < rounds && stopped < 0; r++) {
        for (int f = 0; f < 2; f++) {
            uint64_t off = (uint64_t)r * QUILL_CHUNK;
            int64_t  w   = QuillWriteChunk((file_id_t)fids[f], seeds[f], off);
            if (w != (int64_t)QUILL_CHUNK) {
                stopped = f;
                refusal = w;
                refused_at = off;
                break;
            }
            done++;
        }
    }

    uint64_t size[2], good[2];
    int64_t  rerr[2];
    for (int f = 0; f < 2; f++) {
        size[f] = QuillSize((file_id_t)fids[f]);
        good[f] = QuillReadBack((file_id_t)fids[f], seeds[f], size[f], &rerr[f]);
    }
    uint64_t limit = QuillWidthLimit((file_id_t)fids[0]);
    int  arc   = anchor(0);
    bool whole = arc == 0 && good[0] == size[0] && good[1] == size[1] &&
                 size[0] + size[1] == (uint64_t)done * QUILL_CHUNK;

    if (whole && stopped < 0 && size[0] == QUILL_WEAVE_BYTES && size[1] == QUILL_WEAVE_BYTES) {
        printf("[QUILL] interleave: PASS — quillA.bin and quillB.bin took %u bytes each in %u "
               "alternating writes of %u and read back exactly; anchored for the extent count\n",
               QUILL_WEAVE_BYTES, done, QUILL_CHUNK);
        return QUILL_PASS;
    }
    if (whole && stopped == 0 && refusal == -(int64_t)ERR_IO && limit && refused_at == limit &&
        size[0] == refused_at && size[1] == refused_at) {
        printf("[QUILL] interleave: DEFECT — %s refused its write at byte %llu of %u (error %u) "
               "after %u alternating writes of %u, the byte its record's width puts the refusal "
               "at (%llu); the %llu and %llu bytes the two files acknowledged read back exactly\n",
               names[stopped], (unsigned long long)refused_at, QUILL_WEAVE_BYTES,
               box_errno_of((int)refusal), done, QUILL_CHUNK, (unsigned long long)limit,
               (unsigned long long)size[0], (unsigned long long)size[1]);
        return QUILL_DEFECT;
    }
    printf("[QUILL] interleave: FAIL — %u alternating writes of %u taken (the next answered "
           "%lld at byte %llu, and the record's width refuses at byte %llu); quillA.bin holds "
           "%llu bytes good to %llu, quillB.bin %llu good to %llu (read answers %lld, %lld; "
           "anchor %d)\n", done, QUILL_CHUNK, (long long)refusal, (unsigned long long)refused_at,
           (unsigned long long)limit, (unsigned long long)size[0], (unsigned long long)good[0],
           (unsigned long long)size[1], (unsigned long long)good[1], (long long)rerr[0],
           (long long)rerr[1], arc);
    return QUILL_FAIL;
}

/* =========================================================================
 *  fidhistory — file ids spent on files that are gone
 * ========================================================================= */

/* One name throughout, so the only tag this interns is its stem. What the
 * spent ids cost is not something this program can see: the next mount says
 * it, in its "this mount keeps" line, and the host reads the slope between
 * two mounts. */
QuillVerdict QuillFidHistory(const char *const *args)
{
    (void)args;
    file_id_t *ids = NULL;
    int before = query_all_everywhere(NULL, "hidden", &ids);
    free(ids);
    if (before < 0) {
        printf("[QUILL] fidhistory: FAIL — counting the live files answered %d\n", before);
        return QUILL_FAIL;
    }

    file_id_t first = 0, last = 0;
    uint32_t  spent = 0;
    int broke = 0;
    for (uint32_t i = 0; i < QUILL_FID_CYCLES; i++) {
        int64_t fid = create_everywhere("quillfid.tmp", NULL);
        if (fid <= 0 || (file_id_t)fid <= last) {
            broke = fid <= 0 ? (int)fid : -1;
            break;
        }
        /* The last file lives through an anchor. A book's next mount issues
         * ids past every one its medium saw, never one of those again; a
         * file born and given up inside one cycle reaches no stitch
         * (docket.c JotMerged), and its id may be issued again (spec_v2
         * §4.5 п.8, §16.6 п.7): with no cycle to see the last id, that
         * mount would stand short of the ids this case spent. */
        int lrc = i + 1u == QUILL_FID_CYCLES ? anchor(0) : 0;
        if (lrc != 0) {
            broke = lrc;
            break;
        }
        int drc = delete((file_id_t)fid);
        if (!first)
            first = (file_id_t)fid;
        last = (file_id_t)fid;
        spent++;
        if (drc != 0) {
            broke = drc;
            break;
        }
        if (spent % QUILL_FID_PROGRESS == 0)
            printf("[QUILL] fidhistory: %u of %u file ids spent (last %lu)\n",
                   spent, QUILL_FID_CYCLES, last);
    }
    int arc = anchor(0);
    int after = query_all_everywhere(NULL, "hidden", &ids);
    free(ids);

    if (broke != 0 || arc != 0 || after != before) {
        printf("[QUILL] fidhistory: FAIL — %u file ids spent, %lu to %lu, the last cycle "
               "answered %d; %d files were live before and %d after (anchor %d)\n",
               spent, first, last, broke, before, after, arc);
        return QUILL_FAIL;
    }
    printf("[QUILL] fidhistory: PENDING — spent %u file ids, %lu to %lu, with %d live files "
           "before and after; anchored — the next mount says what they cost\n",
           spent, first, last, before);
    return QUILL_PENDING;
}

#endif /* CONFIG_VOLUME_TESTS */
