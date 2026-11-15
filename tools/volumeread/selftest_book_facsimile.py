"""selftest_book_facsimile — the book's landings of Facsimile's one rule
(mount/facsimile.c, bookmount._Facsimile):
rotten FILES ranges of the Tally stood on that reach into one fid weighed as
one; a slot whose Tally names a file of the keys it cannot stand on, or whose
ribbon replayed for the keys passes over a stitch of them — a record of the
keys refused, or one whose key does not read — laid out again; a stitch
refused for a record of other keys alone, which leaves the keys as the Tally
stood on holds them, no bar. And the file the loader reads the kernel by
carrying a Slip (§16.6 п.4): the book only read, at once. Each a fork of the
base selftest_book.py lays; what volume-read's mount and census must say."""

from . import binder, ondisk, stitch, tally
from .bookmount import Blocks, LastBlock
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK
from .selftest_book import Case, Found, Glass
from .selftest_book_escheat import _NodesWrite, _Past, _SlotAfter
from .selftest_book_tally import Padded, PaddedLay
from .selftest_book_tip_in import _Headers, _LayGapped


def _Leaves(plan):
    return [n for n in plan.nodes if n.tree == tally.FILES and n.level == 0]


def _Gapped(content, fid):
    """`content` with file `fid`'s header two blocks longer than its Parcels
    cover — locked by the Tally that names it so (PARCEL_GAP)."""
    out = tally.Content.Copy(content)
    fs = out.files[fid].Copy()
    fs.size += 2 * BLOCK
    out.files[fid] = fs
    return out


