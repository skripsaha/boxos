/*
 * quilltest — the red tests of the Quill write core, asked one at a time.
 *
 * ‼ A PROGRAM, NOT A KERNEL TEST, BECAUSE THESE DEFECTS LIVE ON THE WAY IN.
 * The kernel's startup tests call tagfs_write directly. Every case here goes
 * the way a person's program goes — boxlib, a Manifest, the Storage and
 * System Decks, ObjWrite's crate bounce, the WriteJob on a machine that has
 * one — so what it sees is what anybody using this machine would see.
 *
 * ‼ IT SAYS WHAT IT SAW, IN FOUR WORDS, AND JUDGES NOTHING.
 * Every case says its verdict in exactly one line, and every figure in it was
 * measured:
 *
 *     [QUILL] <case>: PASS|DEFECT|PENDING|FAIL — <what was measured>
 *
 *   PASS     the machine did what the design says it must
 *   DEFECT   the defect this case was written for, seen, with its number
 *   PENDING  the answer can only be read after a reboot or a power cut
 *   FAIL     anything else, including a case that could not ask its question
 *
 * Progress lines carry numbers and no verdict word. The power-cut modes are
 * cut rather than ended: hog says only what it started and churn what it
 * wrote, a verdict each only when it fails; settle says PENDING before it
 * parks, and a FAIL after it only when the park ends before the power goes.
 *
 * Which defects are known, and which step of the plan discharges each, is
 * the host's table (tools/stand_caveat.sh, caveat_row). A DEFECT here is only
 * an observation; logcheck `quill` decides what it means.
 *
 * ‼ ONE CASE PER RUN, BY NAME, AND NO "ALL".
 * Most cases change the volume, several of them for good, and they need
 * different stands: a volume grown past what they write, a reboot between
 * two halves, a power cut, a machine whose kernel may be deleted. A case
 * runs only when it is named — `quilltest CASE [ARG…]` — and a bare
 * `quilltest` lists every case with what it asks and what it leaves behind.
 *
 * ‼ A CASE THAT CHANGES THE VOLUME ANCHORS BEFORE IT REPORTS.
 * A harness stops the machine by killing the emulator as soon as it has
 * read a verdict, and on this volume only `anchor` and an orderly shutdown
 * make a name, the table and the bitmap durable. So the verdict is printed
 * after the anchor, never before, and a kill cannot leave half a case on
 * the medium. The power-cut modes (settle, churn) are the exception by
 * design: being cut before they settle is what they are for.
 *
 * ‼ A CASE THAT HARMS MORE THAN ITS OWN FILES EXISTS ONLY FOR A THROWAWAY IMAGE.
 * quilltest is on every volume, and it wears `system`. sequential and
 * interleave leak blocks on the synchronous path of HEAD, fidhistory spends
 * fifty thousand file ids for good, usememory interns its 91 tags in the
 * volume's registry for good, and systemtag deletes the kernel the volume
 * boots. Those five are compiled only into a quilltest built with
 * VOLUME_TESTS=on — the key that already means a throwaway image, and that
 * builds the kernel's own writing tests — and anywhere else each answers one
 * FAIL saying so. The others read, or change only files of their own, or
 * are the power-cut modes, and stay in every build.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/use.h"
#include "box/bay.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"
#include "box/time.h"
#include "box/error.h"
#include "quilltest.h"

/* logcheck's quillmut edits these two lines by exact text — keep them verbatim
 * (and three in quilltest_throwaway.c) */
#define QUILL_BIG_BYTES      (256u << 20)
#define QUILL_USE_TAGS       91          /* 91 x "quiluNNNNN" + 90 commas = 1000 bytes */

#define QUILL_LONG_TAG_BYTES 256u        /* the first length ObjQuery drops, one past the tag width */
#define QUILL_READ_WINDOW    (1u << 20)
#define QUILL_COLD_BYTES     (1u << 20)  /* under 2 MiB: malloc leaves it to first touch */
#define QUILL_SEED_BIG       0xB16u
#define QUILL_SEED_COLD      0xC01Du

typedef struct {
    const char   *name;
    const char   *args;      /* the words after the name, as the listing shows them */
    uint32_t      argc;      /* how many words that is */
    QuillVerdict (*run)(const char *const *args);   /* NULL: not in this build */
    const char   *asks;
    const char   *leaves;
    bool          throwaway; /* a throwaway image's case: VOLUME_TESTS=on builds only */
} QuillCase;

