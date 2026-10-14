#include "tally_read.h"
#include "book_unit.h"

/*
 * The Tally, walked (tally_read.h). Every node is read into memory the heap
 * gives, one at a time, and let go before the next: the walk holds only the
 * places still to visit, a stack in the heap — no recursion, so a tree of any
 * height costs one frame.
 */

static const char* const g_tree_name[] = {
    [TALLY_FILES] = "FILES", [TALLY_TAGS] = "TAGS",       [TALLY_USE] = "USE",
    [TALLY_BOOKMARKS] = "BOOKMARKS", [TALLY_LIEN] = "LIEN", [TALLY_DIGESTS] = "DIGESTS",
    [TALLY_MAP] = "MAP",     [TALLY_SHARED] = "SHARED",
};

const char* TallyTreeName(uint64_t tree)
{
    return tree >= 1 && tree <= TALLY_SHARED ? g_tree_name[tree] : "unknown";
}

/* The copies of pointers past the two a place holds (tally_read.h), in the
 * order met: written only by the mount's reads of the Tally, with the door
 * shut, and read by any reader of a leaf afterwards. */
static uint64_t* s_copies;
static uint32_t  s_copies_count;
static uint32_t  s_copies_cap;

uint64_t TallyPtrBlock(const TallyPtr* p, uint32_t k)
{
    return k < TALLY_COPIES_HELD ? p->block[k] : s_copies[p->more + (k - TALLY_COPIES_HELD)];
}

/* Room in the list for `n` copies more: false when the heap will not give it. */
static bool TallyCopiesRoom(uint64_t n)
{
    if (s_copies_count + n <= s_copies_cap)
        return true;
    if (n > UINT32_MAX - s_copies_count)
        return false;
    uint64_t  want = s_copies_count + n;
    uint64_t  cap  = s_copies_cap ? s_copies_cap : 8u;
    while (cap < want)
        cap *= 2u;
    if (cap > UINT32_MAX)
        cap = want;
    uint64_t* more = kmalloc(sizeof(uint64_t) * cap);
    if (!more)
        return false;
    if (s_copies_count)
        memcpy(more, s_copies, sizeof(uint64_t) * s_copies_count);
    kfree(s_copies);
    s_copies     = more;
    s_copies_cap = (uint32_t)cap;
    return true;
}

/* A pointer as numbers, every copy taken in its order (volume_book.h): the
 * first two into the place, the rest into the list. ERR_NO_MEMORY when the
 * list has no room for them. */
static error_t TallyPtrOf(const BookPtr* p, TallyPtr* out)
{
    uint32_t at = 0;
    memset(out, 0, sizeof(*out));
    out->seq    = p->seq;
    out->sheets = p->sheets;
    out->copies = (uint32_t)p->copies;        /* BookPtrGet: no more than the bytes they take */
    if (p->copies > TALLY_COPIES_HELD) {
        if (!TallyCopiesRoom(p->copies - TALLY_COPIES_HELD))
            return ERR_NO_MEMORY;
        out->more = s_copies_count;
    }
    for (uint32_t k = 0; k < out->copies; k++) {
        uint64_t block = 0;
        at += BookVarintGet(p->blocks + at, p->blocks_bytes - at, &block);
        if (k < TALLY_COPIES_HELD)
            out->block[k] = block;
        else
            s_copies[s_copies_count++] = block;
    }
    return OK;
}

error_t TallyRootPlace(const SpineRoot* root, TallyPlace* out)
{
    memset(out, 0, sizeof(*out));
    out->level = root->level;
    out->count = root->items;
    return TallyPtrOf(&root->ptr, &out->ptr);
}

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

/* A copy read as a node: its head, where each record begins (the NodeHead,
 * then each item), and how much longer than known its NodeHead is. */
typedef struct {
    BookUnit  unit;
    NodeHead  head;
    uint32_t* starts;
    uint32_t  nstarts;
    uint32_t  items_at;       /* where the first item begins */
    uint32_t  head_longer;
} TallyNode;

static void TallyNodeLetGo(TallyNode* n)
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

/* What the copies of a node that did not hold said, each of the first
 * TALLY_COPIES_SAID in its own words, for the lines that name them and for
 * the refusal of a root — a pointer may name any number of copies (§16.2
 * п.17), so the ones past them are said by their count alone: a line a copy
 * would be a line for every byte a damaged or foreign pointer spends, said at
 * every mount, past the owner's bound for a list said at mount (09-27). */
#define TALLY_COPIES_SAID  4u

typedef struct {
    uint32_t failed;
    char     why[TALLY_COPIES_SAID + 1u][112];   /* the last, where each copy past them is judged */
} TallyCopiesSaid;

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
static error_t TallyNodeRead(const TallyPlace* p, TallyNode* n, TallyCopiesSaid* said)
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
static void TallyCopiesWords(const TallyCopiesSaid* said, char* out, size_t room)
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

