/*
 * quilltest_room.c — a full volume (logcheck fullvolume): the room a delete
 * gives back reaches the write after it, and deletes are answered when no
 * room is left at all.
 *
 * ‼ ROOMBACK: DELETE A GIGABYTE AND AT ONCE WRITE A GIGABYTE.
 * The ground a delete gives back rests in Fallow until the cycles its rule
 * asks for have settled (fallow.h): the moment after the delete the volume
 * has no free block, and the room is on its way. A write that meets that is
 * not refused: the allocator says the room is coming (MapShort), and the
 * write waits for the cycles that bring it (write_job.c, R6). So roomback
 * lays down _qra.bin of MIB MiB and one more, fills the rest of the volume
 * with _qrb.bin until the volume answers ERR_TAGFS_NO_SPACE — the true answer
 * then, nothing resting — anchors, deletes _qra.bin, and at once writes MIB
 * MiB to a new _qrc.bin: every piece must land whole, and the file read back
 * must be its own stream. The one MiB more _qra.bin had is the room the new
 * file's record may take, never the write's own.
 *
 * ‼ EMPTYFULL: DELETES ON A VOLUME WITH NO ROOM LEFT.
 * A delete gives up a file with a Quitclaim, a record in the pool; on a
 * volume with no free block the pool's tail is closed onto a Spare the
 * reserve keeps for exactly that, and Glean gives the pool's dead records
 * back (the design, decision 29): an empty file frees no ground, so a delete
 * past the deletes' share of the reserve waits for Glean's sweep, never is
 * refused (glean.h). emptyfull makes COUNT empty files — one name, so one
 * tag — fills the volume with _qrf.bin until it answers ERR_TAGFS_NO_SPACE,
 * anchors, and deletes the COUNT files one after another and then _qrf.bin,
 * the file that filled it: every delete must be answered, and answered OK,
 * however long the room for the Quitclaims takes to come. emptyfill and
 * emptyback are the same case with a reboot between its halves: the volume a
 * machine left full of empty files is mounted again, and its reserve made
 * again from the ground the halt gave back, before the deletes.
 *
 * Both say what they measured in quilltest's four words and leave their files
 * for the host to read off the medium. The bytes they write come out of a Bay,
 * never the heap: a cabin may be refused its first heap piece of 2 MiB or more.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/bay.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/time.h"
#include "box/error.h"
#include "quilltest.h"

#define QUILL_ROOM_PIECE     (16u << 20)  /* one write of the fill and of the exchange */
#define QUILL_ROOM_SMALLEST  QUILL_CHUNK  /* the fill halves its piece down to one block */
#define QUILL_ROOM_MIB_MAX   4096u
#define QUILL_ROOM_COUNT_MAX 1000000u
#define QUILL_ROOM_PROGRESS  10000u
#define QUILL_ROOM_SAY       (128ull << 20)  /* a line each time a file passes so many bytes */
#define QUILL_SEED_ROOMA     0xA0u
#define QUILL_SEED_ROOMB     0xB0u
#define QUILL_SEED_ROOMC     0xC0u
#define QUILL_SEED_ROOMF     0xF0u

typedef struct {
    const char *mode;        /* the mode its lines are said under */
    uint64_t *buf;           /* the Bay, QUILL_ROOM_PIECE */
    int       refusal;       /* the answer that stopped the last write, 0 when none did */
    uint64_t  ms;            /* how long the last write took */
} QuillRoom;

/* `bytes` of `seed`'s stream appended to `fid`, named `name`, from `at`, a
 * piece at a time: the bytes the file took, and a line each time the file
 * passes another QUILL_ROOM_SAY — a gigabyte through an emulated disk takes
 * minutes, and the host tells a machine at work from a wedged one by its log.
 * *room's refusal is the answer that stopped it: a short count is followed by
 * the call from there, which meets the refusal itself (box/file.h). */
