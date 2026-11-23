"""binder — the Tally laid down: the one way every writer of the book lays
one (mkfs repeats it byte for byte): a file's runs cut into Parcels as evenly
as they go and none past
PARCEL_STRIPS_MOST Strips, exactly as file_map/tract.c CutPieces/CutLength
cut them; a DIGESTS element a run of blocks inside one window of
DIGEST_ITEM_MOST; leaves filled greedily in key order, a node of more than
one Sheet only for one element longer than a Sheet; the inner levels the
same, bottom up; the trees in the order of their kind, each node on the
boundary of an atom, copy 1 from Tally-Quire A and copy 2 at the same offset
from Tally-Quire B; the ribbon's Quire and its next_quire the grains after —
and refused aloud when that does not fit the data run.

And a node no copy of which reads, named as it lies (§16.4 п.5, the kernel's
settle/binder_nodes.c alike): a rotten node of level L is an element of level
L+1 — its lower separator (the least key for a tree's first node), its old
count, its old pointer — and no node of level L or below holds keys on both
sides of it, the node being filled closed before its first key; an element
whose key lies inside its range is left out (none of FILES may — a file born
since has an id past every one the Tally knew — and DIGESTS is cut at it), and
a range with no end of its own ends at the next element's key.

And the circle: a Tally read back, bound again where it lies, held byte for
byte against the medium.
"""

import bisect

from .bookground import BookGround
from .bookread import EntryPut, Integrity, PtrPut, SeedLanes, VarintPut
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK
from .stitch import MEDIUM_LIED
from .tally import (DIGESTS, FILES, NODE_HEAD, SHEET_PAYLOAD, SPINE_MEDIUM_LIED, TAGS, TREE_NAMES, USE, Node,
                    Root, Rotten, Slot, Tally, DigestsEncode, FileHeadEncode, InnerEncode, ItemBytes, KeyFull,
                    NodeSectors, NodeStream, ParcelEncode, RunShared, SlotBlock, SlotSectors, TagEncode)

PARCEL_STRIPS_MOST = 128             # volume_tally.h PARCEL_STRIPS_MOST

# The most digests one DIGESTS element carries: the largest n for which a
# NodeHead and one element fit one Sheet whatever the numbers are. NodeHead
# of a leaf of one element: tag 1 + len 1 + tree 1 + level 1 + items 1 +
# first key 10 = 15 bytes. The element: len 2 + key 1 (the first element's
# key is written as its distance from the NodeHead's, 0) + n 2 + 8n. 15 + 5 +
# 8n <= 3904 holds to n = 485 (3900 bytes) and not at 486 (3908).
DIGEST_ITEM_MOST = 485               # volume_tally.h DIGEST_ITEM_MOST


def NodeWrite(over, g, seq, blocks, sheets, tree, level, items):
    """A node laid at every block of `blocks`, sealed at `seq`, in `sheets`
    Sheets — refused when its items no longer fit them."""
    stream, starts = NodeStream(tree, level, items)
    if len(stream) > sheets * SHEET_PAYLOAD:
        raise ValueError("the node's %u bytes no longer fit %u Sheet(s)" % (len(stream), sheets))
    for b in blocks:
        over.Put(g.base + b * SECTORS_PER_BLOCK, b"".join(NodeSectors(g, seq, b, sheets, stream, starts)))


def ReadPlan(t):
    """What a Tally `t` was read as, in the shape a binder's plan has: the
    nodes where they lie, the roots, the Tally-Quires from the first copy of
    each, the ribbon's Quire and next as the slot names them."""
    nodes = t.Nodes()
    a = min(n.blocks[0] for n in nodes)
    b = min(n.blocks[1] for n in nodes if len(n.blocks) > 1)
    r = t.slot.ribbon
    roots = {tree: next(n for n in t.nodes if n.lo is None) for tree, t in t.trees.items() if t.nodes}
    return BindPlan(t.content, t.seq, t.slot.seq, a, b, (r["quire_block"], r["quire_blocks"]),
                    (r["next_block"], r["next_blocks"]), sorted(nodes, key=lambda n: n.blocks[0]), roots,
                    None)


def CutPieces(count, most):
    """file_map/tract.c CutPieces: how many pieces `count` things are cut
    into, at most `most` each, as even as they come."""
    return (count + most - 1) // most


