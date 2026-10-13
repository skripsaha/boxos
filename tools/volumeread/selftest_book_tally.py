"""selftest_book_tally — the book's landings in its Spine and its Tally: the
base as a binder lays it and read back byte for byte, a torn slot, a node of a
foreign seq or rotten in one copy or both, a Parcel that does not cover its
file, a run SHARED, BOOKMARKS not empty, a FRAME longer than known, a slot
whose Tally does not read, no slot at all. Each a fork of the base
selftest_book.py lays; what the book's census must say of it."""

from . import binder, bookread, ondisk, stitch, tally
from .bookground import BookGround
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK, RunsNormal
from .selftest_book import PADDING, Born, Found, FrameDeed, Glass, Label
from .selftest_cases import Case


def Pristine(bt):
    """B0: what the base must read as — no finding at all, every file whole
    (byte for byte the v1 image's, when the book was laid over one), and the
    bounds its binder wrote."""
    want = None if bt.t is None else {fid: bt.t.c0.Sha256(e["record"]) for fid, e in bt.t.c0.files.items()
                                      if e["state"] == "live"}
    return [Case("B0 the book as a binder lays it", lambda o: None, checks=[
        ("not one finding — 0 damage, 0 leak, 0 note", lambda c: not c.findings),
        ("the book mounts on slot 0, at seq 2, not under glass",
         lambda c: c.mount.refusal is None and c.mount.slot.index == 0 and c.mount.slot.seq == 2 and
         not c.mount.glass),
        ("every live file reads%s" % (", byte for byte the v1 image's %u" % len(want) if want else ""),
         lambda c: all(c.Sha256(fid) for fid in c.mount.files) and
         (want is None or {fid: c.Sha256(fid) for fid in c.mount.files} == want)),
        ("the next file id %u, tag id %u, bookmark 1, seq 3" % (bt.fid, bt.tag),
         lambda c: c.mount.counts == {"next_fid": bt.fid, "next_tag": bt.tag, "next_bookmark": 1,
                                      "next_seq": 3}),
        ("the kernel in this tree reads no book and refuses the volume as format v2",
         lambda c: c.kernel["mounts"] is False and "format v2" in c.kernel["refusal"]),
        ("the front, the Spine, the Tally, the ribbon, the files, BOOT and the far Deed are heard",
         lambda c: {"front", "spine", "tally", "ribbon", "file", "boot", "far_deed"} <= c.Kinds())])]


def Circles(bt):
    """B0b, B0c: what is read back and written again is the same bytes."""
    S, L = bt.S, bt.L
    gone = max(fid for fid in bt.content.files if fid not in (S.fid, L.fid, bt.c0.boot_fid))
    n = S.Blocks()

    def StitchCircle(o):
        """Every kind a cycle writes, in two stitches: the first a Coined, a
        Use Context, a Born, a Shaped of two splices with a digest and a
        Slip, a Labeled of all three parts and a Quitclaim; the second the
        Undone of the first's Born."""
        pen = bt.Pen(o)
        d = bt.Fresh(o, 0, 2, 1)
        seq = pen.next_seq
        first = pen.Sew([
            stitch.Coined(bt.tag, b"circle", b"value"), stitch.UseContext(b"tag:a,tag:b"),
            Born(bt.F, bt.fid, d[:1], b"circle"),
            stitch.Shaped(fid=S.fid, base=seq - S.gen, size=n * BLOCK + 1, mtime_ns=7,
                          splices=[(0, 1, [stitch.Data(bt.F + 1, 1)]), (n - 1, 0, [stitch.Hole(1)])],
                          digests=d[1:], slip=(0, b"\x01")),
            stitch.Labeled(fid=L.fid, base=seq - L.gen, name=b"renamed", flags=(ondisk.FILE_HIDDEN, 0),
                           add=[bt.tag], drop=list(L.tags[:1])),
            stitch.Quitclaim(fid=gone, base=seq - 1)], flags=stitch.DATA_FLUSHED)
        second = pen.Sew([stitch.Undone(seq_back=1, why=1, fids=[bt.fid])], flags=stitch.DATA_FLUSHED)
        return [first, second]

    sewn = {}
    return [
        Case("B0b the tally read back is bound again byte for byte", lambda o: None,
             checks=[("every node, both copies, and slot 0 come out the same",
                      lambda c: binder.Circle(c.medium, c.mount) == [])]),
        Case("B0c every record sewn, read back and sewn again is byte for byte",
             lambda o: sewn.setdefault("s", StitchCircle(o)), must={"UNDER_GLASS"}, checks=[
                 ("both stitches are taken whole and each record decodes to what was sewn",
                  lambda c: stitch.Circle(c.mount.ribbon, sewn["s"]) == []),
                 ("they apply: %s renamed, fid %u gone, fid %u undone" % (Label(L), gone, bt.fid),
                  lambda c: c.mount.files[L.fid].name == b"renamed" and gone not in c.mount.files and
                  bt.fid not in c.mount.files and bt.tag in c.mount.files[L.fid].tags),
                 ("and the only verdict is the Slip's", lambda c: len(c.mount.glass) == 1 and
                  Glass(c, "Slip"))])]