static uint64_t QuillRoomWrite(QuillRoom *room, file_id_t fid, const char *name, uint32_t seed,
                               uint64_t at, uint64_t bytes)
{
    uint64_t took = 0, t0 = 0, t1 = 0;
    room->refusal = 0;
    time_uptime_ms(&t0);
    while (took < bytes && room->refusal == 0) {
        uint64_t piece = bytes - took < QUILL_ROOM_PIECE ? bytes - took : QUILL_ROOM_PIECE;
        uint64_t from  = at + took;
        QuillFill(room->buf, seed, from, piece);
        for (uint64_t done = 0; done < piece;) {
            int64_t w = fwrite(fid, at + took, (const uint8_t *)room->buf + done, piece - done);
            if (w <= 0) {
                room->refusal = w < 0 ? (int)w : -(int)ERR_IO;
                break;
            }
            done += (uint64_t)w;
            took += (uint64_t)w;
        }
        if ((at + took) / QUILL_ROOM_SAY != from / QUILL_ROOM_SAY)
            printf("[QUILL] %s: %s holds %llu MiB\n", room->mode, name,
                   (unsigned long long)((at + took) >> 20));
    }
    time_uptime_ms(&t1);
    room->ms = t1 - t0;
    return took;
}

/* The volume filled: `name` grown a piece at a time, the piece halved at
 * every refusal down to one block, until a block is refused. The bytes it
 * holds, and in *refusal the answer that ended it. */
static uint64_t QuillRoomFill(QuillRoom *room, const char *name, uint32_t seed, int64_t *fid,
                              int *refusal)
{
    uint64_t size  = 0;
    uint64_t piece = QUILL_ROOM_PIECE;
    *refusal = 0;
    *fid = QuillCreate(name);
    if (*fid <= 0) {
        *refusal = (int)*fid;
        return 0;
    }
    for (;;) {
        size += QuillRoomWrite(room, (file_id_t)*fid, name, seed, size, piece);
        if (room->refusal == 0)
            continue;
        if (room->refusal != -(int)ERR_TAGFS_NO_SPACE || piece == QUILL_ROOM_SMALLEST) {
            *refusal = room->refusal;
            return size;
        }
        piece /= 2;
    }
}

static bool QuillRoomOpen(QuillRoom *room, const char *mode)
{
    memset(room, 0, sizeof(*room));
    room->mode = mode;
    room->buf  = bay_open("quilltest:room", QUILL_ROOM_PIECE, BAY_CREATE);
    if (room->buf)
        return true;
    printf("[QUILL] %s: FAIL — a Bay of %u bytes could not be opened\n", mode, QUILL_ROOM_PIECE);
    return false;
}

/* =========================================================================
 *  roomback — delete a file and at once write as much again
 * ========================================================================= */

QuillVerdict QuillRoomBack(const char *const *args)
{
    uint32_t  mib = 0;
    QuillRoom room;
    if (!QuillNumber(args[0], &mib) || mib == 0 || mib > QUILL_ROOM_MIB_MAX) {
        printf("[QUILL] roomback: FAIL — MIB is 1 to %u; given '%s'\n", QUILL_ROOM_MIB_MAX, args[0]);
        return QUILL_FAIL;
    }
    if (!QuillRoomOpen(&room, "roomback"))
        return QUILL_FAIL;
    uint64_t bytes = (uint64_t)mib << 20;

    int64_t  afid = QuillCreate("_qra.bin");
    uint64_t a    = afid > 0 ? QuillRoomWrite(&room, (file_id_t)afid, "_qra.bin", QUILL_SEED_ROOMA,
                                              0, bytes + (1u << 20)) : 0;
    if (afid <= 0 || a != bytes + (1u << 20)) {
        printf("[QUILL] roomback: FAIL — _qra.bin took %llu of %llu bytes (create %ld, refusal %d): "
               "the volume has no room for the case\n", (unsigned long long)a,
               (unsigned long long)(bytes + (1u << 20)), afid, room.refusal);
        bay_release(room.buf);
        return QUILL_FAIL;
    }
    int64_t  bfid = 0;
    int      bend = 0;
    uint64_t b    = QuillRoomFill(&room, "_qrb.bin", QUILL_SEED_ROOMB, &bfid, &bend);
    int      arc  = anchor(0);
    if (bend != -(int)ERR_TAGFS_NO_SPACE || arc != 0) {
        printf("[QUILL] roomback: FAIL — _qrb.bin filled %llu bytes and ended on %d where the "
               "volume's no-space was due (anchor %d)\n", (unsigned long long)b, bend, arc);
        bay_release(room.buf);
        return QUILL_FAIL;
    }
    printf("[QUILL] roomback: the volume is full — _qra.bin %llu bytes, _qrb.bin %llu bytes, the "
           "next block refused no-space; anchored\n", (unsigned long long)a, (unsigned long long)b);

    int      drc  = delete((file_id_t)afid);
    int64_t  cfid = drc == 0 ? QuillCreate("_qrc.bin") : 0;
    uint64_t c    = cfid > 0 ? QuillRoomWrite(&room, (file_id_t)cfid, "_qrc.bin", QUILL_SEED_ROOMC, 0,
                                              bytes) : 0;
    uint64_t ms   = room.ms;
    int      cend = room.refusal;
    bay_release(room.buf);
    int64_t  rerr = 0;
    uint64_t good = c == bytes ? QuillReadBack((file_id_t)cfid, QUILL_SEED_ROOMC, bytes, &rerr) : 0;
    arc = anchor(0);

    if (drc == 0 && cfid > 0 && c == bytes && good == bytes && arc == 0) {
        printf("[QUILL] roomback: PASS — _qra.bin deleted and _qrc.bin took all %llu bytes at once "
               "in %llu ms, read back whole; anchored\n", (unsigned long long)bytes,
               (unsigned long long)ms);
        return QUILL_PASS;
    }
    printf("[QUILL] roomback: FAIL — _qra.bin deleted (%d), _qrc.bin (create %ld) took %llu of %llu "
           "bytes in %llu ms and was refused %d; read back good to byte %llu (read %lld, anchor %d)\n",
           drc, cfid, (unsigned long long)c, (unsigned long long)bytes, (unsigned long long)ms,
           cend, (unsigned long long)good, (long long)rerr, arc);
    return QUILL_FAIL;
}