def CutLength(count, pieces, i):
    """file_map/tract.c CutLength: how many of them piece `i` holds."""
    return count // pieces + (1 if i < count % pieces else 0)


def Parcels(fs):
    """The file's Parcels: as read, when it was read off a Tally and its runs
    have not changed since (a Parcel of memory is a Parcel of the medium,
    §16.1 п.7); else its runs cut as evenly as they go."""
    if fs.parcels is not None and [r for p in fs.parcels for r in p] == fs.runs:
        return fs.parcels
    pieces, out, at = CutPieces(len(fs.runs), PARCEL_STRIPS_MOST), [], 0
    for i in range(pieces):
        n = CutLength(len(fs.runs), pieces, i)
        out.append(fs.runs[at:at + n])
        at += n
    return out


class Kept(Rotten):
    """A node of the Tally before a Tally names as it lies because nothing
    under it changed (TipIn, §16.5 п.16; settle/binder_kept.c): by its
    pointer, the separator and the count its parent gave it. Its range runs to
    its upper separator, or to the tree's end with none — a key past it would
    have changed it."""

    __slots__ = ()


def KeptOf(node):
    """The node read, as a Tally names it as it lies."""
    return Kept(node.tree, node.lo, node.hi, node.count, node.level,
                {"seq": node.seq, "sheets": node.sheets, "blocks": list(node.blocks)}, None)


def RefLo(r):
    """The key a rotten node is named by: its lower separator, or the least
    key for a tree's first node."""
    if r.lo is not None:
        return r.lo
    return (0, 0) if r.tree == FILES else 0


def RefCovers(r, key):
    """Whether `key` lies inside rotten node r's range: from its lower
    separator up to its upper one — or, with none, at the lower one alone."""
    if r.lo is not None and key < r.lo:
        return False
    if r.hi is not None:
        return key < r.hi
    if isinstance(r, Kept):
        return True
    return r.lo is not None and key == r.lo


def DigestItems(digests, refs=()):
    """[(first block, body)]: the blocks with a digest, a run of them inside
    one window of DIGEST_ITEM_MOST to an element (§16.2 п.24) — so a change
    to one block rewrites one element, wherever it lies; none inside a rotten
    range named as it lies, an element cut before one."""
    refs = [r for r in refs if r.tree == DIGESTS]
    los = sorted(r.lo for r in refs if r.lo is not None)
    out, cur = [], []
    for b in sorted(digests):
        if any(RefCovers(r, b) for r in refs):
            continue
        if cur and (b != cur[-1] + 1 or b // DIGEST_ITEM_MOST != cur[0] // DIGEST_ITEM_MOST or
                    any(cur[-1] < lo <= b for lo in los)):
            out.append((cur[0], DigestsEncode([digests[x] for x in cur])))
            cur = []
        cur.append(b)
    if cur:
        out.append((cur[0], DigestsEncode([digests[x] for x in cur])))
    return out


def Elements(content, seq, refs=()):
    """{tree: [(key, body)]} in key order — the leaves' items, but the ones a
    node named as it lies because nothing under it changed holds; and a tree
    that holds nothing but nodes named as they lie, there with none."""
    files = []
    kept = getattr(content, "kept", {})
    for fid in sorted(set(content.files) | set(kept)):
        if fid in kept:
            # A file locked by name, named again as what of it read lay — each
            # run with the birth it had (§16.4 п.5, settle/binder_items.c).
            head, parts = kept[fid]
            if head is not None:
                files.append(((fid, 0), FileHeadEncode(head)))
            files.extend(((fid, 1 + b), ParcelEncode(runs, seq)) for b, runs in parts)
            continue
        fs = content.files[fid]
        files.append(((fid, 0), FileHeadEncode(fs)))
        at = 0
        for runs in Parcels(fs):
            files.append(((fid, 1 + at), ParcelEncode(runs, seq)))
            at += sum(r.count for r in runs)
    out = {FILES: files, TAGS: [(tid, TagEncode(content.tags[tid])) for tid in sorted(content.tags)],
           USE: ([(0, VarintPut(len(content.use)) + content.use)] if content.use else [])
                + ([(1, VarintPut(len(content.name)) + content.name)] if content.name else []),
           DIGESTS: DigestItems(content.digests, refs)}
    kept = [r for r in refs if isinstance(r, Kept)]
    for tree in out:
        mine = [r for r in kept if r.tree == tree]
        if mine:
            out[tree] = [(key, b) for key, b in out[tree] if not any(RefCovers(r, key) for r in mine)]
    for tree, items in out.items():
        for key, _b in items:
            if tree == FILES and any(r.tree == FILES and not isinstance(r, Kept) and RefCovers(r, key) for r in refs):
                raise ValueError("the FILES element %r lies inside a rotten range named as it lies" % (key,))
    return {tree: items for tree, items in out.items() if items or any(r.tree == tree for r in refs)}


