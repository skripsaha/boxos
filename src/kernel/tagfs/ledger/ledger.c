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

/* The copy the volume stands on, and the highest seq the medium holds — the
 * next write goes to the other copy with that seq + 1: this is the whole of
 * the alternation. The copy stood on is the newest intact one, unless the
 * mount found the medium does not bear its seal out and stood on the one
 * before it (LedgerStandOn); then the next write goes over the newer copy,
 * never the one stood on, and its seq is still past every seq the medium
 * holds. Moved only by a pass that landed, so the copy named here is always
 * intact.
 *
 * What the volume remembers of the Use Context — the comma-joined text it was
 * last told, held by reference and written out with the counters on every
 * Ledger write. One sector is the record's whole world (ledger_read_copy takes
 * one and refuses a longer claim), so this is bounded by what a sector holds
 * past the fields and the seal: a text that will not fit is not kept in part.
 *
 * All of it under g_ledger_lock, which holds memory only: a leaf, taken under
 * the volume's state lock for the snapshot and alone everywhere else; nothing
 * is allocated or freed under it. `g_ledger_use_saying` is the machine's word
 * the memory is (0 for the medium's own, read at the mount), so a word said
 * earlier never lands over a later one. `g_ledger_use_edits` moves with every
 * change of the memory, and `g_ledger_use_landed` is the edit the medium's
 * newest copy carries. */
TagFSLock         g_ledger_lock = TAGFS_LOCK_INIT("ledger", LOCK_RANK_LEAF);
static uint64_t   g_ledger_seq;
static uint32_t   g_ledger_current;
static UseText   *g_ledger_use;
static uint64_t   g_ledger_use_saying;
static uint64_t   g_ledger_use_edits;
static uint64_t   g_ledger_use_landed;
#define LEDGER_USE_MAX  ((size_t)TAGFS_SECTOR_SIZE - sizeof(VolumeLedger))

/* The copies as the mount read them, until it says which it stands on: each
 * intact one's record, and the context it remembers. The mount's alone — it
 * runs with the door shut. */
static VolumeLedger s_read[VOLUME_LEDGER_COPIES];
static bool         s_read_ok[VOLUME_LEDGER_COPIES];
static UseText     *s_read_use[VOLUME_LEDGER_COPIES];
static uint32_t     s_read_newest;

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
        TagFSNoteReadRefused();
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

    /* Summed as it was written, with the sum's own field zero: the field is
     * zeroed in the sector read, whose copy in `led` keeps the value. */
    memset(buf + __builtin_offsetof(VolumeLedger, crc32), 0, sizeof(uint32_t));
    uint32_t have = KCrc32(buf, led.bytes);
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

/* The copies the mount read, let go of — their contexts with them. */
static void LedgerReadForget(void)
{
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        UseTextDrop(s_read_use[c]);
        s_read_use[c] = NULL;
        s_read_ok[c]  = false;
    }
}