def SpineCases(bt):
    g = bt.ground

    def TornSlot(o):
        slot = tally.SlotRead(BookGround(o), 0)
        sectors = tally.SlotSectors(g, 1, 3, slot)
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(sectors[:4]))

    leaf = bt.Node(tally.FILES)

    def ForeignSeq(o):
        binder.NodeWrite(o, g, 7, [leaf.blocks[0]], leaf.sheets, leaf.tree, leaf.level, leaf.items)

    return [
        Case("B1 torn slot", TornSlot, must={"SLOT_TORN"}, checks=[
            ("the book stands on slot 0, seq 2", lambda c: c.mount.slot.index == 0),
            ("the torn slot's seq 3 is never given again: the next seq is 4",
             lambda c: c.mount.counts["next_seq"] == 4)]),
        Case("B2 a node of a foreign seq", ForeignSeq, must={"NODE_COPY_ROTTEN"}, checks=[
            ("copy 1 is named, sealed at seq 7 where its pointer names 1",
             lambda c: Found(c, "NODE_COPY_ROTTEN", "copy 1", "seq 7")),
            ("copy 2 is read and every file stays", lambda c: set(c.mount.files) == set(bt.content.files)),
            ("the seq 7 seen is never given again", lambda c: c.mount.counts["next_seq"] == 8)]),
        Case("B3 copy 1 rotten", lambda o: bt.RotNode(o, leaf, (0,)), must={"NODE_COPY_ROTTEN"}, checks=[
            ("copy 1 of a FILES leaf is named, mended by the next tally",
             lambda c: Found(c, "NODE_COPY_ROTTEN", "copy 1", "FILES")),
            ("every file reads", lambda c: all(c.Sha256(fid) == bt.c0.Sha256(fid) for fid in bt.content.files))]),
    ]


def Padded(bt):
    """The base's content and PADDING more files, each of one block of its
    own at F + i, named long so a few fill a Sheet: FILES then has a second,
    last leaf of three headers or more — the files a rotten last leaf takes,
    so the real ones stay what the image boots. The first one's name is as
    long as makes that leaf open with the Parcel of a file whose header ends
    the leaf before, a file known by its header and locked by fid. (content,
    plan)."""
    room = next(n for s, n in bt.c0.free_runs if s == bt.F)
    if room < PADDING:
        raise Unreadable("the free run at block %u holds %u blocks and %u more files take one each"
                         % (bt.F, room, PADDING))
    for first in range(128):
        content = tally.Content.Copy(bt.content)
        for i in range(PADDING):
            fid = bt.maxfid + 1 + i
            content.files[fid] = tally.FileState(fid, 1, ondisk.FILE_ACTIVE, BLOCK, 1, 1,
                                                 b"pad-%03u-" % i + b"p" * (first if i == 0 else 40), [],
                                                 [tally.Run(False, bt.F + i, 1, 1)])
            content.digests[bt.F + i] = bookread.Integrity(bt.Pattern(i), bt.lanes)
        content.next_fid = bt.maxfid + 1 + PADDING
        plan = binder.Bind(content, bt.ground, a=bt.plan.a)
        if plan.a < bt.F + PADDING and bt.F < plan.next[0] + plan.next[1]:
            raise Unreadable("the grown Tally, its ribbon and next_quire would lie over the landings' files "
                             "at block %u" % bt.F)
        leaves = [n for n in plan.nodes if n.tree == tally.FILES and n.level == 0]
        if len(leaves) >= 2 and sum(1 for k, _b in leaves[-1].items if k[1] == 0) >= 3 and \
                leaves[-1].items[0][0][1] != 0:
            return content, plan
    raise Unreadable("no name of the first added file makes FILES split between a header and its Parcel")


