#include "ledger.h"
#include "../tagfs.h"
#include "../scribe/scribe.h"
#include "../settle/settle.h"
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
 * All of it under g_ledger_lock, which holds memory only: a leaf, taken under
 * the volume's state lock for the snapshot and alone everywhere else.
 * `g_ledger_use_edits` moves with every change of the context, and
 * `g_ledger_use_landed` is the edit the medium's newest copy carries. */
TagFSLock         g_ledger_lock = TAGFS_LOCK_INIT("ledger", LOCK_RANK_LEAF);
static uint64_t   g_ledger_seq;
static uint32_t   g_ledger_current;
static char       g_ledger_use[TAGFS_SECTOR_SIZE];
static uint16_t   g_ledger_use_len;
static uint64_t   g_ledger_use_edits;
static uint64_t   g_ledger_use_landed;
#define LEDGER_USE_MAX  ((size_t)TAGFS_SECTOR_SIZE - sizeof(VolumeLedger))

_Static_assert(sizeof(VolumeLedger) < TAGFS_SECTOR_SIZE,
               "the Ledger's fixed fields, and room for a Use Context, fit its one sector");

/*
 * ‼ ONE WRITER: THE SETTLE CYCLE'S S4. The image is taken at the cycle's S0,
 * under the volume's state lock and then the Ledger's — the counters of the
 * same instant as the pool the cycle writes — and written at S4 with no lock
 * held. The holder of the Settle's Scribe alone touches it, one cycle at a
 * time, so two writers can never build a record for the same older copy.
 */
static struct {
    uint8_t  sector[TAGFS_SECTOR_SIZE];
    uint64_t seq;
    uint32_t slot;
    uint32_t epoch;
    uint64_t use_edits;
} s_image;

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

void LedgerImageTake(void)
{
    TagFSState  *fs  = tagfs_get_state();
    uint64_t     now = rtc_get_unix64();
    VolumeLedger led;

    memset(s_image.sector, 0, sizeof(s_image.sector));

    TagFSLockTake(&g_ledger_lock);
    led = fs->ledger;
    uint16_t use_len = g_ledger_use_len;
    memcpy(s_image.sector + sizeof(led), g_ledger_use, use_len);
    s_image.seq       = g_ledger_seq + 1;
    s_image.slot      = (g_ledger_current + 1) % VOLUME_LEDGER_COPIES;
    s_image.epoch     = tagfs_mount_epoch();
    s_image.use_edits = g_ledger_use_edits;
    TagFSLockDrop(&g_ledger_lock);

    /* The remembered Use Context follows the fixed fields and is summed with
     * them: a torn write loses the context together with the counters, never
     * one without the other. */
    memcpy(led.magic, g_ledger_magic, sizeof(g_ledger_magic));
    led.seq                = s_image.seq;
    led.written_unix       = now;
    led.use_context_offset = use_len ? (uint16_t)sizeof(VolumeLedger) : 0;
    led.use_context_bytes  = use_len;
    led.bytes              = (uint32_t)sizeof(VolumeLedger) + use_len;
    led.crc32              = 0;
    memcpy(s_image.sector, &led, sizeof(led));
    led.crc32 = KCrc32(s_image.sector, led.bytes);
    memcpy(s_image.sector + __builtin_offsetof(VolumeLedger, crc32), &led.crc32,
           sizeof(led.crc32));
}

/*
 * The image to the older copy. Only once it landed is the new copy the one the
 * volume stands on — and only for the image's own volume, by the Settle's own
 * holder: a mount between the snapshot and here means another volume's Ledger
 * is the one in memory now. A refusal leaves the volume standing on the copy
 * it stood on; the copy aimed at may be torn or untouched, and it is the older
 * one either way, which the next cycle aims at again.
 */
error_t LedgerImageWrite(void)
{
    int wrote = tagfs_volume_write(ledger_vlba(s_image.slot), 1, s_image.sector);

    TagFSLockTake(&g_ledger_lock);
    if (wrote == 0 && ScribeHeldHere(&g_settle_scribe) && tagfs_mount_epoch() == s_image.epoch) {
        g_ledger_seq        = s_image.seq;
        g_ledger_current    = s_image.slot;
        g_ledger_use_landed = s_image.use_edits;
    }
    uint64_t stands_seq = g_ledger_seq;
    uint32_t stands     = g_ledger_current;
    TagFSLockDrop(&g_ledger_lock);

    if (wrote != 0) {
        kprintf("[Ledger] copy %u would not take seq %llu — the volume still "
                "stands on copy %u (seq %llu)\n",
                s_image.slot, (unsigned long long)s_image.seq, stands,
                (unsigned long long)stands_seq);
        return ERR_TAGFS_METADATA_ERROR;
    }
    return OK;
}

error_t tagfs_remember_use_context(const char *list, size_t len, bool *remembered)
{
    if (!list && len) return ERR_INVALID_ARGUMENT;

    /* A list the record cannot hold is not cut short and not kept: the volume
     * forgets rather than remembering half of what was said, and says so. */
    bool fits = len <= LEDGER_USE_MAX;

    /* What the medium already holds is not written again: a mount recalls the
     * context and remembers it at once, and an unchanged one costs nothing. */
    TagFSLockTake(&g_ledger_lock);
    uint16_t kept   = fits ? (uint16_t)len : 0;
    bool     same   = g_ledger_use_len == kept &&
                      (kept == 0 || memcmp(g_ledger_use, list, kept) == 0);
    bool     landed = same && g_ledger_use_landed == g_ledger_use_edits;
    if (!same) {
        g_ledger_use_len = kept;
        if (kept) memcpy(g_ledger_use, list, kept);
        g_ledger_use_edits++;
    }
    TagFSLockDrop(&g_ledger_lock);

    /* A demand on the one writer: the cycle that covers it carries the
     * context in the Ledger it writes. */
    error_t rc = landed ? OK : TagFSSettle();
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
    g_ledger_use_len    = 0;
    g_ledger_use_edits  = 0;
    g_ledger_use_landed = 0;
    TagFSLockDrop(&g_ledger_lock);
}