/* A throwaway image's case is compiled only into a VOLUME_TESTS=on build
 * (the header says why); anywhere else its row stands, with nothing to run. */
#ifdef CONFIG_VOLUME_TESTS
#define QUILL_THROWAWAY(run) (run)
#else
#define QUILL_THROWAWAY(run) NULL
#endif

static uint64_t *g_window;                        /* QUILL_READ_WINDOW, taken on first read */

/* =========================================================================
 *  Bytes that say where they belong
 * ========================================================================= */

/* The word at file offset `off` of a stream seeded `seed`. Every 8 bytes of
 * every file written here are a function of where they sit, so a readback
 * names the first byte that is not where it was written — a block landed in
 * the wrong place reads as wrong, not merely as different. */
static uint64_t QuillWord(uint32_t seed, uint64_t off)
{
    uint64_t x = off ^ ((uint64_t)seed << 56);
    x *= 0x9E3779B97F4A7C15ull;
    return x ^ (x >> 29);
}

/* `len` bytes (a multiple of 8) of the stream, from file offset `base`. */
void QuillFill(uint64_t *dst, uint32_t seed, uint64_t base, uint64_t len)
{
    for (uint64_t i = 0; i < len / 8; i++)
        dst[i] = QuillWord(seed, base + i * 8);
}

/* Read [0, size) back and compare every word. Returns the offset of the
 * first byte that is not what was written — `size` when all of it is — and
 * leaves in *err the answer of the read that stopped it (a refusal, or a
 * short count), 0 when every read answered in full. A window at a time and
 * never the whole file: ObjRead bounces its answer through one kernel-heap
 * allocation of the whole read, the sibling of the defect bigwrite stands
 * for, so a read as big as the heap would fail for another reason.
 *
 * ‼ THE WINDOW IS TOUCHED, EVERY PAGE, BEFORE THE FIRST READ, so the cases
 * that read back ask about the write core and nothing else. Whether a read
 * may land in pages a program has not touched yet is coldread's question,
 * and quillmut takes the kernel's answer to it away to see coldread notice —
 * without the cases beside it noticing too. */
uint64_t QuillReadBack(uint32_t fid, uint32_t seed, uint64_t size, int64_t *err)
{
    *err = 0;
    if (!g_window) {
        g_window = malloc(QUILL_READ_WINDOW);
        if (!g_window) {
            *err = -(int64_t)ERR_NO_MEMORY;
            return 0;
        }
        memset(g_window, 0, QUILL_READ_WINDOW);
    }
    for (uint64_t at = 0; at < size; at += QUILL_READ_WINDOW) {
        uint64_t want = size - at < QUILL_READ_WINDOW ? size - at : QUILL_READ_WINDOW;
        int64_t  got  = fread(fid, at, g_window, want);
        if (got < 0) {
            *err = got;
            return at;
        }
        for (uint64_t w = 0; w < (uint64_t)got / 8; w++)
            if (g_window[w] != QuillWord(seed, at + w * 8))
                return at + w * 8;
        if ((uint64_t)got < want) {
            *err = got;
            return at + (uint64_t)got;
        }
    }
    return size;
}

/* fwrite, asked again for as long as the answer is ERR_BUSY. That word is
 * the AHCI port's "every slot is taken, or a cache flush holds it whole —
 * come back", and the kernel leaves the coming back to whoever holds the
 * bytes (write_job.c); a writer that took it for a verdict would report the
 * flush of somebody else's write as its own failure. Every other answer is
 * the answer. */
int64_t QuillWrite(uint32_t fid, uint64_t off, const void *buf, size_t len)
{
    int64_t w;
    while ((w = fwrite(fid, off, buf, len)) == -(int64_t)ERR_BUSY)
        yield();
    return w;
}

/* =========================================================================
 *  Files by name, on the whole volume
 * ========================================================================= */

/* The name up to its last dot — the tag TagFS stamps on every file it
 * creates (tagfs.c, "The name is a tag"), and so the way to ask for it. */
void QuillStem(const char *name, char *out, size_t cap)
{
    size_t len = strlen(name);
    for (size_t i = len; i > 0; i--) {
        if (name[i - 1] == '.') {
            len = i - 1;
            break;
        }
    }
    if (len >= cap)
        len = cap - 1;
    memcpy(out, name, len);
    out[len] = '\0';
}

