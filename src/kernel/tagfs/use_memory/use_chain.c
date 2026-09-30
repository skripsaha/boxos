#include "use_memory_internal.h"
#include "../../../lib/kernel/crypto.h"
#include "boardroom.h"   /* BoardroomSeatRun — how much the medium takes in one command */

/*
 * The Use Context's chain on the medium (volume_ledger.h, UseChainLink): read
 * and held to its name, written a window at a time, staked, and looked for
 * among the blocks nothing names. Nothing here takes a lock but the give-back
 * of blocks never named.
 */

static uint64_t UseBlockVlba(uint32_t block)
{
    return ((uint64_t)tagfs_get_state()->layout.data_block + block) * TAGFS_BLOCK_SECTORS;
}

/* The blocks the medium takes in one command — its answer, not a number
 * chosen here — at least one, and never more than `most`. */
static uint32_t UseWindowBlocks(uint64_t most)
{
    uint64_t window = BoardroomSeatRun(tagfs_get_seat()) / TAGFS_BLOCK_SECTORS;
    if (window == 0)
        window = 1;
    if (window > most)
        window = most;
    return (uint32_t)window;
}

/* A buffer of up to `want` blocks, halved while the heap will not give it:
 * NULL only when not one block fits. */
static uint8_t *UseWindowBuffer(uint32_t *want)
{
    uint8_t *buf = NULL;
    while (*want && !(buf = kmalloc((size_t)*want * TAGFS_BLOCK_SIZE)))
        *want /= 2u;
    return buf;
}

uint64_t UseChainLinksFor(uint64_t bytes)
{
    return (bytes + USE_CHAIN_TEXT - 1u) / USE_CHAIN_TEXT;
}

uint32_t UseTextCrc32(const UseText *text)
{
    uint32_t crc = 0;
    size_t   at  = 0;
    while (at < text->bytes) {
        size_t piece = text->bytes - at;
        if (piece > 0x40000000u)
            piece = 0x40000000u;
        crc = KCrc32Continue(crc, (const uint8_t *)text->text + at, (uint32_t)piece);
        at += piece;
    }
    return crc;
}

bool UseChainRefOf(const VolumeLedger *led, UseChainRef *out)
{
    if (!LedgerHas(led, VOLUME_LEDGER_USE_CHAIN_END))
        return false;
    out->first   = led->use_chain_first;
    out->attempt = led->use_chain_attempt;
    out->seq     = led->use_chain_seq;
    out->bytes   = led->use_chain_bytes;
    out->crc32   = led->use_chain_crc32;
    return true;
}

void UseChainForget(UseChain *chain)
{
    kfree(chain->runs);
    memset(chain, 0, sizeof(*chain));
}

/* `block` after the chain's blocks so far: onto its last run when it follows
 * it, else a run of its own. False with no memory to grow the runs. */
static bool UseRunsAppend(UseChain *chain, uint32_t *cap, uint32_t block, uint32_t count)
{
    if (chain->run_count) {
        FileExtent *last = &chain->runs[chain->run_count - 1];
        if (last->start_block + last->block_count == block &&
            (uint32_t)last->block_count + count <= 0xFFFFu) {
            last->block_count = (uint16_t)(last->block_count + count);
            return true;
        }
    }
    if (chain->run_count == *cap) {
        uint32_t    grown = *cap ? *cap * 2u : 4u;
        FileExtent *more  = kmalloc(sizeof(FileExtent) * grown);
        if (!more)
            return false;
        if (chain->run_count)
            memcpy(more, chain->runs, sizeof(FileExtent) * chain->run_count);
        kfree(chain->runs);
        chain->runs = more;
        *cap        = grown;
    }
    chain->runs[chain->run_count].start_block = block;
    chain->runs[chain->run_count].block_count = (uint16_t)count;
    chain->run_count++;
    return true;
}

// ----------------------------------------------------------------------------
// Reading
// ----------------------------------------------------------------------------

/*
 * ‼ EXACTLY AS MANY HOPS AS THE NAME SAYS, AND EVERY LINK HELD TO IT.
 *
 * The number of links follows from the length the Ledger names, and never
 * more are followed than the data run has blocks: a chain that loops, or a
 * next that points anywhere, is found out before the walk runs away. A link
 * carrying another magic or another (seq, attempt) is its block's previous
 * content — FOREIGN, the write of that link never landed — and is not this
 * chain's block; one that carries this chain's pair and disagrees with it in
 * any other field, or a text that does not sum, is ROTTEN.
 */
