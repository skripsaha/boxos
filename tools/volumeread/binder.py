"""binder — the Tally laid down (spec_v2 §2.5, §2.9, §9.3): the one way every
writer of the book lays one (§16.1 п.7, §16.2 п.24; mkfs repeats it byte for
byte): a file's runs cut into Parcels as evenly as they go and none past
PARCEL_STRIPS_MOST Strips, exactly as file_map/tract.c CutPieces/CutLength
cut them; a DIGESTS element a run of blocks inside one window of
DIGEST_ITEM_MOST; leaves filled greedily in key order, a node of more than
one Sheet only for one element longer than a Sheet; the inner levels the
same, bottom up; the trees in the order of their kind, each node on the
boundary of an atom, copy 1 from Tally-Quire A and copy 2 at the same offset
from Tally-Quire B; the ribbon's Quire and its next_quire the grains after —
and refused aloud when that does not fit the data run.

And the circle: a Tally read back, bound again where it lies, held byte for
byte against the medium.
"""

from .bookground import BookGround
from .bookread import EntryPut, PtrPut, VarintPut
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK
from .tally import (DIGESTS, FILES, NODE_HEAD, SHEET_PAYLOAD, TAGS, TREE_NAMES, USE, Node, Root, Slot,
                    DigestsEncode, FileHeadEncode, InnerEncode, ItemBytes, KeyFull, NodeSectors, NodeStream,
                    ParcelEncode, SlotBlock, SlotSectors, TagEncode)

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


def DigestItems(digests):
    """[(first block, body)]: the blocks with a digest, a run of them inside
    one window of DIGEST_ITEM_MOST to an element (§16.2 п.24) — so a change
    to one block rewrites one element, wherever it lies."""
    out, cur = [], []
    for b in sorted(digests):
        if cur and (b != cur[-1] + 1 or b // DIGEST_ITEM_MOST != cur[0] // DIGEST_ITEM_MOST):
            out.append((cur[0], DigestsEncode([digests[x] for x in cur])))
            cur = []
        cur.append(b)
    if cur:
        out.append((cur[0], DigestsEncode([digests[x] for x in cur])))
    return out


def Elements(content, seq):
    """{tree: [(key, body)]} in key order — the leaves' items."""
    files = []
    for fid in sorted(content.files):
        fs = content.files[fid]
        files.append(((fid, 0), FileHeadEncode(fs)))
        at = 0
        for runs in Parcels(fs):
            files.append(((fid, 1 + at), ParcelEncode(runs, seq)))
            at += sum(r.count for r in runs)
    out = {FILES: files, TAGS: [(tid, TagEncode(content.tags[tid])) for tid in sorted(content.tags)],
           USE: [(0, VarintPut(len(content.use)) + content.use)] if content.use else [],
           DIGESTS: DigestItems(content.digests)}
    return {tree: items for tree, items in out.items() if items}


def Pack(tree, level, items, counts):
    """Nodes of `level` filled greedily in key order: an item that would not
    fit the Sheet of the node being filled begins the next; an item longer
    than a Sheet alone is the only one of its node, of as many Sheets as it
    takes (§2.9, §16.2 п.24)."""
    nodes, cur, size, first = [], [], 0, None

    def Head(n):
        return len(EntryPut(NODE_HEAD, VarintPut(tree) + VarintPut(level) + VarintPut(n) +
                            KeyFull(tree, first)))

    def Close():
        nodes.append(Node(tree, level, [it for it, _n in cur], sum(n for _it, n in cur),
                          max(1, -(-size // SHEET_PAYLOAD))))

    for it, n in zip(items, counts):
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


def _Lay(elements, seq, a, b, grid):
    """The nodes in their order of writing, with the copies at a and b, each
    begun on an atom of `grid` blocks (L-9)."""
    nodes, roots, off = [], {}, 0
    for tree in sorted(elements):
        items = elements[tree]
        counts = [1 if tree != FILES else int(key[1] == 0) for key, _b in items]
        level, layer = 0, Pack(tree, 0, items, counts)
        while True:
            for n in layer:
                off = -(-off // grid) * grid
                n.off, n.blocks = off, [a + off, b + off]
                off += n.sheets
                nodes.append(n)
            if len(layer) == 1:
                roots[tree] = layer[0]
                break
            level += 1
            items = [(n.FirstKey(), InnerEncode(n.count, PtrPut(seq, n.sheets, n.blocks))) for n in layer]
            layer = Pack(tree, level, items, [n.count for n in layer])
    return nodes, roots, off


def Bind(content, g, after=None, a=None, b=None, ribbon=None, nxt=None, seq=1, slot_seq=2):
    """The plan of the Tally of `content` (§9.3, §16.2 п.23): from the first
    grain at or past block `after` (or from block `a`), Tally-Quire A of as
    many grains as the nodes take, Tally-Quire B as long right after it, then
    one grain of ribbon and one of its next_quire — refused (ValueError)
    when they would run past the data run; or at `a`, `b`, `ribbon` and `nxt`
    all given, to bind again what was read (Circle)."""
    elements = Elements(content, seq)
    grain, grid = g.grain, max(1, g.atom // BLOCK)
    if b is None:
        a = -(-after // grain) * grain if a is None else a
        grains = 1
        while True:
            nodes, roots, used = _Lay(elements, seq, a, a + grains * grain, grid)
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
        nodes, roots, _used = _Lay(elements, seq, a, b, grid)
    return BindPlan(content, seq, slot_seq, a, b, ribbon, nxt, nodes, roots, after)


def PlanSlot(plan, content, index=0):
    slot = Slot(index, None)
    slot.tally_seq, slot.stitch_seq, slot.stitch_clasp, slot.made_ns = plan.seq, 0, 0, content.made_ns
    for tree, n in plan.roots.items():
        slot.roots[tree] = Root(0, n.level, n.count, PtrPut(plan.seq, n.sheets, n.blocks))
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


def Circle(medium, mount):
    """Bind again what the mount read of its slot's Tally, where it lies, and
    hold every sector against the medium: [what differs] — empty when the
    read and the binder agree byte for byte (§2.4: one value, one spelling)."""
    g, slot, tally = BookGround(medium), mount.slot, mount.tally
    nodes = tally.Nodes()
    if not nodes:
        return ["the Tally has no node to bind again"]
    a = min(n.blocks[0] for n in nodes)
    b = min(n.blocks[1] for n in nodes if len(n.blocks) > 1)
    r = slot.ribbon
    plan = Bind(tally.content, g, a=a, b=b, ribbon=(r["quire_block"], r["quire_blocks"]),
                nxt=(r["next_block"], r["next_blocks"]), seq=slot.tally_seq, slot_seq=slot.seq)
    wrong = []
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
    if len(plan.nodes) != len(nodes):
        wrong.append("%u nodes bound against %u read" % (len(plan.nodes), len(nodes)))
    return wrong