def PaddedLay(bt, content, plan):
    """What lays Padded's book over a fork: each added file's block, then the
    grown Tally where the base's was."""
    def Lay(o):
        for i, fid in enumerate(sorted(set(content.files) - set(bt.content.files))):
            bt.PutBlock(o, bt.F + i, bt.Pattern(i))
        binder.BindWrite(o, bt.ground, plan)
    return Lay


def RangeCases(bt):
    content, plan = Padded(bt)
    Lay = PaddedLay(bt, content, plan)
    leaf = [n for n in plan.nodes if n.tree == tally.FILES and n.level == 0][-1]
    pdig = [n for n in plan.nodes if n.tree == tally.DIGESTS and n.level == 0][-1]
    locked = sorted({k[0] for k, _b in leaf.items})
    whole = [fid for fid in locked if (fid, 0) in {k for k, _b in leaf.items}]
    held = RunsNormal([(r.start, r.count) for fid in locked for r in content.files[fid].runs if not r.hole])
    end = bt.ground.data_end

    def RotFiles(o, digests=False):
        Lay(o)
        bt.RotNode(o, leaf)
        if digests:
            bt.RotNode(o, pdig)

    def Named(o):
        """B4, and a stitch that renames a file whose header the rotten leaf
        held and gives birth to a file past every id the range had."""
        RotFiles(o)
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=whole[0], base=pen.next_seq - 1, name=b"renamed"),
                 stitch.Born(fid=content.next_fid, flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1,
                             modified_ns=1, name=b"anew", tags=(), runs=[stitch.Hole(1)], digests=[])],
                flags=stitch.DATA_FLUSHED)

    dig = bt.Node(tally.DIGESTS)
    pad = {bt.tag + i: tally.TagState(0, b"pad%04u" % i, None) for i in range(400)}

    def RotTags(o):
        content = tally.Content.Copy(bt.content)
        content.tags.update(pad)
        content.next_tag = bt.tag + 400
        plan = binder.Bind(content, bt.ground, a=bt.plan.a)
        binder.BindWrite(o, bt.ground, plan)
        leaves = [n for n in plan.nodes if n.tree == tally.TAGS and n.level == 0]
        if len(leaves) < 2:
            raise Unreadable("400 more tags still make one TAGS leaf")
        bt.RotNode(o, leaves[-1])

    def Within(c, lo):
        """Is every block from `lo` to the far Deed that no living owner
        names held?"""
        return not ondisk.RunsSubtract(ondisk.RunsSubtract([(lo, end - lo)], c.Owned()), c.held)

    return [
        Case("B4 a FILES leaf rotten in both copies", RotFiles,
             must={"RANGE_ROTTEN", "HELD_BLOCKS"}, checks=[
                 ("the files of the range are locked, %u by the parent's count" % len(whole),
                  lambda c: Found(c, "RANGE_ROTTEN", "FILES", "%u file(s) locked" % len(whole))),
                 ("none of the files the leaf held is live, nor any whose Parcel it held",
                  lambda c: not set(locked) & set(c.mount.files) and
                  set(c.mount.locked) == set(locked) - set(whole)),
                 ("every other file reads", lambda c: set(c.mount.files) == set(content.files) - set(locked)),
                 ("their blocks are held, %s" % ondisk.RunsText(held), lambda c: c.held == held),
                 ("their ids are never given again: the next file id stays %u" % content.next_fid,
                  lambda c: c.mount.counts["next_fid"] == content.next_fid)]),
        Case("B4b the ribbon names a file of the rotten range, and bears a new one", Named,
             must={"RANGE_ROTTEN", "HELD_BLOCKS"}, checks=[
                 ("fid %u's record stays with its locked range: no mismatch, no roll-back" % whole[0],
                  lambda c: not Found(c, "BASE_MISMATCH") and not c.mount.glass and
                  whole[0] not in c.mount.files),
                 ("fid %u, past every id the range had, is born anew" % content.next_fid,
                  lambda c: content.next_fid in c.mount.files)]),
        Case("B5 a TAGS leaf rotten in both copies", RotTags, must={"RANGE_ROTTEN"}, checks=[
            ("the names of the range are lost, every file stays",
             lambda c: Found(c, "RANGE_ROTTEN", "TAGS") and set(c.mount.files) == set(bt.content.files)),
            ("no tag id of the range is given again: the next tag id is %u" % (bt.tag + 400),
             lambda c: c.mount.counts["next_tag"] == bt.tag + 400)]),
        Case("B6 the last DIGESTS leaf rotten in both copies", lambda o: bt.RotNode(o, dig),
             must={"RANGE_ROTTEN"}, checks=[
                 ("its blocks are read unchecked", lambda c: Found(c, "RANGE_ROTTEN", "DIGESTS", "unchecked")),
                 ("and nothing is held: no FILES range is rotten, so every owner is known (§16.1 п.5)",
                  lambda c: not c.held)]),
        Case("B6b a FILES leaf and the last DIGESTS leaf rotten", lambda o: RotFiles(o, digests=True),
             must={"RANGE_ROTTEN", "HELD_BLOCKS"}, checks=[
                 ("the DIGESTS leaf holds its whole range from block %u but what the living name"
                  % pdig.items[0][0], lambda c: Within(c, pdig.items[0][0]))]),
    ]