UseChainState UseChainRead(const UseChainRef *ref, UseText **text, UseChain *out)
{
    uint32_t data_blocks = tagfs_get_state()->layout.data_blocks;
    uint64_t links       = UseChainLinksFor(ref->bytes);
    uint32_t cap         = 0;

    memset(out, 0, sizeof(*out));
    out->ref = *ref;
    if (text)
        *text = NULL;
    if (ref->first == VOLUME_LEDGER_NO_BLOCK || ref->bytes == 0 || links > data_blocks)
        return USE_CHAIN_ROTTEN;

    UseChainLink *link = kmalloc(sizeof(UseChainLink));
    UseText      *room = text && ref->bytes <= (uint64_t)SIZE_MAX / 2u ? UseTextRoom((size_t)ref->bytes)
                                                                       : NULL;
    if (!link) {
        UseTextDrop(room);
        return USE_CHAIN_UNHELD;
    }

    UseChainState state = USE_CHAIN_WHOLE;
    uint32_t      block = ref->first;
    uint32_t      crc   = 0;
    uint64_t      at    = 0;
    for (uint64_t i = 0; i < links; i++) {
        if (block == VOLUME_LEDGER_NO_BLOCK || block >= data_blocks) {
            state = USE_CHAIN_ROTTEN;
            break;
        }
        if (tagfs_volume_read(UseBlockVlba(block), TAGFS_BLOCK_SECTORS, link) != 0) {
            TagFSNoteReadRefused();
            state = USE_CHAIN_UNREAD;
            break;
        }
        if (link->magic != USE_CHAIN_MAGIC || link->seq != ref->seq || link->attempt != ref->attempt) {
            state = USE_CHAIN_FOREIGN;
            break;
        }
        if (!UseRunsAppend(out, &cap, block, 1)) {
            state = USE_CHAIN_UNHELD;
            break;
        }
        uint64_t want = i + 1 < links ? USE_CHAIN_TEXT : ref->bytes - i * USE_CHAIN_TEXT;
        if (link->index != i || link->n_links != links || link->total_bytes != ref->bytes ||
            link->text_crc32 != ref->crc32 || link->used != want) {
            state = USE_CHAIN_ROTTEN;
            break;
        }
        crc = KCrc32Continue(crc, link->text, link->used);
        if (room)
            memcpy(room->text + at, link->text, link->used);
        at   += link->used;
        block = link->next;
    }
    kfree(link);

    if (state == USE_CHAIN_WHOLE && crc != ref->crc32)
        state = USE_CHAIN_ROTTEN;
    if (state == USE_CHAIN_WHOLE && text && !room)
        state = USE_CHAIN_UNHELD;
    if (state == USE_CHAIN_WHOLE && text)
        *text = room;
    else
        UseTextDrop(room);
    return state;
}

// ----------------------------------------------------------------------------
// Staking and writing
// ----------------------------------------------------------------------------

error_t UseChainStake(uint64_t n, UseChain *out, uint64_t *got)
{
    uint32_t cap  = 0;
    uint32_t hint = 0;

    memset(out, 0, sizeof(*out));
    *got = 0;
    while (*got < n) {
        uint64_t left  = n - *got;
        uint32_t want  = left > 0xFFFFu ? 0xFFFFu : (uint32_t)left;
        uint32_t start = 0, took = 0;
        error_t  rc    = TagFSTakeRun(want, hint, &start, &took);
        if (rc == OK && !UseRunsAppend(out, &cap, start, took)) {
            TagFSLockTake(UseMemoryLock());
            (void)BlockMapGiveLocked(start, took);
            TagFSLockDrop(UseMemoryLock());
            rc = ERR_NO_MEMORY;
        }
        if (rc != OK) {
            UseChainUnstake(out);
            return rc;
        }
        *got += took;
        hint  = start + took;
    }
    return OK;
}

