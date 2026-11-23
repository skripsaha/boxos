/*
 * book_bind.c — the book bound: the first Tally of a volume in both its
 * Tally-Quires, slot 0 of its Spine naming it, and the ribbon's two Quires
 * blank for the first stitch a kernel will sew (book_bind.h). The host's
 * volume maker and the kernel's bind with it.
 *
 * ‼ THE SAME BINDER AS volume-read's, SAID AGAIN IN C. tools/volumeread/
 * binder.py lays a Tally by one rule, and so does this, step for step —
 * Pack, then the layers bottom up, then the grains they take — so that the
 * same files, uuid and BORN come out the same bytes from either. That is what
 * lets the host bind a mkfs volume's Tally again where it lies and hold it
 * byte for byte (binder.Circle), and a kernel's whole Tally be held to the
 * Python one.
 *
 * The rule, once:
 *   - a file's runs cut into Parcels as evenly as they go, none past
 *     PARCEL_STRIPS_MOST (file_map/tract.c CutPieces, CutLength);
 *   - leaves filled greedily in key order — an element that would not fit the
 *     Sheet of the node being filled begins the next, and one longer than a
 *     Sheet is the only element of a node of as many Sheets as it takes;
 *   - the inner levels the same, bottom up, each element a child's count and
 *     BookPtr, until one node — the root — is left;
 *   - the trees in the order of their kind, every node on an atom, copy 1
 *     from Tally-Quire A and copy 2 at the same offset in B;
 *   - A the first grain at or past the end of the files, k grains each — the
 *     least k all the nodes fit in, found again whenever the pointers k
 *     moves grow an inner node; the ribbon's Quire and its next_quire the two
 *     grains past B — refused when they run past the data run.
 * Every node of a Tally is written in both copies (§2.5), and an empty tree
 * is not written at all: a volume given no name has no USE (§1.4).
 */

#if __STDC_HOSTED__
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define BookAlloc(n)       malloc(n)
#define BookRealloc(p, n)  realloc((p), (n))
#define BookFree(p)        free(p)
#else
#include "ktypes.h"
#include "klib.h"
#define BookAlloc(n)       kmalloc(n)
#define BookRealloc(p, n)  krealloc((p), (n))
#define BookFree(p)        kfree(p)
#endif
#include "volume_deed.h"
#include "book_bind.h"

/* Room for `n` things of `size`, zeroed — the heap of either side has no
 * calloc of its own in common. */
static void* BookZalloc(uint64_t n, uint64_t size)
{
    uint64_t bytes = n * size;
    void*    p     = BookAlloc(bytes ? bytes : 1u);
    if (p)
        memset(p, 0, bytes);
    return p;
}

/* ====================================================================
 *  The leaves
 * ==================================================================== */

/* Room for one body, freed with the leaves; NULL when memory is out. */
uint8_t* BindBody(BindLeaves* l, uint64_t bytes) {
    if (l->nbodies == l->bodies_room) {
        uint32_t  room  = l->bodies_room ? l->bodies_room * 2u : 64u;
        uint8_t** grown = BookRealloc(l->bodies, room * sizeof(*grown));
        if (!grown) return NULL;
        l->bodies      = grown;
        l->bodies_room = room;
    }
    uint8_t* body = BookAlloc(bytes ? bytes : 1u);
    if (body) l->bodies[l->nbodies++] = body;
    return body;
}

/* One element after the last of tree `tree`'s — the caller hands them in
 * key order. 0, or -1 when memory is out. */
int BindAdd(BindLeaves* l, uint32_t tree, TallyKey key, const uint8_t* body, uint32_t len,
            uint64_t count) {
    if (l->count[tree] == l->room[tree]) {
        uint32_t  room  = l->room[tree] ? l->room[tree] * 2u : 64u;
        BindItem* grown = BookRealloc(l->items[tree], room * sizeof(*grown));
        if (!grown) return -1;
        l->items[tree] = grown;
        l->room[tree]  = room;
    }
    l->items[tree][l->count[tree]++] = (BindItem){ key, body, len, count };
    return 0;
}