/* Every file called `name`, whatever Use Context is set. Returns how many
 * there are (the first `max` of them in ids) or a negative -error_t. */
int QuillFindEverywhere(const char *name, uint32_t *ids, int max)
{
    char stem[64];
    QuillStem(name, stem, sizeof(stem));

    uint32_t *all = NULL;
    int n = query_all_everywhere(stem, &all);
    if (n < 0)
        return n;

    int found = 0;
    for (int i = 0; i < n; i++) {
        file_info_t info;
        if (file_info(all[i], &info) != 0 || strcmp(info.filename, name) != 0)
            continue;
        if (found < max)
            ids[found] = all[i];
        found++;
    }
    free(all);
    return found;
}

/* Delete every file called `name` on the whole volume that does not wear
 * system or boot: a previous run's leftovers, or whatever the operator named. */
static int QuillForget(const char *name)
{
    uint32_t ids[8];
    for (;;) {
        int n = QuillFindEverywhere(name, ids, 8);
        if (n <= 0)
            return n;
        for (int i = 0; i < n && i < 8; i++) {
            int rc = delete(ids[i]);
            if (rc < 0)
                return rc;
        }
    }
}

/* A fresh, empty `name` on the whole volume: its file id, or <= 0. */
int QuillCreate(const char *name)
{
    int rc = QuillForget(name);
    if (rc < 0)
        return rc;
    return create_everywhere(name, NULL);
}

uint64_t QuillSize(uint32_t fid)
{
    file_info_t info;
    return file_info(fid, &info) == 0 ? info.size : 0;
}

/* =========================================================================
 *  longquery — a tag of 256 bytes
 * ========================================================================= */

/* A tag is at most 255 bytes, and one longer is refused out loud: that is the
 * only answer that behaves. No file wears a tag of q's, so "no file" is also
 * what a kernel answers that parses the query and cuts the tag short — the
 * 31 bytes parse_tag_list keeps, or the 255 tagfs_parse_tag keeps — and the
 * case cannot tell that from a query answered as asked. */
static QuillVerdict QuillLongQuery(const char *const *args)
{
    (void)args;
    static char tag[QUILL_LONG_TAG_BYTES + 1];
    uint32_t   *ids = NULL;

    int all = query_all_everywhere(NULL, &ids);
    free(ids);
    if (all <= 0) {
        printf("[QUILL] longquery: FAIL — a query for every file answered %d, so there is "
               "nothing to compare with\n", all);
        return QUILL_FAIL;
    }
    int none = query_all_everywhere("quillnosuchtag", &ids);
    free(ids);
    if (none != 0) {
        printf("[QUILL] longquery: FAIL — a query for quillnosuchtag, a tag no file wears, "
               "answered %d where 0 was due\n", none);
        return QUILL_FAIL;
    }

    memset(tag, 'q', QUILL_LONG_TAG_BYTES);
    tag[QUILL_LONG_TAG_BYTES] = '\0';
    int got = query_all_everywhere(tag, &ids);
    free(ids);

    if (got == all) {
        printf("[QUILL] longquery: DEFECT — a query for one tag %u bytes long returned all "
               "%d of the volume's %d files\n", QUILL_LONG_TAG_BYTES, got, all);
        return QUILL_DEFECT;
    }
    if (got < 0) {
        printf("[QUILL] longquery: PASS — a query for one tag %u bytes long was refused "
               "(error %u)\n", QUILL_LONG_TAG_BYTES, box_errno_of(got));
        return QUILL_PASS;
    }
    if (got == 0) {
        printf("[QUILL] longquery: FAIL — a query for one tag %u bytes long, over the 255-byte "
               "tag width, answered no file of the volume's %d where a refusal was due; a query "
               "silently cut to a shorter tag answers the same\n", QUILL_LONG_TAG_BYTES, all);
        return QUILL_FAIL;
    }
    printf("[QUILL] longquery: FAIL — a query for one tag %u bytes long returned %d of the "
           "volume's %d files\n", QUILL_LONG_TAG_BYTES, got, all);
    return QUILL_FAIL;
}

/* =========================================================================
 *  bigwrite — one write bigger than the kernel heap
 * ========================================================================= */