void UseChainUnstake(UseChain *chain)
{
    TagFSLockTake(UseMemoryLock());
    for (uint32_t r = 0; r < chain->run_count; r++) {
        error_t back = BlockMapGiveLocked(chain->runs[r].start_block, chain->runs[r].block_count);
        if (back != OK)
            kprintf("[TagFS] blocks %u..+%u staked for the Use Context's chain and never named "
                    "would not go back to the allocator (error %u) — they stay taken\n",
                    chain->runs[r].start_block, (unsigned)chain->runs[r].block_count,
                    (unsigned)back);
    }
    TagFSLockDrop(UseMemoryLock());
    UseChainForget(chain);
}

/* Link `index` of `links`, carrying its piece of `text`, into `link`. */
static void UseLinkBuild(UseChainLink *link, const UseChainRef *ref, const UseText *text,
                         uint64_t index, uint64_t links, uint32_t next)
{
    uint64_t from = index * USE_CHAIN_TEXT;
    uint64_t left = text->bytes - from;
    uint32_t used = left < USE_CHAIN_TEXT ? (uint32_t)left : USE_CHAIN_TEXT;

    memset(link, 0, sizeof(*link));
    link->magic       = USE_CHAIN_MAGIC;
    link->next        = next;
    link->seq         = ref->seq;
    link->attempt     = ref->attempt;
    link->index       = (uint32_t)index;
    link->used        = used;
    link->n_links     = (uint32_t)links;
    link->total_bytes = text->bytes;
    link->text_crc32  = ref->crc32;
    memcpy(link->text, text->text + from, used);
}

/* The block of the link after the one at `k` of run `r`: the run's next
 * block, the next run's first, or 0 after the last. */
static uint32_t UseLinkNext(const UseChain *chain, uint32_t r, uint32_t k)
{
    const FileExtent *run = &chain->runs[r];
    if (k + 1u < run->block_count)
        return run->start_block + k + 1u;
    return r + 1u < chain->run_count ? chain->runs[r + 1u].start_block : VOLUME_LEDGER_NO_BLOCK;
}

error_t UseChainWrite(const UseChain *chain, const UseText *text)
{
    uint64_t links  = UseChainLinksFor(text->bytes);
    uint32_t window = UseWindowBlocks(links);
    uint8_t *buf    = UseWindowBuffer(&window);
    if (!buf) {
        kprintf("[TagFS] no memory for one block of the Use Context's chain — its links are "
                "not written, and the Ledger does not name them\n");
        return ERR_NO_MEMORY;
    }

    uint64_t index = 0;
    error_t  rc    = OK;
    for (uint32_t r = 0; r < chain->run_count && rc == OK; r++) {
        const FileExtent *run = &chain->runs[r];
        for (uint32_t k = 0; k < run->block_count && rc == OK; ) {
            uint32_t piece = run->block_count - k;
            if (piece > window)
                piece = window;
            for (uint32_t j = 0; j < piece; j++, index++)
                UseLinkBuild((UseChainLink *)(buf + (size_t)j * TAGFS_BLOCK_SIZE), &chain->ref,
                             text, index, links, UseLinkNext(chain, r, k + j));
            if (tagfs_volume_write(UseBlockVlba(run->start_block + k),
                                   piece * TAGFS_BLOCK_SECTORS, buf) != 0) {
                kprintf("[TagFS] the medium would not take links %llu..%llu of the Use "
                        "Context's chain at data block %u\n",
                        (unsigned long long)(index - piece), (unsigned long long)(index - 1u),
                        run->start_block + k);
                rc = ERR_TAGFS_METADATA_ERROR;
            }
            for (uint32_t j = 0; j < piece; j++)
                tagfs_readahead_invalidate(run->start_block + k + j);
            k += piece;
        }
    }
    kfree(buf);
    return rc;
}

// ----------------------------------------------------------------------------
// Among the orphans
// ----------------------------------------------------------------------------

/* Whether every block of `chain` lies in the orphan runs (sorted, as the census
 * closes them): a chain that reaches into ground something names is not one a
 * mount may take up. */
static bool UseChainAllOrphan(const UseChain *chain, const UseOrphanRun *runs, uint32_t count)
{
    for (uint32_t r = 0; r < chain->run_count; r++) {
        uint32_t lo = 0, hi = count, from = chain->runs[r].start_block;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (runs[mid].start + runs[mid].count <= from)
                lo = mid + 1u;
            else
                hi = mid;
        }
        if (lo == count || runs[lo].start > from ||
            from + chain->runs[r].block_count > runs[lo].start + runs[lo].count)
            return false;
    }
    return true;
}

