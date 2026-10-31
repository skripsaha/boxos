"""selftest_book_escheat — the book's landings of a rotten range a Tally after
it no longer leaves rotten (spec_v2 §4.5 п.3, §16.5 п.19-20): a leaf of FILES
laid anew between two slots, the newer copy rotten in both, read from the older
slot's Tally and the older slot's ribbon replayed for its keys (Facsimile); a
leaf both slots name as it lies, rotten, which no older slot can stand in for;
and a Tally written after the act that lets a rotten range go (Escheat), which
names no node where the slot before named the rotten one. Each a fork of the
base selftest_book.py lays; what volume-read's mount, census and oracle
(binder.BindCheck) must say of it."""

from . import binder, stitch, tally
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK, RunsNormal, RunsSubtract, RunsText
from .selftest_book import Found
from .selftest_book_tally import Padded, PaddedLay
from .selftest_book_tip_in import _Headers, _LayGapped
from .selftest_cases import Case


def _NodesWrite(o, g, plan):
    """Every node of `plan`, both copies — the ribbon left as it lies."""
    for n in plan.nodes:
        stream, starts = binder.NodeStream(n.tree, n.level, n.items)
        for blk in n.blocks:
            o.Put(g.base + blk * SECTORS_PER_BLOCK, b"".join(binder.NodeSectors(g, plan.seq, blk, n.sheets, stream,
                                                                              starts)))


def _SlotAfter(o, g, plan, index, sewn, at):
    """Slot `index` naming `plan`'s Tally as the state after stitch `sewn`,
    the ribbon going on at block `at`."""
    slot = binder.PlanSlot(plan, plan.content, index)
    slot.stitch_seq, slot.stitch_clasp = sewn.seq, sewn.clasp
    slot.ribbon["at"] = at
    o.Put(g.base + binder.SlotBlock(g, index) * SECTORS_PER_BLOCK,
          b"".join(binder.SlotSectors(g, index, plan.slot_seq, slot)))


