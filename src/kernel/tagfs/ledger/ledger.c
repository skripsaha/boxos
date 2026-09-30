#include "ledger.h"
#include "../tagfs.h"
#include "../scribe/scribe.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../../../lib/kernel/crypto.h"

static const uint8_t g_ledger_magic[8] = {
    VOLUME_LEDGER_MAGIC_0, VOLUME_LEDGER_MAGIC_1, VOLUME_LEDGER_MAGIC_2,
    VOLUME_LEDGER_MAGIC_3, VOLUME_LEDGER_MAGIC_4, VOLUME_LEDGER_MAGIC_5,
    VOLUME_LEDGER_MAGIC_6, VOLUME_LEDGER_MAGIC_7
};

/* The seq and the copy the volume stands on — the newest copy the medium
 * holds, as far as this kernel wrote or read it. The next write goes to the
 * other copy with seq + 1: this is the whole of the alternation. Moved only
 * by a pass that landed, so the copy named here is always intact.
 *
 * What the volume remembers of the Use Context — the comma-joined list it was
 * last told, kept beside the counters and written out with them on every
 * Ledger write. One sector is the record's whole world (ledger_read_copy takes
 * one and refuses a longer claim), so this is bounded by what a sector holds
 * past the fixed fields: a list that will not fit is not kept in part.
 *
 * All four under g_ledger_lock, which holds memory only: a leaf, taken under
 * the volume's state lock for the snapshot and alone everywhere else. */
TagFSLock         g_ledger_lock = TAGFS_LOCK_INIT("ledger", LOCK_RANK_LEAF);
static uint64_t   g_ledger_seq;
static uint32_t   g_ledger_current;
static char       g_ledger_use[TAGFS_SECTOR_SIZE];
static uint16_t   g_ledger_use_len;
#define LEDGER_USE_MAX  ((size_t)TAGFS_SECTOR_SIZE - sizeof(VolumeLedger))

_Static_assert(sizeof(VolumeLedger) < TAGFS_SECTOR_SIZE,
               "the Ledger's fixed fields, and room for a Use Context, fit its one sector");

/* One Ledger write on its way to the medium at a time, for the whole volume.
 * Two writers reading the same seq would each build a record for the same
 * older copy, and the medium would keep whichever landed last while memory
 * kept whichever committed last — not necessarily the same one. */
static Scribe g_ledger_scribe = SCRIBE_INIT(SCRIBE_RANK_LEDGER, "the volume's Ledger");

static uint64_t ledger_vlba(uint32_t copy)
{
    return ((uint64_t)tagfs_get_state()->layout.state_block + copy) * TAGFS_BLOCK_SECTORS;
}

/*
 * Read one copy. Returns true when it is present, intact, and long enough to
 * hold what this kernel knows how to read — a Ledger written by a newer kernel
 * is longer than this one expects and is read up to what is understood, which
 * is what `bytes` is for.
 */
