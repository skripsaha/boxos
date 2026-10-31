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
    try:
        cost = dog_ear.TipInBlocks(t, spans, binder.Lives(c.mount.files, c.mount.tags, c.mount.use))
    except (AttributeError, TypeError):
        return False                         # red until a leaf costs what is left live in it
    return (spans[tally.FILES] == [((fid, 0), (fid, 1))] and not spans[tally.DIGESTS] and
            cost == sum(n.sheets for n in under) < sum(n.sheets for n in t.Nodes()))


def _FidsOf(n):
    """The file ids a leaf of FILES holds keys of: from its lower key's — the
    tree's first from none — to its upper key's, that file too when the leaf
    ends inside it (settle/binder_kept.c LeafFids)."""
    lo = n.lo[0] if n.lo is not None else 0
    hi = None if n.hi is None else n.hi[0] + 1 if n.hi[1] else n.hi[0]
    return lo, hi


def _Within(x, lo, hi):
    return x >= lo and (hi is None or x < hi)


def _Keeps(tree, n, files):
    """Whether leaf `n` of `tree` keeps a key of a state of `files`: a live
    file's id in its ids, a block of a live file's ground in its range."""
    if tree == tally.FILES:
        lo, hi = _FidsOf(n)
        return any(_Within(fid, lo, hi) for fid in files)
    lo = n.lo if n.lo is not None else 0
    return any(s + c > lo and (n.hi is None or s < n.hi) for fs in files.values() for s, c in fs.Ground())


def _GivenUpCosts(c):
    """What a Tally laid in after every file with ground in a leaf of DIGESTS
    is given up is reckoned to cost (§16.6 п.6), one rule with the kernel
    (settle/binder_cost.c, binder_live.c): each leaf the spans meet as much as
    is left live in it — a leaf no key of the state is left in, nothing — and
    the path above every one of them; so the leaves those files emptied are
    out of what the leaves they met would cost at the size they had."""
    from . import dog_ear
    t, files = c.mount.tally, c.mount.files
    leaves = sorted((n for n in t.trees[tally.DIGESTS].nodes if n.level == 0),
                    key=lambda n: (n.lo is not None, n.lo))
    pick = leaves[1] if len(leaves) > 1 else None
    if pick is None:
        return False
    gone = {fid for fid, fs in files.items() if _Keeps(tally.DIGESTS, pick, {fid: fs})}
    after = {fid: fs for fid, fs in files.items() if fid not in gone}
    spans = dog_ear.Fresh()
    for fid in sorted(gone):
        dog_ear.FileChanged(spans, fid, files[fid], None)
    emptied = sum(n.sheets for tree in (tally.FILES, tally.DIGESTS) for n in t.trees[tree].nodes
                  if n.level == 0 and _Keeps(tree, n, files) and not _Keeps(tree, n, after))
    try:
        got = dog_ear.TipInBlocks(t, spans, binder.Lives(after, c.mount.tags, c.mount.use))
        laid = dog_ear.TipInBlocks(t, spans, lambda tree, lo, hi: True)
    except (AttributeError, TypeError):
        return False                         # red until a leaf costs what is left live in it
    return gone and emptied >= pick.sheets and got == laid - emptied


def _Path(t, tree, leaf):
    """The Sheets of every inner node over `leaf` of `tree` in Tally `t`, one
    a level: the last node of the level whose lower key is not past the
    leaf's — counted here, not by dog_ear.py."""
    nodes, blocks = t.trees[tree].nodes, 0
    for level in range(1, 1 + max(n.level for n in nodes)):
        run = [n for n in nodes if n.level == level]
        over = [n for n in run if n.lo is None or (leaf.lo is not None and not leaf.lo < n.lo)]
        blocks += max(over, key=lambda n: (n.lo is not None, n.lo)).sheets
    return blocks


def _LeafAlone(c, tree, leaf, after, held, live):
    """The next Tally reckoned with one span over `leaf` of `tree` alone and
    the state of `after` (and `held`, ground held for what rotted), against
    what it is by the rule, counted here: the leaf's Sheets where `live` says
    a key is left in it, none where none is, and its path either way."""
    from . import dog_ear
    t = c.mount.tally
    spans = dog_ear.Fresh()
    spans[tree].append((leaf.lo, leaf.hi))
    try:
        got = dog_ear.TipInBlocks(t, spans, binder.Lives(after, c.mount.tags, c.mount.use, held))
    except (AttributeError, TypeError):
        return False                         # red until a leaf costs what is left live in it
    return got == (leaf.sheets if live else 0) + _Path(t, tree, leaf) and _Path(t, tree, leaf) > 0