def TreeShapeCases(bt):
    """A Tally whose shape the format does not allow, or allows and a reader
    must follow (§2.9, §4.1): a slot whose newer Tally keeps an older one's
    nodes, a leaf whose keys run outside its parent's separators, counts that
    lie, and leads that lie in a node and in a slot."""
    g = bt.ground
    content, plan = Padded(bt)
    Lay = PaddedLay(bt, content, plan)
    leaves = [n for n in plan.nodes if n.tree == tally.FILES and n.level == 0]
    inner = next(n for n in plan.nodes if n.tree == tally.FILES and n.level == 1)
    fid0 = leaves[0].items[0][0][0]

    def Kept(o):
        """Slot 1 at seq 4 for a Tally of seq 3 that names the nodes of the
        Tally of seq 1 as they lie — as a Tally that changed none of them."""
        slot = tally.SlotRead(BookGround(o), 0)
        slot.tally_seq = 3
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, 4, slot)))

    def Impostor(o):
        Lay(o)
        dup = content.files[fid0].Copy()
        dup.name = b"impostor"
        last = leaves[-1]
        binder.NodeWrite(o, g, plan.seq, last.blocks, last.sheets, last.tree, last.level,
                        [((fid0, 0), tally.FileHeadEncode(dup))] + last.items)

    def Counts(o):
        Lay(o)
        items = []
        for k, body in inner.items:
            c = bookread.Cursor(body)
            items.append((k, tally.InnerEncode(c.V() + 7, body[c.at:])))
        binder.NodeWrite(o, g, plan.seq, inner.blocks, inner.sheets, inner.tree, inner.level, items)

    leaf = bt.Node(tally.FILES)
    births = lambda c: {r.birth for fs in c.mount.files.values() for r in fs.runs if not r.hole}
    return [
        Case("B36 a newer Tally that keeps an older one's nodes", Kept, checks=[
            ("the book stands on slot 1, Tally seq 3", lambda c: c.mount.slot.index == 1 and c.mount.tally.seq == 3),
            ("every run is born at seq 1 — its node's seq, not the Tally's (§2.9)", lambda c: births(c) == {1}),
            ("and nothing is said", lambda c: not c.findings)]),
        Case("B37 a FILES leaf whose keys run outside its parent's range", Impostor,
             must={"RANGE_ROTTEN", "HELD_BLOCKS"}, checks=[
                 ("the leaf does not hold: a header of fid %u lies before its range" % fid0,
                  lambda c: Found(c, "RANGE_ROTTEN", "FILES", "outside")),
                 ("fid %u keeps the name its own leaf gives it" % fid0,
                  lambda c: c.mount.files[fid0].name == content.files[fid0].name)]),
        Case("B38 the counts of an inner FILES node lie", Counts, must={"RECORD_SHAPE"}, checks=[
            ("the counts are said not to be what is under them",
             lambda c: Found(c, "RECORD_SHAPE", "FILES", "count")),
            ("and every file stays", lambda c: set(c.mount.files) == set(content.files))]),
        Case("B39 a FILES leaf whose lead lies", lambda o: [bt.BumpLead(o, b * SECTORS_PER_BLOCK)
                                                            for b in leaf.blocks],
             must={"RECORD_SHAPE"}, checks=[
                 ("its sector 0 names a lead its records do not give it",
                  lambda c: Found(c, "RECORD_SHAPE", "FILES", "lead")),
                 ("and every file reads", lambda c: set(c.mount.files) == set(bt.content.files))]),
        Case("B40 a slot whose lead lies", lambda o: bt.BumpLead(o, tally.SlotBlock(g, 0) * SECTORS_PER_BLOCK),
             must={"RECORD_SHAPE"}, checks=[
                 ("its sector 0 names a lead its records do not give it",
                  lambda c: Found(c, "RECORD_SHAPE", "slot 0", "lead") and c.mount.slot.index == 0)]),
    ]


