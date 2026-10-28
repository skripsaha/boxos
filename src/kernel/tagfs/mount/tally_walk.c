#include "tally_read_internal.h"

/*
 * The Tally, walked (tally_read.h). Every node is read into memory the heap
 * gives, one at a time, and let go before the next: the walk holds only the
 * places still to visit, a stack in the heap — no recursion, so a tree of any
 * height costs one frame.
 *
 * Its own file beside tally_read.c, which keeps the copies of pointers the
 * walk takes and the DIGESTS leaves it names.
 */

static int TallyKeyCmp(TallyKey a, TallyKey b)
{
    if (a.k1 != b.k1)
        return a.k1 < b.k1 ? -1 : 1;
    return a.k2 < b.k2 ? -1 : a.k2 > b.k2;
}

/* A key as the book's lines write it: k1, or (k1, k2). */
static void TallyKeySay(uint64_t tree, TallyKey k, char* out, size_t room)
{
    if (TallyPairKeyed(tree))
        ksnprintf(out, room, "(%llu, %llu)", (unsigned long long)k.k1, (unsigned long long)k.k2);
    else
        ksnprintf(out, room, "%llu", (unsigned long long)k.k1);
}

// ----------------------------------------------------------------------------
// One node, from the first of its copies that holds
// ----------------------------------------------------------------------------

void TallyNodeLetGo(TallyNode* n)
{
    BookUnitLetGo(&n->unit);
    kfree(n->starts);
    n->starts  = NULL;
    n->nstarts = 0;
}

/* The NodeHead that opens the stream, and the bytes past what this reader
 * knows of it. False when it is no NodeHead. */
static bool TallyNodeHead(const uint8_t* s, uint32_t room, uint32_t* at, TallyNode* n)
{
    uint64_t tag;
    uint32_t p = 0, body, len;
    int      head = NodeHeadGet(s, room, at, &n->head);
    if (head == TALLY_BODY_BROKEN)
        return false;
    BookEntryNext(s, room, &p, &tag, &body, &len);
    n->head_longer = head == TALLY_BODY_LONGER ? len - NodeHeadBodyBytes(&n->head) : 0;
    return true;
}

/*
 * A copy's stream as a node at place `p`: a NodeHead, its items each whole
 * in the stream with keys that rise from its first key, nothing but PAD after
 * the last — tally.py NodeParse's rule — and every key inside [lo, hi), the
 * range its parent gives it (§16.3 п.2). OK, or ERR_TAGFS_CORRUPTED with why
 * the copy does not hold; ERR_NO_MEMORY.
 */
static error_t TallyNodeParse(const TallyPlace* p, TallyNode* n, char* why, size_t room)
{
    const uint8_t* s   = n->unit.stream;
    uint32_t       end = n->unit.count * BOOK_PAYLOAD_BYTES, at = 0, body, len;
    TallyKey       prev, key;
    char           k[48], lo[48], hi[48];

    if (!TallyNodeHead(s, end, &at, n)) {
        ksnprintf(why, room, "it does not parse: it opens with no NodeHead");
        return ERR_TAGFS_CORRUPTED;
    }
    n->items_at = at;
    if (n->head.items > end) {
        ksnprintf(why, room, "it does not parse: it counts %llu items in %u bytes",
                  (unsigned long long)n->head.items, end);
        return ERR_TAGFS_CORRUPTED;
    }
    if (!(n->starts = kmalloc(sizeof(uint32_t) * (n->head.items + 1u))))
        return ERR_NO_MEMORY;
    n->starts[n->nstarts++] = 0;
    prev = n->head.first;
    for (uint64_t i = 0; i < n->head.items; i++) {
        n->starts[n->nstarts++] = at;
        if (!TallyItemGet(s, end, &at, n->head.tree, prev, &key, &body, &len)) {
            ksnprintf(why, room, "it does not parse: item %llu runs past the node", (unsigned long long)i);
            return ERR_TAGFS_CORRUPTED;
        }
        if ((i && TallyKeyCmp(key, prev) <= 0) || TallyKeyCmp(key, n->head.first) < 0) {
            ksnprintf(why, room, "it does not parse: the keys do not rise at item %llu", (unsigned long long)i);
            return ERR_TAGFS_CORRUPTED;
        }
        prev = key;
    }
    if (at < end && s[at] != 0) {
        ksnprintf(why, room, "it does not parse: a byte other than PAD follows its last item");
        return ERR_TAGFS_CORRUPTED;
    }
    at   = n->items_at;
    prev = n->head.first;
    for (uint64_t i = 0; i < n->head.items; i++) {
        TallyItemGet(s, end, &at, n->head.tree, prev, &key, &body, &len);
        prev = key;
        if ((p->has_lo && TallyKeyCmp(key, p->lo) < 0) || (p->has_hi && TallyKeyCmp(key, p->hi) >= 0)) {
            TallyKeySay(n->head.tree, key, k, sizeof(k));
            TallyKeySay(n->head.tree, p->lo, lo, sizeof(lo));
            TallyKeySay(n->head.tree, p->hi, hi, sizeof(hi));
            ksnprintf(why, room, "its keys run outside [%s, %s), the range its parent gives it — %s first",
                      p->has_lo ? lo : "None", p->has_hi ? hi : "the end", k);
            return ERR_TAGFS_CORRUPTED;
        }
    }
    return OK;
}

