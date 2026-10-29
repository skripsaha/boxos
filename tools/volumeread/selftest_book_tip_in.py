"""selftest_book_tip_in — the book's landings of a Tally laid in among the
nodes of the one before it (spec_v2 §4.1, §16.5 п.16, TipIn): a second Tally
over the base that lays anew the one leaf of FILES a file renamed changed and
the path above it — TAGS, USE and DIGESTS laid anew besides — and names every
other leaf of FILES as it lies, by the pointer, the separator and the count
the Tally before named it with; and the same Tally laid with a gap, a node
not where a writer lays it. Each a fork of the base selftest_book.py lays;
what volume-read's oracle (binder.Circle) must say of it."""

from . import binder, tally
from .medium import Unreadable
from .ondisk import BLOCK
from .selftest_book_tally import Padded, PaddedLay
from .selftest_cases import Case


def _Headers(node):
    """The files a leaf of FILES counts: its headers."""
    return sum(1 for k, _b in node.items if k[1] == 0)


def _LayGapped(elements, seq, a, b, grid, refs, gap):
    """binder._Lay, with `gap` blocks left between the first node and the
    next: a Tally whose nodes all read, each where its parent names it, and
    one of them not where a writer lays it."""
    nodes, roots, off, first = [], {}, 0, True
    for tree in sorted(elements):
        mine = [r for r in refs if r.tree == tree]
        items = elements[tree]
        counts = [1 if tree != tally.FILES else int(key[1] == 0) for key, _b in items]
        level = 0
        layer = binder.Pack(tree, 0, items, counts, [r.lo for r in mine if r.lo is not None])
        while True:
            for n in layer:
                off = -(-off // grid) * grid
                n.off, n.blocks = off, [a + off, b + off]
                off += n.sheets + (gap if first else 0)
                first = False
                nodes.append(n)
            if len(layer) == 1 and not any(r.level >= level for r in mine):
                roots[tree] = layer[0]
                break
            items, counts = binder._Inner(tree, seq, layer, mine, level)
            level += 1
            layer = binder.Pack(tree, level, items, counts, [r.lo for r in mine if r.level >= level and
                                                             r.lo is not None])
    return nodes, roots, off


def _LaidIn(bt, content, plan, gap=0, tree=tally.FILES):
    """What lays the second Tally over Padded's book: (lay, fid renamed, the
    new name) — laid in among the leaves of `tree`, every one but the last
    named as it lies; FILES's last file renamed, DIGESTS's state as it was.
    Its nodes go past the last of the base's in each Tally-Quire, on an atom —
    copy 2 as far into B as copy 1 into A — and `gap` blocks between its first
    node and the rest."""
    g = bt.ground
    leaves = [n for n in plan.nodes if n.tree == tree and n.level == 0]
    if len(leaves) < 2:
        raise Unreadable("Padded's %s is one leaf: nothing to name as it lies" % tally.TREE_NAMES[tree])
    last = leaves[-1]
    later, fid, name = content, None, None
    if tree == tally.FILES:
        fid = next(k[0] for k, _b in last.items if k[1] == 0)
        name = b"laid-in-" + bytes(content.files[fid].name)[:40]
        later = tally.Content.Copy(content)
        later.files[fid] = content.files[fid].Copy()
        later.files[fid].name = name
    refs = []
    for i, n in enumerate(leaves[:-1]):
        count = _Headers(n) if tree == tally.FILES else len(n.items)
        refs.append(tally.Rotten(tree, n.FirstKey(), leaves[i + 1].FirstKey(), count, 0,
                                 {"seq": plan.seq, "sheets": n.sheets, "blocks": list(n.blocks)}, None))
    elements = binder.Elements(later, plan.slot_seq + 1)
    elements[tree] = [(k, b) for k, b in elements[tree] if k >= last.FirstKey()]
    grid = max(1, g.atom // BLOCK)
    a = -(-(plan.a + plan.Used()) // grid) * grid
    b = plan.b + (a - plan.a)
    seq = plan.slot_seq + 1
    nodes, roots, used = _LayGapped(elements, seq, a, b, grid, refs, gap)
    if used > plan.b - a:
        raise Unreadable("the Tally-Quires of the base hold no room for a Tally laid in past its nodes")
    p2 = binder.BindPlan(later, seq, seq + 1, a, b, plan.ribbon, plan.next, nodes, roots, None)

    def Lay(o):
        PaddedLay(bt, content, plan)(o)
        binder.BindWrite(o, g, p2, index=1)

    return Lay, fid, name


def _Holds(n, key):
    return (n.lo is None or not key < n.lo) and (n.hi is None or key < n.hi)


def _RenameCosts(c, fid):
    """What a Tally laid in after `fid` is renamed is reckoned to cost, one
    rule with the kernel (settle/binder_cost.c): the header's span alone, no
    window of DIGESTS, and the leaf it lies in with the path above it."""
    try:
        from . import dog_ear
    except ImportError:
        return False                         # red until the one rule is volume-read's too
    t = c.mount.tally
    fs = c.mount.files[fid]
    renamed = fs.Copy()
    renamed.name = b"renamed"
    spans = dog_ear.Fresh()
    dog_ear.FileChanged(spans, fid, fs, renamed)
    nodes = t.trees[tally.FILES].nodes
    under = [n for n in nodes if _Holds(n, (fid, 0))]
    return (spans[tally.FILES] == [((fid, 0), (fid, 1))] and not spans[tally.DIGESTS] and
            dog_ear.TipInBlocks(t, spans) == sum(n.sheets for n in under) < sum(n.sheets for n in t.Nodes()))


def _QuireHeld(c):
    """Every Tally-Quire a node of the stood Tally lies in is the Tallies'
    whole — the tail past its last node too — and none of it free (§16.5
    п.17, Р-d3: mount/ground_census.c GroundQuiresHeld, bookcensus.py)."""
    q = c.mount.slot.ribbon["quire_blocks"]
    nodes = [(b, n.sheets) for n in c.mount.tally.Nodes() for b in n.blocks]
    quires = {b // q * q for b, _n in nodes}
    tallied = [r for x in c.claims if x.kind == "tally" for r in x.runs]
    whole = all(any(s <= x < s + n for s, n in tallied) for qs in quires for x in range(qs, qs + q))
    free = any(s < qs + q and qs < s + n for s, n in c.free_runs for qs in quires)
    bare = any(not any(b <= x < b + n for b, n in nodes) for qs in quires for x in range(qs, qs + q))
    return whole and not free and bare


def TipInCases(bt):
    """B0e, B0f: a Tally laid in, bound again byte for byte; and one with a
    node not where a writer lays it, which the oracle names. B0i: one laid in
    among the leaves of DIGESTS, bound again byte for byte. B0g: what the
    next Tally laid in is reckoned to cost after a rename. B0h: a Tally-Quire
    held whole while one node in it lives."""
    content, plan = Padded(bt)
    if sum(1 for n in plan.nodes if n.tree == tally.FILES and n.level == 0) < 2:
        raise Unreadable("Padded's FILES is one leaf: nothing to name as it lies")
    lay, fid, name = _LaidIn(bt, content, plan)
    gapped, _f, _n = _LaidIn(bt, content, plan, gap=1)
    digests, _f, _n = _LaidIn(bt, content, plan, tree=tally.DIGESTS)
    return [
        Case("B0e a Tally laid in among the nodes of the one before it is bound again byte for byte", lay, checks=[
            ("the book mounts on slot 1, the Tally laid in, not under glass",
             lambda c: c.mount.refusal is None and c.mount.slot.index == 1 and not c.mount.glass),
            ("the renamed file reads by its new name, every other as it was",
             lambda c: c.mount.files[fid].name == name and set(c.mount.files) == set(content.files)),
            ("the leaves of FILES it did not change are named as they lie, by the Tally before's seq",
             lambda c: sum(1 for n in c.mount.tally.Nodes() if n.tree == tally.FILES and n.level == 0 and
                           n.seq < c.mount.slot.tally_seq) >= 1),
            ("every node laid anew, both copies, and slot 1 come out the same (binder.Circle)",
             lambda c: binder.Circle(c.medium, c.mount) == [])]),
        Case("B0g a Tally laid in after a rename is reckoned at its leaf and the path above it", lay, checks=[
            ("the leaf of FILES the renamed header lies in and every node over it, no other (dog_ear.py)",
             lambda c: _RenameCosts(c, fid))]),
        Case("B0h a Tally-Quire is held whole while one node in it lives", lay, checks=[
            ("every Quire a node of the Tally stood on lies in is the Tallies' — its first block, its last, the "
             "blocks no node takes among them — and no block of one is free", _QuireHeld)]),
        Case("B0i a Tally laid in among the leaves of DIGESTS is bound again byte for byte", digests, checks=[
            ("the book mounts on slot 1, the Tally laid in, not under glass",
             lambda c: c.mount.refusal is None and c.mount.slot.index == 1 and not c.mount.glass),
            ("the leaves of DIGESTS it did not lay are named as they lie, by the Tally before's seq",
             lambda c: sum(1 for n in c.mount.tally.Nodes() if n.tree == tally.DIGESTS and n.level == 0 and
                           n.seq < c.mount.slot.tally_seq) >= 1),
            ("every node laid anew, both copies, and slot 1 come out the same (binder.Circle)",
             lambda c: binder.Circle(c.medium, c.mount) == [])]),
        Case("B0f a Tally laid in with a node a block past where a writer lays it", gapped, checks=[
            ("the book mounts on slot 1", lambda c: c.mount.refusal is None and c.mount.slot.index == 1),
            ("and the oracle names what differs (binder.Circle)",
             lambda c: binder.Circle(c.medium, c.mount) != [])])]
