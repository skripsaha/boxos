#include "file_map.h"
#include "name_index.h"

/*
 * A B+-tree of order 32: a node holds up to 32 keys, and every node but the
 * root at least 16. A leaf's keys are file ids with their entries beside them
 * and a link to the leaf on its right; an inner node's key[i] is the least id
 * under kid[i + 1], so kid[i] holds the ids from key[i - 1] up to key[i].
 */
#define FILE_MAP_ORDER 32u
#define FILE_MAP_LEAST (FILE_MAP_ORDER / 2u)

typedef struct {
    FileRecord* record;      /* the pool's newest record of the file, or NULL */
    RecordWhere where;       /* where the record sits, and which Ink put it there */
} FileMapEntry;

typedef struct FileMapNode FileMapNode;
struct FileMapNode {
    uint16_t     count;
    bool         leaf;
    FileMapNode* next;       /* a leaf's right neighbour; a let-go node's successor */
    uint64_t     key[FILE_MAP_ORDER];
    union {
        FileMapEntry entry[FILE_MAP_ORDER];
        FileMapNode* kid[FILE_MAP_ORDER + 1];
    } u;
};

/* The files whose record bears a mark, by id in order — a list the map keeps
 * where a record is put or let go, under its lock. `marked` counts them,
 * written under the lock and read with none where its reader says so — one
 * word, stored whole. The list is short when one had no room in it, and so
 * not every one, until no record bears the mark again. */
typedef struct {
    uint64_t*         fid;
    uint32_t          count, room;
    bool              short_of_room;
    volatile uint64_t marked;
} FileMapMarked;

struct FileMap {
    TagFSLock    lock;
    FileMapNode* root;
    uint32_t     height;         /* levels, the leaves' included; 0 when empty */
    uint32_t     files;
    uint32_t     nodes;
    uint64_t     record_bytes;
    /* The files whose record holds Slips (FileMapSlipped), their count read
     * with no lock (FileMapSlips) — the order of its reads and of the takes
     * beside it kept by the reader (allotment.c). */
    FileMapMarked slipped;
    /* The files of programs that show their tags (FileMapShowing). */
    FileMapMarked showing;
    /* The files whose record keeps a passport (FileMapProgramFiles). */
    FileMapMarked carrying;
    /* The files by the word their name is, or its stem is (name_index.h):
     * put and let go with the records, under the lock. */
    NameIndex    names;
    bool         said_short;     /* a refusal is said once, until the heap gives again */
};

typedef struct {
    FileMapNode* node[FILE_MAP_HEIGHT_MOST];
    uint32_t     slot[FILE_MAP_HEIGHT_MOST];
    uint32_t     depth;          /* the inner nodes above the leaf */
} FileMapPath;

/* What a change took out of the map, given back once its lock is let go. */
typedef struct {
    FileMapNode* nodes;
    FileRecord*  record;
} FileMapLeftover;

// ----------------------------------------------------------------------------
// The tree, under the map's lock
// ----------------------------------------------------------------------------