def ElementCases(bt):
    S, L = bt.S, bt.L
    sleaf = next(n for n in bt.plan.nodes if n.tree == tally.FILES and n.level == 0 and
                 any(k == (S.fid, 1) for k, _b in n.items))
    run = S.runs[0]

    def Parcel(o, runs, key=(S.fid, 1)):
        bt.Reitem(o, sleaf, lambda items: [(key, tally.ParcelEncode(runs, bt.plan.seq))
                                           if k == (S.fid, 1) else (k, b) for k, b in items])

    def Longer(o):
        bt.Reitem(o, sleaf, lambda items: [(k, b + b"\0") if k == (S.fid, 0) else (k, b) for k, b in items])

    shared = tally.Run(False, run.start, run.count, run.birth, ext=bookread.EntryPut(tally.EXT_SHARED, b""))
    return [
        Case("B7 a Parcel with a hole before it", lambda o: Parcel(o, S.runs, key=(S.fid, 2)),
             must={"PARCEL_GAP", "HELD_BLOCKS"}, checks=[
                 ("%s is locked: its Parcels do not cover block 0" % Label(S),
                  lambda c: S.fid in c.mount.locked and Found(c, "PARCEL_GAP", "fid %u" % S.fid)),
                 ("the blocks it names are held", lambda c: c.held == [(run.start, run.count)])]),
        Case("B8 two files name one block",
             lambda o: Parcel(o, [tally.Run(False, L.runs[0].start, S.Blocks(), run.birth)]),
             must={"DOUBLE_OWNER"}, checks=[
                 ("the double names %s and %s" % (Label(S), Label(L)),
                  lambda c: Found(c, "DOUBLE_OWNER", Label(S), Label(L)))]),
        Case("B9 a FILES header one byte longer than known", Longer, must={"UNDER_GLASS"}, checks=[
            ("the volume is read only: written by a kernel that knows more",
             lambda c: Glass(c, "knows more", "FILES")),
            ("and every file still reads", lambda c: set(c.mount.files) == set(bt.content.files))]),
        Case("B31 a block of a file rotten", lambda o: bt.Rot(o, run.start * SECTORS_PER_BLOCK + 3),
             must={"DIGEST_MISMATCH"}, checks=[
                 ("%s's block %u does not match its digest" % (Label(S), run.start),
                  lambda c: Found(c, "DIGEST_MISMATCH", Label(S), "%u+1" % run.start))]),
        Case("B12 a run carries SHARED", lambda o: Parcel(o, [shared]), must={"UNDER_GLASS"}, checks=[
            ("the volume is read only: a run's extension this kernel cannot carry on",
             lambda c: Glass(c, "extension")),
            ("%s still reads" % Label(S), lambda c: c.Sha256(S.fid) == bt.c0.Sha256(S.fid))]),
    ]