/* How many of the copies that did not hold are named one by one. */
static uint32_t TallyCopiesNamed(const TallyCopiesSaid* said)
{
    return said->failed < TALLY_COPIES_SAID ? said->failed : TALLY_COPIES_SAID;
}

/*
 * The node at place `p` from the first of its copies that holds — a whole
 * SHEET unit of the pointer's seq, inside the volume, parsed as a node in
 * its range — into *n. OK with n->unit.whole when one held, the copies before
 * it in *said; OK with it false when none did; ERR_NO_MEMORY, or the medium's
 * refusal. A copy past the volume's last block holds nothing a medium could.
 */
error_t TallyNodeRead(const TallyPlace* p, TallyNode* n, TallyCopiesSaid* said)
{
    uint64_t end = tagfs_get_state()->block_end;
    memset(said, 0, sizeof(*said));
    for (uint32_t k = 0; k < p->ptr.copies; k++) {
        char*    why   = said->why[said->failed < TALLY_COPIES_SAID ? said->failed : TALLY_COPIES_SAID];
        uint64_t block = TallyPtrBlock(&p->ptr, k);
        error_t  rc    = OK;
        memset(n, 0, sizeof(*n));
        if (block >= end || p->ptr.sheets > end - block) {
            ksnprintf(why, sizeof(said->why[0]), "it names blocks past the volume's end");
        } else {
            rc = BookUnitRead(block * VOLUME_BLOCK_SECTORS,
                              (uint32_t)(p->ptr.sheets * VOLUME_BLOCK_SECTORS), BOOK_UNIT_SHEET,
                              p->ptr.seq, &n->unit);
            if (rc == OK && !n->unit.whole)
                memcpy(why, n->unit.why, sizeof(n->unit.why));
            else if (rc == OK && (rc = TallyNodeParse(p, n, why, sizeof(said->why[0]))) == OK)
                return OK;
        }
        TallyNodeLetGo(n);
        if (rc != OK && rc != ERR_TAGFS_CORRUPTED)
            return rc;
        said->failed++;
    }
    return OK;
}

/* "copy 1: why; copy 2: why", as a refusal names a root that will not read —
 * and the copies past the ones named, by their count. */
void TallyCopiesWords(const TallyCopiesSaid* said, char* out, size_t room)
{
    uint32_t named = TallyCopiesNamed(said);
    size_t   at    = 0;
    out[0] = '\0';
    for (uint32_t k = 0; k < named && at + 1u < room; k++)
        at += ksnprintf(out + at, room - at, "%scopy %u: %s", k ? "; " : "", k + 1u, said->why[k]);
    if (said->failed > named && at + 1u < room)
        ksnprintf(out + at, room - at, "; and %u more copies", said->failed - named);
}