void BindLeavesFree(BindLeaves* l) {
    for (uint32_t i = 0; i < l->nbodies; i++) BookFree(l->bodies[i]);
    BookFree(l->bodies);
    for (uint32_t t = 0; t <= TALLY_SHARED; t++) BookFree(l->items[t]);
    memset(l, 0, sizeof(*l));
}

/* file_map/tract.c's cut: how many pieces `count` things go into, at most
 * `most` each, and how many piece `i` holds — as even as they come. */
static uint64_t CutPieces(uint64_t count, uint64_t most) {
    return (count + most - 1u) / most;
}

static uint64_t CutLength(uint64_t count, uint64_t pieces, uint64_t i) {
    return count / pieces + (i < count % pieces ? 1u : 0u);
}

/* A run in the Tally's form takes at most a head, a start and a birth_delta
 * of ten bytes each, and its extensions with their length. */
static uint64_t BindRunsMost(const TallyRun* runs, uint64_t n) {
    uint64_t bytes = 10u;
    for (uint64_t i = 0; i < n; i++) bytes += 40u + runs[i].ext_len;
    return bytes;
}

/* fid's runs as its Parcels — elements (fid, 1 + b) of FILES, b the file
 * block each begins at, each counting 0 toward its parent: FILES counts
 * files, by their headers. An empty file has none. 0, or -1. */
int BindParcels(BindLeaves* l, uint64_t fid, const TallyRun* runs, uint64_t n) {
    uint64_t pieces = CutPieces(n, PARCEL_STRIPS_MOST), at = 0, block = 0;
    for (uint64_t i = 0; i < pieces; i++) {
        uint64_t len  = CutLength(n, pieces, i);
        uint8_t* body = BindBody(l, BindRunsMost(runs + at, len));
        if (!body) return -1;
        uint32_t bytes = TallyParcelPut(body, runs + at, (uint32_t)len, l->seq);
        if (BindAdd(l, TALLY_FILES, (TallyKey){ fid, 1u + block }, body, bytes, 0) != 0) return -1;
        for (uint64_t r = at; r < at + len; r++) block += runs[r].count;
        at += len;
    }
    return 0;
}

/* ====================================================================
 *  Nodes: a level packed, and a node's stream
 * ==================================================================== */

/* A node's worth of a level's elements: `n` of them from `from`, the bytes
 * of its stream, and the count it carries. */
typedef struct {
    uint32_t from;
    uint32_t n;
    uint64_t size;
    uint64_t count;
} BindCut;

static uint32_t BindHeadBytes(uint32_t tree, uint32_t level, uint64_t items, TallyKey first) {
    NodeHead h = { .tree = tree, .level = level, .items = items, .first = first };
    return NodeHeadBytes(&h);
}

/*
 * The nodes of one level (binder.py Pack): elements in key order, greedily —
 * one that would not fit the Sheet of the node being filled begins the next;
 * one longer than a Sheet alone is the only element of its node, of as many
 * Sheets as it takes. The NodeHead grows with the count it carries, and its
 * growth is counted. Into *cuts (at most one node an element); -1 when memory
 * is out.
 */
static int BindPack(uint32_t tree, uint32_t level, const BindItem* items, uint32_t n, BindCut** cuts,
                    uint32_t* ncuts) {
    BindCut* out = BookAlloc(sizeof(*out) * (n ? n : 1u));
    uint32_t m = 0, cur = 0;
    BindCut  c = { 0, 0, 0, 0 };
    TallyKey first = { 0, 0 };
    if (!out) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (cur) {
            uint64_t grown = c.size + TallyItemBytes(tree, items[i].key, items[i - 1].key, items[i].len) +
                             BindHeadBytes(tree, level, cur + 1u, first) -
                             BindHeadBytes(tree, level, cur, first);
            if (grown <= TALLY_SHEET_PAYLOAD) {
                cur++;
                c.size   = grown;
                c.count += items[i].count;
                continue;
            }
            c.n      = cur;
            out[m++] = c;
        }
        first = items[i].key;
        cur   = 1;
        c     = (BindCut){ i, 1, BindHeadBytes(tree, level, 1, first) +
                                 TallyItemBytes(tree, first, first, items[i].len), items[i].count };
        if (c.size > TALLY_SHEET_PAYLOAD) {
            out[m++] = c;
            cur      = 0;
        }
    }
    if (cur) {
        c.n      = cur;
        out[m++] = c;
    }
    *cuts  = out;
    *ncuts = m;
    return 0;
}