/*
 * Both copies read, and the newer intact one taken up for now: the counters
 * into the volume's state. Which copy the volume stands on is the mount's to
 * say once it has held the seals against the medium (LedgerStandOn).
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
    TagFSState *fs   = tagfs_get_state();
    uint32_t    live = 0;

    LedgerReadForget();

    /* Each copy's remembered context, read with it. Off the stack: two
     * sectors' worth. */
    char *tails = kmalloc((size_t)VOLUME_LEDGER_COPIES * TAGFS_SECTOR_SIZE);
    uint16_t tail_len[VOLUME_LEDGER_COPIES];
    if (!tails) return ERR_NO_MEMORY;

    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        s_read_ok[c] = ledger_read_copy(c, &s_read[c], tails + (size_t)c * TAGFS_SECTOR_SIZE,
                                        &tail_len[c]);
        if (s_read_ok[c]) live++;
    }

    if (live == 0) {
        kfree(tails);
        kprintf("[Ledger] neither copy is readable — this volume cannot say "
                "what is on it\n");
        return ERR_TAGFS_CORRUPTED;
    }

    uint32_t newest = 0;
    bool     have   = false;
    uint64_t top    = 0;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (!s_read_ok[c]) continue;
        if (!have || s_read[c].seq > s_read[newest].seq) {
            newest = c;
            have   = true;
        }
        if (s_read[c].seq > top)
            top = s_read[c].seq;
    }

    /* A record of this kernel's has its context after the seal: a list
     * longer than that leaves the room for — one an earlier kernel kept
     * where the seal is now — would overrun the sector the next write
     * builds. Forgotten whole, as remembering forgets one — never kept in
     * part. */
    error_t rc = OK;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES && rc == OK; c++) {
        uint16_t kept = s_read_ok[c] ? tail_len[c] : 0;
        if (kept > LEDGER_USE_MAX) {
            if (c == newest)
                kprintf("[Ledger] copy %u remembers a Use Context of %u bytes, longer "
                        "than the %u a record holds — it is forgotten, not kept in part\n",
                        c, (unsigned)kept, (unsigned)LEDGER_USE_MAX);
            kept = 0;
        }
        if (kept && !(s_read_use[c] = UseTextMake(tails + (size_t)c * TAGFS_SECTOR_SIZE, kept)))
            rc = ERR_NO_MEMORY;
    }
    kfree(tails);
    if (rc != OK) {
        LedgerReadForget();
        return rc;
    }

    s_read_newest = newest;
    fs->ledger    = s_read[newest];

    TagFSLockTake(&g_ledger_lock);
    g_ledger_seq     = top;
    g_ledger_current = newest;
    TagFSLockDrop(&g_ledger_lock);

    if (live < VOLUME_LEDGER_COPIES) {
        kprintf("[Ledger] running on copy %u alone (seq %llu); the other one "
                "did not survive\n",
                newest, (unsigned long long)s_read[newest].seq);
    }
    return OK;
}

uint32_t LedgerNewestCopy(void)
{
    return s_read_newest;
}

bool LedgerCopySeal(uint32_t copy, SettleMark *out)
{
    if (copy >= VOLUME_LEDGER_COPIES || !s_read_ok[copy])
        return false;
    const VolumeLedger *led = &s_read[copy];
    if (!LedgerHas(led, VOLUME_LEDGER_SEAL_END) || led->mark.settle == 0 ||
        led->mark.settle != led->seq)
        return false;
    *out = led->mark;
    return true;
}

void LedgerStandOn(uint32_t copy)
{
    TagFSState *fs = tagfs_get_state();
    if (copy >= VOLUME_LEDGER_COPIES || !s_read_ok[copy])
        copy = s_read_newest;

    fs->ledger = s_read[copy];
    UseText *memory = s_read_use[copy];
    s_read_use[copy] = NULL;

    TagFSLockTake(&g_ledger_lock);
    UseText *gone       = g_ledger_use;
    g_ledger_current    = copy;
    g_ledger_use        = memory;
    g_ledger_use_saying = 0;
    TagFSLockDrop(&g_ledger_lock);
    UseTextDrop(gone);
    LedgerReadForget();
}

void LedgerImageTake(void)
{
    TagFSState  *fs  = tagfs_get_state();
    uint64_t     now = rtc_get_unix64();
    VolumeLedger led;

    memset(s_image.sector, 0, sizeof(s_image.sector));

    TagFSLockTake(&g_ledger_lock);
    led = fs->ledger;
    uint16_t use_len = g_ledger_use ? (uint16_t)g_ledger_use->bytes : 0;
    if (use_len)
        memcpy(s_image.sector + sizeof(led), g_ledger_use->text, use_len);
    s_image.seq       = g_ledger_seq + 1;
    s_image.slot      = (g_ledger_current + 1) % VOLUME_LEDGER_COPIES;
    s_image.epoch     = tagfs_mount_epoch();
    s_image.use_edits = g_ledger_use_edits;
    TagFSLockDrop(&g_ledger_lock);

    /* The remembered Use Context follows the fields and the seal and is
     * summed with them: a torn write loses the context together with the
     * counters, never one without the other. The seal and the sum go in at
     * S4, once the cycle knows what it left on the medium. */
    memcpy(led.magic, g_ledger_magic, sizeof(g_ledger_magic));
    led.seq                = s_image.seq;
    led.written_unix       = now;
    led.use_context_offset = use_len ? (uint16_t)sizeof(VolumeLedger) : 0;
    led.use_context_bytes  = use_len;
    led.bytes              = (uint32_t)sizeof(VolumeLedger) + use_len;
    led.crc32              = 0;
    memset(&led.mark, 0, sizeof(led.mark));
    memcpy(s_image.sector, &led, sizeof(led));
}