/* The blocks of a node's copies, "a/b", as its lines name it. */
static void TallyBlocksSay(const TallyPtr* p, char* out, size_t room)
{
    size_t at = 0;
    out[0] = '\0';
    for (uint32_t k = 0; k < p->copies && at + 1u < room; k++)
        at += ksnprintf(out + at, room - at, "%s%llu", k ? "/" : "", (unsigned long long)TallyPtrBlock(p, k));
}

error_t TallyRootReads(uint64_t tree, const SpineRoot* root, char* why, size_t room)
{
    TallyPlace    p;
    TallyReading* r = kmalloc(sizeof(TallyReading));
    if (!r)
        return ERR_NO_MEMORY;
    memset(&r->node, 0, sizeof(r->node));
    error_t rc = TallyRootPlace(root, &p);
    if (rc == OK)
        rc = TallyNodeRead(&p, &r->node, &r->said);
    if (rc == OK && !r->node.unit.whole) {
        TallyCopiesWords(&r->said, why, room);
        rc = ERR_TAGFS_CORRUPTED;
    } else if (rc == OK && (r->node.head.tree != tree || r->node.head.level != p.level)) {
        ksnprintf(why, room, "it is a node of tree %llu level %llu", (unsigned long long)r->node.head.tree,
                  (unsigned long long)r->node.head.level);
        rc = ERR_TAGFS_CORRUPTED;
    }
    TallyNodeLetGo(&r->node);
    kfree(r);
    return rc;
}

// ----------------------------------------------------------------------------
// The walk
// ----------------------------------------------------------------------------

/* The places still to visit, a stack in the heap. */
typedef struct {
    TallyPlace* at;
    uint32_t    n;
    uint32_t    cap;
} TallyStack;

static bool TallyPush(TallyStack* st, const TallyPlace* p)
{
    if (st->n == st->cap) {
        uint32_t    cap  = st->cap ? st->cap * 2u : 16u;
        TallyPlace* more = kmalloc(sizeof(TallyPlace) * cap);
        if (!more)
            return false;
        if (st->n)
            memcpy(more, st->at, sizeof(TallyPlace) * st->n);
        kfree(st->at);
        st->at  = more;
        st->cap = cap;
    }
    st->at[st->n++] = *p;
    return true;
}

/* A shape the format does not allow, said and counted — the node read all
 * the same. */
static void TallyShape(const TallySink* sink, const char* what)
{
    kprintf("[TagFS] the book is written against its own shape — %s\n", what);
    if (sink->shapes)
        (*sink->shapes)++;
}

/* The count a node's parent — or its slot's SpineRoot — carries for it,
 * against the elements under it (§16.3 п.3). */
static void TallyCountSay(const TallySink* sink, uint64_t tree, const TallyPlace* p, uint64_t under)
{
    char words[192];
    if (under == p->count)
        return;
    ksnprintf(words, sizeof(words), "the %s node of level %llu at block %llu counts %llu element(s) under "
              "it and %s counts %llu", TallyTreeName(tree), (unsigned long long)p->level,
              (unsigned long long)p->ptr.block[0], (unsigned long long)under,
              p->has_lo ? "its parent" : "its slot's SpineRoot", (unsigned long long)p->count);
    TallyShape(sink, words);
}

/* What a node read says beside its items: the copies before it that did not
 * hold, a NodeHead longer than known, a lead its records do not give a sector. */