/* The node of `cut` laid at `off`: its stream — NodeHead, then each element
 * its key written from the one before it (the first from itself, as 0) —
 * and where each record of it begins. 0, or -1 when memory is out. */
static int BindNodeMake(BindPlan* plan, uint32_t tree, uint32_t level, const BindItem* items,
                        const BindCut* cut, uint64_t off) {
    if (plan->nnodes == plan->room) {
        uint32_t  room  = plan->room ? plan->room * 2u : 64u;
        BindNode* grown = BookRealloc(plan->nodes, room * sizeof(*grown));
        if (!grown) return -1;
        plan->nodes = grown;
        plan->room  = room;
    }
    uint64_t  sheets = cut->size > TALLY_SHEET_PAYLOAD ?
                       (cut->size + TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD : 1u;
    BindNode* node   = &plan->nodes[plan->nnodes];
    *node = (BindNode){ .tree = tree, .level = level, .count = cut->count, .sheets = sheets, .off = off,
                        .first = items[cut->from].key };
    node->stream = BookZalloc(sheets, TALLY_SHEET_PAYLOAD);
    node->starts = BookAlloc(sizeof(uint32_t) * (cut->n + 1u));
    if (!node->stream || !node->starts) {
        BookFree(node->stream);
        BookFree(node->starts);
        return -1;
    }
    NodeHead h   = { .tree = tree, .level = level, .items = cut->n, .first = node->first };
    uint32_t at  = NodeHeadPut(node->stream, &h);
    node->starts[0] = 0;
    for (uint32_t i = 0; i < cut->n; i++) {
        const BindItem* it = &items[cut->from + i];
        node->starts[i + 1u] = at;
        at += TallyItemPut(node->stream + at, tree, it->key, i ? it[-1].key : it->key, it->body, it->len);
    }
    node->len     = at;
    node->nstarts = cut->n + 1u;
    plan->nnodes++;
    return 0;
}

/* The inner elements naming the `n` nodes of a level from `first` — each the
 * node's first key, and its count and BookPtr to both its copies — into
 * *items, their bodies in *bodies. -1 when memory is out. */
static int BindInner(const BindPlan* plan, uint32_t first, uint32_t n, uint64_t a, uint64_t b, uint64_t seq,
                     BindItem** items, uint8_t** bodies) {
    enum { INNER_MOST = 10u + 10u * (3u + 2u) };    /* count, and a pointer of two copies */
    *items  = BookAlloc(sizeof(BindItem) * n);
    *bodies = BookAlloc((size_t)INNER_MOST * n);
    if (!*items || !*bodies) return -1;
    for (uint32_t j = 0; j < n; j++) {
        const BindNode* node   = &plan->nodes[first + j];
        uint64_t        at[2]  = { a + node->off, b + node->off };
        uint8_t         ptr[INNER_MOST];
        uint32_t        plen   = BookPtrPut(ptr, seq, node->sheets, at, 2);
        uint8_t*        body   = *bodies + (size_t)INNER_MOST * j;
        (*items)[j] = (BindItem){ node->first, body, TallyInnerPut(body, node->count, ptr, plen), node->count };
    }
    return 0;
}

/* ====================================================================
 *  The Tally laid: every tree, at A and B
 * ==================================================================== */

/* One tree's nodes, its leaves up to its root, laid from *off on. 0, or -1
 * when memory is out. */
static int BindTree(const BindLeaves* l, uint32_t tree, uint64_t a, uint64_t b, BindPlan* plan,
                    uint64_t* off) {
    const BindItem* items  = l->items[tree];
    uint32_t        n      = l->count[tree];
    BindItem*       inner  = NULL;
    uint8_t*        bodies = NULL;
    int             rc     = -1;
    for (uint32_t level = 0;; level++) {
        BindCut* cuts;
        uint32_t ncuts, first = plan->nnodes;
        if (BindPack(tree, level, items, n, &cuts, &ncuts) != 0) goto out;
        for (uint32_t i = 0; i < ncuts; i++) {
            *off = (*off + l->grid - 1u) / l->grid * l->grid;
            if (BindNodeMake(plan, tree, level, items, &cuts[i], *off) != 0) {
                BookFree(cuts);
                goto out;
            }
            *off += plan->nodes[plan->nnodes - 1u].sheets;
        }
        BookFree(cuts);
        BookFree(inner);        /* the level below's elements are in its nodes' streams now */
        BookFree(bodies);
        inner  = NULL;
        bodies = NULL;
        if (ncuts == 1) {
            plan->root[tree] = (int32_t)first;
            rc = 0;
            goto out;
        }
        if (BindInner(plan, first, ncuts, a, b, l->seq, &inner, &bodies) != 0) goto out;
        items = inner;
        n     = ncuts;
    }
out:
    BookFree(inner);
    BookFree(bodies);
    return rc;
}

void BindPlanFree(BindPlan* plan) {
    for (uint32_t i = 0; i < plan->nnodes; i++) {
        BookFree(plan->nodes[i].stream);
        BookFree(plan->nodes[i].starts);
    }
    BookFree(plan->nodes);
    memset(plan, 0, sizeof(*plan));
}

/* Every tree laid with copy 1 at `a` and copy 2 at `b`: *used, the blocks
 * from the first node's to past the last's. 0, or -1 when memory is out. */
static int BindLayAt(const BindLeaves* l, uint64_t a, uint64_t b, BindPlan* plan, uint64_t* used) {
    uint64_t off = 0;
    memset(plan, 0, sizeof(*plan));
    for (uint32_t t = 0; t <= TALLY_SHARED; t++) plan->root[t] = -1;
    for (uint32_t tree = TALLY_FILES; tree <= TALLY_SHARED; tree++) {
        if (l->count[tree] && BindTree(l, tree, a, b, plan, &off) != 0) {
            BindPlanFree(plan);
            return -1;
        }
    }
    *used = off;
    return 0;
}

/*
 * Where the book goes (§9.3, §16.3 п.5): Tally-Quire A at the first grain at
 * or past block `after` — the end of the files — and B k grains on, k the
 * least number of grains the nodes take. The pointers of the inner nodes
 * name blocks of A and B, so a k that grows can grow a node in turn: laid
 * again until k holds still (it only grows, and the nodes are finite). The
 * ribbon's Quire and its next_quire are the two grains past B. BIND_NO_ROOM
 * when next_quire runs past `data_end` — the block of the far Deed.
 */
int QuireLay(const BindLeaves* l, uint64_t after, uint64_t data_end, BindPlan* plan) {
    uint64_t grain = l->grain, a = (after + grain - 1u) / grain * grain, grains = 1, used;
    for (;;) {
        if (BindLayAt(l, a, a + grains * grain, plan, &used) != 0)
            return BIND_NO_MEMORY;
        if (used <= grains * grain) break;
        grains = (used + grain - 1u) / grain;
        BindPlanFree(plan);
    }
    plan->a      = a;
    plan->k      = grains;
    plan->b      = a + grains * grain;
    plan->ribbon = plan->b + grains * grain;
    plan->next   = plan->ribbon + grain;
    if (plan->next + grain > data_end) {
        uint64_t next = plan->next;
        BindPlanFree(plan);
        plan->a    = a;
        plan->next = next;
        return BIND_NO_ROOM;
    }
    return BIND_OK;
}

/* ====================================================================
 *  Onto the medium
 * ==================================================================== */

/* Every node of the plan sealed at `seq` and written in both copies — the
 * sectors of each copy sealed at their own addresses (§2.5). */
int BindTally(const BindSink* sink, const BindPlan* plan, const uint8_t uuid[16], uint64_t seq,
              uint64_t* failed) {
    for (uint32_t i = 0; i < plan->nnodes; i++) {
        const BindNode* node  = &plan->nodes[i];
        uint64_t        bytes = node->sheets * VOLUME_BLOCK_BYTES;
        uint8_t*        unit  = BookAlloc(bytes);
        const uint64_t  at[2] = { plan->a + node->off, plan->b + node->off };
        if (!unit)
            return BIND_NO_MEMORY;
        for (uint32_t c = 0; c < 2u; c++) {
            uint64_t vlba = at[c] * VOLUME_BLOCK_SECTORS;
            TallyUnitSeal(unit, node->stream, node->len, node->starts, node->nstarts, BOOK_UNIT_SHEET, seq,
                          vlba, (uint32_t)(node->sheets * VOLUME_BLOCK_SECTORS), uuid);
            if (sink->write(sink->ctx, at[c], unit, bytes) != 0) {
                *failed = at[c];
                BookFree(unit);
                return BIND_REFUSED;
            }
        }
        BookFree(unit);
    }
    return BIND_OK;
}

/*
 * Slot 0 (block `slot_block`) at `slot_seq`: `head`, a SpineRoot for every
 * tree the plan wrote — its root's level and count and a BookPtr to both its
 * copies, at the Tally's seq — SpineRibbon (the ribbon from its Quire's first
 * block, its next_quire after it) and `counts`; no SpineFlags, no flag being
 * set. And the ribbon's two Quires blank: the first stitch a kernel sews
 * begins at the first of them (§16.3 п.5).
 */
int BindSpine(const BindSink* sink, const BindPlan* plan, uint64_t slot_block, uint64_t grain,
              const uint8_t uuid[16], const SpineHead* head, const SpineCounts* counts,
              uint64_t slot_seq, uint64_t* failed) {
    uint8_t*              stream = BookZalloc(1, TALLY_SHEET_PAYLOAD);
    uint8_t*              unit   = BookZalloc(1, VOLUME_BLOCK_BYTES);
    uint8_t*              blank  = BookZalloc(1, VOLUME_BLOCK_BYTES);
    uint32_t              starts[TALLY_SHARED + 4u], n = 0, at = 0;
    const SpineRibbon     ribbon = { plan->ribbon, plan->ribbon, grain, plan->next, grain };

    int                   rc     = BIND_OK;
    if (!stream || !unit || !blank) {
        rc = BIND_NO_MEMORY;
        goto out;
    }
    starts[n++] = at;
    at += SpineHeadPut(stream + at, head);
    for (uint32_t tree = TALLY_FILES; tree <= TALLY_SHARED; tree++) {
        if (plan->root[tree] < 0) continue;
        const BindNode* root   = &plan->nodes[plan->root[tree]];
        const uint64_t  at2[2] = { plan->a + root->off, plan->b + root->off };
        uint8_t         ptr[64];
        uint32_t        plen   = BookPtrPut(ptr, head->tally_seq, root->sheets, at2, 2);
        starts[n++] = at;
        at += SpineRootPut(stream + at, tree, 0, root->level, root->count, ptr, plen);
    }
    starts[n++] = at;
    at += SpineRibbonPut(stream + at, &ribbon);
    starts[n++] = at;
    at += SpineCountsPut(stream + at, counts);
    TallyUnitSeal(unit, stream, at, starts, n, BOOK_UNIT_SLOT, slot_seq, slot_block * VOLUME_BLOCK_SECTORS,
                  VOLUME_BLOCK_SECTORS, uuid);
    if (sink->write(sink->ctx, slot_block, unit, VOLUME_BLOCK_BYTES) != 0) {
        *failed = slot_block;
        rc      = BIND_REFUSED;
        goto out;
    }
    for (uint64_t b = plan->ribbon; b < plan->next + grain; b++) {
        if (sink->write(sink->ctx, b, blank, VOLUME_BLOCK_BYTES) != 0) {
            *failed = b;
            rc      = BIND_REFUSED;
            goto out;
        }
    }
out:
    BookFree(stream);
    BookFree(unit);
    BookFree(blank);
    return rc;
}