static QuillVerdict QuillBigWrite(const char *const *args)
{
    (void)args;
    int fid = QuillCreate("quillbig.bin");
    if (fid <= 0) {
        int arc = anchor(0);
        printf("[QUILL] bigwrite: FAIL — quillbig.bin could not be created (answer %d, anchor %d)\n",
               fid, arc);
        return QUILL_FAIL;
    }

    /* A Bay and not malloc: a cabin's heap stops at 256 MiB, and the buffer
     * is exactly that. A Bay is backed by 2 MiB pages the kernel walks as
     * one window. */
    uint64_t *buf = bay_open("quilltest:bigwrite", QUILL_BIG_BYTES, BAY_CREATE);
    if (!buf) {
        delete((uint32_t)fid);
        int arc = anchor(0);
        printf("[QUILL] bigwrite: FAIL — a Bay of %u bytes could not be opened, so no write "
               "of that size could be asked for (anchor %d)\n", QUILL_BIG_BYTES, arc);
        return QUILL_FAIL;
    }
    QuillFill(buf, QUILL_SEED_BIG, 0, QUILL_BIG_BYTES);

    uint64_t t0 = 0, t1 = 0;
    time_uptime_ms(&t0);
    int64_t wrote = QuillWrite((uint32_t)fid, 0, buf, QUILL_BIG_BYTES);
    time_uptime_ms(&t1);
    bay_release(buf);

    uint64_t size = QuillSize((uint32_t)fid);
    int64_t  rerr = 0;
    uint64_t good = (wrote == (int64_t)QUILL_BIG_BYTES && size == QUILL_BIG_BYTES)
                        ? QuillReadBack((uint32_t)fid, QUILL_SEED_BIG, size, &rerr) : 0;
    delete((uint32_t)fid);
    int arc = anchor(0);

    if (arc == 0 && wrote == (int64_t)QUILL_BIG_BYTES && size == QUILL_BIG_BYTES &&
        good == QUILL_BIG_BYTES) {
        printf("[QUILL] bigwrite: PASS — one fwrite of %u bytes landed whole in %llu ms and "
               "read back exactly\n", QUILL_BIG_BYTES, (unsigned long long)(t1 - t0));
        return QUILL_PASS;
    }
    if (arc == 0 && wrote == -(int64_t)ERR_INVALID_ADDRESS && size == 0) {
        printf("[QUILL] bigwrite: DEFECT — one fwrite of %u bytes was refused with error %u "
               "and nothing reached the file\n", QUILL_BIG_BYTES, (unsigned)ERR_INVALID_ADDRESS);
        return QUILL_DEFECT;
    }
    printf("[QUILL] bigwrite: FAIL — one fwrite of %u bytes answered %lld and left the file "
           "at %llu bytes, read back good to byte %llu (read answer %lld, anchor %d)\n",
           QUILL_BIG_BYTES, (long long)wrote, (unsigned long long)size,
           (unsigned long long)good, (long long)rerr, arc);
    return QUILL_FAIL;
}

#ifdef CONFIG_VOLUME_TESTS
/* =========================================================================
 *  usememory — a long Use Context, across a reboot (a throwaway image's case)
 * ========================================================================= */

static char g_probe[QUILL_USE_TAGS * 11];     /* "quiluNNNNN," per tag, the last one NUL */

/* "quilu00000,quilu00001,…" — QUILL_USE_TAGS tags; returns its length. */
static size_t QuillUseProbe(void)
{
    size_t len = 0;
    for (uint32_t i = 0; i < QUILL_USE_TAGS; i++) {
        if (i)
            g_probe[len++] = ',';
        memcpy(g_probe + len, "quilu", 5);
        uint32_t v = i;
        for (int d = 9; d >= 5; d--) {
            g_probe[len + (size_t)d] = (char)('0' + v % 10);
            v /= 10;
        }
        len += 10;
    }
    g_probe[len] = '\0';
    return len;
}

/* The second half: the probe is what the machine came up with. A context
 * every tag of which is one the probe says — "quilu" and five digits below
 * QUILL_USE_TAGS — is the probe come back damaged, and is cleared; any other
 * is somebody's own, and is left as it is. */