def _Leaves(c, tree):
    leaves = sorted((n for n in c.mount.tally.trees[tree].nodes if n.level == 0),
                    key=lambda n: (n.lo is not None, n.lo))
    return [(n, leaves[i + 1].lo if i + 1 < len(leaves) else None) for i, n in enumerate(leaves)]


def _FilesEmptied(c):
    """Every file of the first leaf of FILES given up, its upper key's file
    among them: the leaf costs nothing, its path what it did."""
    leaf, hi = _Leaves(c, tally.FILES)[0]
    files = c.mount.files
    lo, top = _FidsOf(leaf)
    after = {fid: fs for fid, fs in files.items() if not _Within(fid, lo, top)}
    return len(after) < len(files) and _LeafAlone(c, tally.FILES, _Span(leaf, hi), after, (), False)


def _FilesEndInside(c):
    """The first leaf of FILES ends inside a file — its upper key that file's
    and not its header: every file below that one given up, the leaf keeps the
    file's keys it holds, and costs its Sheets."""
    leaf, hi = _Leaves(c, tally.FILES)[0]
    if hi is None or hi[1] == 0:
        return False                         # Padded's first leaf ends at a header: nothing to hold here
    files = c.mount.files
    after = {fid: fs for fid, fs in files.items() if fid >= hi[0]}
    return hi[0] in files and _LeafAlone(c, tally.FILES, _Span(leaf, hi), after, (), True)


def _DigestsEmptied(c):
    """A leaf of DIGESTS past the first, every file with ground in it given up
    and a run held for what rotted ending at its lower key and one beginning at
    its upper: neither lies in its range, it costs nothing, and its path —
    met by no live leaf — what it did."""
    leaves = _Leaves(c, tally.DIGESTS)
    leaf, hi = leaves[len(leaves) // 2]
    files = c.mount.files
    after = {fid: fs for fid, fs in files.items()
             if not any(s < (hi if hi is not None else s + n + 1) and s + n > leaf.lo for s, n in fs.Ground())}
    held = [(leaf.lo - 1, 1)] + ([(hi, 1)] if hi is not None else [])
    return (len(after) < len(files) and _LeafAlone(c, tally.DIGESTS, _Span(leaf, hi), after, held, False) and
            _LeafAlone(c, tally.DIGESTS, _Span(leaf, hi), after, [(leaf.lo, 1)], True))


def _UseSaid(c):
    """A Tally whose USE is one leaf, the Use Context said since: the leaf
    costs nothing once the volume remembers none, its Sheets while it
    remembers one — a tree of one leaf has no path."""
    from types import SimpleNamespace
    from . import dog_ear
    leaf = SimpleNamespace(level=0, lo=None, hi=None, sheets=1)
    t = SimpleNamespace(trees={tally.USE: SimpleNamespace(nodes=[leaf])})
    spans = dog_ear.Fresh()
    spans[tally.USE].append((0, 1))
    try:
        none = dog_ear.TipInBlocks(t, spans, binder.Lives(c.mount.files, c.mount.tags, b""))
        said = dog_ear.TipInBlocks(t, spans, binder.Lives(c.mount.files, c.mount.tags, b"use"))
    except (AttributeError, TypeError):
        return False                         # red until a leaf costs what is left live in it
    return none == 0 and said == leaf.sheets


class _Span:
    """A leaf as a span over its own range: its lower key, its upper — the
    next leaf's lower, None for the last — and its Sheets."""

    def __init__(self, leaf, hi):
        self.lo, self.hi, self.sheets = leaf.lo, hi, leaf.sheets


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
    next Tally laid in is reckoned to cost after a rename; B0k: after files
    are given up. B0h: a Tally-Quire held whole while one node in it lives."""
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
        Case("B0k a Tally laid in after files are given up is reckoned at what is left live in its leaves",
             lay, checks=[
                 ("every file with ground in a leaf of DIGESTS given up: the leaves they empty cost nothing, every "
                  "other leaf they meet and the path above them what it did (dog_ear.py, §16.6 п.6)", _GivenUpCosts),
                 ("every file of a leaf of FILES given up, its upper key's file among them: the leaf costs nothing, "
                  "the path above it what it did", _FilesEmptied),
                 ("a leaf of FILES that ends inside a file keeps that file's keys: every file below it given up, "
                  "the leaf costs its Sheets", _FilesEndInside),
                 ("a leaf of DIGESTS met alone, every file with ground in it given up: it costs nothing — a run "
                  "ending at its lower key and one beginning at its upper are not in its range — and the path "
                  "above it what it did; a run held at its lower key keeps it", _DigestsEmptied),
                 ("a leaf of USE costs nothing once the volume remembers no Use Context, its Sheets while it "
                  "remembers one", _UseSaid)]),
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