/*
 * The image, sealed, to the other copy. Only once it landed is the new copy
 * the one the volume stands on — and only for the image's own volume, by the
 * Settle's own holder: a mount between the snapshot and here means another
 * volume's Ledger is the one in memory now. A refusal leaves the volume
 * standing on the copy it stood on; the copy aimed at may be torn or
 * untouched, and it is never the one stood on, which the next cycle aims past
 * again.
 */
error_t LedgerImageWrite(const SettleMark *mark)
{
    SettleMark sealed = *mark;
    uint32_t   bytes;
    sealed.settle = s_image.seq;
    memcpy(s_image.sector + __builtin_offsetof(VolumeLedger, mark), &sealed, sizeof(sealed));
    memcpy(&bytes, s_image.sector + __builtin_offsetof(VolumeLedger, bytes), sizeof(bytes));
    memset(s_image.sector + __builtin_offsetof(VolumeLedger, crc32), 0, sizeof(uint32_t));
    uint32_t crc = KCrc32(s_image.sector, bytes);
    memcpy(s_image.sector + __builtin_offsetof(VolumeLedger, crc32), &crc, sizeof(crc));

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

error_t tagfs_remember_use_context(UseText *said, uint64_t saying, bool *remembered)
{
    /* A text the record cannot hold is not cut short and not kept: the volume
     * forgets rather than remembering half of what was said, and says so. */
    bool     fits = !said || said->bytes <= LEDGER_USE_MAX;
    UseText *keep = UseTextHold(fits ? said : NULL);
    UseText *gone = NULL;

    /* What the medium already holds is not written again: a mount recalls the
     * context and remembers it at once, and an unchanged one costs nothing. */
    TagFSLockTake(&g_ledger_lock);
    bool later  = saying < g_ledger_use_saying;
    bool landed = false;
    if (!later) {
        bool same = UseTextSame(g_ledger_use, keep);
        landed              = same && g_ledger_use_landed == g_ledger_use_edits;
        g_ledger_use_saying = saying;
        if (!same) {
            gone         = g_ledger_use;
            g_ledger_use = keep;
            keep         = NULL;
            g_ledger_use_edits++;
        }
    }
    TagFSLockDrop(&g_ledger_lock);
    UseTextDrop(keep);
    UseTextDrop(gone);

    /* A later word already stands: this one is not what the volume holds. */
    if (later) {
        if (remembered) *remembered = false;
        return OK;
    }

    /* A demand on the one writer: the cycle that covers it carries the
     * context in the Ledger it writes. */
    error_t rc = landed ? OK : TagFSSettle();
    if (remembered) *remembered = fits && rc == OK;
    return rc;
}

UseText *tagfs_recall_use_context(uint64_t *out_saying)
{
    TagFSLockTake(&g_ledger_lock);
    UseText *memory = UseTextHold(g_ledger_use);
    uint64_t saying = g_ledger_use_saying;
    TagFSLockDrop(&g_ledger_lock);
    if (out_saying) *out_saying = saying;
    return memory;
}

void LedgerLetGo(void)
{
    LedgerReadForget();
    TagFSLockTake(&g_ledger_lock);
    UseText *gone       = g_ledger_use;
    g_ledger_use        = NULL;
    g_ledger_use_saying = 0;
    g_ledger_use_edits  = 0;
    g_ledger_use_landed = 0;
    TagFSLockDrop(&g_ledger_lock);
    UseTextDrop(gone);
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
