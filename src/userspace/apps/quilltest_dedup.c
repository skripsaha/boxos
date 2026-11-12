#include "box/print.h"
#include "box/file.h"
#include "box/string.h"
#include "box/memory.h"
#include "box/error.h"
#include "quilltest.h"

/*
 * quilltest dedup — a file and its copy on a book, for the pass Glean runs
 * after each Tally (logcheck dedup; spec_v2 §8.2, §16.7 пп.7–9):
 *
 *   NAME MIB   NAME-a.d of MIB mebibytes written in one write and anchored,
 *              then NAME-b.d of its very bytes the same way, every earlier
 *              file of either name given up first; both read back
 *   NAME       both found and read back, every byte — after the pass laid the
 *              copy on the original's blocks, or a mount later
 *
 * Every 8 bytes of the stream are a function of where they sit (QuillFill),
 * so no two blocks of one file hold the same bytes: the copy is the only
 * duplicate of the original, block for block, and the pass's saving is the
 * copy's blocks exactly.
 */

#define QUILL_DEDUP_SEED      0xDED0u
#define QUILL_DEDUP_MIB_MOST  256u
#define QUILL_DEDUP_NAME_MOST 24u

/* `file` made anew and written whole in ONE write of `bytes` from `buf`,
 * then anchored: its file id, or the refusal and in *what the step. */
static int64_t QuillDedupWrite(const char *file, const uint64_t *buf, uint64_t bytes, const char **what)
{
    *what       = "the create";
    int64_t fid = QuillCreate(file);
    if (fid <= 0)
        return fid ? fid : -1;
    *what         = "the one write";
    int64_t wrote = fwrite((file_id_t)fid, 0, buf, bytes);
    if (wrote != (int64_t)bytes)
        return wrote < 0 ? wrote : -1;
    *what      = "the anchor";
    int64_t rc = anchor(0);
    return rc < 0 ? rc : fid;
}

/* `file` found once and read back whole against the stream: its size in
 * *size, OK; or the refusal, the step in *what, the byte that differs in *at. */
static int64_t QuillDedupRead(const char *file, uint64_t *size, uint64_t *at, const char **what)
{
    file_id_t ids[2];
    int64_t   err = 0;
    *what = "finding it";
    if (QuillFindEverywhere(file, ids, 2) != 1)
        return -1;
    *size = QuillSize(ids[0]);
    *what = "the read back";
    *at   = QuillReadBack(ids[0], QUILL_DEDUP_SEED, *size, &err);
    if (*size == 0 || *at != *size)
        return err < 0 ? err : -2;
    return (int64_t)ids[0];
}

/* Both files written alike: OK, said; or the refusal, said. */
static QuillVerdict QuillDedupMake(const char *a, const char *b, uint32_t mib)
{
    uint64_t    bytes = (uint64_t)mib << 20;
    uint64_t   *buf   = malloc(bytes);
    const char *what  = "the buffer";
    int64_t     fa = -1, fb = -1;
    if (buf) {
        QuillFill(buf, QUILL_DEDUP_SEED, 0, bytes);
        fa = QuillDedupWrite(a, buf, bytes, &what);
        if (fa > 0)
            fb = QuillDedupWrite(b, buf, bytes, &what);
    }
    free(buf);
    if (fa <= 0 || fb <= 0) {
        printf("[QUILL] dedup: FAIL — %s of %s answered %lld (error %u)\n", what, fa <= 0 ? a : b,
               (long long)(fa <= 0 ? fa : fb), (unsigned)box_errno_of((int)(fa <= 0 ? fa : fb)));
        return QUILL_FAIL;
    }
    printf("[QUILL] dedup: written — %s (fid %lld) and %s (fid %lld), %u MiB each of the same bytes, each in one "
           "write and anchored\n", a, (long long)fa, b, (long long)fb, mib);
    return QUILL_PASS;
}

QuillVerdict QuillBookDedup(const char *const *args)
{
    const char *name = args[0], *what = "";
    char        a[QUILL_DEDUP_NAME_MOST + 8], b[QUILL_DEDUP_NAME_MOST + 8];
    uint32_t    mib = 0;
    uint64_t    size[2] = { 0, 0 }, at = 0;
    if (strlen(name) == 0 || strlen(name) > QUILL_DEDUP_NAME_MOST) {
        printf("[QUILL] dedup: FAIL — NAME must be 1 to %u characters\n", QUILL_DEDUP_NAME_MOST);
        return QUILL_FAIL;
    }
    if (args[1] && (!QuillNumber(args[1], &mib) || mib == 0 || mib > QUILL_DEDUP_MIB_MOST)) {
        printf("[QUILL] dedup: FAIL — MIB must be 1 to %u, and was %s\n", QUILL_DEDUP_MIB_MOST, args[1]);
        return QUILL_FAIL;
    }
    snprintf(a, sizeof(a), "%s-a.d", name);
    snprintf(b, sizeof(b), "%s-b.d", name);
    if (mib && QuillDedupMake(a, b, mib) != QUILL_PASS)
        return QUILL_FAIL;
    int64_t fa = QuillDedupRead(a, &size[0], &at, &what);
    int64_t fb = fa > 0 ? QuillDedupRead(b, &size[1], &at, &what) : -1;
    if (fa <= 0 || fb <= 0 || size[0] != size[1] || (mib && size[0] != (uint64_t)mib << 20)) {
        printf("[QUILL] dedup: FAIL — %s of %s answered %lld%s, sizes %llu and %llu\n", what, fa <= 0 ? a : b,
               (long long)(fa <= 0 ? fa : fb), (fa == -2 || fb == -2) ? " at a byte that differs" : "",
               (unsigned long long)size[0], (unsigned long long)size[1]);
        if (fa == -2 || fb == -2)
            printf("[QUILL] dedup: the byte at offset %llu is not the one written\n", (unsigned long long)at);
        return QUILL_FAIL;
    }
    printf("[QUILL] dedup: PASS — %s (fid %lld) and %s (fid %lld) %s, every byte of both read back: %llu bytes "
           "each\n", a, (long long)fa, b, (long long)fb, mib ? "written alike" : "found", (unsigned long long)size[0]);
    return QUILL_PASS;
}