def SlotRootCases(bt):
    g = bt.ground

    def Bookmarks(o):
        free = bt.plan.FreeSheet()
        binder.NodeWrite(o, g, bt.plan.seq, free, 1, tally.BOOKMARKS, 0, [(1, b"\x01\x02\x03")])
        slot = tally.SlotRead(BookGround(o), 0)
        slot.roots[tally.BOOKMARKS] = tally.Root(0, 0, 1, bookread.PtrPut(bt.plan.seq, 1, free))
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, 3, slot)))

    def Unread(o):
        """Slot 1 at seq 3, its TAGS root a node whose keys do not rise: its
        second item under its first one's key."""
        free, tags = bt.plan.FreeSheet(), bt.Node(tally.TAGS, level=bt.plan.roots[tally.TAGS].level, index=0)
        items = list(tags.items)
        items[1] = (items[0][0], items[1][1])
        binder.NodeWrite(o, g, bt.plan.seq, free, tags.sheets, tally.TAGS, tags.level, items)
        slot = tally.SlotRead(BookGround(o), 0)
        root = slot.roots[tally.TAGS]
        slot.roots[tally.TAGS] = tally.Root(root.flags, root.level, root.items,
                                            bookread.PtrPut(bt.plan.seq, tags.sheets, free))
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, 3, slot)))

    def Wide(o, frame_bytes=32, slots=None):
        """The Deed sealed again: its FRAME `frame_bytes` long, or its ring of
        `slots` — what a kernel that knows more, or another maker, wrote."""
        deed = BookGround(o).deed
        frame = dict(deed["frame"], spine_slots=slots or deed["frame"]["spine_slots"])
        boot = tuple(deed["boot"][k] for k in ondisk.VOLUME_BOOT.names)
        for role, vlba in ((ondisk.DEED_ROLE_HEAD, 0), (ondisk.DEED_ROLE_TAIL, deed["tail_sector"])):
            o.Put(g.base + vlba, FrameDeed(deed, frame, boot, role, frame_bytes=frame_bytes))

    return [
        Case("B13 BOOKMARKS not empty", Bookmarks, must={"UNDER_GLASS"}, checks=[
            ("the newest slot, 1 at seq 3, is stood on", lambda c: c.mount.slot.index == 1),
            ("and the volume is read only: a bookmark this kernel cannot hold",
             lambda c: Glass(c, "BOOKMARKS"))]),
        Case("B25 a FRAME of 32 bytes", Wide, must={"UNDER_GLASS"}, checks=[
            ("the volume is read only: its FRAME was written by a kernel that knows more",
             lambda c: Glass(c, "FRAME", "knows more"))]),
        Case("B41 a Spine ring of two slots", lambda o: Wide(o, frame_bytes=24, slots=2), must={"FRAME_FOREIGN"},
             checks=[("a volume of another maker, said; it mounts on slot 0 and is not under glass",
                      lambda c: Found(c, "FRAME_FOREIGN", "2 slots") and c.mount.slot.index == 0 and
                      not c.mount.glass)]),
        Case("B34 the newest slot's Tally does not read", Unread, must={"SLOT_FELL_BACK"}, checks=[
            ("slot 1's TAGS root does not parse in either copy, and slot 0 is stood on",
             lambda c: Found(c, "SLOT_FELL_BACK", "slot 1", "TAGS", "do not rise") and c.mount.slot.index == 0),
            ("and every file reads", lambda c: set(c.mount.files) == set(bt.content.files))]),
        Case("B35 no slot holds", lambda o: bt.Zero(o, tally.SlotBlock(g, 0) * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK),
             must={"SPINE_NONE"}, checks=[
                 ("the book does not mount: none of the slots holds",
                  lambda c: c.mount.refusal is not None and "none of the 3 slots" in c.mount.refusal),
                 ("and no file is listed", lambda c: not c.mount.files)]),
    ]