static QuillVerdict QuillUseReadBack(size_t probe_len, size_t need)
{
    char *have = malloc(need);
    if (!have) {
        printf("[QUILL] usememory: FAIL — no room for the %u-byte context the machine holds\n",
               (unsigned)need);
        return QUILL_FAIL;
    }
    int  tags = use_get(have, need, NULL);
    bool same = tags == QUILL_USE_TAGS && strcmp(have, g_probe) == 0;
    bool ours = tags > 0;
    for (size_t i = 0, from = 0; ours; i++) {
        if (have[i] != ',' && have[i] != '\0')
            continue;
        uint32_t v = 0;
        ours = i - from == 10 && memcmp(have + from, "quilu", 5) == 0;
        for (size_t d = from + 5; ours && d < i; d++) {
            ours = have[d] >= '0' && have[d] <= '9';
            v = v * 10 + (uint32_t)(have[d] - '0');
        }
        ours = ours && v < QUILL_USE_TAGS;
        if (have[i] == '\0')
            break;
        from = i + 1;
    }
    size_t   len  = strlen(have);
    uint32_t diff = 0;
    while (diff < len && diff < probe_len && have[diff] == g_probe[diff])
        diff++;
    free(have);
    if (!same && ours) {
        bool drem = false;
        int  drc  = use_clear(&drem);
        int  darc = anchor(0);
        printf("[QUILL] usememory: FAIL — after a reboot the probe of %u bytes in %d tags came "
               "back as %d tags, %u bytes, differing from byte %u; cleared (answer %d, "
               "remembered=%d, anchor %d)\n", (unsigned)probe_len, QUILL_USE_TAGS, tags,
               (unsigned)len, diff, drc, drem ? 1 : 0, darc);
        return QUILL_FAIL;
    }
    if (!same) {
        printf("[QUILL] usememory: FAIL — the machine has a Use Context of its own (%u bytes, "
               "%d tags); this case replaces it across a reboot and could not give it back — "
               "use clear first\n", (unsigned)(need - 1), tags);
        return QUILL_FAIL;
    }

    bool rem = false;
    int  crc = use_clear(&rem);
    int  arc = anchor(0);
    if (crc < 0 || !rem || arc < 0) {
        printf("[QUILL] usememory: FAIL — the probe came back whole (%d tags, %u bytes), and "
               "clearing it answered %d with remembered=%d (anchor %d)\n",
               tags, (unsigned)probe_len, crc, rem ? 1 : 0, arc);
        return QUILL_FAIL;
    }
    printf("[QUILL] usememory: PASS — after a reboot the machine gave back all %d tags, %u "
           "bytes, in the order they were said; the probe is cleared (remembered=%d)\n",
           tags, (unsigned)probe_len, rem ? 1 : 0);
    return QUILL_PASS;
}

/* The first half: say the probe and ask whether the volume keeps it. */
static QuillVerdict QuillUseMemory(const char *const *args)
{
    (void)args;
    size_t probe_len = QuillUseProbe();
    size_t need = 0;
    int n = use_get(NULL, 0, &need);
    if (n < 0) {
        printf("[QUILL] usememory: FAIL — the machine would not say its Use Context (error %u)\n",
               box_errno_of(n));
        return QUILL_FAIL;
    }
    if (need > 1)
        return QuillUseReadBack(probe_len, need);

    bool rem = false;
    int  src = use_set(g_probe, &rem);
    if (src < 0) {
        printf("[QUILL] usememory: FAIL — the machine refused to take the probe context of %u "
               "bytes (error %u)\n", (unsigned)probe_len, box_errno_of(src));
        return QUILL_FAIL;
    }
    if (!rem) {
        /* The clear is the control. An empty context always fits, so a
         * volume that remembers the clear took a Ledger write just after the
         * probe's was dropped: the volume is up, the medium takes the write,
         * and the length is what was refused. A refusal of the probe's own
         * write alone still reads the same — `remembered` is one bit — until
         * the kernel says why it did not remember. */
        bool crem = false;
        int  crc  = use_clear(&crem);
        int  arc  = anchor(0);
        if (crc == 0 && crem && arc == 0) {
            printf("[QUILL] usememory: DEFECT — a Use Context of %u bytes in %d tags was set and "
                   "the volume did not remember it (remembered=0; cleared with remembered=1, "
                   "anchor 0)\n", (unsigned)probe_len, QUILL_USE_TAGS);
            return QUILL_DEFECT;
        }
        printf("[QUILL] usememory: FAIL — a Use Context of %u bytes was not remembered, and "
               "clearing it answered %d with remembered=%d (anchor %d) — the volume is not "
               "taking the Ledger, so the length was never asked\n", (unsigned)probe_len, crc,
               crem ? 1 : 0, arc);
        return QUILL_FAIL;
    }
    int arc = anchor(0);
    if (arc < 0) {
        printf("[QUILL] usememory: FAIL — a Use Context of %u bytes was remembered and the "
               "anchor after it answered %d\n", (unsigned)probe_len, arc);
        return QUILL_FAIL;
    }
    printf("[QUILL] usememory: PENDING — a Use Context of %u bytes in %d tags is remembered "
           "(remembered=1); after a reboot quilltest usememory reads it back\n",
           (unsigned)probe_len, QUILL_USE_TAGS);
    return QUILL_PENDING;
}
#endif /* CONFIG_VOLUME_TESTS */