/* A node in hand and what its copies said, in the heap: a walk reads them on
 * the road from a program's read to the medium, which has a frame to spare
 * for neither. */
typedef struct {
    TallyNode       node;
    TallyCopiesSaid said;
    char            words[TALLY_COPIES_SAID * 120u];   /* a line about the node, made here */
} TallyReading;

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

#ifdef CONFIG_VOLUME_TESTS
/* The nodes the walk of the Tally the mount stands on reached — the trees it
 * reads the leaves of — by tree, in the order it reached them. */
typedef struct {
    uint64_t   tree;
    TallyPlace place;
} TallyStandNode;

static TallyStandNode* s_stand_nodes;
static uint32_t        s_stand_count;
static uint32_t        s_stand_cap;

static void TallyStandKeep(uint64_t tree, const TallyPlace* p)
{
    if (s_stand_count == s_stand_cap) {
        uint32_t        cap  = s_stand_cap ? s_stand_cap * 2u : 32u;
        TallyStandNode* more = kmalloc(sizeof(TallyStandNode) * cap);
        if (!more)
            return;
        if (s_stand_count)
            memcpy(more, s_stand_nodes, sizeof(TallyStandNode) * s_stand_count);
        kfree(s_stand_nodes);
        s_stand_nodes = more;
        s_stand_cap   = cap;
    }
    s_stand_nodes[s_stand_count++] = (TallyStandNode){ tree, *p };
}

bool TallyStandNodeWhere(uint64_t tree, uint32_t index, TallyPlace* out)
{
    for (uint32_t i = 0; i < s_stand_count; i++) {
        if (s_stand_nodes[i].tree == tree && index-- == 0) {
            *out = s_stand_nodes[i].place;
            return true;
        }
    }
    return false;
}
#endif

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

// ----------------------------------------------------------------------------
// DIGESTS: kept by leaf at the mount, read by leaf when a block is
// ----------------------------------------------------------------------------

/* A leaf of DIGESTS as the mount's walk named it — or a range of the tree no
 * copy of whose node reads. Written at the mount with the door shut, read by
 * every core afterwards: never changed while the volume is up. */
typedef struct {
    TallyPlace place;
    bool       rotten;
} TallyDigestLeaf;

static TallyDigestLeaf* s_leaves;
static uint32_t         s_leaf_count;
static uint32_t         s_leaf_cap;

/*
 * The leaves read lately, each as its stream — four, the most recent kept:
 * a read walks a file block by block, and a file's blocks lie in a row, which
 * one leaf of up to DIGEST_ITEM_MOST digests to an element covers. Under a
 * leaf lock of its own, which holds nothing but this table: the leaf is read
 * with no lock held, and put in after.
 */
#define TALLY_DIGEST_SHEETS  4u

typedef struct {
    uint32_t leaf;        /* the index of the leaf it is; UINT32_MAX: empty */
    uint8_t* stream;      /* NULL: the leaf does not read */
    uint32_t room;
    uint64_t used;
} TallyDigestSheet;

static TagFSLock        s_digest_lock = TAGFS_LOCK_INIT("digests", LOCK_RANK_LEAF);
static TallyDigestSheet s_sheets[TALLY_DIGEST_SHEETS] = {
    { UINT32_MAX, NULL, 0, 0 }, { UINT32_MAX, NULL, 0, 0 },
    { UINT32_MAX, NULL, 0, 0 }, { UINT32_MAX, NULL, 0, 0 },
};
static uint64_t         s_sheet_tick;
static volatile uint8_t* s_leaf_said;     /* a leaf whose rot was said: once each */

error_t TallyDigestsKeep(const TallyPlace* leaf, bool rotten)
{
    if (s_leaf_count == s_leaf_cap) {
        uint32_t         cap  = s_leaf_cap ? s_leaf_cap * 2u : 32u;
        TallyDigestLeaf* more = kmalloc(sizeof(TallyDigestLeaf) * cap);
        uint8_t*         said = kmalloc(cap);
        if (!more || !said) {
            kfree(more);
            kfree(said);
            return ERR_NO_MEMORY;
        }
        memset(said, 0, cap);
        if (s_leaf_count)
            memcpy(more, s_leaves, sizeof(TallyDigestLeaf) * s_leaf_count);
        kfree(s_leaves);
        kfree((void*)s_leaf_said);
        s_leaves    = more;
        s_leaf_said = said;
        s_leaf_cap  = cap;
    }
    s_leaves[s_leaf_count++] = (TallyDigestLeaf){ *leaf, rotten };
    return OK;
}

/* The leaf whose range holds `block` — the last whose first key is not past
 * it — or UINT32_MAX. Kept in the walk's order, which is the keys'. */