/* The first key at or past `key`. */
static uint32_t LowerBound(const FileMapNode* n, uint64_t key)
{
    uint32_t lo = 0, hi = n->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (n->key[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The kid of an inner node that holds `key`: past every key at or under it. */
static uint32_t KidFor(const FileMapNode* n, uint64_t key)
{
    uint32_t lo = 0, hi = n->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (n->key[mid] <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The leaf that holds `key`, or would, and the way down to it. */
static FileMapNode* MapDescend(const FileMap* m, uint64_t key, FileMapPath* path)
{
    FileMapNode* n = m->root;
    path->depth = 0;
    while (n && !n->leaf) {
        uint32_t i = KidFor(n, key);
        path->node[path->depth] = n;
        path->slot[path->depth] = i;
        path->depth++;
        n = n->u.kid[i];
    }
    return n;
}

static FileMapEntry* MapFind(const FileMap* m, uint64_t key)
{
    FileMapPath  path;
    FileMapNode* leaf = MapDescend(m, key, &path);
    if (!leaf)
        return NULL;
    uint32_t pos = LowerBound(leaf, key);
    return pos < leaf->count && leaf->key[pos] == key ? &leaf->u.entry[pos] : NULL;
}

/* The first entry at or past `key`: its leaf and place, or a NULL leaf. */
static FileMapNode* MapFrom(const FileMap* m, uint64_t key, uint32_t* out_pos)
{
    FileMapPath  path;
    FileMapNode* leaf = MapDescend(m, key, &path);
    if (!leaf)
        return NULL;
    uint32_t pos = LowerBound(leaf, key);
    if (pos == leaf->count) {
        leaf = leaf->next;
        pos  = 0;
    }
    *out_pos = pos;
    return leaf;
}

/* How many nodes adding `key` would take now: one per level that splits, and
 * one more for a new root when every level does. */
static uint32_t MapNodesToInsert(const FileMap* m, uint64_t key)
{
    if (!m->root)
        return 1;
    FileMapPath  path;
    FileMapNode* leaf = MapDescend(m, key, &path);
    if (leaf->count < FILE_MAP_ORDER)
        return 0;
    uint32_t need = 1;
    for (uint32_t d = path.depth; d-- > 0;) {
        if (path.node[d]->count < FILE_MAP_ORDER)
            return need;
        need++;
    }
    return need + 1;
}

static FileMapNode* MapTake(FileMap* m, FileMapStake* s, bool leaf)
{
    FileMapNode* n = s->node[--s->count];
    memset(n, 0, sizeof(*n));
    n->leaf = leaf;
    m->nodes++;
    return n;
}

static void MapLetGoNode(FileMap* m, FileMapNode* n, FileMapLeftover* left)
{
    m->nodes--;
    n->next     = left->nodes;
    left->nodes = n;
}

static void LeafOpen(FileMapNode* n, uint32_t pos, uint64_t key)
{
    memmove(&n->key[pos + 1], &n->key[pos], sizeof(uint64_t) * (n->count - pos));
    memmove(&n->u.entry[pos + 1], &n->u.entry[pos], sizeof(FileMapEntry) * (n->count - pos));
    n->key[pos] = key;
    memset(&n->u.entry[pos], 0, sizeof(FileMapEntry));
    n->count++;
}

static void LeafClose(FileMapNode* n, uint32_t pos)
{
    n->count--;
    memmove(&n->key[pos], &n->key[pos + 1], sizeof(uint64_t) * (n->count - pos));
    memmove(&n->u.entry[pos], &n->u.entry[pos + 1], sizeof(FileMapEntry) * (n->count - pos));
}

/* Take key[k] and kid[k + 1] out of an inner node. */
static void InnerClose(FileMapNode* n, uint32_t k)
{
    memmove(&n->key[k], &n->key[k + 1], sizeof(uint64_t) * (n->count - k - 1));
    memmove(&n->u.kid[k + 1], &n->u.kid[k + 2], sizeof(FileMapNode*) * (n->count - k - 1));
    n->count--;
}

/* A split below handed up `sep` and the new node `right` beside the kid the
 * path went down: each level takes them, or splits in turn and hands its own
 * middle key up, and a root that splits gets a new root above it. */
static void MapLift(FileMap* m, const FileMapPath* path, uint64_t sep, FileMapNode* right,
                    FileMapStake* s)
{
    for (uint32_t d = path->depth; d-- > 0;) {
        FileMapNode* p = path->node[d];
        uint32_t     i = path->slot[d];

        if (p->count < FILE_MAP_ORDER) {
            memmove(&p->key[i + 1], &p->key[i], sizeof(uint64_t) * (p->count - i));
            memmove(&p->u.kid[i + 2], &p->u.kid[i + 1], sizeof(FileMapNode*) * (p->count - i));
            p->key[i]       = sep;
            p->u.kid[i + 1] = right;
            p->count++;
            return;
        }

        uint64_t     keys[FILE_MAP_ORDER + 1];
        FileMapNode* kids[FILE_MAP_ORDER + 2];
        memcpy(keys, p->key, sizeof(uint64_t) * i);
        keys[i] = sep;
        memcpy(&keys[i + 1], &p->key[i], sizeof(uint64_t) * (FILE_MAP_ORDER - i));
        memcpy(kids, p->u.kid, sizeof(FileMapNode*) * (i + 1));
        kids[i + 1] = right;
        memcpy(&kids[i + 2], &p->u.kid[i + 1], sizeof(FileMapNode*) * (FILE_MAP_ORDER - i));

        FileMapNode* q = MapTake(m, s, false);
        p->count = FILE_MAP_LEAST;
        memcpy(p->key, keys, sizeof(uint64_t) * FILE_MAP_LEAST);
        memcpy(p->u.kid, kids, sizeof(FileMapNode*) * (FILE_MAP_LEAST + 1));
        q->count = FILE_MAP_ORDER - FILE_MAP_LEAST;
        memcpy(q->key, &keys[FILE_MAP_LEAST + 1], sizeof(uint64_t) * q->count);
        memcpy(q->u.kid, &kids[FILE_MAP_LEAST + 1], sizeof(FileMapNode*) * (q->count + 1));

        sep   = keys[FILE_MAP_LEAST];
        right = q;
    }

    FileMapNode* root = MapTake(m, s, false);
    root->count    = 1;
    root->key[0]   = sep;
    root->u.kid[0] = m->root;
    root->u.kid[1] = right;
    m->root        = root;
    m->height++;
}

/* A zeroed entry for `key`, which the map does not hold, the nodes it splits
 * taken from the stake — which holds at least MapNodesToInsert of them. */
static FileMapEntry* MapInsert(FileMap* m, uint64_t key, FileMapStake* s)
{
    m->files++;
    if (!m->root) {
        FileMapNode* leaf = MapTake(m, s, true);
        LeafOpen(leaf, 0, key);
        m->root   = leaf;
        m->height = 1;
        return &leaf->u.entry[0];
    }

    FileMapPath  path;
    FileMapNode* leaf = MapDescend(m, key, &path);
    uint32_t     pos  = LowerBound(leaf, key);
    if (leaf->count < FILE_MAP_ORDER) {
        LeafOpen(leaf, pos, key);
        return &leaf->u.entry[pos];
    }

    FileMapNode* right = MapTake(m, s, true);
    uint32_t     keep  = pos <= FILE_MAP_LEAST ? FILE_MAP_LEAST : FILE_MAP_LEAST + 1;
    right->count = (uint16_t)(FILE_MAP_ORDER - keep);
    memcpy(right->key, &leaf->key[keep], sizeof(uint64_t) * right->count);
    memcpy(right->u.entry, &leaf->u.entry[keep], sizeof(FileMapEntry) * right->count);
    leaf->count = (uint16_t)keep;
    right->next = leaf->next;
    leaf->next  = right;

    FileMapEntry* at;
    if (pos <= FILE_MAP_LEAST) {
        LeafOpen(leaf, pos, key);
        at = &leaf->u.entry[pos];
    } else {
        LeafOpen(right, pos - keep, key);
        at = &right->u.entry[pos - keep];
    }
    MapLift(m, &path, right->key[0], right, s);
    return at;
}

/* The leaf at kid i of p fell under its least: a key borrowed from a
 * neighbour that can spare one, or the two merged into one. */
static void LeafMend(FileMap* m, FileMapNode* p, uint32_t i, FileMapLeftover* left)
{
    FileMapNode* n = p->u.kid[i];
    FileMapNode* l = i > 0 ? p->u.kid[i - 1] : NULL;
    FileMapNode* r = i < p->count ? p->u.kid[i + 1] : NULL;

    if (l && l->count > FILE_MAP_LEAST) {
        LeafOpen(n, 0, l->key[l->count - 1]);
        n->u.entry[0] = l->u.entry[l->count - 1];
        l->count--;
        p->key[i - 1] = n->key[0];
        return;
    }
    if (r && r->count > FILE_MAP_LEAST) {
        n->key[n->count]     = r->key[0];
        n->u.entry[n->count] = r->u.entry[0];
        n->count++;
        LeafClose(r, 0);
        p->key[i] = r->key[0];
        return;
    }
    if (l) {
        memcpy(&l->key[l->count], n->key, sizeof(uint64_t) * n->count);
        memcpy(&l->u.entry[l->count], n->u.entry, sizeof(FileMapEntry) * n->count);
        l->count += n->count;
        l->next = n->next;
        InnerClose(p, i - 1);
        MapLetGoNode(m, n, left);
        return;
    }
    memcpy(&n->key[n->count], r->key, sizeof(uint64_t) * r->count);
    memcpy(&n->u.entry[n->count], r->u.entry, sizeof(FileMapEntry) * r->count);
    n->count += r->count;
    n->next = r->next;
    InnerClose(p, i);
    MapLetGoNode(m, r, left);
}

/* The same for an inner node: a key rotated through the parent, or a merge
 * that pulls the parent's separator down between the two. */
static void InnerMend(FileMap* m, FileMapNode* p, uint32_t i, FileMapLeftover* left)
{
    FileMapNode* n = p->u.kid[i];
    FileMapNode* l = i > 0 ? p->u.kid[i - 1] : NULL;
    FileMapNode* r = i < p->count ? p->u.kid[i + 1] : NULL;

    if (l && l->count > FILE_MAP_LEAST) {
        memmove(&n->key[1], &n->key[0], sizeof(uint64_t) * n->count);
        memmove(&n->u.kid[1], &n->u.kid[0], sizeof(FileMapNode*) * (n->count + 1));
        n->key[0]   = p->key[i - 1];
        n->u.kid[0] = l->u.kid[l->count];
        n->count++;
        p->key[i - 1] = l->key[l->count - 1];
        l->count--;
        return;
    }
    if (r && r->count > FILE_MAP_LEAST) {
        n->key[n->count]       = p->key[i];
        n->u.kid[n->count + 1] = r->u.kid[0];
        n->count++;
        p->key[i] = r->key[0];
        memmove(&r->key[0], &r->key[1], sizeof(uint64_t) * (r->count - 1));
        memmove(&r->u.kid[0], &r->u.kid[1], sizeof(FileMapNode*) * r->count);
        r->count--;
        return;
    }
    if (l) {
        l->key[l->count] = p->key[i - 1];
        memcpy(&l->key[l->count + 1], n->key, sizeof(uint64_t) * n->count);
        memcpy(&l->u.kid[l->count + 1], n->u.kid, sizeof(FileMapNode*) * (n->count + 1));
        l->count += 1 + n->count;
        InnerClose(p, i - 1);
        MapLetGoNode(m, n, left);
        return;
    }
    n->key[n->count] = p->key[i];
    memcpy(&n->key[n->count + 1], r->key, sizeof(uint64_t) * r->count);
    memcpy(&n->u.kid[n->count + 1], r->u.kid, sizeof(FileMapNode*) * (r->count + 1));
    n->count += 1 + r->count;
    InnerClose(p, i);
    MapLetGoNode(m, r, left);
}

/* `key` out of the tree, and every level it left short mended on the way up;
 * nodes a merge emptied go to `left`. The entry's record is the caller's. */
static void MapRemove(FileMap* m, uint64_t key, FileMapLeftover* left)
{
    FileMapPath  path;
    FileMapNode* node = MapDescend(m, key, &path);
    if (!node)
        return;
    uint32_t pos = LowerBound(node, key);
    if (pos >= node->count || node->key[pos] != key)
        return;
    LeafClose(node, pos);
    m->files--;

    for (uint32_t d = path.depth;; d--) {
        if (d == 0) {
            if (node->count == 0) {
                m->root = node->leaf ? NULL : node->u.kid[0];
                m->height--;
                MapLetGoNode(m, node, left);
            }
            return;
        }
        if (node->count >= FILE_MAP_LEAST)
            return;
        FileMapNode* p = path.node[d - 1];
        if (node->leaf)
            LeafMend(m, p, path.slot[d - 1], left);
        else
            InnerMend(m, p, path.slot[d - 1], left);
        node = p;
    }
}

/* Where `fid` is, or goes, in the list. */
static uint32_t MarkedAt(const FileMapMarked* l, uint64_t fid)
{
    uint32_t lo = 0, hi = l->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (l->fid[mid] < fid) lo = mid + 1u;
        else                   hi = mid;
    }
    return lo;
}

/* `fid` into the list, or the list short when there is no room for it. */
static void MarkedAdd(FileMapMarked* l, uint64_t fid)
{
    if (l->short_of_room)
        return;
    uint32_t at = MarkedAt(l, fid);
    if (at < l->count && l->fid[at] == fid)
        return;
    if (l->count == l->room) {
        uint32_t  room = l->room ? l->room * 2u : 16u;
        uint64_t* made = room > l->room ? kmalloc((size_t)room * sizeof(uint64_t)) : NULL;
        if (!made) {
            l->short_of_room = true;
            return;
        }
        if (l->count)
            memcpy(made, l->fid, (size_t)l->count * sizeof(uint64_t));
        kfree(l->fid);
        l->fid  = made;
        l->room = room;
    }
    memmove(l->fid + at + 1u, l->fid + at, (size_t)(l->count - at) * sizeof(uint64_t));
    l->fid[at] = fid;
    l->count++;
}

static void MarkedTake(FileMapMarked* l, uint64_t fid)
{
    uint32_t at = MarkedAt(l, fid);
    if (at < l->count && l->fid[at] == fid) {
        memmove(l->fid + at, l->fid + at + 1u, (size_t)(l->count - at - 1u) * sizeof(uint64_t));
        l->count--;
    }
    if (l->marked == 0) {
        /* None bears the mark: the list, empty, is every one again. */
        l->count         = 0;
        l->short_of_room = false;
    }
}

/* The list as `fid`'s record bears the mark `before` and `after`. */
static void MarkedMet(FileMapMarked* l, uint64_t fid, bool before, bool after)
{
    if (before == after)
        return;
    __atomic_store_n(&l->marked, l->marked + (after ? 1u : 0u) - (before ? 1u : 0u), __ATOMIC_RELEASE);
    if (after) MarkedAdd(l, fid);
    else       MarkedTake(l, fid);
}

/* From `from` on, the files of the list into `out`, `room` of them at the
 * most — UINT32_MAX when it is short. Under the lock. */
static uint32_t MarkedPage(const FileMapMarked* l, uint64_t from, uint64_t* out, uint32_t room)
{
    if (l->short_of_room)
        return UINT32_MAX;
    uint32_t n = 0;
    for (uint32_t at = MarkedAt(l, from); at < l->count && n < room; at++)
        out[n++] = l->fid[at];
    return n;
}

/* The map's lists as `fid`'s `was` becomes `now` (NULL for none): the files
 * with Slips, the files that show their tags, the files that keep a passport. */
static void MapMarksMet(FileMap* m, uint64_t fid, const FileRecord* was, const FileRecord* now)
{
    MarkedMet(&m->slipped, fid, was && was->slip, now && now->slip);
    MarkedMet(&m->showing, fid, was && was->shown_count, now && now->shown_count);
    MarkedMet(&m->carrying, fid, was && was->passport, now && now->passport);
}

static void MapLetGoEntry(FileMap* m, uint64_t key, FileMapEntry* e, FileMapLeftover* left)
{
    if (e->record) {
        m->record_bytes -= e->record->bytes;
        MapMarksMet(m, key, e->record, NULL);
        NameIndexPut(&m->names, key, e->record, NULL);
        left->record = e->record;
    }
    MapRemove(m, key, left);
}

static void LeftoverFree(FileMapLeftover* left)
{
    while (left->nodes) {
        FileMapNode* n = left->nodes;
        left->nodes = n->next;
        kfree(n);
    }
    FileRecordRelease(left->record);
    left->record = NULL;
}

// ----------------------------------------------------------------------------
// Stakes
// ----------------------------------------------------------------------------

/* ‼ A SHORTAGE IS SAID WITH THE MAP'S NUMBERS, once until the heap gives
 * again: which file could not be held, what was asked, and what the map
 * already holds — the heap's own refusal names the class it charged. */
static void MapSayShort(FileMap* m, uint64_t fid, uint32_t need)
{
    TagFSLockTake(&m->lock);
    bool     said  = m->said_short;
    uint32_t files = m->files;
    size_t   held  = (size_t)m->nodes * sizeof(FileMapNode) + (size_t)m->record_bytes;
    m->said_short  = true;
    TagFSLockDrop(&m->lock);
    if (!said)
        kprintf("[TagFS] the index of live files could not take file %llu: %u node(s) of %u "
                "bytes were refused, and it holds %u file(s) in %zu bytes of kernel heap\n",
                fid, need, (unsigned)sizeof(FileMapNode), files, held);
}

static error_t StakeGrow(FileMap* m, FileMapStake* s, uint64_t fid, uint32_t need)
{
    if (need > FILE_MAP_STAKE_MOST)
        need = FILE_MAP_STAKE_MOST;
    while (s->count < need) {
        void* n = kmalloc(sizeof(FileMapNode));
        if (!n) {
            MapSayShort(m, fid, need - s->count);
            return ERR_NO_MEMORY;
        }
        s->node[s->count++] = n;
    }
    if (need && __atomic_load_n(&m->said_short, __ATOMIC_RELAXED)) {
        TagFSLockTake(&m->lock);
        m->said_short = false;
        TagFSLockDrop(&m->lock);
    }
    return OK;
}

/* The name index made room for the file about to be added, with no lock held
 * while the larger table is made: a table that could not be had leaves the
 * chains longer and every answer as exact as it was. */
static void MapNamesGrow(FileMap* m, uint32_t buckets)
{
    NameBucket* table = NameIndexTableMake(buckets);
    if (!table)
        return;
    TagFSLockTake(&m->lock);
    table = NameIndexAdopt(&m->names, table, buckets);
    TagFSLockDrop(&m->lock);
    kfree(table);
}

error_t FileMapStakeFor(FileMap* map, uint64_t fid, FileMapStake* stake)
{
    stake->count = 0;
    TagFSLockTake(&map->lock);
    bool     added   = !MapFind(map, fid);
    uint32_t need    = added ? MapNodesToInsert(map, fid) : 0;
    uint32_t buckets = added ? NameIndexRoom(&map->names, 1) : 0;
    TagFSLockDrop(&map->lock);
    if (buckets)
        MapNamesGrow(map, buckets);
    return StakeGrow(map, stake, fid, need);
}

void FileMapStakeLetGo(FileMapStake* stake)
{
    while (stake->count)
        kfree(stake->node[--stake->count]);
}

/* The file's entry under the map's lock, added from the stake when it is not
 * there; the lock is let go and refused with ERR_NO_MEMORY when the stake
 * could not be made to cover the insert. */
static error_t MapEntryHeld(FileMap* m, uint64_t fid, FileMapStake* s, FileMapEntry** out)
{
    for (;;) {
        TagFSLockTake(&m->lock);
        FileMapEntry* e = MapFind(m, fid);
        if (!e) {
            uint32_t need = MapNodesToInsert(m, fid);
            if (need > s->count) {
                TagFSLockDrop(&m->lock);
                if (StakeGrow(m, s, fid, need) != OK)
                    return ERR_NO_MEMORY;
                continue;
            }
            e = MapInsert(m, fid, s);
        }
        *out = e;
        return OK;
    }
}

// ----------------------------------------------------------------------------
// The map
// ----------------------------------------------------------------------------

FileMap* FileMapCreate(void)
{
    FileMap* map = kmalloc(sizeof(FileMap));
    if (!map)
        return NULL;
    memset(map, 0, sizeof(*map));
    if (!NameIndexOpen(&map->names)) {
        kfree(map);
        return NULL;
    }
    TagFSLockInit(&map->lock, "files", LOCK_RANK_LEAF);
    return map;
}

static void MapFreeNode(FileMapNode* n)
{
    if (n->leaf) {
        for (uint32_t i = 0; i < n->count; i++)
            FileRecordRelease(n->u.entry[i].record);
    } else {
        for (uint32_t i = 0; i <= n->count; i++)
            MapFreeNode(n->u.kid[i]);
    }
    kfree(n);
}

void FileMapDestroy(FileMap* map)
{
    if (!map)
        return;
    if (map->root)
        MapFreeNode(map->root);
    kfree(map->slipped.fid);
    kfree(map->showing.fid);
    kfree(map->carrying.fid);
    NameIndexClose(&map->names);
    kfree(map);
}

error_t FileMapRecordPut(FileMap* map, uint64_t fid, FileRecord* rec,
                         const RecordWhere* where, FileMapStake* stake, RecordWhere* was)
{
    FileMapEntry* e;
    memset(was, 0, sizeof(*was));
    if (MapEntryHeld(map, fid, stake, &e) != OK) {
        FileRecordRelease(rec);
        return ERR_NO_MEMORY;
    }
    FileMapLeftover left = { NULL, e->record };
    if (e->record) {
        map->record_bytes -= e->record->bytes;
        *was = e->where;
    }
    MapMarksMet(map, fid, e->record, rec);
    NameIndexPut(&map->names, fid, e->record, rec);
    map->record_bytes += rec->bytes;
    e->record = rec;
    e->where  = *where;
    TagFSLockDrop(&map->lock);
    LeftoverFree(&left);
    return OK;
}

uint32_t FileMapNodesToAdd(FileMap* map, uint64_t fid)
{
    TagFSLockTake(&map->lock);
    uint32_t need = MapFind(map, fid) ? 0 : MapNodesToInsert(map, fid);
    TagFSLockDrop(&map->lock);
    return need;
}

FileRecord* FileMapRecordHold(FileMap* map, uint64_t fid, RecordWhere* out)
{
    TagFSLockTake(&map->lock);
    FileMapEntry* e   = MapFind(map, fid);
    FileRecord*   rec = e ? e->record : NULL;
    if (rec) {
        __atomic_add_fetch(&rec->refs, 1, __ATOMIC_ACQ_REL);
        if (out)
            *out = e->where;
    }
    TagFSLockDrop(&map->lock);
    return rec;
}

void FileMapDrop(FileMap* map, uint64_t fid, RecordWhere* was)
{
    FileMapLeftover left = { NULL, NULL };
    if (was)
        memset(was, 0, sizeof(*was));
    TagFSLockTake(&map->lock);
    FileMapEntry* e = MapFind(map, fid);
    if (e && e->record && was)
        *was = e->where;
    if (e)
        MapLetGoEntry(map, fid, e, &left);
    TagFSLockDrop(&map->lock);
    LeftoverFree(&left);
}

uint32_t FileMapFiles(FileMap* map, uint64_t from, uint64_t* out, uint32_t room)
{
    uint32_t n = 0, pos = 0;
    TagFSLockTake(&map->lock);
    for (FileMapNode* leaf = MapFrom(map, from, &pos); leaf && n < room; leaf = leaf->next, pos = 0)
        for (; pos < leaf->count && n < room; pos++)
            out[n++] = leaf->key[pos];
    TagFSLockDrop(&map->lock);
    return n;
}

uint32_t FileMapStartingWith(FileMap* map, const uint8_t* bytes, uint32_t length, uint64_t from, uint64_t* out,
                             uint32_t room, uint64_t* next, uint32_t* total)
{
    uint32_t n = 0, pos = 0;
    *next = 0;
    if (total)
        *total = 0;
    TagFSLockTake(&map->lock);
    for (FileMapNode* leaf = MapFrom(map, from, &pos); leaf && !(*next && !total); leaf = leaf->next, pos = 0) {
        for (; pos < leaf->count && !(*next && !total); pos++) {
            const FileRecord* rec = leaf->u.entry[pos].record;
            if (!rec || (rec->flags & (TAGFS_FILE_DELETED | TAGFS_FILE_ARRIVING)) || rec->head_len < length ||
                memcmp(rec->head, bytes, length) != 0)
                continue;
            if (total)
                (*total)++;
            if (n < room)
                out[n++] = leaf->key[pos];
            else if (!*next)
                *next = leaf->key[pos];
        }
    }
    TagFSLockDrop(&map->lock);
    return n;
}

/* One file met by FileMapProgramFiles: counted, written while there is room, and
 * the card it names taken — the least live one, else the least kept deleted. */
static void CarrierMet(uint64_t fid, const FileRecord* rec, uint64_t* out, uint32_t room, uint32_t* n,
                       uint64_t* card, bool* card_live)
{
    bool live = !(rec->flags & TAGFS_FILE_DELETED);
    if (*n < room)
        out[*n] = fid;
    (*n)++;
    if (!*card || (live && !*card_live)) {
        *card      = fid;
        *card_live = live;
    }
}

uint32_t FileMapProgramFiles(FileMap* map, uint64_t owner, uint64_t* out, uint32_t room, uint64_t* card)
{
    uint32_t n    = 0;
    uint64_t best = 0;
    bool     live = false;
    TagFSLockTake(&map->lock);
    if (!(owner & TAGFS_PASSPORT_BIT)) {
        const FileMapEntry* e = owner ? MapFind(map, owner) : NULL;
        if (e && e->record && !e->record->passport)
            CarrierMet(owner, e->record, out, room, &n, &best, &live);
    } else if (!map->carrying.short_of_room) {
        for (uint32_t at = 0; at < map->carrying.count; at++) {
            const FileMapEntry* e = MapFind(map, map->carrying.fid[at]);
            if (e && e->record && e->record->passport == owner)
                CarrierMet(map->carrying.fid[at], e->record, out, room, &n, &best, &live);
        }
    } else {
        uint32_t pos = 0;
        for (FileMapNode* leaf = MapFrom(map, 1, &pos); leaf; leaf = leaf->next, pos = 0)
            for (; pos < leaf->count; pos++) {
                const FileRecord* rec = leaf->u.entry[pos].record;
                if (rec && rec->passport == owner)
                    CarrierMet(leaf->key[pos], rec, out, room, &n, &best, &live);
            }
    }
    TagFSLockDrop(&map->lock);
    if (card)
        *card = best;
    return n;
}

uint32_t FileMapNamed(FileMap* map, const char* word, size_t length, uint64_t from, uint64_t* out,
                      uint32_t room, uint64_t* next)
{
    TagFSLockTake(&map->lock);
    uint32_t n = NameIndexFind(&map->names, word, length, from, out, room, next);
    TagFSLockDrop(&map->lock);
    return n;
}

uint32_t FileMapSlipped(FileMap* map, uint64_t from, uint64_t* out, uint32_t room)
{
    TagFSLockTake(&map->lock);
    uint32_t n = MarkedPage(&map->slipped, from, out, room);
    TagFSLockDrop(&map->lock);
    return n;
}

uint32_t FileMapShowing(FileMap* map, uint64_t from, uint64_t* out, uint32_t room)
{
    TagFSLockTake(&map->lock);
    uint32_t n = MarkedPage(&map->showing, from, out, room);
    TagFSLockDrop(&map->lock);
    return n;
}

uint32_t FileMapCount(FileMap* map)
{
    TagFSLockTake(&map->lock);
    uint32_t files = map->files;
    TagFSLockDrop(&map->lock);
    return files;
}

#ifdef CONFIG_VOLUME_TESTS
uint32_t FileMapNameLinks(FileMap* map)
{
    TagFSLockTake(&map->lock);
    uint32_t links = map->names.links;
    TagFSLockDrop(&map->lock);
    return links;
}
#endif

uint64_t FileMapSlips(const FileMap* map)
{
    return map ? __atomic_load_n(&map->slipped.marked, __ATOMIC_ACQUIRE) : 0;
}

size_t FileMapBytes(FileMap* map)
{
    TagFSLockTake(&map->lock);
    size_t bytes = (size_t)map->nodes * sizeof(FileMapNode) + (size_t)map->record_bytes +
                   (size_t)map->names.buckets * sizeof(NameBucket);
    TagFSLockDrop(&map->lock);
    return bytes;
}