/* =========================================================================
 *  coldread / coldwrite — memory this program has never touched
 * ========================================================================= */

/* Two buffers of QUILL_COLD_BYTES, taken one after the other: `warm` for what
 * this program writes and looks at, `cold` for nothing at all. A block under
 * 2 MiB is backed by its first touch, not in advance (boxlib memory.c), and
 * `cold` begins where `warm` ends, so the only store near it is malloc's own
 * header: every page of it past the first is one this program owns and has
 * never touched. */
static bool QuillColdPair(const char *which, uint64_t **warm, uint64_t **cold)
{
    *warm = malloc(QUILL_COLD_BYTES);
    *cold = *warm ? malloc(QUILL_COLD_BYTES) : NULL;
    if (*cold)
        return true;
    printf("[QUILL] %s: FAIL — the heap would not give two buffers of %u bytes\n",
           which, QUILL_COLD_BYTES);
    return false;
}

/* How many of the `len` bytes at `buf` are not the stream seeded `seed` —
 * or, with `seed` 0, not zero — and in *first the offset of the first. */
static uint64_t QuillColdWrong(const uint64_t *buf, uint64_t len, uint32_t seed, uint64_t *first)
{
    uint64_t wrong = 0;
    *first = len;
    for (uint64_t w = 0; w < len / 8; w++) {
        if (buf[w] == (seed ? QuillWord(seed, w * 8) : 0))
            continue;
        if (wrong == 0)
            *first = w * 8;
        wrong += 8;
    }
    return wrong;
}

static QuillVerdict QuillColdRead(const char *const *args)
{
    (void)args;
    uint64_t *warm, *cold;
    if (!QuillColdPair("coldread", &warm, &cold))
        return QUILL_FAIL;
    int fid = QuillCreate("quillcold.bin");
    if (fid <= 0) {
        int arc = anchor(0);
        printf("[QUILL] coldread: FAIL — quillcold.bin could not be created (answer %d, anchor %d)\n",
               fid, arc);
        return QUILL_FAIL;
    }

    QuillFill(warm, QUILL_SEED_COLD, 0, QUILL_COLD_BYTES);
    int64_t wrote = QuillWrite((uint32_t)fid, 0, warm, QUILL_COLD_BYTES);
    int64_t got   = wrote == (int64_t)QUILL_COLD_BYTES
                        ? fread((uint32_t)fid, 0, cold, QUILL_COLD_BYTES) : 0;

    /* Only now is `cold` looked at, and the look is a first touch of its
     * own: a page the read did not fill comes in as zeros. */
    uint64_t first = 0, zero = 0;
    uint64_t wrong = got > 0 ? QuillColdWrong(cold, (uint64_t)got, QUILL_SEED_COLD, &first) : 0;
    for (uint64_t w = first / 8; wrong && w < (uint64_t)got / 8; w++)
        zero += cold[w] == 0 && QuillWord(QUILL_SEED_COLD, w * 8) != 0 ? 8 : 0;
    delete((uint32_t)fid);
    int arc = anchor(0);

    if (wrote != (int64_t)QUILL_COLD_BYTES) {
        printf("[QUILL] coldread: FAIL — quillcold.bin took %lld of %u bytes from a buffer this "
               "program wrote, so there was nothing to read (anchor %d)\n",
               (long long)wrote, QUILL_COLD_BYTES, arc);
        return QUILL_FAIL;
    }
    if (arc == 0 && got == (int64_t)QUILL_COLD_BYTES && wrong == 0) {
        printf("[QUILL] coldread: PASS — a read of %u bytes into memory this program never "
               "touched delivered all %u, each one the file's\n", QUILL_COLD_BYTES, QUILL_COLD_BYTES);
        return QUILL_PASS;
    }
    if (arc == 0 && got == (int64_t)QUILL_COLD_BYTES) {
        printf("[QUILL] coldread: DEFECT — a read of %u bytes into memory this program never "
               "touched reported all %u, and %llu of them are not the file's, from byte %llu "
               "on (%llu of those are zeros)\n", QUILL_COLD_BYTES, QUILL_COLD_BYTES,
               (unsigned long long)wrong, (unsigned long long)first, (unsigned long long)zero);
        return QUILL_DEFECT;
    }
    printf("[QUILL] coldread: FAIL — a read of %u bytes into memory this program never touched "
           "answered %lld (error %u); %llu of the bytes it answered for are not the file's "
           "(anchor %d)\n", QUILL_COLD_BYTES, (long long)got,
           got < 0 ? (unsigned)box_errno_of((int)got) : 0u, (unsigned long long)wrong, arc);
    return QUILL_FAIL;
}