static uint32_t TallyDigestLeafOf(uint64_t block)
{
    uint32_t lo = 0, hi = s_leaf_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!s_leaves[mid].place.has_lo || s_leaves[mid].place.lo.k1 <= block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo == 0)
        return UINT32_MAX;
    const TallyPlace* p = &s_leaves[lo - 1u].place;
    return !p->has_hi || block < p->hi.k1 ? lo - 1u : UINT32_MAX;
}

/* The element of a leaf's stream that holds `block`, its digest in *digest:
 * 1 found, 0 not named, -1 an element on the way does not read. */
static int TallyDigestIn(const uint8_t* s, uint32_t room, uint64_t block, uint64_t* digest)
{
    NodeHead       h;
    TallyKey       prev, key;
    uint32_t       at = 0, body, len;
    uint64_t       n;
    const uint8_t* d;

    if (NodeHeadGet(s, room, &at, &h) == TALLY_BODY_BROKEN)
        return -1;
    prev = h.first;
    for (uint64_t i = 0; i < h.items; i++) {
        if (!TallyItemGet(s, room, &at, TALLY_DIGESTS, prev, &key, &body, &len))
            return -1;
        prev = key;
        if (key.k1 > block)
            return 0;
        if (TallyDigestsGet(s + body, len, &n, &d) == TALLY_BODY_BROKEN)
            return -1;
        if (block - key.k1 < n) {
            *digest = TallyDigestAt(d, block - key.k1);
            return 1;
        }
    }
    return 0;
}

/* A leaf's rot, said once — by why, its blocks read unchecked (§4.5 п.3). */
static void TallyDigestRotSay(uint32_t leaf, const char* why)
{
    const TallyPlace* p = &s_leaves[leaf].place;
    if (__atomic_exchange_n(&s_leaf_said[leaf], 1u, __ATOMIC_ACQ_REL))
        return;
    kprintf("[TagFS] the DIGESTS range of blocks [%llu, %s%llu) rots in every copy (%s): those blocks "
            "read unchecked\n", (unsigned long long)(p->has_lo ? p->lo.k1 : 0), p->has_hi ? "" : "the end, ",
            (unsigned long long)(p->has_hi ? p->hi.k1 : tagfs_get_state()->block_end), why);
}

/* Leaf `leaf` read off the medium: OK with its stream in *stream — NULL when
 * it does not read in any copy, said — or the refusal: memory's, or the
 * medium's. */
static error_t TallyDigestLeafRead(uint32_t leaf, uint8_t** stream, uint32_t* room)
{
    TallyReading* r = kmalloc(sizeof(TallyReading));
    *stream = NULL;
    if (!r)
        return ERR_NO_MEMORY;
    error_t rc = TallyNodeRead(&s_leaves[leaf].place, &r->node, &r->said);
    if (rc == OK && r->node.unit.whole) {
        *stream = r->node.unit.stream;
        *room   = r->node.unit.count * BOOK_PAYLOAD_BYTES;
        r->node.unit.stream = NULL;
    } else if (rc == OK) {
        TallyCopiesWords(&r->said, r->words, sizeof(r->words));
        TallyDigestRotSay(leaf, r->words);
    }
    TallyNodeLetGo(&r->node);
    kfree(r);
    return rc;
}

/* Whether leaf `leaf` is in the table, its digest of `block` asked under the
 * lock: 1 found, 0 not named, -1 its element does not read, -2 not held. */
static int TallyDigestCached(uint32_t leaf, uint64_t block, uint64_t* digest)
{
    int found = -2;
    TagFSLockTake(&s_digest_lock);
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++) {
        if (s_sheets[i].leaf == leaf && s_sheets[i].stream) {
            s_sheets[i].used = ++s_sheet_tick;
            found = TallyDigestIn(s_sheets[i].stream, s_sheets[i].room, block, digest);
            break;
        }
    }
    TagFSLockDrop(&s_digest_lock);
    return found;
}

/* A leaf read with no lock held, put in the table in place of the least
 * lately used — the one it pushes out let go of after the lock. */
static void TallyDigestCache(uint32_t leaf, uint8_t* stream, uint32_t room)
{
    uint8_t* gone = stream;
    TagFSLockTake(&s_digest_lock);
    uint32_t oldest = 0;
    bool     held   = false;
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++) {
        held |= s_sheets[i].leaf == leaf && s_sheets[i].stream;
        if (s_sheets[i].used < s_sheets[oldest].used)
            oldest = i;
    }
    if (!held) {
        gone = s_sheets[oldest].stream;
        s_sheets[oldest] = (TallyDigestSheet){ leaf, stream, room, ++s_sheet_tick };
    }
    TagFSLockDrop(&s_digest_lock);
    kfree(gone);
}