/* =========================================================================
 *  emptyfull — COUNT empty files deleted on a full volume; emptyfill and
 *  emptyback — the same, a reboot between the fill and the deletes
 * ========================================================================= */

/* COUNT empty files of one name: how many were made, their ids in `ids`. */
static uint32_t QuillRoomEmpties(const char *mode, file_id_t *ids, uint32_t count, int *refusal)
{
    int64_t first = QuillCreate("_qre.bin");
    *refusal = first > 0 ? 0 : (int)first;
    if (first <= 0)
        return 0;
    ids[0] = (file_id_t)first;
    for (uint32_t i = 1; i < count; i++) {
        int64_t fid = create_everywhere("_qre.bin", NULL);
        if (fid <= 0) {
            *refusal = (int)fid;
            return i;
        }
        ids[i] = (file_id_t)fid;
        if ((i + 1) % QUILL_ROOM_PROGRESS == 0)
            printf("[QUILL] %s: %u of %u empty files made\n", mode, i + 1, count);
    }
    return count;
}

/* COUNT from the case's word, and room for that many file ids: NULL, said
 * under `mode`, when either is not there. */
static file_id_t *QuillRoomIds(const char *mode, const char *word, uint32_t *count)
{
    if (!QuillNumber(word, count) || *count == 0 || *count > QUILL_ROOM_COUNT_MAX) {
        printf("[QUILL] %s: FAIL — COUNT is 1 to %u; given '%s'\n", mode, QUILL_ROOM_COUNT_MAX,
               word);
        return NULL;
    }
    file_id_t *ids = malloc((size_t)*count * sizeof(file_id_t));
    if (!ids)
        printf("[QUILL] %s: FAIL — no room for %u file ids\n", mode, *count);
    return ids;
}

/* The case set: COUNT empty files made, then _qrf.bin grown until the volume
 * answers ERR_TAGFS_NO_SPACE, and anchored. True with _qrf.bin's id in *ffid;
 * false, said under `mode`, when the case could not be set. */
static bool QuillEmptyFill(const char *mode, file_id_t *ids, uint32_t count, int64_t *ffid)
{
    QuillRoom room;
    if (!QuillRoomOpen(&room, mode))
        return false;
    int      made_end = 0, fend = 0;
    uint32_t made     = QuillRoomEmpties(mode, ids, count, &made_end);
    uint64_t f        = made == count ? QuillRoomFill(&room, "_qrf.bin", QUILL_SEED_ROOMF, ffid,
                                                      &fend) : 0;
    bay_release(room.buf);
    int arc = made == count ? anchor(0) : -1;
    if (made != count || fend != -(int)ERR_TAGFS_NO_SPACE || arc != 0) {
        printf("[QUILL] %s: FAIL — %u of %u empty files made (refusal %d), _qrf.bin filled "
               "%llu bytes and ended on %d where the volume's no-space was due (anchor %d)\n",
               mode, made, count, made_end, (unsigned long long)f, fend, arc);
        return false;
    }
    printf("[QUILL] %s: the volume is full — %u empty files, _qrf.bin %llu bytes, the next "
           "block refused no-space; anchored\n", mode, count, (unsigned long long)f);
    return true;
}