static bool ledger_read_copy(uint32_t copy, VolumeLedger *out,
                             char *tail, uint16_t *tail_len)
{
    uint8_t buf[TAGFS_SECTOR_SIZE];
    if (tagfs_volume_read(ledger_vlba(copy), 1, buf) != 0) {
        kprintf("[Ledger] copy %u could not be read off the medium\n", copy);
        return false;
    }

    VolumeLedger led;
    memcpy(&led, buf, sizeof(led));

    if (memcmp(led.magic, g_ledger_magic, sizeof(g_ledger_magic)) != 0) {
        kprintf("[Ledger] copy %u carries no ledger\n", copy);
        return false;
    }

    /* Bounded before it is summed: `bytes` came off the medium and a stated
     * length longer than the sector it lives in would sum over memory this
     * does not own. Required is what every reader needs, not what this one
     * knows: a record from before the Use Context was kept ends at the
     * counters, and refusing it would refuse every volume made before then. */
    if (led.bytes < VOLUME_LEDGER_REQUIRED_BYTES || led.bytes > TAGFS_SECTOR_SIZE) {
        kprintf("[Ledger] copy %u states a length of %u, which cannot be one\n",
                copy, led.bytes);
        return false;
    }

    uint8_t probe[TAGFS_SECTOR_SIZE];
    memcpy(probe, buf, led.bytes);
    memset(probe + __builtin_offsetof(VolumeLedger, crc32), 0, sizeof(uint32_t));
    uint32_t have = KCrc32(probe, led.bytes);
    if (have != led.crc32) {
        kprintf("[Ledger] copy %u does not match its own checksum "
                "(0x%08x against 0x%08x) — it was torn mid-write\n",
                copy, have, led.crc32);
        return false;
    }

    /* What a shorter record does not carry reads as zero, not as whatever
     * followed it on the medium. */
    if (led.bytes < sizeof(led))
        memset((uint8_t *)&led + led.bytes, 0, sizeof(led) - led.bytes);

    /* The remembered Use Context rides after the fixed fields, inside the
     * summed bytes — so it is already proven intact by the checksum above;
     * only its place in the record is checked here. */
    *tail_len = 0;
    if (led.use_context_bytes) {
        uint32_t off = led.use_context_offset;
        uint32_t len = led.use_context_bytes;
        if (off < VOLUME_LEDGER_REQUIRED_BYTES || off + len > led.bytes) {
            kprintf("[Ledger] copy %u places its Use Context at %u for %u bytes, "
                    "outside its own record — it remembers none\n", copy, off, len);
        } else {
            memcpy(tail, buf + off, len);
            *tail_len = (uint16_t)len;
        }
    }

    *out = led;
    return true;
}

/* One copy, read off the medium by the reader above and handed over as it
 * is — for a proof that what was said to the volume is what it holds. */
bool tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, char *tail, uint16_t *tail_len)
{
    if (copy >= VOLUME_LEDGER_COPIES || !out || !tail || !tail_len) return false;
    return ledger_read_copy(copy, out, tail, tail_len);
}

/*
 * Take up the newer of the two.
 *
 * One unreadable copy is survivable and is said out loud, because a volume
 * running on a single Ledger has no second opinion left. Both unreadable is
 * not survivable: the counters are gone, and inventing them would hand out
 * block numbers that belong to files.
 *
 * Called by the mount with its door shut, so the volume's state is nobody
 * else's yet and takes the counters without its lock.
 */
error_t ledger_load(void)
{
    TagFSState  *fs = tagfs_get_state();
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    bool         ok[VOLUME_LEDGER_COPIES];
    uint32_t     live = 0;

    /* Each copy's remembered context, read with it; the newest copy's is the
     * one the volume stands on. Off the stack: two sectors' worth. */
    char *tails = kmalloc((size_t)VOLUME_LEDGER_COPIES * TAGFS_SECTOR_SIZE);
    uint16_t tail_len[VOLUME_LEDGER_COPIES];
    if (!tails) return ERR_NO_MEMORY;

    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        ok[c] = ledger_read_copy(c, &led[c], tails + (size_t)c * TAGFS_SECTOR_SIZE,
                                 &tail_len[c]);
        if (ok[c]) live++;
    }

    if (live == 0) {
        kfree(tails);
        kprintf("[Ledger] neither copy is readable — this volume cannot say "
                "what is on it\n");
        return ERR_TAGFS_CORRUPTED;
    }

    uint32_t newest = 0;
    bool     have   = false;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (!ok[c]) continue;
        if (!have || led[c].seq > led[newest].seq) {
            newest = c;
            have   = true;
        }
    }

    /* The reader lets a context start as early as the counters end, which is
     * four bytes before this kernel's record does: a list longer than a record
     * of this kernel's holds would overrun the sector the next write builds.
     * Forgotten whole, as remembering forgets one — never kept in part. */
    uint16_t kept = tail_len[newest];
    if (kept > LEDGER_USE_MAX) {
        kprintf("[Ledger] copy %u remembers a Use Context of %u bytes, longer "
                "than the %u a record holds — it is forgotten, not kept in part\n",
                newest, (unsigned)kept, (unsigned)LEDGER_USE_MAX);
        kept = 0;
    }

    fs->ledger = led[newest];

    TagFSLockTake(&g_ledger_lock);
    g_ledger_seq     = led[newest].seq;
    g_ledger_current = newest;
    g_ledger_use_len = kept;
    memcpy(g_ledger_use, tails + (size_t)newest * TAGFS_SECTOR_SIZE, kept);
    TagFSLockDrop(&g_ledger_lock);
    kfree(tails);

    if (live < VOLUME_LEDGER_COPIES) {
        kprintf("[Ledger] running on copy %u alone (seq %llu); the other one "
                "did not survive\n",
                newest, (unsigned long long)led[newest].seq);
    }
    return OK;
}

