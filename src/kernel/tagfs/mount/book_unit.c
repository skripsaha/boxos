#include "book_unit.h"
#include "crypto.h"
#include "cpuid.h"

/*
 * Sectors of the book read and judged (book_unit.h).
 */

/* Whether the crc32 instruction has been held to the book's table this boot,
 * and whether a want of memory to hold it was said. */
static volatile uint32_t s_sums_judged;
static volatile uint32_t s_sums_short;

/*
 * ‼ THE INSTRUCTION IS HELD TO THE TABLE BEFORE IT JUDGES A BOOK. KCrc32c sums
 * by crc32 where every core has SSE4.2 and by the book's table where one has
 * not — the same sum either way, by the arithmetic, and the startup suite
 * `book` holds them to each other on every boot that mounts a volume. But
 * the book suite runs inside a mounted volume (tests.c), and a book is
 * judged by its sums before it is mounted: on a processor whose instruction
 * answered otherwise, every sector would be refused as rot, and the volume
 * with them, with nothing said about why. So the first sector of a book this
 * boot judges is preceded by the check, once: CRC-32/ISCSI's check text and a
 * sector's worth of the golden rule's bytes (volume_book.h), by the kernel's
 * sum and by the table. A disagreement is said, and has_sse42 is lowered on
 * every core — the one way it moves (cpuid.c, cpu_intersect_features_ap) —
 * so every sum from then on is the table's.
 */