/* The COUNT empty files in `ids` deleted one after another, then _qrf.bin:
 * every delete answered, and answered OK; judged under `mode`. */
static QuillVerdict QuillEmptyDrop(const char *mode, const file_id_t *ids, uint32_t count,
                                   int64_t ffid)
{
    uint32_t answered = 0, refused = 0, first_refused = 0;
    int      first_code = 0;
    uint64_t t0 = 0, t1 = 0;
    time_uptime_ms(&t0);
    for (uint32_t i = 0; i < count; i++) {
        int drc = delete(ids[i]);
        answered++;
        if (drc != 0 && refused++ == 0) {
            first_refused = i;
            first_code    = drc;
        }
        if ((i + 1) % QUILL_ROOM_PROGRESS == 0) {
            time_uptime_ms(&t1);
            printf("[QUILL] %s: %u of %u deletes answered, %u refused, %llu ms\n", mode, i + 1,
                   count, refused, (unsigned long long)(t1 - t0));
        }
    }
    time_uptime_ms(&t1);
    int      frc   = delete((file_id_t)ffid);
    file_id_t left[2];
    int      still = QuillFindEverywhere("_qre.bin", left, 2);
    int      arc   = anchor(0);
    if (answered == count && refused == 0 && frc == 0 && still == 0 && arc == 0) {
        printf("[QUILL] %s: PASS — all %u deletes on the full volume answered OK in %llu ms, "
               "and _qrf.bin's after them; no _qre.bin is left; anchored\n", mode, count,
               (unsigned long long)(t1 - t0));
        return QUILL_PASS;
    }
    printf("[QUILL] %s: FAIL — %u of %u deletes answered in %llu ms, %u refused (the first, "
           "index %u, with %d), and _qrf.bin's after them answered %d; %d _qre.bin left "
           "(anchor %d)\n", mode, answered, count, (unsigned long long)(t1 - t0), refused,
           first_refused, first_code, frc, still, arc);
    return QUILL_FAIL;
}

QuillVerdict QuillEmptyFull(const char *const *args)
{
    uint32_t  count = 0;
    int64_t   ffid  = 0;
    file_id_t *ids   = QuillRoomIds("emptyfull", args[0], &count);
    if (!ids)
        return QUILL_FAIL;
    QuillVerdict verdict = QuillEmptyFill("emptyfull", ids, count, &ffid)
                               ? QuillEmptyDrop("emptyfull", ids, count, ffid) : QUILL_FAIL;
    free(ids);
    return verdict;
}

QuillVerdict QuillEmptyFillOnly(const char *const *args)
{
    uint32_t  count = 0;
    int64_t   ffid  = 0;
    file_id_t *ids   = QuillRoomIds("emptyfill", args[0], &count);
    if (!ids)
        return QUILL_FAIL;
    bool set = QuillEmptyFill("emptyfill", ids, count, &ffid);
    free(ids);
    if (!set)
        return QUILL_FAIL;
    printf("[QUILL] emptyfill: PASS — %u empty files and _qrf.bin fill the volume, anchored, for "
           "emptyback to give up after a reboot\n", count);
    return QUILL_PASS;
}

/* After a reboot: the files emptyfill left, found by name — COUNT of _qre.bin
 * and one _qrf.bin — and given up as emptyfull gives them up. */
QuillVerdict QuillEmptyBack(const char *const *args)
{
    uint32_t  count = 0;
    file_id_t qrf[2];
    file_id_t *ids   = QuillRoomIds("emptyback", args[0], &count);
    if (!ids)
        return QUILL_FAIL;
    int found = QuillFindEverywhere("_qre.bin", ids, (int)count);
    int fills = QuillFindEverywhere("_qrf.bin", qrf, 2);
    if (found != (int)count || fills != 1) {
        printf("[QUILL] emptyback: FAIL — the volume mounted again holds %d _qre.bin of the %u "
               "emptyfill made, and %d _qrf.bin where one was due\n", found, count, fills);
        free(ids);
        return QUILL_FAIL;
    }
    printf("[QUILL] emptyback: the volume mounted again holds all %u empty files and _qrf.bin\n",
           count);
    QuillVerdict verdict = QuillEmptyDrop("emptyback", ids, count, (int64_t)qrf[0]);
    free(ids);
    return verdict;
}