/*
 * One pass: a snapshot, one sector to the medium, and — only if it landed —
 * the copy the volume stands on moves. Run by the holder of the Ledger's
 * Scribe alone.
 *
 * The snapshot is taken under the volume's state lock and then the Ledger's,
 * so the counters are the ones of one instant, with the Use Context as it
 * stood at that same instant; the clock is read before either lock, and the
 * sector goes to the medium after both are let go. The sector lives on this
 * pass's own stack: nothing else ever writes into it.
 *
 * A refusal leaves the volume standing on the copy it stood on — the copy
 * this pass aimed at may be torn or untouched, and it is the older one either
 * way. Nothing is written again on its own: the next ask starts the next pass.
 */
static error_t LedgerStroke(void)
{
    TagFSState  *fs  = tagfs_get_state();
    uint64_t     now = rtc_get_unix64();
    uint8_t      sector[TAGFS_SECTOR_SIZE];
    VolumeLedger led;

    memset(sector, 0, sizeof(sector));

    TagFSLockTake(&fs->lock);
    TagFSLockTake(&g_ledger_lock);
    led = fs->ledger;
    uint16_t use_len = g_ledger_use_len;
    memcpy(sector + sizeof(led), g_ledger_use, use_len);
    uint64_t seq   = g_ledger_seq + 1;
    uint32_t slot  = (g_ledger_current + 1) % VOLUME_LEDGER_COPIES;
    uint32_t epoch = tagfs_mount_epoch();
    TagFSLockDrop(&g_ledger_lock);
    TagFSLockDrop(&fs->lock);

    /* The remembered Use Context follows the fixed fields and is summed with
     * them: a torn write loses the context together with the counters, never
     * one without the other. */
    memcpy(led.magic, g_ledger_magic, sizeof(g_ledger_magic));
    led.seq                = seq;
    led.written_unix       = now;
    led.use_context_offset = use_len ? (uint16_t)sizeof(VolumeLedger) : 0;
    led.use_context_bytes  = use_len;
    led.bytes              = (uint32_t)sizeof(VolumeLedger) + use_len;
    led.crc32              = 0;
    memcpy(sector, &led, sizeof(led));
    led.crc32 = KCrc32(sector, led.bytes);
    memcpy(sector + __builtin_offsetof(VolumeLedger, crc32), &led.crc32, sizeof(led.crc32));

    int wrote = tagfs_volume_write(ledger_vlba(slot), 1, sector);

    /* Only now is the new copy the one the volume stands on. And only for the
     * pass's own volume, by the pass's own writer: a mount between the
     * snapshot and here means another volume's Ledger is the one in memory
     * now, and the Scribe is never taken from a holder (scribe.h) — this is
     * where that is relied on, so it is where it is asked. */
    TagFSLockTake(&g_ledger_lock);
    if (wrote == 0 && ScribeHeldHere(&g_ledger_scribe) && tagfs_mount_epoch() == epoch) {
        g_ledger_seq     = seq;
        g_ledger_current = slot;
    }
    uint64_t stands_seq = g_ledger_seq;
    uint32_t stands     = g_ledger_current;
    TagFSLockDrop(&g_ledger_lock);

    if (wrote != 0) {
        kprintf("[Ledger] copy %u would not take seq %llu — the volume still "
                "stands on copy %u (seq %llu)\n",
                slot, (unsigned long long)seq, stands, (unsigned long long)stands_seq);
        return ERR_TAGFS_METADATA_ERROR;
    }
    return OK;
}