static QuillVerdict QuillColdWrite(const char *const *args)
{
    (void)args;
    uint64_t *warm, *cold;
    if (!QuillColdPair("coldwrite", &warm, &cold))
        return QUILL_FAIL;
    int fid = QuillCreate("quillcold.bin");
    if (fid <= 0) {
        int arc = anchor(0);
        printf("[QUILL] coldwrite: FAIL — quillcold.bin could not be created (answer %d, anchor %d)\n",
               fid, arc);
        return QUILL_FAIL;
    }

    int64_t  wrote = QuillWrite((uint32_t)fid, 0, cold, QUILL_COLD_BYTES);
    uint64_t size  = QuillSize((uint32_t)fid);

    /* Read back into `warm`, filled first with bytes that are not zero, so a
     * read that left any of it alone cannot pass for one that read zeros. */
    memset(warm, 0xA5, QUILL_COLD_BYTES);
    int64_t  got   = size > 0 && size <= QUILL_COLD_BYTES ? fread((uint32_t)fid, 0, warm, size) : 0;
    uint64_t first = 0;
    uint64_t wrong = got > 0 ? QuillColdWrong(warm, (uint64_t)got, 0, &first) : 0;
    delete((uint32_t)fid);
    int arc = anchor(0);

    bool whole = arc == 0 && wrote == (int64_t)QUILL_COLD_BYTES && size == QUILL_COLD_BYTES &&
                 got == (int64_t)QUILL_COLD_BYTES;
    if (whole && wrong == 0) {
        printf("[QUILL] coldwrite: PASS — a write of %u bytes out of memory this program never "
               "touched wrote all %u, and the file reads back as zeros, every byte\n",
               QUILL_COLD_BYTES, QUILL_COLD_BYTES);
        return QUILL_PASS;
    }
    if (whole) {
        printf("[QUILL] coldwrite: DEFECT — a write of %u bytes out of memory this program never "
               "touched reported all %u, and %llu bytes of the file are not zeros, from byte "
               "%llu on\n", QUILL_COLD_BYTES, QUILL_COLD_BYTES, (unsigned long long)wrong,
               (unsigned long long)first);
        return QUILL_DEFECT;
    }
    printf("[QUILL] coldwrite: FAIL — a write of %u bytes out of memory this program never "
           "touched answered %lld (error %u); the file holds %llu bytes, read back answered "
           "%lld with %llu not zeros (anchor %d)\n", QUILL_COLD_BYTES, (long long)wrote,
           wrote < 0 ? (unsigned)box_errno_of((int)wrote) : 0u, (unsigned long long)size,
           (long long)got, (unsigned long long)wrong, arc);
    return QUILL_FAIL;
}

/* =========================================================================
 *  The table, and the one way in
 * ========================================================================= */