TallyDigestSays TallyDigestFind(uint64_t block, uint64_t* digest, error_t* refused)
{
    uint32_t leaf = TallyDigestLeafOf(block);
    if (leaf == UINT32_MAX)
        return TALLY_DIGEST_UNNAMED;
    if (s_leaves[leaf].rotten) {
        TallyDigestRotSay(leaf, "a node above it does not read");
        return TALLY_DIGEST_ROTTEN;
    }
    int found = TallyDigestCached(leaf, block, digest);
    if (found == -2) {
        uint32_t room   = 0;
        uint8_t* stream = NULL;
        error_t  rc     = TallyDigestLeafRead(leaf, &stream, &room);
        if (rc != OK) {
            *refused = rc;
            return TALLY_DIGEST_UNREAD;
        }
        if (!stream)
            return TALLY_DIGEST_ROTTEN;
        found = TallyDigestIn(stream, room, block, digest);
        TallyDigestCache(leaf, stream, room);
    }
    if (found < 0) {
        TallyDigestRotSay(leaf, "its element does not read");
        return TALLY_DIGEST_ROTTEN;
    }
    return found == 1 ? TALLY_DIGEST_NAMED : TALLY_DIGEST_UNNAMED;
}

/* One leaf's elements, each a run of digested blocks — and one that does not
 * read, the range up to the next element's key, or the leaf's end. */
static void TallyDigestsOf(uint32_t leaf, const uint8_t* s, uint32_t room,
                           void (*run)(void* ctx, uint64_t start, uint64_t count),
                           void (*rot)(void* ctx, uint64_t start, uint64_t count, bool to_end), void* ctx)
{
    const TallyPlace* p = &s_leaves[leaf].place;
    NodeHead          h;
    TallyKey          prev, key;
    uint32_t          at = 0, body, len;
    uint64_t          n, broken = 0;
    bool              open = false;
    const uint8_t*    d;

    NodeHeadGet(s, room, &at, &h);
    prev = h.first;
    for (uint64_t i = 0; i < h.items && TallyItemGet(s, room, &at, TALLY_DIGESTS, prev, &key, &body, &len); i++) {
        prev = key;
        if (open) {
            rot(ctx, broken, key.k1 - broken, false);
            open = false;
        }
        if (TallyDigestsGet(s + body, len, &n, &d) != TALLY_BODY_BROKEN) {
            run(ctx, key.k1, n);
        } else {
            TallyDigestRotSay(leaf, "its element does not read");
            broken = key.k1;
            open   = true;
        }
    }
    if (open)
        rot(ctx, broken, p->has_hi && p->hi.k1 > broken ? p->hi.k1 - broken : 0, !p->has_hi);
}

error_t TallyDigestsEach(void (*run)(void* ctx, uint64_t start, uint64_t count),
                         void (*rot)(void* ctx, uint64_t start, uint64_t count, bool to_end), void* ctx)
{
    for (uint32_t leaf = 0; leaf < s_leaf_count; leaf++) {
        const TallyPlace* p    = &s_leaves[leaf].place;
        uint64_t          lo   = p->has_lo ? p->lo.k1 : 0;
        uint32_t          room = 0;
        uint8_t*          s    = NULL;
        error_t           rc   = s_leaves[leaf].rotten ? OK : TallyDigestLeafRead(leaf, &s, &room);
        if (rc != OK)
            return rc;
        if (!s) {
            if (s_leaves[leaf].rotten)
                TallyDigestRotSay(leaf, "a node above it does not read");
            rot(ctx, lo, p->has_hi && p->hi.k1 > lo ? p->hi.k1 - lo : 0, !p->has_hi);
            continue;
        }
        TallyDigestsOf(leaf, s, room, run, rot, ctx);
        kfree(s);
    }
    return OK;
}

void TallyDigestsLetGo(void)
{
    TagFSLockTake(&s_digest_lock);
    uint8_t* gone[TALLY_DIGEST_SHEETS];
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++) {
        gone[i]     = s_sheets[i].stream;
        s_sheets[i] = (TallyDigestSheet){ UINT32_MAX, NULL, 0, 0 };
    }
    TagFSLockDrop(&s_digest_lock);
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++)
        kfree(gone[i]);
    kfree(s_leaves);
    kfree((void*)s_leaf_said);
    s_leaves     = NULL;
    s_leaf_said  = NULL;
    s_leaf_count = s_leaf_cap = 0;
    kfree(s_copies);
    s_copies       = NULL;
    s_copies_count = s_copies_cap = 0;
#ifdef CONFIG_VOLUME_TESTS
    kfree(s_stand_nodes);
    s_stand_nodes = NULL;
    s_stand_count = s_stand_cap = 0;
#endif
}
