#include "ledger.h"
#include "../tagfs.h"
#include "../scribe/scribe.h"
#include "../settle/settle.h"
#include "../use_memory/use_memory.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../../../lib/kernel/crypto.h"
#include "boardroom.h"     /* a copy read off whichever seat the medium sits in */

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
 * Under g_ledger_lock, which holds memory only: a leaf, taken under the
 * volume's state lock for the snapshot and alone everywhere else; nothing is
 * allocated or freed under it. */
TagFSLock         g_ledger_lock = TAGFS_LOCK_INIT("ledger", LOCK_RANK_LEAF);
static uint64_t   g_ledger_seq;
static uint32_t   g_ledger_current;

/* The record is one sector: the fixed fields, the seal and the Use Context's
 * chain's name all stay inside what ledger_read_copy takes. */
STATIC_ASSERT(sizeof(VolumeLedger) <= TAGFS_SECTOR_SIZE, "the Ledger's record is one sector");

/* The copies as the mount read them, until it says which it stands on: each
 * intact one's record, and the text a kernel before the Use Context's chain
 * kept in it. The mount's alone — it runs with the door shut. */
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
} s_image;

static uint64_t ledger_vlba(uint32_t copy)
{
    return ((uint64_t)TagFSLayout()->state_block + copy) * TAGFS_BLOCK_SECTORS;
}

/*
 * Read one copy off `seat`, the volume's ground beginning at sector `base`.
 * Returns true when it is present, intact, and long enough to hold what this
 * kernel knows how to read — a Ledger written by a newer kernel is longer than
 * this one expects and is read up to what is understood, which is what `bytes`
 * is for. `tail` may be NULL for a reader that wants no inherited text.
 */