static const QuillCase g_cases[] = {
    { "longquery", "", 0, QuillLongQuery,
      "is a query for one tag of 256 bytes, over the tag width, refused rather than answered",
      "nothing", false },
    { "bigwrite", "", 0, QuillBigWrite,
      "does one fwrite of 256 MiB, more than the kernel heap, land whole",
      "nothing: quillbig.bin is deleted and the volume anchored", false },
    { "sequential", "", 0, QUILL_THROWAWAY(QuillSequential),
      "does one writer's 3 MiB, 4 KiB at a time in a row, fit one file's record",
      "quillS.bin, anchored; on the synchronous path a refused write's block, orphaned", true },
    { "interleave", "", 0, QUILL_THROWAWAY(QuillInterleave),
      "do two files written 4 KiB each in turn, 64 MiB each, stay whole",
      "quillA.bin and quillB.bin, anchored; on the synchronous path a refused block, orphaned",
      true },
    { "coldread", "", 0, QuillColdRead,
      "does a read into memory this program never touched deliver every byte it reports",
      "nothing: quillcold.bin is deleted and the volume anchored", false },
    { "coldwrite", "", 0, QuillColdWrite,
      "does a write out of memory this program never touched write zeros, and say how many",
      "nothing: quillcold.bin is deleted and the volume anchored", false },
    { "usememory", "", 0, QUILL_THROWAWAY(QuillUseMemory),
      "is a 1000-byte Use Context remembered across a reboot (run it before and after)",
      "between its halves a remembered context of 91 tags; for good, tags quilu* interned", true },
    { "fidhistory", "", 0, QUILL_THROWAWAY(QuillFidHistory),
      "what 50000 file ids spent on deleted files cost the next mount",
      "50000 file ids spent for good, and their dead records in the pool", true },
    { "systemtag", "", 0, QUILL_THROWAWAY(QuillSystemTag),
      "may programs that do not wear system — untag, trash, erase — take the system tag off "
      "kernel.bin and delete it",
      "DESTRUCTIVE: where the answer is yes, kernel.bin is gone and the volume will not boot", true },
    { "hog", "", 0, QuillHog, "nothing: it computes without end",
      "a cabin spinning until the machine stops; never returns", false },
    { "settle", "NAME", 1, QuillSettle,
      "for the power-cut stand: a write left unanchored beside a hog",
      "DESTRUCTIVE: every file called NAME that does not wear system or boot is deleted first, "
      "whatever it holds; then NAME holding one record, not anchored, and a hog; never returns "
      "while the hog spins", false },
    { "hogged", "PROGRAM", 1, QuillHogged,
      "is PROGRAM answered while a hog spins (anchor_test: does an anchor answer)",
      "whatever PROGRAM leaves; the hog is ended", false },
    { "verify", "NAME", 1, QuillVerify,
      "does NAME hold the whole record settle or churn wrote", "nothing", false },
    { "churn", "NAME", 1, QuillChurn, "for the power-cut stand: a writer that never stops",
      "NAME, rewritten until the machine stops; never returns", false },
    { "race", "NAME ROUNDS", 2, QuillRace, "do ROUNDS anchors hold while a churn writes NAME",
      "DESTRUCTIVE: every file called NAME that does not wear system or boot is deleted first, "
      "and the anchors make it durable; then NAME, and the churn it started, still writing",
      false },
};

#define QUILL_CASE_COUNT ((uint32_t)(sizeof(g_cases) / sizeof(g_cases[0])))

int main(void)
{
    uint32_t words = luggage_word_count();
    if (words < 2) {
        println("[QUILL] quilltest CASE [ARG...] — one case per run:");
        for (uint32_t i = 0; i < QUILL_CASE_COUNT; i++)
            printf("[QUILL]   %s%s%s — asks: %s; leaves: %s%s\n", g_cases[i].name,
                   g_cases[i].argc ? " " : "", g_cases[i].args, g_cases[i].asks,
                   g_cases[i].leaves, g_cases[i].throwaway
                       ? " — a throwaway image's case, in a VOLUME_TESTS=on build only" : "");
        printf("[QUILL] FAIL: no case named — %u cases are listed above, and quilltest asks "
               "only what it is told to, because most of them change the volume\n",
               QUILL_CASE_COUNT);
        return 1;
    }

    const char *asked = luggage_word(1);
    const QuillCase *c = NULL;
    for (uint32_t i = 0; i < QUILL_CASE_COUNT && !c; i++)
        if (strcmp(asked, g_cases[i].name) == 0)
            c = &g_cases[i];
    if (!c) {
        printf("[QUILL] FAIL: \"%s\" names no case — a bare quilltest lists the %u there are\n",
               asked, QUILL_CASE_COUNT);
        return 1;
    }
    if (words - 2 != c->argc) {
        printf("[QUILL] %s: FAIL — it takes %u word(s) after its name (%s) and was given %u\n",
               c->name, c->argc, c->argc ? c->args : "none", words - 2);
        return 1;
    }

    if (!c->run) {
        printf("[QUILL] %s: FAIL — this case exists only in a quilltest built with "
               "VOLUME_TESTS=on, for a throwaway image — what it leaves: %s\n", c->name,
               c->leaves);
        return 1;
    }

    const char *args[2] = { luggage_word(2), luggage_word(3) };
    return c->run(args) == QUILL_FAIL ? 1 : 0;
}