def _Rewritten(bt, older, oplan, later, anew=1, extra=(), fresh=None, before=(), add=()):
    """Slot 0 the Tally of `older` (`oplan`); a stitch renaming the first file
    whose header the last leaf of FILES holds and putting `add` on it,
    `before` records sewn ahead of it and `extra` after (`fresh` its Clasp's
    count when a record of them does not read); then a Tally of `later` over
    slot 1 that lays the last `anew` leaves of FILES anew and names every
    other node as it lies. (lay — it returns the stitch sewn —, the leaves
    laid anew, the renamed fid, its name)."""
    g, leaves = bt.ground, _Leaves(oplan)
    if len(leaves) < anew:
        raise Unreadable("Padded's FILES has %u leaves, not %u to lay anew" % (len(leaves), anew))
    first = leaves[-anew]
    fid = next(k[0] for k, _b in leaves[-1].items if k[1] == 0)
    name = b"facsimile-" + bytes(later.files[fid].name)[:40]
    built = {}

    def Build(seq):
        lat = tally.Content.Copy(later)
        lat.files[fid] = later.files[fid].Copy()
        lat.files[fid].name = name
        lat.files[fid].gen = seq - 1
        refs = [tally.Rotten(tally.FILES, n.FirstKey(), leaves[i + 1].FirstKey(), _Headers(n), 0,
                             {"seq": oplan.seq, "sheets": n.sheets, "blocks": list(n.blocks)}, None)
                for i, n in enumerate(leaves[:-anew])]
        elements = binder.Elements(lat, seq)
        elements[tally.FILES] = [(k, b) for k, b in elements[tally.FILES] if k >= first.FirstKey()]
        a, b, grid = _Past(oplan, g)
        nodes, roots, used = _LayGapped(elements, seq, a, b, grid, refs, 0)
        if used > oplan.b - a:
            raise Unreadable("the Tally-Quires of the base hold no room for a Tally laid in past its nodes")
        return binder.BindPlan(lat, seq, seq + 1, a, b, oplan.ribbon, oplan.next, nodes, roots, None)

    def Lay(o):
        PaddedLay(bt, older, oplan)(o)
        pen = bt.Pen(o)
        sewn = pen.Sew(list(before) + [stitch.Labeled(fid=fid, base=pen.next_seq - older.files[fid].gen, name=name,
                                                      add=list(add))] + list(extra),
                       flags=stitch.DATA_FLUSHED, fresh=fresh)
        p2 = built["p"] = Build(sewn.seq + 1)
        _NodesWrite(o, g, p2)
        _SlotAfter(o, g, p2, 1, sewn, pen.place.quire[0] + pen.place.off // SECTORS_PER_BLOCK)
        return sewn

    def Laid():
        return _Leaves(built["p"])[-anew:]

    return Lay, Laid, fid, name


def _Rotted(bt, lay, laid):
    """`lay`, then every leaf slot 1 laid anew rotten in both copies."""
    def Plant(o):
        lay(o)
        for n in laid():
            bt.RotNode(o, n)
    return Plant


def _Header(leaf, other_than=()):
    """A file whose header `leaf` holds, not one of `other_than`."""
    fid = next((k[0] for k, _b in leaf.items if k[1] == 0 and k[0] not in other_than), None)
    if fid is None:
        raise Unreadable("the leaf holds no header of a file but %s" % list(other_than))
    return fid


def _Born(fid):
    """A Born of a file of no bytes — refused where `fid` lives (§3.7)."""
    return stitch.Born(fid=fid, flags=ondisk.FILE_ACTIVE, size=0, created_ns=1, modified_ns=1, name=b"born-again",
                       tags=(), runs=[], digests=[])


def _Taken(c, fid, name, files):
    """Is every rotten range of the Tally stood on taken by Facsimile from slot
    0, every file `files` names live, `fid` by `name`, nothing locked or held
    — by the kernel's census too, which names each range taken and the slot it
    is read from?"""
    taken = sum(1 for x in c.findings if x.code == "RANGE_FACSIMILE")
    return (c.mount.refusal is None and c.mount.slot.index == 1 and not c.mount.glass and
            Found(c, "RANGE_FACSIMILE", "FILES", "slot 0") and not Found(c, "RANGE_ROTTEN") and
            set(c.mount.files) == set(files) and c.mount.files[fid].name == name and not c.mount.locked and
            not c.held and c.kernel["mounts"] is True and not c.kernel["rotten"] and not c.kernel["held"] and
            c.kernel["facsimile"] == [["FILES", 0]] * taken)


def _Laid(c, ranges):
    """Did slot 0 not stand in: `ranges` FILES range(s) rotten, none taken,
    their files locked by their parents' counts and their ground held — as
    the kernel's census says too, which names no range taken?"""
    return (c.mount.refusal is None and c.mount.slot.index == 1 and not Found(c, "RANGE_FACSIMILE") and
            sum(1 for x in c.findings if x.code == "RANGE_ROTTEN" and "FILES" in x.text) == ranges and
            bool(c.held) and
            [r["tree"] for r in c.kernel["rotten"]].count("FILES") == ranges and not c.kernel["facsimile"])


def FacsimileRuleCases(bt):
    """B59c: two rotten FILES ranges sharing a fid, a file of the first locked
    in slot 0's Tally — one group, so neither is taken (a guard of groups: the
    last range alone would be). B59d: a file of the range locked in slot 0's
    Tally — slot 0 does not stand in (a guard). B59e: the ribbon to the stitch
    the Tally stood on passes over a stitch for a record of the range's keys —
    laid out again (a guard). B59f: a stitch passed over for a record of other
    keys alone — the range's replay is whole, and taken (red until one rule:
    the keys'). B59g: a stitch passed over for a record whose key does not
    read — it may be one of the keys, so laid out again. B59h: two rotten
    FILES ranges sharing a fid, both taken as one."""
    content, plan = Padded(bt)
    leaves = _Leaves(plan)
    if len(leaves) < 2:
        raise Unreadable("Padded's FILES is one leaf")
    shared, fid1 = leaves[-1].items[0][0][0], _Header(leaves[-1])
    lay2, laid2, fid2, name2 = _Rewritten(bt, content, plan, content, anew=2)
    inner = _Header(leaves[-2], (shared, fid2))
    gapped = _Gapped(content, inner)
    lay2g, laid2g, _f, _n = _Rewritten(bt, gapped, binder.Bind(gapped, bt.ground, a=bt.plan.a), content, anew=2)
    late = _Header(leaves[-1], (fid1, shared))
    lgapped = _Gapped(content, late)
    lay1g, laid1g, _f, _n = _Rewritten(bt, lgapped, binder.Bind(lgapped, bt.ground, a=bt.plan.a), content)
    away = min(fid for fid in content.files if fid < shared)
    keyed, laidk, _f, _n = _Rewritten(bt, content, plan, content, extra=[_Born(late)])
    other, laido, fido, nameo = _Rewritten(bt, content, plan, content, extra=[_Born(away)])
    broken, laidb, _f, _n = _Rewritten(bt, content, plan, content, extra=[stitch.Record(stitch.BORN, b"\xff")],
                                    fresh=0)
    return [
        Case("B59c two rotten FILES ranges sharing a fid, a file of the first locked in slot 0's Tally",
             _Rotted(bt, lay2g, laid2g), must={"RANGE_ROTTEN", "HELD_BLOCKS"}, may={"BOOT_MISMATCH"}, checks=[
                 ("fid %u reaches into both: one group, which slot 0 does not stand in for — neither is taken"
                  % shared, lambda c: _Laid(c, 2))]),
        Case("B59d a file of the rotten FILES range locked in slot 0's Tally", _Rotted(bt, lay1g, laid1g),
             must={"RANGE_ROTTEN", "HELD_BLOCKS"}, may={"BOOT_MISMATCH"}, checks=[
                 ("fid %u does not stand in slot 0's Tally either: the range stays rotten" % late,
                  lambda c: _Laid(c, 1))]),
        Case("B59e the ribbon to the stitch stood on passes over a stitch for a record of the range's keys",
             _Rotted(bt, keyed, laidk), must={"RANGE_ROTTEN", "HELD_BLOCKS"}, may={"BOOT_MISMATCH"}, checks=[
                 ("its Born of fid %u, which lives, refuses the stitch: slot 0 does not stand in" % late,
                  lambda c: _Laid(c, 1))]),
        Case("B59f the ribbon to the stitch stood on passes over a stitch for a record of other keys alone",
             _Rotted(bt, other, laido), must={"RANGE_FACSIMILE"}, checks=[
                 ("its Born of fid %u, outside the range, leaves the range's keys whole: taken from slot 0"
                  % away, lambda c: _Taken(c, fido, nameo, content.files))]),
        Case("B59g the ribbon to the stitch stood on passes over a stitch for a record whose key does not read",
             _Rotted(bt, broken, laidb), must={"RANGE_ROTTEN", "HELD_BLOCKS"}, may={"BOOT_MISMATCH"}, checks=[
                 ("a Born that does not read may be of the range's keys: slot 0 does not stand in",
                  lambda c: _Laid(c, 1))]),
        Case("B59h two rotten FILES ranges sharing a fid, taken as one", _Rotted(bt, lay2, laid2),
             must={"RANGE_FACSIMILE"}, checks=[
                 ("fid %u reaches into both: both taken from slot 0, every file live" % shared,
                  lambda c: _Taken(c, fid2, name2, content.files) and
                  sum(1 for x in c.findings if x.code == "RANGE_FACSIMILE") == 2)]),
    ]


def _Worn(content, fid, tid, key):
    """`content` with tag `tid` coined as `key` and worn by file `fid`."""
    out = tally.Content.Copy(content)
    out.tags[tid] = tally.TagState(0, key, None)
    out.next_tag = max(out.next_tag, tid + 1)
    out.files[fid] = content.files[fid].Copy()
    out.files[fid].tags = sorted(set(out.files[fid].tags) | {tid})
    return out


def FacsimileReplayCases(bt):
    """The ribbon to the stitch the Tally stood on, replayed for the range's
    keys as the kernel replays it (RibbonFacsimile). B59i: the stitch coins a
    tag and puts it on a file of the range — the replay knows every tag the
    Tally stood on names, as the kernel's registry does, and takes the file
    wearing it (a guard, bitten by a replay that knows the older slot's tags
    alone). B59j: the stitch carries a record of a kind this reader does not
    know and may not step over — it may be of the keys, so laid out again
    (red until the kernel applies it; volume-read's guard). B59k: a stitch
    past the one stood on carries an Undone of it — the Tally stood on
    absorbed it (§2.8), so the replay to that stitch passes over nothing and
    the range is taken with the stitch's records (red until volume-read
    gathers the Undones of the stitches to it alone, as the kernel does).
    B59l: the record of an unknown kind may be stepped over — taken (a
    guard)."""
    content, plan = Padded(bt)
    leaves = _Leaves(plan)
    fid, tid = _Header(leaves[-1]), content.next_tag
    key = b"facsimile-coined"
    coined, laidc, _f, namec = _Rewritten(bt, content, plan, _Worn(content, fid, tid, key),
                                          before=[stitch.Coined(tid, key)], add=[tid])
    lay, laid, _f, name = _Rewritten(bt, content, plan, content)
    alien, laida, _f, _n = _Rewritten(bt, content, plan, content, extra=[stitch.Unknown(40, False, b"\x01")])
    stepped, laids, _f, names = _Rewritten(bt, content, plan, content, extra=[stitch.Unknown(40, True, b"\x01")])

    def Undone(o):
        k = lay(o).seq
        pen = bt.Pen(o)
        pen.Sew([stitch.Undone(seq_back=pen.next_seq - k, why=1, fids=[fid])], flags=stitch.DATA_FLUSHED)
        for n in laid():
            bt.RotNode(o, n)

    return [
        Case("B59i the stitch stood on coins a tag and puts it on a file of the rotten FILES range",
             _Rotted(bt, coined, laidc), must={"RANGE_FACSIMILE"}, checks=[
                 ("fid %u taken from slot 0 wearing tag %u, which the Tally stood on names" % (fid, tid),
                  lambda c: _Taken(c, fid, namec, content.files) and tid in c.mount.files[fid].tags)]),
        Case("B59j the ribbon to the stitch stood on carries a record of a kind not to be stepped over",
             _Rotted(bt, alien, laida), must={"RANGE_ROTTEN", "HELD_BLOCKS"}, may={"BOOT_MISMATCH"}, checks=[
                 ("a record of kind 40 may be of the range's keys: slot 0 does not stand in",
                  lambda c: _Laid(c, 1))]),
        Case("B59k a stitch past the one stood on carries an Undone of it", Undone, must={"RANGE_FACSIMILE"},
             checks=[("the Tally stood on absorbed the Undone: fid %u taken from slot 0 renamed" % fid,
                      lambda c: _Taken(c, fid, name, content.files))]),
        Case("B59l the ribbon to the stitch stood on carries a record of a kind to step over",
             _Rotted(bt, stepped, laids), must={"RANGE_FACSIMILE"}, checks=[
                 ("a record of kind 40 stepped over: fid %u taken from slot 0" % fid,
                  lambda c: _Taken(c, fid, names, content.files))]),
    ]


def BootSlipCases(bt):
    """B65: a Shaped of the file the loader reads the kernel by laying a Slip
    of its last block's own bytes — a stitch only another writer sews: no call
    of this kernel changes that file, so its tail is neither laid in its block
    nor let go by Escheat, and the mount takes the book to read only at once
    (§16.6 п.4)."""
    boot = bt.c0.boot_fid
    if boot is None:
        raise Unreadable("the image names no file the loader reads the kernel by")
    fs = bt.content.files[boot]
    last = LastBlock(fs)
    if last is None or last[1] is None or last[1] == "beyond its runs":
        raise Unreadable("the kernel's file has no last block of data to lay a Slip over")
    cut = fs.size - (Blocks(fs.size) - 1) * BLOCK
    raw = bt.base.sectors(bt.ground.base + last[1] * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK)
    slip = (0, bytes(raw[:min(16, cut)]))

    def Slipped(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Shaped(fid=boot, base=pen.next_seq - fs.gen, size=fs.size, mtime_ns=fs.modified_ns,
                               splices=[], digests=[], slip=slip)], flags=stitch.DATA_FLUSHED)

    return [
        Case("B65 a Slip on the file the loader reads the kernel by", Slipped, checks=[
            ("the book mounts, the file carrying the Slip",
             lambda c: c.mount.refusal is None and c.mount.files[boot].slips == [slip]),
            ("and may only be read, the file named: no call changes it (§16.6 п.4)",
             lambda c: Glass(c, "fid %u" % boot, "loader", "Slip") and c.kernel["glass"] == 1),
            ("and the census names where in the file its Slip lies: its last block's first byte, which the "
             "guest's read by `show` is said to stop short of or reach",
             lambda c: [f["at"] for f in c.kernel["slipped"] if f["fid"] == boot] ==
             [(Blocks(fs.size) - 1) * BLOCK + slip[0]])]),
    ]