static void TallyNodeSaid(const TallySink* sink, uint64_t tree, const TallyPlace* p, const TallyReading* r)
{
    char words[224], blocks[96];
    TallyBlocksSay(&p->ptr, blocks, sizeof(blocks));
    uint32_t one_by_one = TallyCopiesNamed(&r->said);
    for (uint32_t k = 0; k < one_by_one; k++)
        kprintf("[TagFS] the %s node of level %llu at blocks %s: copy %u does not hold — %s; copy %u is "
                "read, and the next Tally writes the node again\n", TallyTreeName(tree),
                (unsigned long long)p->level, blocks, k + 1u, r->said.why[k], r->said.failed + 1u);
    if (r->said.failed > one_by_one)
        kprintf("[TagFS] the %s node of level %llu at blocks %s: and %u more of its copies do not hold; copy %u "
                "is read, and the next Tally writes the node again\n", TallyTreeName(tree),
                (unsigned long long)p->level, blocks, r->said.failed - one_by_one, r->said.failed + 1u);
    if (sink->mended)
        *sink->mended += r->said.failed;
    if (r->node.head_longer) {
        ksnprintf(words, sizeof(words), "a NodeHead is %u byte(s) longer than this reader knows — written "
                  "by a kernel that knows more", r->node.head_longer);
        TagFSUnderGlass(words);
    }
    uint32_t named = 0, given = 0;
    uint32_t k = BookUnitLeadWrong(&r->node.unit, r->node.starts, r->node.nstarts, &named, &given);
    if (k < r->node.unit.count) {
        ksnprintf(words, sizeof(words), "the %s node of level %llu at block %llu: sector %u names lead %u "
                  "where its records give it %u (§2.1)", TallyTreeName(tree), (unsigned long long)p->level,
                  (unsigned long long)p->ptr.block[0], k, named, given);
        TallyShape(sink, words);
    }
}

/* A leaf's items, handed to the reader in their order. */
static error_t TallyLeafItems(uint64_t tree, const TallyPlace* p, const TallyReading* r,
                              const TallySink* sink)
{
    const uint8_t* s     = r->node.unit.stream;
    uint32_t       end   = r->node.unit.count * BOOK_PAYLOAD_BYTES, at = r->node.items_at, body, len;
    TallyKey       prev  = r->node.head.first, key;
    uint64_t       under = 0;
    for (uint64_t i = 0; i < r->node.head.items; i++) {
        TallyItemGet(s, end, &at, tree, prev, &key, &body, &len);
        prev = key;
        under += tree != TALLY_FILES || key.k2 == 0;
        error_t rc = sink->item(sink->ctx, key, s + body, len, p->ptr.seq);
        if (rc != OK)
            return rc;
    }
    if (sink->said)
        TallyCountSay(sink, tree, p, under);
    return OK;
}

/* An inner item's child, at the range [its separator, the parent's end) —
 * the next separator closes it once it is read. A pointer that does not read
 * leaves the child with no copies: a rotten range the walk hands on when it
 * comes to it, in its turn among the keys. OK with *read when the pointer
 * read; ERR_NO_MEMORY for no room for its copies past two. */
static error_t TallyKid(uint64_t tree, const TallyPlace* p, TallyKey key, const uint8_t* b, uint32_t len,
                        const TallySink* sink, TallyPlace* kid, bool* read)
{
    BookPtr  ptr;
    uint64_t count = 0;
    int      got   = TallyInnerGet(b, len, &count, &ptr);

    memset(kid, 0, sizeof(*kid));
    kid->level  = p->level - 1u;
    kid->lo     = key;
    kid->has_lo = true;
    kid->hi     = p->hi;
    kid->has_hi = p->has_hi;
    *read       = got != TALLY_BODY_BROKEN;
    if (!*read)
        return OK;
    if (got == TALLY_BODY_LONGER && sink->said) {
        char     words[160];
        BookPtr  again;
        uint32_t known = TallyVarintBytes(count);
        known += BookPtrGet(b + known, len - known, &again);
        ksnprintf(words, sizeof(words), "an inner item of %s is %u byte(s) longer than this reader knows — "
                  "written by a kernel that knows more", TallyTreeName(tree), len - known);
        TagFSUnderGlass(words);
    }
    kid->count = count;
    return TallyPtrOf(&ptr, &kid->ptr);
}

/*
 * An inner node's children, pushed so the leftmost is visited first: each at
 * the range [its separator, the next separator) — the last one's reaching to
 * the parent's own end (tally.py TreeRead._Kids). The count the node carries
 * is held to theirs only when every pointer read.
 */