def _Past(plan, g):
    """Where a Tally laid after `plan`'s goes in its Tally-Quires: past its
    last node on an atom, copy 2 as far into B as copy 1 into A."""
    grid = max(1, g.atom // BLOCK)
    a = -(-(plan.a + plan.Used()) // grid) * grid
    return a, plan.b + (a - plan.a), grid


def _Rewritten(bt, content, plan):
    """The base of Padded's book, a stitch renaming the first file whose
    header the last leaf of FILES holds, then a Tally laid in after it over
    slot 1 — that leaf laid anew, every other named as it lies: (lay, the
    leaf laid anew, the leaf kept, fid, the new name)."""
    g = bt.ground
    leaves = [n for n in plan.nodes if n.tree == tally.FILES and n.level == 0]
    if len(leaves) < 2:
        raise Unreadable("Padded's FILES is one leaf: no leaf to lay anew beside one named as it lies")
    last = leaves[-1]
    fid = next(k[0] for k, _b in last.items if k[1] == 0)
    name = b"facsimile-" + bytes(content.files[fid].name)[:40]
    sewn, built = {}, {}

    def Build(seq):
        later = tally.Content.Copy(content)
        later.files[fid] = content.files[fid].Copy()
        later.files[fid].name = name
        later.files[fid].gen = seq - 1
        refs = [tally.Rotten(tally.FILES, n.FirstKey(), leaves[i + 1].FirstKey(), _Headers(n), 0,
                             {"seq": plan.seq, "sheets": n.sheets, "blocks": list(n.blocks)}, None)
                for i, n in enumerate(leaves[:-1])]
        elements = binder.Elements(later, seq)
        elements[tally.FILES] = [(k, b) for k, b in elements[tally.FILES] if k >= last.FirstKey()]
        a, b, grid = _Past(plan, g)
        nodes, roots, used = _LayGapped(elements, seq, a, b, grid, refs, 0)
        if used > plan.b - a:
            raise Unreadable("the Tally-Quires of the base hold no room for a Tally laid in past its nodes")
        return binder.BindPlan(later, seq, seq + 1, a, b, plan.ribbon, plan.next, nodes, roots, None)

    def Lay(o):
        PaddedLay(bt, content, plan)(o)
        pen = bt.Pen(o)
        sewn["s"] = pen.Sew([stitch.Labeled(fid=fid, base=pen.next_seq - content.files[fid].gen, name=name)],
                            flags=stitch.DATA_FLUSHED)
        p2 = built["p"] = Build(sewn["s"].seq + 1)
        _NodesWrite(o, g, p2)
        _SlotAfter(o, g, p2, 1, sewn["s"], pen.place.quire[0] + pen.place.off // SECTORS_PER_BLOCK)

    def Laid():
        return next(n for n in built["p"].nodes if n.tree == tally.FILES and n.level == 0)

    return Lay, Laid, leaves[0], fid, name


def _Escheated(bt, content, plan, leaf, dig=None, wrong=False, unheld=None):
    """B4's book — and, `dig`, B6c's, that DIGESTS leaf rotten too — and a
    Tally over slot 1 of the state the act that lets the rotten range go
    leaves (§16.5 пп.19-20): every file the rotten leaf held a key of lost,
    the blocks they and the range held named by no one — DIGESTS naming the
    blocks of the live files alone, those of the rotten DIGESTS leaf by the
    digests of their data, and those of a file locked for another reason,
    which the act does not let go; `wrong`, one of those a digest the data
    does not have; `unheld`, the locked file whose digests the Tally leaves
    out — its ground let go with the range's. (lay, the fids lost, the runs
    B4 holds)."""
    g = bt.ground
    lost = sorted({k[0] for k, _b in leaf.items})
    held = RunsNormal([(r.start, r.count) for fid in lost for r in content.files[fid].runs if not r.hole])
    after = tally.Content.Copy(content)
    for fid in lost:
        del after.files[fid]
    live = {b for fs in after.files.values() for s, n in fs.Ground() for b in range(s, s + n)}
    if unheld is not None:
        live -= {b for s, n in after.files[unheld].Ground() for b in range(s, s + n)}
    after.digests = {b: d for b, d in content.digests.items() if b in live}
    if wrong:
        b0 = min(b for b in after.digests if dig.FirstKey() <= b)
        after.digests[b0] ^= 1
    a, b, _grid = _Past(plan, g)
    p2 = binder.Bind(after, g, a=a, b=b, ribbon=plan.ribbon, nxt=plan.next, seq=plan.slot_seq + 1,
                     slot_seq=plan.slot_seq + 2)
    if p2.Used() > plan.b - a:
        raise Unreadable("the Tally-Quires of the base hold no room for a whole Tally past its nodes")

    def Lay(o):
        PaddedLay(bt, content, plan)(o)
        bt.RotNode(o, leaf)
        if dig is not None:
            bt.RotNode(o, dig)
        binder.BindWrite(o, g, p2, index=1)

    return Lay, lost, held


def _LockedBesides(bt, content):
    """B4's content with a file of the base locked for a reason other than
    the rot — snake.elf, which neither the boot nor the stand runs, its header
    two blocks longer than its Parcels cover (PARCEL_GAP) — and its plan.
    (content, plan, fid, the runs it names)."""
    fid = next((f for f, fs in content.files.items() if fs.name == b"snake.elf"), None)
    if fid is None:
        raise Unreadable("the base holds no snake.elf to lock beside the rotten range")
    locked = tally.Content.Copy(content)
    fs = locked.files[fid].Copy()
    fs.size += 2 * BLOCK
    locked.files[fid] = fs
    plan = binder.Bind(locked, bt.ground, a=bt.plan.a)
    return locked, plan, fid, RunsNormal([(r.start, r.count) for r in fs.runs if not r.hole])


def LockedCases(bt):
    """B4g: B4's book and a file locked for another reason beside it. B60e:
    the Tally after Escheat carries that file as it lay and keeps its ground
    held — the act lets go only what it lets go (§16.5 п.20). B60f: one that
    leaves its digests out, its ground let go with the range's, which the
    reader refuses (a guard: a file the act does not let go keeps its blocks)."""
    content, _plan = Padded(bt)
    locked, plan, fid, runs = _LockedBesides(bt, content)
    leaf = [n for n in plan.nodes if n.tree == tally.FILES and n.level == 0][-1]
    kept, _l, _h = _Escheated(bt, locked, plan, leaf)
    dropped, _l, _h = _Escheated(bt, locked, plan, leaf, unheld=fid)

    def Planted(o):
        PaddedLay(bt, locked, plan)(o)
        bt.RotNode(o, leaf)

    def Holds(c):
        """Is the locked file's ground held, and nothing else but it?"""
        return not RunsSubtract(runs, c.held) and not RunsSubtract(c.held, runs)

    return [
        Case("B4g a FILES leaf rotten in both copies and a file locked beside it for its Parcels", Planted,
             must={"RANGE_ROTTEN", "HELD_BLOCKS", "PARCEL_GAP"}, may={"BOOT_MISMATCH"}, checks=[
                 ("fid %u is locked for its Parcels, apart from the range" % fid,
                  lambda c: fid in c.mount.locked and Found(c, "PARCEL_GAP", "fid %u" % fid))]),
        Case("B60e a Tally after Escheat keeps a file locked for another reason, and its ground held", kept,
             must={"PARCEL_GAP", "HELD_BLOCKS"}, checks=[
                 ("the book mounts on slot 1 with no range rotten, fid %u locked alone" % fid,
                  lambda c: c.mount.refusal is None and c.mount.slot.index == 1 and not c.mount.tally.rotten and
                  set(c.mount.locked) == {fid}),
                 ("only the blocks it names, %s, are held — by the census of the kernel too" % RunsText(runs),
                  lambda c: Holds(c) and c.kernel["held"] == sum(n for _s, n in runs)),
                 ("and the oracle takes the Tally (binder.BindCheck)", lambda c: binder.BindCheck(c.medium) == [])]),
        Case("B60f a Tally after Escheat lets the ground of a file it does not let go go with the range's",
             dropped, must={"PARCEL_GAP"}, may={"HELD_BLOCKS"}, checks=[
                 ("the oracle refuses the Tally: a file the act keeps keeps its blocks held",
                  lambda c: binder.BindCheck(c.medium) != [])]),
    ]


def EscheatCases(bt):
    """B59: a FILES leaf laid anew between the slots, the newer rotten, read
    from the older slot and its ribbon (Facsimile). B59b: a FILES leaf both
    slots name as it lies, rotten: no older slot stands in for it, the range
    stays rotten (a guard — the honest price of nodes shared between Tallies).
    B60: a Tally after Escheat names no node where the slot before named a
    rotten one, and the reader takes it. B60c: on B6c's book, the rotten
    DIGESTS leaf laid anew of the data (§16.5 п.19), which the reader holds to
    the data; B60d: one digest of it not the data's, which it refuses."""
    content, plan = Padded(bt)
    lay, laid, kept, fid, name = _Rewritten(bt, content, plan)
    leaf = [n for n in plan.nodes if n.tree == tally.FILES and n.level == 0][-1]
    fdig = [n for n in plan.nodes if n.tree == tally.DIGESTS and n.level == 0][0]
    escheated, lost, held = _Escheated(bt, content, plan, leaf)
    anew, _l, _h = _Escheated(bt, content, plan, leaf, dig=fdig)
    false, _l, _h = _Escheated(bt, content, plan, leaf, dig=fdig, wrong=True)

    def Facsimile(o):
        lay(o)
        bt.RotNode(o, laid())

    def Shared(o):
        lay(o)
        bt.RotNode(o, kept)

    def Freed(c):
        """Is every block B4 holds free now, but the nodes slot 1 laid?"""
        return not RunsSubtract(RunsSubtract(held, c.free_runs), c.Owned())

    return [
        Case("B59 a FILES leaf laid anew between the slots, the newer rotten: Facsimile", Facsimile,
             must={"RANGE_FACSIMILE"}, checks=[
                 ("the book mounts on slot 1, not under glass",
                  lambda c: c.mount.refusal is None and c.mount.slot.index == 1 and not c.mount.glass),
                 ("the range is read from slot 0's Tally and its ribbon replayed for its keys: not rotten",
                  lambda c: Found(c, "RANGE_FACSIMILE", "FILES", "slot 0") and not Found(c, "RANGE_ROTTEN")),
                 ("every file of the range reads, fid %u by the name its stitch gave it" % fid,
                  lambda c: set(c.mount.files) == set(content.files) and c.mount.files[fid].name == name and
                  not c.mount.locked),
                 ("nothing is held", lambda c: not c.held),
                 ("and the kernel of this tree mounts it with no range rotten, the range read from slot 0",
                  lambda c: c.kernel["mounts"] is True and not c.kernel["rotten"] and not c.kernel["held"] and
                  c.kernel["facsimile"] == [["FILES", 0]])]),
        Case("B59b a FILES leaf both slots name as it lies, rotten: no older slot stands in", Shared,
             must={"RANGE_ROTTEN", "HELD_BLOCKS"}, may={"BOOT_MISMATCH"}, checks=[
                 ("the range stays rotten — slot 0 names the same node (§16.5 п.19)",
                  lambda c: Found(c, "RANGE_ROTTEN", "FILES") and not Found(c, "RANGE_FACSIMILE") and
                  not c.kernel["facsimile"])]),
        Case("B60 a Tally after Escheat names no node where the slot before named a rotten one", escheated,
             checks=[
                 ("the book mounts on slot 1 with no range rotten, nothing held",
                  lambda c: c.mount.refusal is None and c.mount.slot.index == 1 and not c.mount.tally.rotten and
                  not c.held and c.kernel["held"] == 0),
                 ("the %u file(s) of the range are gone and their ids not given again" % len(lost),
                  lambda c: not set(lost) & set(c.mount.files) and c.mount.counts["next_fid"] == content.next_fid),
                 ("the blocks B4 holds, %s, are free" % RunsText(held), Freed),
                 ("and the oracle takes the Tally: it may leave out a range the slot before named rotten "
                  "(binder.BindCheck)", lambda c: binder.BindCheck(c.medium) == [])]),
        Case("B60c a Tally after Escheat lays the rotten DIGESTS leaf anew of the data", anew, checks=[
            ("the book mounts on slot 1 with no range rotten, nothing held",
             lambda c: c.mount.refusal is None and c.mount.slot.index == 1 and not c.mount.tally.rotten and
             not c.held and c.kernel["held"] == 0),
            ("and the oracle takes the Tally, every digest of the leaf's range its block's data's (binder.BindCheck)",
             lambda c: binder.BindCheck(c.medium) == [])]),
        Case("B60d a Tally after Escheat names a digest of the rotten DIGESTS leaf's range the data does not have",
             false, must={"DIGEST_MISMATCH"}, checks=[
                 ("the oracle refuses the Tally: a range laid anew is held to the data, not passed over",
                  lambda c: any("DIGESTS" in w for w in binder.BindCheck(c.medium)))]),
    ]