/* The answer to one ask: a pass that began after it — this call's own, or
 * one another core was already making — wrote the Ledger, or was refused. */
static error_t LedgerWriteFor(uint64_t ticket)
{
    uint64_t cover = 0;
    error_t  rc    = OK;

    switch (ScribeWait(&g_ledger_scribe, ticket, &cover, &rc)) {
    case SCRIBE_MINE:
        rc = LedgerStroke();
        ScribeGive(&g_ledger_scribe, cover, rc);
        return rc;
    case SCRIBE_SELF:
        kprintf("[Ledger] write refused: a Ledger write on this core is "
                "suspended beneath this one, and waiting for it here would "
                "never end — what changed goes out with the next one\n");
        return ERR_WOULD_BLOCK;
    case SCRIBE_REFUSED:
        return rc;
    default:
        return OK;      /* SCRIBE_WRITTEN */
    }
}

error_t tagfs_write_ledger(void)
{
    if (tagfs_get_state()->layout.state_blocks < VOLUME_LEDGER_COPIES) {
        return ERR_TAGFS_METADATA_ERROR;
    }
    return LedgerWriteFor(ScribeAsk(&g_ledger_scribe));
}

error_t tagfs_remember_use_context(const char *list, size_t len, bool *remembered)
{
    if (!list && len) return ERR_INVALID_ARGUMENT;

    /* A list the record cannot hold is not cut short and not kept: the volume
     * forgets rather than remembering half of what was said, and says so. */
    bool fits = len <= LEDGER_USE_MAX;

    TagFSLockTake(&g_ledger_lock);
    g_ledger_use_len = fits ? (uint16_t)len : 0;
    if (g_ledger_use_len) memcpy(g_ledger_use, list, g_ledger_use_len);
    TagFSLockDrop(&g_ledger_lock);

    error_t rc = tagfs_write_ledger();
    if (remembered) *remembered = fits && rc == OK;
    return rc;
}

size_t tagfs_recall_use_context(char *buf, size_t cap)
{
    TagFSLockTake(&g_ledger_lock);
    size_t len = g_ledger_use_len;
    if (buf && cap >= len && len) memcpy(buf, g_ledger_use, len);
    TagFSLockDrop(&g_ledger_lock);
    return len;
}

void LedgerLetGo(void)
{
    TagFSLockTake(&g_ledger_lock);
    g_ledger_use_len = 0;
    TagFSLockDrop(&g_ledger_lock);
}

/*
 * ‼ A FILE ID IS ISSUED ONCE, AND NEVER ZERO.
 *
 * The id is 32 bits wide on format v1 (volume_ledger.h) and was handed out by
 * a bare increment, so the 2^32-th create was given id 0 — the id no file may
 * wear, the one a retired record carries — and the create after it id 1, a
 * file the volume already holds. The last id issued is 0xFFFFFFFE: issuing
 * 0xFFFFFFFF would leave the counter at 0, and every walk bounded by it would
 * see no file at all. So a counter at 0xFFFFFFFF says every id has been
 * issued, and a counter at 0 is a Ledger an earlier kernel let wrap; both are
 * refused out loud and the counter does not move. Format v2 carries 64-bit
 * ids (Quill design §2.3).
 */
error_t LedgerIssueFileId(VolumeLedger *led, uint32_t *out_fid)
{
    uint32_t next = led->next_file_id;
    if (next == 0 || next == UINT32_MAX) {
        if (next)
            kprintf("[Ledger] no file is made: every file id a 32-bit id carries has been "
                    "issued on this volume, the last of them %u — format v2 carries "
                    "64-bit ids\n", UINT32_MAX - 1u);
        else
            kprintf("[Ledger] no file is made: this volume's Ledger names file id 0 as "
                    "the next, a count an earlier kernel let wrap past %u — the ids "
                    "after it belong to files the volume holds\n", UINT32_MAX);
        return ERR_TAGFS_FID_EXHAUSTED;
    }
    *out_fid          = next;
    led->next_file_id = next + 1u;
    return OK;
}