static error_t TallyInnerKids(uint64_t tree, const TallyPlace* p, const TallyReading* r,
                              const TallySink* sink, TallyStack* st)
{
    const uint8_t* s     = r->node.unit.stream;
    uint32_t       end   = r->node.unit.count * BOOK_PAYLOAD_BYTES, at = r->node.items_at, body, len;
    uint32_t       base  = st->n;
    bool           all   = true;
    TallyKey       prev  = r->node.head.first, key;
    uint64_t       under = 0;

    for (uint64_t i = 0; i < r->node.head.items; i++) {
        TallyPlace kid;
        TallyItemGet(s, end, &at, tree, prev, &key, &body, &len);
        prev = key;
        if (base < st->n) {
            st->at[st->n - 1u].hi     = key;
            st->at[st->n - 1u].has_hi = true;
        }
        bool read = false;
        if (TallyKid(tree, p, key, s + body, len, sink, &kid, &read) != OK)
            return ERR_NO_MEMORY;
        if (read)
            under += kid.count;
        else
            all = false;
        if (!TallyPush(st, &kid))
            return ERR_NO_MEMORY;
    }
    if (all && sink->said)
        TallyCountSay(sink, tree, p, under);
    /* Visited leftmost first: the pushed run reversed. */
    for (uint32_t a = base, z = st->n; a + 1u < z; a++, z--) {
        TallyPlace t = st->at[a];
        st->at[a]    = st->at[z - 1u];
        st->at[z - 1u] = t;
    }
    return OK;
}

/* One place of the walk: the node read and handed on, or the range it names
 * rotten. `root` the tree's own root, which the mount asked about first. */
static error_t TallyVisit(uint64_t tree, const TallyPlace* p, bool leaves, const TallySink* sink,
                          TallyStack* st, TallyReading* r)
{
    char*   why = r->words;
    error_t rc  = sink->node ? sink->node(sink->ctx, p) : OK;
    if (rc != OK)
        return rc;
    if (p->ptr.copies == 0)
        return sink->rotten(sink->ctx, p, "its pointer: it does not read");
    if (p->level == 0 && !leaves)
        return OK;
#ifdef CONFIG_VOLUME_TESTS
    if (leaves)
        TallyStandKeep(tree, p);
#endif
    if ((rc = TallyNodeRead(p, &r->node, &r->said)) != OK)
        return rc;
    if (!r->node.unit.whole) {
        TallyCopiesWords(&r->said, why, sizeof(r->words));
        rc = sink->rotten(sink->ctx, p, why);
    } else if (r->node.head.tree != tree || r->node.head.level != p->level) {
        ksnprintf(why, sizeof(r->words), "it is a node of tree %llu level %llu",
                  (unsigned long long)r->node.head.tree, (unsigned long long)r->node.head.level);
        rc = sink->rotten(sink->ctx, p, why);
    } else {
        if (sink->said)
            TallyNodeSaid(sink, tree, p, r);
        rc = p->level == 0 ? TallyLeafItems(tree, p, r, sink) : TallyInnerKids(tree, p, r, sink, st);
    }
    TallyNodeLetGo(&r->node);
    return rc;
}

error_t TallyRead(uint64_t tree, const SpineRoot* root, bool leaves, const TallySink* sink)
{
    TallyStack    st = { NULL, 0, 0 };
    TallyPlace    p;
    TallyReading* r  = kmalloc(sizeof(TallyReading));
    error_t       rc = r ? OK : ERR_NO_MEMORY;

    if (rc == OK)
        rc = TallyRootPlace(root, &p);
    if (rc == OK && !TallyPush(&st, &p))
        rc = ERR_NO_MEMORY;
    while (rc == OK && st.n > 0) {
        p  = st.at[--st.n];
        rc = TallyVisit(tree, &p, leaves, sink, &st, r);
    }
    kfree(st.at);
    kfree(r);
    return rc;
}