/* The heads among the orphans: every block holding a first link, as the name
 * it states of its chain. Grown in the heap; *count stays what fitted. */
static UseChainRef *UseOrphanHeads(const UseOrphanRun *runs, uint32_t count, uint32_t *found)
{
    uint32_t longest = 1;
    for (uint32_t i = 0; i < count; i++)
        if (runs[i].count > longest)
            longest = runs[i].count;
    uint32_t window = UseWindowBlocks(longest);
    uint8_t *buf    = UseWindowBuffer(&window);
    UseChainRef *heads = NULL;
    uint32_t     cap   = 0;
    uint32_t     lost  = 0;

    *found = 0;
    if (!buf) {
        kprintf("[TagFS] no memory to read the blocks nothing names for a Use Context's chain\n");
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t k = 0; k < runs[i].count; ) {
            uint32_t piece = runs[i].count - k < window ? runs[i].count - k : window;
            uint32_t at    = runs[i].start + k;
            k += piece;
            if (tagfs_volume_read(UseBlockVlba(at), piece * TAGFS_BLOCK_SECTORS, buf) != 0) {
                TagFSNoteReadRefused();
                continue;
            }
            for (uint32_t j = 0; j < piece; j++) {
                const UseChainLink *link = (const UseChainLink *)(buf + (size_t)j * TAGFS_BLOCK_SIZE);
                if (link->magic != USE_CHAIN_MAGIC || link->index != 0)
                    continue;
                if (*found == cap) {
                    uint32_t     grown = cap ? cap * 2u : 8u;
                    UseChainRef *more  = kmalloc(sizeof(UseChainRef) * grown);
                    if (!more) {
                        lost++;
                        continue;
                    }
                    if (*found)
                        memcpy(more, heads, sizeof(UseChainRef) * *found);
                    kfree(heads);
                    heads = more;
                    cap   = grown;
                }
                heads[(*found)++] = (UseChainRef){ .first = at + j, .attempt = link->attempt,
                                                   .seq = link->seq, .bytes = link->total_bytes,
                                                   .crc32 = link->text_crc32 };
            }
        }
    }
    kfree(buf);
    if (lost)
        kprintf("[TagFS] no memory to hold %u more first link(s) of a Use Context chain found "
                "among the blocks nothing names — they are not followed\n", lost);
    return heads;
}

/* Newest (seq, attempt) first. */
static bool UseRefNewer(const UseChainRef *a, const UseChainRef *b)
{
    return a->seq != b->seq ? a->seq > b->seq : a->attempt > b->attempt;
}

uint32_t UseChainFindOrphans(const UseOrphanRun *runs, uint32_t count, UseChain **out,
                             UseText ***texts)
{
    uint32_t     heads_n = 0;
    UseChainRef *heads   = UseOrphanHeads(runs, count, &heads_n);
    uint32_t     whole   = 0;

    *out   = NULL;
    *texts = NULL;
    if (!heads_n) {
        kfree(heads);
        return 0;
    }
    for (uint32_t i = 1; i < heads_n; i++)
        for (uint32_t j = i; j > 0 && UseRefNewer(&heads[j], &heads[j - 1]); j--) {
            UseChainRef t = heads[j];
            heads[j]      = heads[j - 1];
            heads[j - 1]  = t;
        }

    *out   = kmalloc(sizeof(UseChain) * heads_n);
    *texts = kmalloc(sizeof(UseText *) * heads_n);
    if (!*out || !*texts) {
        kprintf("[TagFS] no memory to follow the %u Use Context chain(s) found among the blocks "
                "nothing names\n", heads_n);
        kfree(*out);
        kfree(*texts);
        *out   = NULL;
        *texts = NULL;
        kfree(heads);
        return 0;
    }
    for (uint32_t i = 0; i < heads_n; i++) {
        UseChain chain;
        UseText *text  = NULL;
        UseChainState state = UseChainRead(&heads[i], &text, &chain);
        if (state == USE_CHAIN_WHOLE && UseChainAllOrphan(&chain, runs, count)) {
            (*out)[whole]   = chain;
            (*texts)[whole] = text;
            whole++;
            continue;
        }
        UseTextDrop(text);
        UseChainForget(&chain);
    }
    kfree(heads);
    return whole;
}