static void BookSumsJudged(void)
{
    static const uint8_t check[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

    if (__atomic_exchange_n(&s_sums_judged, 1u, __ATOMIC_ACQ_REL) || !g_cpu_caps.has_sse42)
        return;
    uint8_t* text = kmalloc(BOOK_SECTOR_BYTES);
    if (!text) {
        __atomic_store_n(&s_sums_judged, 0u, __ATOMIC_RELEASE);
        if (!__atomic_exchange_n(&s_sums_short, 1u, __ATOMIC_ACQ_REL))
            kprintf("[TagFS] the crc32 instruction is not yet held to the book's table: there is no "
                    "memory for a sector of its bytes — it is asked again before the next unit is "
                    "judged\n");
        return;
    }
    uint32_t had  = KCrc32c(check, sizeof(check));
    uint32_t want = BOOK_CRC32C_CHECK;
    if (had == want) {
        for (uint32_t i = 0; i < BOOK_SECTOR_BYTES; i++)
            text[i] = (uint8_t)(i * 7u + 3u);
        had  = KCrc32c(text, BOOK_SECTOR_BYTES);
        want = ~BookCrc32cOn(0xFFFFFFFFu, text, BOOK_SECTOR_BYTES);
    }
    kfree(text);
    if (had == want)
        return;
    __atomic_store_n(&g_cpu_caps.has_sse42, false, __ATOMIC_RELEASE);
    kprintf("[TagFS] this processor's crc32 instruction sums otherwise than the book's table "
            "(0x%08x where the table says 0x%08x) — every core sums the book by the table from "
            "here\n", had, want);
}

/* One sector at `vlba`: whether its head is one a writer writes and its sum,
 * seeded with the volume and the sector's own address, agrees. */
static uint8_t BookSectorJudged(const uint8_t* s, const uint8_t uuid[16], uint64_t vlba)
{
    BookSectorHead h;
    if (!BookSectorHeadHolds(s, &h))
        return BOOK_SECTOR_LOOSE;
    uint32_t sum = KCrc32cContinue(~BookSectorSeed(uuid, vlba), s + 4, BOOK_SECTOR_BYTES - 4u);
    return sum == h.crc32c ? BOOK_SECTOR_HOLDS : BOOK_SECTOR_LOOSE;
}

/* The largest seq a sector of the book read held at (spec_v2 §2.2, §4.5
 * п.8): the next seq is past everything the mount saw hold, the torn tail and
 * the direct scan among it — every reader of a book reads through here, so
 * nothing read is left out. The mount reads it with its door shut; but the
 * DIGESTS leaves a read of a block opens after the mount (TallyDigestFind)
 * read through here too, on any core at once — so it only ever rises, by an
 * atomic max, and no fold is lost to another. */
static uint64_t s_seq_seen;

static void BookSeqFold(const BookUnit* u)
{
    for (uint32_t k = 0; k < u->count; k++) {
        BookSectorHead h;
        if (u->state[k] != BOOK_SECTOR_HOLDS)
            continue;
        memcpy(&h, u->sectors + (uint64_t)k * BOOK_SECTOR_BYTES, sizeof(h));
        uint64_t seen = __atomic_load_n(&s_seq_seen, __ATOMIC_RELAXED);
        while (h.seq > seen &&
               !__atomic_compare_exchange_n(&s_seq_seen, &seen, h.seq, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;
    }
}

error_t BookUnitRefused(int rc)
{
    TagFSNoteReadRefused();
    return rc == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY : ERR_READ_FAILED;
}

/* A run the medium could not read whole, read again a sector at a time: the
 * ones it cannot read named UNREAD, every other judged. */
static error_t BookUnitBySector(BookUnit* u, const uint8_t uuid[16])
{
    for (uint32_t k = 0; k < u->count; k++) {
        uint8_t* s  = u->sectors + (uint64_t)k * BOOK_SECTOR_BYTES;
        int      rc = tagfs_volume_read(u->vlba + k, 1, s);
        if (rc == (int)ERR_BAD_SECTOR) {
            memset(s, 0, BOOK_SECTOR_BYTES);
            u->state[k] = BOOK_SECTOR_UNREAD;
            continue;
        }
        if (rc != 0)
            return BookUnitRefused(rc);
        u->state[k] = BookSectorJudged(s, uuid, u->vlba + k);
    }
    return OK;
}

/* Whether the judged sectors are one whole unit of `kind` and `seq` (0: any),
 * in the reader's words when they are not — the census's words, so the two
 * readers of a book say a copy is lost the same way (bookground.py, Unit).
 * ERR_NO_MEMORY when a whole unit's stream had none to be laid in. */
static error_t BookUnitWholeOf(BookUnit* u, uint16_t kind, uint64_t seq)
{
    for (uint32_t k = 0; k < u->count; k++) {
        if (u->state[k] != BOOK_SECTOR_HOLDS) {
            ksnprintf(u->why, sizeof(u->why), "sector %u %s", k,
                      u->state[k] == BOOK_SECTOR_UNREAD ? "will not read" : "does not hold");
            return OK;
        }
    }
    BookSectorHead first;
    memcpy(&first, u->sectors, sizeof(first));
    if (!BookUnitHeadsAgree(u->sectors, u->count, &u->head) || (kind && first.kind != kind)) {
        /* Named where it first parts from the unit the run should be. */
        for (uint32_t k = 0; k < u->count; k++) {
            BookSectorHead h;
            memcpy(&h, u->sectors + (uint64_t)k * BOOK_SECTOR_BYTES, sizeof(h));
            if ((kind && h.kind != kind) || h.at != k || h.of != u->count || h.seq != first.seq ||
                h.kind != first.kind) {
                ksnprintf(u->why, sizeof(u->why), "sector %u is part %u of %u of a unit of kind %u, seq %llu",
                          k, h.at, h.of, h.kind, (unsigned long long)h.seq);
                return OK;
            }
        }
    }
    if (seq && u->head.seq != seq) {
        ksnprintf(u->why, sizeof(u->why), "it was sealed at seq %llu where its pointer names %llu",
                  (unsigned long long)u->head.seq, (unsigned long long)seq);
        return OK;
    }
    u->stream = kmalloc((uint64_t)u->count * BOOK_PAYLOAD_BYTES);
    if (!u->stream)
        return ERR_NO_MEMORY;
    BookUnitStream(u->sectors, u->count, u->stream);
    u->whole = true;
    return OK;
}

/*
 * ‼ THE LENGTH A POINTER NAMES IS ITS WORD UNTIL THE UNIT AGREES. A node's
 * Sheets are bounded only by the volume's end (tally_read.c): a pointer worn
 * or written wrong may name a unit of a hundred thousand sectors where its
 * node is eight, and the heap may have no room for that many. Before such a
 * refusal is called a want of memory, the unit's first sector is asked what
 * unit it begins: one that does not hold, or begins another — of another
 * kind, seq or length — is a copy that does not hold, and the next copy is
 * read (§2.5); one that agrees begins a unit that long, and the want of
 * memory is real. *u holds that one sector.
 */
static error_t BookUnitFirstSays(BookUnit* u, const uint8_t uuid[16], uint16_t kind, uint64_t seq)
{
    BookSectorHead h;
    uint32_t       count = u->count;

    kfree(u->sectors);
    kfree(u->state);
    u->count   = 1;
    u->sectors = kmalloc(BOOK_SECTOR_BYTES);
    u->state   = kmalloc(1);
    if (!u->sectors || !u->state)
        return ERR_NO_MEMORY;
    int rc = tagfs_volume_read(u->vlba, 1, u->sectors);
    if (rc != 0 && rc != (int)ERR_BAD_SECTOR)
        return BookUnitRefused(rc);
    u->state[0] = rc ? BOOK_SECTOR_UNREAD : BookSectorJudged(u->sectors, uuid, u->vlba);
    memcpy(&h, u->sectors, sizeof(h));
    if (u->state[0] != BOOK_SECTOR_HOLDS)
        ksnprintf(u->why, sizeof(u->why), "sector 0 %s", rc ? "will not read" : "does not hold");
    else if (h.kind != kind || h.at != 0 || h.of != count || (seq && h.seq != seq))
        ksnprintf(u->why, sizeof(u->why), "its pointer names %u sectors, and sector 0 begins a unit of "
                  "kind %u, %u sectors, seq %llu", count, h.kind, h.of, (unsigned long long)h.seq);
    else
        return ERR_NO_MEMORY;
    return OK;
}

error_t BookUnitRead(uint64_t vlba, uint32_t count, uint16_t kind, uint64_t seq, BookUnit* out)
{
    const uint8_t* uuid = tagfs_get_state()->uuid;

    memset(out, 0, sizeof(*out));
    out->vlba  = vlba;
    out->count = count;
    if (count == 0) {
        ksnprintf(out->why, sizeof(out->why), "it names a unit of no sectors");
        return OK;
    }
    BookSumsJudged();
    out->sectors = kmalloc((uint64_t)count * BOOK_SECTOR_BYTES);
    out->state   = kmalloc(count);
    if (!out->sectors || !out->state)
        return kind == BOOK_UNIT_SHEET && count > 1 ? BookUnitFirstSays(out, uuid, kind, seq) : ERR_NO_MEMORY;

    int rc = tagfs_volume_read(vlba, count, out->sectors);
    if (rc == (int)ERR_BAD_SECTOR) {
        error_t again = BookUnitBySector(out, uuid);
        if (again != OK)
            return again;
    } else if (rc != 0) {
        return BookUnitRefused(rc);
    } else {
        for (uint32_t k = 0; k < count; k++)
            out->state[k] = BookSectorJudged(out->sectors + (uint64_t)k * BOOK_SECTOR_BYTES, uuid,
                                             vlba + k);
    }
    BookSeqFold(out);
    return BookUnitWholeOf(out, kind, seq);
}

uint64_t BookSeqSeen(void)
{
    return __atomic_load_n(&s_seq_seen, __ATOMIC_RELAXED);
}

void BookSeqGiven(uint64_t seq)
{
    uint64_t seen = __atomic_load_n(&s_seq_seen, __ATOMIC_RELAXED);
    while (seq > seen &&
           !__atomic_compare_exchange_n(&s_seq_seen, &seen, seq, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

void BookSeqSeenForget(void)
{
    __atomic_store_n(&s_seq_seen, 0, __ATOMIC_RELAXED);
}

void BookUnitLetGo(BookUnit* u)
{
    kfree(u->sectors);
    kfree(u->state);
    kfree(u->stream);
    u->sectors = u->state = u->stream = NULL;
    u->whole   = false;
}

bool BookUnitBlank(const BookUnit* u)
{
    uint64_t bytes = (uint64_t)u->count * BOOK_SECTOR_BYTES;
    for (uint32_t k = 0; k < u->count; k++)
        if (u->state[k] == BOOK_SECTOR_UNREAD)
            return false;
    for (uint64_t i = 0; i < bytes; i++)
        if (u->sectors[i])
            return false;
    return true;
}

uint32_t BookUnitLeadWrong(const BookUnit* u, const uint32_t* starts, uint32_t n, uint32_t* named,
                           uint32_t* given)
{
    uint32_t s = 0;
    for (uint32_t k = 0; k < u->count; k++) {
        uint64_t lo = (uint64_t)k * BOOK_PAYLOAD_BYTES;
        while (s < n && starts[s] < lo)
            s++;
        uint32_t want = s < n && starts[s] < lo + BOOK_PAYLOAD_BYTES
                            ? BOOK_HEAD_BYTES + (uint32_t)(starts[s] - lo) : 0;
        BookSectorHead h;
        memcpy(&h, u->sectors + (uint64_t)k * BOOK_SECTOR_BYTES, sizeof(h));
        if (h.lead != want) {
            *named = h.lead;
            *given = want;
            return k;
        }
    }
    return u->count;
}