def Pack(tree, level, items, counts, bars=()):
    """Nodes of `level` filled greedily in key order: an item that would not
    fit the Sheet of the node being filled begins the next; an item longer
    than a Sheet alone is the only one of its node, of as many Sheets as it
    takes (§2.9, §16.2 п.24) — and a node is closed before the first key `bars`
    names that it would hold keys on both sides of (a rotten node of this
    level or above, named as it lies)."""
    nodes, cur, size, first = [], [], 0, None

    def Head(n):
        return len(EntryPut(NODE_HEAD, VarintPut(tree) + VarintPut(level) + VarintPut(n) +
                            KeyFull(tree, first)))

    def Close():
        nodes.append(Node(tree, level, [it for it, _n in cur], sum(n for _it, n in cur),
                          max(1, -(-size // SHEET_PAYLOAD))))

    for it, n in zip(items, counts):
        if cur and any(cur[-1][0][0] < lo <= it[0] for lo in bars):
            Close()
            cur = []
        if cur:
            grown = size + len(ItemBytes(tree, it, cur[-1][0][0])) + Head(len(cur) + 1) - Head(len(cur))
            if grown <= SHEET_PAYLOAD:
                cur.append((it, n))
                size = grown
                continue
            Close()
        cur, first = [(it, n)], it[0]
        size = Head(1) + len(ItemBytes(tree, it, first))
        if size > SHEET_PAYLOAD:
            Close()
            cur = []
    if cur:
        Close()
    return nodes


class BindPlan:
    """Where a Tally goes: its content and seq, the nodes in the order they
    are written (each with its blocks), the roots, the first blocks of the two
    Tally-Quires, the ribbon's Quire and its next_quire."""

    def __init__(self, content, seq, slot_seq, a, b, ribbon, nxt, nodes, roots, after):
        self.content, self.seq, self.slot_seq, self.a, self.b = content, seq, slot_seq, a, b
        self.ribbon, self.next, self.nodes, self.roots, self.after = ribbon, nxt, nodes, roots, after

    def Used(self):
        """The blocks of Tally-Quire A from its first to past its last node."""
        return max((n.blocks[0] + n.sheets for n in self.nodes), default=self.a) - self.a

    def FreeSheet(self):
        """[copy 1, copy 2] of the first Sheet past the last node of each
        Tally-Quire — room a landing may put one node of its own in."""
        if self.Used() >= self.b - self.a:
            raise Unreadable("the Tally-Quires have no Sheet left")
        return [self.a + self.Used(), self.b + self.Used()]


def _Inner(tree, seq, layer, refs, level):
    """The elements of `level` + 1: each node of `layer`, and each rotten node
    of `level` named as it lies, in key order — with their counts."""
    named = [(n.FirstKey(), InnerEncode(n.count, PtrPut(seq, n.sheets, n.blocks)), n.count) for n in layer]
    named += [(RefLo(r), InnerEncode(r.count, PtrPut(r.ptr["seq"], r.ptr["sheets"], r.ptr["blocks"])), r.count)
              for r in refs if r.tree == tree and r.level == level]
    named.sort(key=lambda e: e[0])
    return [(k, body) for k, body, _c in named], [c for _k, _b, c in named]


def _Lay(elements, seq, a, b, grid, refs=()):
    """The nodes in their order of writing, with the copies at a and b, each
    begun on an atom of `grid` blocks (L-9); rotten nodes named as they lie."""
    nodes, roots, off = [], {}, 0
    refs = [r for r in refs if r.ptr]
    for tree in sorted(elements):
        items = elements[tree]
        counts = [1 if tree != FILES else int(key[1] == 0) for key, _b in items]
        mine = [r for r in refs if r.tree == tree]
        whole = [r for r in mine if isinstance(r, Kept) and r.lo is None and r.hi is None]
        if whole:
            # A tree nothing in changed: its root named as it lies, no node laid.
            r = whole[0]
            roots[tree] = Node(tree, r.level, [], r.count, r.ptr["sheets"], r.ptr["blocks"], r.ptr["seq"])
            continue

        def Bars(level):
            return [r.lo for r in mine if r.level >= level and r.lo is not None]

        level, layer = 0, Pack(tree, 0, items, counts, Bars(0))
        while True:
            for n in layer:
                off = -(-off // grid) * grid
                n.off, n.blocks = off, [a + off, b + off]
                off += n.sheets
                nodes.append(n)
            above = any(r.level >= level for r in mine)
            if len(layer) == 1 and not above:
                roots[tree] = layer[0]
                break
            if not layer and not above:
                break
            items, counts = _Inner(tree, seq, layer, mine, level)
            level += 1
            layer = Pack(tree, level, items, counts, Bars(level))
    return nodes, roots, off


def Bind(content, g, after=None, a=None, b=None, ribbon=None, nxt=None, seq=1, slot_seq=2, refs=()):
    """The plan of the Tally of `content` (§9.3, §16.2 п.23): from the first
    grain at or past block `after` (or from block `a`), Tally-Quire A of as
    many grains as the nodes take, Tally-Quire B as long right after it, then
    one grain of ribbon and one of its next_quire — refused (ValueError)
    when they would run past the data run; or at `a`, `b`, `ribbon` and `nxt`
    all given, to bind again what was read (Circle)."""
    refs = [r for r in refs if r.ptr]
    elements = Elements(content, seq, refs)
    grain, grid = g.grain, max(1, g.atom // BLOCK)
    if b is None:
        a = -(-after // grain) * grain if a is None else a
        grains = 1
        while True:
            nodes, roots, used = _Lay(elements, seq, a, a + grains * grain, grid, refs)
            if used <= grains * grain:
                break
            grains = -(-used // grain)
        b = a + grains * grain
        ribbon = (b + grains * grain, grain)
        nxt = (ribbon[0] + grain, grain)
        if nxt[0] + nxt[1] > g.data_end:
            raise ValueError("the book does not fit the data run: its Tally-Quires from block %u, its ribbon "
                             "and next_quire run to block %u, past the data run's end at block %u (a writer "
                             "refuses so; mkfs as well)" % (a, nxt[0] + nxt[1], g.data_end))
    else:
        nodes, roots, _used = _Lay(elements, seq, a, b, grid, refs)
    return BindPlan(content, seq, slot_seq, a, b, ribbon, nxt, nodes, roots, after)


def PlanSlot(plan, content, index=0):
    slot = Slot(index, None)
    slot.tally_seq, slot.stitch_seq, slot.stitch_clasp, slot.made_ns = plan.seq, 0, 0, content.made_ns
    for tree, n in plan.roots.items():
        slot.roots[tree] = Root(0, n.level, n.count, PtrPut(n.seq or plan.seq, n.sheets, n.blocks))
    slot.ribbon = dict(at=plan.ribbon[0], quire_block=plan.ribbon[0], quire_blocks=plan.ribbon[1],
                       next_block=plan.next[0], next_blocks=plan.next[1])
    slot.counts = dict(next_fid=content.next_fid, next_tag=content.next_tag,
                       next_bookmark=content.next_bookmark)
    return slot


def BindWrite(over, g, plan, index=0):
    """The plan laid over `over`: every node in both copies, the ribbon's
    Quire and its next_quire blank, and slot `index` at the plan's slot seq."""
    for n in plan.nodes:
        stream, starts = NodeStream(n.tree, n.level, n.items)
        for blk in n.blocks:
            over.Put(g.base + blk * SECTORS_PER_BLOCK,
                     b"".join(NodeSectors(g, plan.seq, blk, n.sheets, stream, starts)))
    for start, blocks in (plan.ribbon, plan.next):
        over.Put(g.base + start * SECTORS_PER_BLOCK, bytes(blocks * BLOCK))
    slot = PlanSlot(plan, plan.content, index)
    over.Put(g.base + SlotBlock(g, index) * SECTORS_PER_BLOCK,
             b"".join(SlotSectors(g, index, plan.slot_seq, slot)))


def KeptRefs(read, seq):
    """The nodes of the Tally read that a Tally of `seq` names as they lie:
    those sealed before it whose parent it laid — or its root, a tree nothing
    in changed — in key order; none under another of them."""
    out = []
    for tree, t in sorted(read.trees.items()):
        old = sorted((n for n in t.nodes if n.seq != seq and n.seq),
                     key=lambda n: ((0,) if n.lo is None else (1, n.lo), -n.level))
        for n in old:
            k = KeptOf(n)
            if not (out and _Inside(k, out[-1])):
                out.append(k)
    return out


def _Inside(r, k):
    """Whether node r lies under k, a node named as it lies."""
    return r.tree == k.tree and r.level < k.level and (k.lo is None or (r.lo is not None and k.lo <= r.lo)) and \
        (k.hi is None or (r.hi is not None and r.hi <= k.hi))


def KeptNamed(mount, kept):
    """[what differs]: each node named as it lies named so by a Tally before —
    a slot of an older seq whose Tally holds it at its level and on its lower
    separator."""
    older = [s for s in mount.slots if s.state == "valid" and s.seq < mount.slot.seq]
    seen = set()
    for s in older:
        try:
            for n in Tally(mount.g, s).Nodes():
                seen.add((n.tree, n.level, tuple(n.blocks), n.lo))
        except Unreadable:
            continue
    return ["the %s node of level %u at block %u is named as it lies, and no Tally before names it on that range"
            % (TREE_NAMES[r.tree], r.level, r.ptr["blocks"][0]) for r in kept
            if (r.tree, r.level, tuple(r.ptr["blocks"]), r.lo) not in seen]


def Circle(medium, mount):
    """Bind again what the mount read of its slot's Tally, where it lies, and
    hold every sector against the medium: [what differs] — empty when the
    read and the binder agree byte for byte (§2.4: one value, one spelling).
    A Tally laid in among the nodes of the one before it (TipIn, §16.5 п.16):
    the nodes it laid bound again byte for byte, in their order from the first
    of them, the ones it names as they lie as references — each named by a
    Tally before on that range."""
    g, slot, tally = mount.g, mount.slot, mount.tally
    nodes = tally.Nodes()
    if not nodes:
        return ["the Tally has no node to bind again"]
    kept = KeptRefs(tally, slot.tally_seq)
    fresh = [n for n in nodes if n.seq == slot.tally_seq]
    if not fresh:
        return KeptNamed(mount, kept)
    a = min(n.blocks[0] for n in fresh)
    b = min(n.blocks[1] for n in fresh if len(n.blocks) > 1)
    r = slot.ribbon
    refs = [x for x in tally.rotten if x.ptr and not any(_Inside(x, k) for k in kept)] + kept
    plan = Bind(tally.content, g, a=a, b=b, ribbon=(r["quire_block"], r["quire_blocks"]),
                nxt=(r["next_block"], r["next_blocks"]), seq=slot.tally_seq, slot_seq=slot.seq, refs=refs)
    wrong = KeptNamed(mount, kept)
    for n in plan.nodes:
        stream, starts = NodeStream(n.tree, n.level, n.items)
        for blk in n.blocks:
            want = b"".join(NodeSectors(g, plan.seq, blk, n.sheets, stream, starts))
            have = g.medium.sectors(g.base + blk * SECTORS_PER_BLOCK, n.sheets * SECTORS_PER_BLOCK)
            if want != have:
                wrong.append("the %s node of level %u at block %u" % (TREE_NAMES[n.tree], n.level, blk))
    mine = PlanSlot(plan, tally.content, slot.index)
    mine.flags = slot.flags
    mine.ribbon = dict(slot.ribbon)
    mine.stitch_seq, mine.stitch_clasp = slot.stitch_seq, slot.stitch_clasp
    want = b"".join(SlotSectors(g, slot.index, slot.seq, mine))
    if want != g.medium.sectors(g.base + slot.block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK):
        wrong.append("slot %u" % slot.index)
    if len(plan.nodes) != len(fresh):
        wrong.append("%u nodes bound against %u laid" % (len(plan.nodes), len(fresh)))
    return wrong


def _RunsKey(fs):
    """A file's runs as the book says them, whatever Parcels they were cut
    into: a hole's length, or ground from its start with its birth and
    whether it is SHARED — runs that go on one another of one kind and one
    birth taken as one."""
    out = []
    for r in fs.runs:
        birth, kind = 0 if r.hole else r.birth, 1 if r.hole else 2 if RunShared(r) else 0
        if out and out[-1][0] == kind and out[-1][3] == birth and (r.hole or out[-1][1] + out[-1][2] == r.start):
            out[-1][2] += r.count
            continue
        out.append([kind, 0 if r.hole else r.start, r.count, birth])
    return out


def _FilesDiffer(have, want):
    wrong = []
    for fid in sorted(set(have) | set(want)):
        a, b = have.get(fid), want.get(fid)
        if a is None or b is None:
            wrong.append("fid %u is %s the Tally and %s the state it is of" % (fid, "in" if a else "not in",
                                                                               "in" if b else "not in"))
            continue
        ha = (a.gen, a.flags, a.size, a.created_ns, a.modified_ns, a.name, sorted(a.tags))
        hb = (b.gen, b.flags, b.size, b.created_ns, b.modified_ns, b.name, sorted(b.tags))
        if ha != hb:
            wrong.append("fid %u's header is %r in the Tally and %r in the state it is of" % (fid, ha, hb))
        if _RunsKey(a) != _RunsKey(b):
            wrong.append("fid %u's runs are %r in the Tally and %r in the state it is of" % (fid, _RunsKey(a)[:6],
                                                                                           _RunsKey(b)[:6]))
    return wrong


def _LetGo(m, p):
    """The rotten ranges of the Tally the state `p` stands on that the newest
    Tally (`m`'s) names no rotten node on — let go by Escheat, or laid anew:
    a Tally may leave out a range the slot before named rotten (§16.5 п.20)."""
    named = {(r.tree, r.lo) for r in m.tally.rotten}
    return [r for r in p.tally.rotten if (r.tree, r.lo) not in named]


def _LockedKept(p, gone):
    """The files `p` locks that no range let go (`gone`) touches."""
    return {fid for fid in p.locked if not any(r.tree == FILES and r.Touches((fid, 0), (fid + 1, 0))
                                               for r in gone)}


def _DigestsWanted(p, gone=(), g=None):
    """The digests a Tally of the state `p` mounted carries (the kernel's
    settle/binder_slice.c BinderGroundTake): of every block of every live file, of every
    block a locked file is known to name — the census holds those whatever
    locked it (bookcensus.py _Held) — and, a FILES range rotten, of every
    block the Tally before named a digest of and none of its living files
    owns. What is held keeps its digest, and stays held — but what a range
    let go (`gone`, _LetGo) held: its files are lost and their blocks with
    them; and a DIGESTS range let go is laid anew of the data (§16.5 п.19,
    the kernel's settle/escheat.c EscheatDigestOf): a block of it the ribbon
    gives no digest of carries its data's, read off the ground `g` — none
    when it does not read."""
    old, fresh = p.tally.content.digests, p.overlay
    anew = [r for r in gone if r.tree == DIGESTS]
    lanes = SeedLanes(g.uuid) if anew else None
    kept = _LockedKept(p, gone)
    blocks = {b for fs in p.files.values() for s, n in fs.Ground() for b in range(s, s + n)}
    blocks |= {r.start + i for fid, runs in p.locked_runs.items() if fid in kept for r in runs if not r.hole
               for i in range(r.count)}
    if any(r.tree == FILES for r in p.tally.rotten if r not in gone):
        owned = {b for fs in p.tally.content.files.values() for s, n in fs.Ground() for b in range(s, s + n)}
        blocks |= set(old) - owned
    out = {}
    for b in blocks:
        d = fresh.get(b)
        if d is None and any(r.Holds(b) for r in anew):
            raw = g.Block(b)
            d = None if raw is None else Integrity(raw, lanes)
        elif d is None:
            d = old.get(b)
        if d is not None:
            out[b] = d
    return out


def _RibbonPast(p, s0, k):
    """Past stitch k, as the walk from the older slot `s0` keeps it
    (stitch_ribbon.py Ribbon._Take): k itself, the Quire the ribbon stands in
    where it goes on, the next_quire named there — (0, 0) for none — and
    whether the medium was known to lie by then: the older slot's word or a
    stitch's up to k."""
    r = s0.ribbon
    quire, nxt = (r["quire_block"], r["quire_blocks"]), (r["next_block"], r["next_blocks"])
    lied = bool(s0.flags & SPINE_MEDIUM_LIED)
    for u in p.ribbon.stitches:
        if u.places[-1].quire != quire:
            quire, nxt = u.places[-1].quire, (0, 0)
        if u.head is not None:
            nxt = u.head.next if u.head.next[1] else nxt
            lied = lied or bool(u.head.flags & MEDIUM_LIED)
        if u.seq == k:
            return u, (u.end.quire if u.end is not None else quire), nxt, lied
    return None, None, None, lied


def _SlotPast(s1, p, s0, k):
    """[what differs] between the newest slot's SpineHead and SpineRibbon and
    the ribbon past stitch k: its Clasp, the Quire `at` lies in and the
    next_quire named there; and MEDIUM_LIED, which a slot never leaves off
    once the medium was known to lie (a later lie may set it besides)."""
    u, quire, nxt, lied = _RibbonPast(p, s0, k)
    if u is None:
        return []
    wrong, r = [], s1.ribbon
    if u.clasp != s1.stitch_clasp:
        wrong.append("the slot names stitch %u's Clasp 0x%08x, and the stitch carries 0x%08x" %
                     (k, s1.stitch_clasp, u.clasp or 0))
    if (r["quire_block"], r["quire_blocks"]) != tuple(quire) or (r["next_block"], r["next_blocks"]) != tuple(nxt):
        wrong.append("the slot's SpineRibbon names Quire %u+%u and next %u+%u, and the ribbon past stitch %u "
                     "stands in %u+%u with next %u+%u" % (r["quire_block"], r["quire_blocks"], r["next_block"],
                                                          r["next_blocks"], k, quire[0], quire[1], nxt[0], nxt[1]))
    if lied and not s1.flags & SPINE_MEDIUM_LIED:
        wrong.append("the medium was known to lie by stitch %u, and the slot leaves MEDIUM_LIED off" % k)
    return wrong


def _AtOf(p, k):
    """The block the ribbon goes on at past stitch k, as the walk reads it."""
    for u in p.ribbon.stitches:
        if u.seq == k and u.end is not None and u.end.off < u.end.quire[1] * SECTORS_PER_BLOCK:
            return u.end.quire[0] + u.end.off // SECTORS_PER_BLOCK
    return None


def Useful(u):
    """The useful bytes of stitch u (§4.2): its records from its StitchHead to
    its Clasp's end — not the sectors' heads, not the PAD."""
    return u.parsed.end if u.parsed and u.parsed.why is None else 0


def Lives(files, tags, use, held=(), name=b""):
    """Whether a key of the state of `files`, `tags`, `use` and `name` lies in the
    range [lo, hi) of a leaf of a tree — lo None from its first key, hi None
    to its end (settle/binder_live.c, §16.6 п.6): of FILES, a live file's id
    among the ids the leaf holds keys of, its upper key's file too when it
    ends inside it; of TAGS, a tag's id; of USE, a Use Context or a volume's
    name; of DIGESTS, a
    block of a live file's ground or of `held` — runs a locked file is known
    to name, held for what rotted — as DIGESTS speaks for both
    (_DigestsWanted). The kernel tells the last by its block map, past the
    book's own ground and what rests in Fallow: the same blocks at a quiet
    point."""
    ground = sorted([r for fs in files.values() for r in fs.Ground()] + list(held))
    starts = [s for s, _n in ground]
    reach, most = [], 0
    for s, n in ground:
        most = max(most, s + n)
        reach.append(most)
    fids = sorted(files)
    tids = sorted(tags)

    def Within(keys, lo, hi):
        i = bisect.bisect_left(keys, lo)
        return i < len(keys) and (hi is None or keys[i] < hi)

    def Live(tree, lo, hi):
        if tree == FILES:
            return Within(fids, lo[0] if lo is not None else 0,
                          None if hi is None else hi[0] + 1 if hi[1] else hi[0])
        if tree == TAGS:
            return Within(tids, lo if lo is not None else 0, hi)
        if tree == USE:
            return bool(use) or bool(name)
        if tree == DIGESTS:
            i = bisect.bisect_left(starts, hi) if hi is not None else len(starts)
            return i > 0 and reach[i - 1] > (lo if lo is not None else 0)
        return True
    return Live


def BindSaid(m):
    """What a reader of the newest slot says of its Tally against TALLY_SLACK:
    (the blocks its Tally's whole book takes a copy, the bytes the next is
    reckoned to cost, the bytes a whole Tally of it would, the useful bytes of
    the ribbon past its `at`). The next is reckoned laid in — the leaves the
    stitches past `at` changed, each at what is left live in it, and their
    path, both copies, and the slot — never more than the whole book
    (settle/binder.c TallyCostBytes, settle/binder_cost.c; dog_ear.py)."""
    from .dog_ear import TipInBlocks
    blocks = sum(n.sheets for n in m.tally.Nodes())
    whole = (2 * blocks + 1) * BLOCK
    held = [(r.start, r.count) for fid, runs in m.locked_runs.items() if fid in m.locked for r in runs
            if not r.hole]
    tip = TipInBlocks(m.tally, m.dog_ear, Lives(m.files, m.tags, m.use, held, m.name))
    cost = (2 * tip + 1) * BLOCK if tip < blocks else whole
    return blocks, cost, whole, sum(Useful(u) for u in m.ribbon.stitches)


def BindCheck(medium, start=None):
    """[what differs], empty when the newest slot's Tally is what a writer of
    one lays (§16.1 п.7): bound again where it lies, byte for byte (Circle);
    and, for its state, the state an older slot's Tally and its ribbon replayed
    to the stitch the newest slot names hold — files, their runs and births,
    tags, the Use Context, the digests, the numbers given next and where the
    ribbon goes on."""
    from .bookmount import BookMount
    g = BookGround(medium, start)
    m = BookMount(g)
    if m.refusal is not None:
        return ["the book does not mount: %s" % m.refusal]
    wrong = Circle(medium, m)
    s1, k = m.slot, m.slot.stitch_seq
    for s0 in sorted((s for s in m.slots if s.state == "valid" and s.seq < s1.seq), key=lambda s: -s.seq):
        p = BookMount(g, stand=s0.index, upto=k)
        if p.refusal is not None or p.tally is None or (k != s0.stitch_seq and k not in p.applied):
            continue
        t = m.tally.content
        wrong += _FilesDiffer(t.files, p.files)
        gone = _LetGo(m, p)
        if set(m.tally.locked) != _LockedKept(p, gone):
            wrong.append("the Tally locks fids %s and the state it is of %s" % (sorted(m.tally.locked)[:8],
                                                                               sorted(_LockedKept(p, gone))[:8]))
        tags = {i: (x.flags, x.key, x.value) for i, x in t.tags.items()}
        want = {i: (x.flags, x.key, x.value) for i, x in p.tags.items()}
        if tags != want:
            wrong.append("the Tally's tags %s and the state's %s differ" % (
                sorted(set(tags) ^ set(want))[:8], [i for i in tags if i in want and tags[i] != want[i]][:8]))
        if t.use != p.use:
            wrong.append("the Tally's Use Context is %u byte(s), the state's %u" % (len(t.use), len(p.use)))
        if t.name != p.name:
            wrong.append("the Tally's volume name is %u byte(s), the state's %u" % (len(t.name), len(p.name)))
        dw = _DigestsWanted(p, gone, g)
        odd = sorted(b for b in set(t.digests) | set(dw) if t.digests.get(b) != dw.get(b))
        if odd:
            wrong.append("the Tally's DIGESTS and the state's differ at %u block(s), first %s" % (len(odd), odd[:6]))
        counts = {x: s1.counts[x] for x in ("next_fid", "next_tag", "next_bookmark")}
        mine = {x: p.counts[x] for x in ("next_fid", "next_tag", "next_bookmark")}
        if counts != mine:
            wrong.append("the slot's counts %r and the state's %r differ" % (counts, mine))
        at = _AtOf(p, k) if k != s0.stitch_seq else None
        if at is not None and at != s1.ribbon["at"]:
            wrong.append("the slot puts the ribbon on at block %u, and stitch %u ends at %u" % (s1.ribbon["at"], k, at))
        if k != s0.stitch_seq:
            wrong += _SlotPast(s1, p, s0, k)
        return wrong
    return wrong + ["no older slot's ribbon carries stitch %u, the one the newest slot's Tally is the state "
                    "after" % k]