static bool ledger_read_copy(uint8_t seat, uint64_t base, uint32_t copy, VolumeLedger *out,
                             char *tail, uint16_t *tail_len)
{
    uint8_t buf[TAGFS_SECTOR_SIZE];
    if (BoardroomRead(seat, base + ledger_vlba(copy), 1, buf) != 0) {
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

    /* A text a kernel before the Use Context's chain kept rides inside the
     * summed bytes — so it is already proven intact by the checksum above;
     * only its place in the record is checked here. A record that names a
     * chain carries none. */
    *tail_len = 0;
    if (tail && led.use_context_bytes && !LedgerHas(&led, VOLUME_LEDGER_USE_CHAIN_END)) {
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
 * is, and what it remembers read off the medium too — its chain, followed
 * and proven, or the text a kernel before the chain kept in it: a proof that
 * what was said to the volume is what it holds. */
bool tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, UseText **memory)
{
    if (memory) *memory = NULL;
    if (copy >= VOLUME_LEDGER_COPIES || !out) return false;

    char    *tail     = kmalloc(TAGFS_SECTOR_SIZE);
    uint16_t tail_len = 0;
    if (!tail) return false;
    bool ok = ledger_read_copy(tagfs_get_seat(), tagfs_get_volume_base(), copy, out, tail,
                               &tail_len);

    UseChainRef ref;
    if (ok && memory && UseChainRefOf(out, &ref) && ref.first != VOLUME_LEDGER_NO_BLOCK) {
        UseChain chain;
        (void)UseChainRead(&ref, memory, &chain);
        UseChainForget(&chain);
    } else if (ok && memory && tail_len) {
        *memory = UseTextMake(tail, tail_len);
    }
    kfree(tail);
    return ok;
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
        s_read_ok[c] = ledger_read_copy(tagfs_get_seat(), tagfs_get_volume_base(), c, &s_read[c],
                                        tails + (size_t)c * TAGFS_SECTOR_SIZE, &tail_len[c]);
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

    /* The texts kernels before the Use Context's chain kept, for the use
     * memory to carry into one (use_memory.h): never written back here. */
    error_t rc = OK;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES && rc == OK; c++) {
        uint16_t kept = s_read_ok[c] ? tail_len[c] : 0;
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

    /* And what the copies remember of the Use Context — read with them, the
     * chains they name followed and proven (use_memory.h). */
    rc = UseMemoryLoad();
    if (rc != OK) {
        kprintf("[Ledger] there is no memory to hold what this volume remembers of its "
                "Use Context\n");
        LedgerReadForget();
    }
    return rc;
}

uint32_t LedgerNewestCopy(void)
{
    return s_read_newest;
}

const VolumeLedger *LedgerCopyRead(uint32_t copy)
{
    return copy < VOLUME_LEDGER_COPIES && s_read_ok[copy] ? &s_read[copy] : NULL;
}

UseText *LedgerCopyInherited(uint32_t copy)
{
    return copy < VOLUME_LEDGER_COPIES && s_read_ok[copy] ? UseTextHold(s_read_use[copy]) : NULL;
}

/* A record's seal, when it carries one that is its own. */
static bool ledger_seal_of(const VolumeLedger *led, SettleMark *out)
{
    if (!LedgerHas(led, VOLUME_LEDGER_SEAL_END) || led->mark.settle == 0 ||
        led->mark.settle != led->seq)
        return false;
    *out = led->mark;
    return true;
}

bool LedgerCopySeal(uint32_t copy, SettleMark *out)
{
    if (copy >= VOLUME_LEDGER_COPIES || !s_read_ok[copy])
        return false;
    return ledger_seal_of(&s_read[copy], out);
}

/*
 * What this kernel left on the medium, for a medium that comes back
 * (LedgerLeftAs): the record the volume stands on once a cycle settled it —
 * or the one the mount stood on — and the image the last S4 aimed at the
 * other copy, answered or not: a medium that went mid-cycle may hold either.
 * Before any S4, that other is the newer copy the mount would not stand on.
 * Each is known by its seq and its seal's {settle, closed_crc32, tail_crc32};
 * a record with no seal of its own is known by its seq alone. Under
 * g_ledger_lock.
 */
typedef struct {
    bool     known;
    uint64_t seq;
    uint64_t settle;
    uint32_t closed_crc32;
    uint32_t tail_crc32;
} LedgerLeftMark;

static LedgerLeftMark s_left_settled;
static LedgerLeftMark s_left_tried;

static LedgerLeftMark ledger_left_mark(const VolumeLedger *led)
{
    SettleMark     seal;
    LedgerLeftMark m = { .known = true, .seq = led->seq };
    if (ledger_seal_of(led, &seal)) {
        m.settle       = seal.settle;
        m.closed_crc32 = seal.closed_crc32;
        m.tail_crc32   = seal.tail_crc32;
    }
    return m;
}

void LedgerStandOn(uint32_t copy)
{
    TagFSState *fs = tagfs_get_state();
    if (copy >= VOLUME_LEDGER_COPIES || !s_read_ok[copy])
        copy = s_read_newest;

    fs->ledger = s_read[copy];

    /* A newer copy the mount would not stand on is on the medium all the
     * same, as this kernel found it: until a cycle writes past it, it is what
     * a medium that comes back holds newest. */
    TagFSLockTake(&g_ledger_lock);
    g_ledger_current = copy;
    s_left_settled   = ledger_left_mark(&s_read[copy]);
    s_left_tried.known = false;
    if (copy != s_read_newest && s_read_ok[s_read_newest])
        s_left_tried = ledger_left_mark(&s_read[s_read_newest]);
    TagFSLockDrop(&g_ledger_lock);
    LedgerReadForget();
}

void LedgerSettled(void)
{
    TagFSLockTake(&g_ledger_lock);
    s_left_settled     = s_left_tried;
    s_left_tried.known = false;
    TagFSLockDrop(&g_ledger_lock);
}

bool LedgerNewestOn(uint8_t seat, uint64_t base, VolumeLedger *out)
{
    VolumeLedger copy;
    uint16_t     none = 0;
    bool         have = false;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (!ledger_read_copy(seat, base, c, &copy, NULL, &none))
            continue;
        if (!have || copy.seq > out->seq)
            *out = copy;
        have = true;
    }
    return have;
}

LedgerLeft LedgerLeftAs(const VolumeLedger *newest, uint64_t *out_settled_seq)
{
    TagFSLockTake(&g_ledger_lock);
    LedgerLeftMark settled = s_left_settled;
    LedgerLeftMark tried   = s_left_tried;
    TagFSLockDrop(&g_ledger_lock);

    LedgerLeftMark there = ledger_left_mark(newest);
    *out_settled_seq = settled.seq;
    for (int i = 0; i < 2; i++) {
        const LedgerLeftMark *m = i ? &tried : &settled;
        if (m->known && m->seq == there.seq && m->settle == there.settle &&
            m->closed_crc32 == there.closed_crc32 && m->tail_crc32 == there.tail_crc32)
            return LEDGER_AS_LEFT;
    }
    return settled.known && there.seq < settled.seq ? LEDGER_OLDER : LEDGER_OTHER;
}

void LedgerImageTake(void)
{
    TagFSState  *fs  = tagfs_get_state();
    uint64_t     now = rtc_get_unix64();
    VolumeLedger led;

    memset(s_image.sector, 0, sizeof(s_image.sector));

    TagFSLockTake(&g_ledger_lock);
    led = fs->ledger;
    s_image.seq   = g_ledger_seq + 1;
    s_image.slot  = (g_ledger_current + 1) % VOLUME_LEDGER_COPIES;
    s_image.epoch = tagfs_mount_epoch();
    TagFSLockDrop(&g_ledger_lock);

    /* ‼ NO TEXT IS LAID IN THIS SECTOR. The Use Context lives in its own
     * chain, which the image names (LedgerImageUseChain); the fields a
     * kernel before the chain kept a text in are zero, so no reader finds
     * one. The seal and the sum go in at S4, once the cycle knows what it
     * left on the medium. */
    memcpy(led.magic, g_ledger_magic, sizeof(g_ledger_magic));
    led.seq                = s_image.seq;
    led.written_unix       = now;
    led.use_context_offset = 0;
    led.use_context_bytes  = 0;
    led.bytes              = (uint32_t)sizeof(VolumeLedger);
    led.crc32              = 0;
    memset(&led.mark, 0, sizeof(led.mark));
    led.use_chain_first    = VOLUME_LEDGER_NO_BLOCK;
    led.use_chain_attempt  = 0;
    led.use_chain_seq      = 0;
    led.use_chain_bytes    = 0;
    led.use_chain_crc32    = 0;
    memcpy(s_image.sector, &led, sizeof(led));
}

uint64_t LedgerImageSeq(void)
{
    return s_image.seq;
}

uint32_t LedgerImageSlot(void)
{
    return s_image.slot;
}

void LedgerImageUseChain(const UseChainRef *ref)
{
    bool         named = ref->first != VOLUME_LEDGER_NO_BLOCK;
    VolumeLedger led;
    memcpy(&led, s_image.sector, sizeof(led));
    led.use_chain_first   = ref->first;
    led.use_chain_attempt = named ? ref->attempt : 0;
    led.use_chain_seq     = named ? ref->seq : 0;
    led.use_chain_bytes   = named ? ref->bytes : 0;
    led.use_chain_crc32   = named ? ref->crc32 : 0;
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
    if (SettleV1OnBook("the Ledger's write"))
        return ERR_UNSUPPORTED;
    SettleMark sealed = *mark;
    uint32_t   bytes;
    sealed.settle = s_image.seq;
    memcpy(s_image.sector + __builtin_offsetof(VolumeLedger, mark), &sealed, sizeof(sealed));
    memcpy(&bytes, s_image.sector + __builtin_offsetof(VolumeLedger, bytes), sizeof(bytes));
    memset(s_image.sector + __builtin_offsetof(VolumeLedger, crc32), 0, sizeof(uint32_t));
    uint32_t crc = KCrc32(s_image.sector, bytes);
    memcpy(s_image.sector + __builtin_offsetof(VolumeLedger, crc32), &crc, sizeof(crc));

    /* Named as what this kernel may have left on the medium before a byte of
     * it moves: a medium that goes mid-write may keep it whole. */
    TagFSLockTake(&g_ledger_lock);
    if (tagfs_mount_epoch() == s_image.epoch)
        s_left_tried = (LedgerLeftMark){ .known = true, .seq = s_image.seq,
                                         .settle = sealed.settle,
                                         .closed_crc32 = sealed.closed_crc32,
                                         .tail_crc32 = sealed.tail_crc32 };
    TagFSLockDrop(&g_ledger_lock);

    int wrote = tagfs_volume_write(ledger_vlba(s_image.slot), 1, s_image.sector);

    TagFSLockTake(&g_ledger_lock);
    if (wrote == 0 && ScribeHeldHere(&g_settle_scribe) && tagfs_mount_epoch() == s_image.epoch) {
        g_ledger_seq     = s_image.seq;
        g_ledger_current = s_image.slot;
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

void LedgerLetGo(void)
{
    LedgerReadForget();
    TagFSLockTake(&g_ledger_lock);
    s_left_settled.known = false;
    s_left_tried.known   = false;
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
 * refused out loud and the counter does not move. The kernel holds an id in
 * 64 bits; this counter is format v1's, so no id it issues is past a u32 —
 * format v2 carries 64-bit ids (Quill design §2.3).
 */
error_t LedgerIssueFileId(VolumeLedger *led, uint64_t *out_fid)
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
