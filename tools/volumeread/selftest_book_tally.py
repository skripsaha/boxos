"""selftest_book_tally — the book's landings in its Spine and its Tally: the
base as a binder lays it and read back byte for byte, a torn slot, a node of a
foreign seq or rotten in one copy or both, a Parcel that does not cover its
file, a run SHARED, BOOKMARKS not empty, a FRAME longer than known, a slot
whose Tally does not read, no slot at all. Each a fork of the base
selftest_book.py lays; what the book's census must say of it."""

from . import binder, bookread, ondisk, stitch, stitch_pen, tally
from .bookground import BookGround
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK, RunsIntersect, RunsNormal
from .selftest_book import LANDING_ROOM, PADDING, Born, Case, Found, FrameDeed, Glass, Label


def Pristine(bt):
    """B0: what the base must read as — no finding at all, every file whole,
    and the bounds its binder wrote."""
    return [Case("B0 the book as a binder lays it", lambda o: None, checks=[
        ("not one finding — 0 damage, 0 leak, 0 note", lambda c: not c.findings),
        ("the book mounts on slot 0, at seq 2, not under glass",
         lambda c: c.mount.refusal is None and c.mount.slot.index == 0 and c.mount.slot.seq == 2 and
         not c.mount.glass),
        ("every live file reads", lambda c: all(c.Sha256(fid) for fid in c.mount.files)),
        ("the next file id %u, tag id %u, bookmark 1, seq 3" % (bt.fid, bt.tag),
         lambda c: c.mount.counts == {"next_fid": bt.fid, "next_tag": bt.tag, "next_bookmark": 1,
                                      "next_seq": 3}),
        ("the kernel of this tree mounts it on slot 0, and nothing holds it to glass: its writer goes on where "
         "the ribbon ends",
         lambda c: c.kernel["mounts"] is True and c.kernel["slot"] == 0 and c.kernel["glass"] == 0 and
         c.kernel["files"] == len(c.mount.files) and not c.kernel["locked"] and not c.kernel["rotten"]),
        ("the front, the Spine, the Tally, the ribbon, the files, BOOT and the far Deed are heard",
         lambda c: {"front", "spine", "tally", "ribbon", "file", "boot", "far_deed"} <= c.Kinds())])]


def LaidAfter(bt, c):
    """Where a binder lays the book of what the base's mount read from
    scratch, against where it lies (§16.3 п.5): Tally-Quire A at the first
    grain at or past the end of the files, B as many grains on, the ribbon's
    Quire and its next_quire the two grains after B. [] when they agree."""
    files = c.mount.tally.content.files.values()
    after = max([c.g.data_block] + [r.start + r.count for fs in files for r in fs.runs if not r.hole])
    fresh = binder.Bind(c.mount.tally.content, c.g, after=after)
    want = (fresh.a, fresh.b, tuple(fresh.ribbon), tuple(fresh.next))
    have = (bt.plan.a, bt.plan.b, tuple(bt.plan.ribbon), tuple(bt.plan.next))
    return [] if want == have else ["laid from block %u: A %u, B %u, ribbon %s, next %s; it lies at A %u, B %u, "
                                    "ribbon %s, next %s" % ((after,) + want + have)]


def Circles(bt):
    """B0b, B0c, B0d: what is read back and written again is the same bytes,
    and the book lies where a binder lays it."""
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
            stitch.Labeled(fid=L.fid, base=seq - L.gen, name=b"renamed", flags=(ondisk.FILE_LAID_BIT, 0),
                           add=[bt.tag], drop=list(L.tags[:1])),
            stitch.Quitclaim(fid=gone, base=seq - 1)], flags=stitch.DATA_FLUSHED)
        second = pen.Sew([stitch.Undone(seq_back=1, why=1, fids=[bt.fid])], flags=stitch.DATA_FLUSHED)
        return [first, second]

    sewn = {}
    return [
        Case("B0b the tally read back is bound again byte for byte", lambda o: None,
             checks=[("every node, both copies, and slot 0 come out the same",
                      lambda c: binder.Circle(c.medium, c.mount) == [])]),
        Case("B0d the book lies where a binder lays it", lambda o: None, checks=[
            ("Tally-Quire A at the first grain past the files, B k grains on, the ribbon and its next_quire "
             "the grains after (§16.3 п.5)", lambda c: LaidAfter(bt, c) == []),
            ("slot 0 was made at the Deed's BORN, in nanoseconds",
             lambda c: c.mount.slot.made_ns == c.g.deed["born"]["created_unix"] * 10 ** 9)]),
        Case("B0c every record sewn, read back and sewn again is byte for byte",
             lambda o: sewn.setdefault("s", StitchCircle(o)), checks=[
                 ("both stitches are taken whole and each record decodes to what was sewn",
                  lambda c: stitch_pen.Circle(c.mount.ribbon, sewn["s"]) == []),
                 ("they apply: %s renamed, fid %u gone, fid %u undone" % (Label(L), gone, bt.fid),
                  lambda c: c.mount.files[L.fid].name == b"renamed" and gone not in c.mount.files and
                  bt.fid not in c.mount.files and bt.tag in c.mount.files[L.fid].tags),
                 ("and the Slip is carried on: no glass (§16.5 п.13)", lambda c: not c.mount.glass)])]


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
    fdig = [n for n in plan.nodes if n.tree == tally.DIGESTS and n.level == 0][0]
    locked = sorted({k[0] for k, _b in leaf.items})
    whole = [fid for fid in locked if (fid, 0) in {k for k, _b in leaf.items}]
    held = RunsNormal([(r.start, r.count) for fid in locked for r in content.files[fid].runs if not r.hole])
    end = bt.ground.data_end

    def RotFiles(o, digests=False, first=False):
        Lay(o)
        bt.RotNode(o, leaf)
        if digests:
            bt.RotNode(o, fdig if first else pdig)

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
        Case("B4p a FILES of two leaves, whole", Lay, checks=[
            ("not one finding", lambda c: not c.findings),
            ("its FILES tree is a root over two leaves or more",
             lambda c: sum(1 for n in c.mount.tally.trees[tally.FILES].nodes if n.level == 0) >= 2 and
             any(n.level >= 1 for n in c.mount.tally.trees[tally.FILES].nodes)),
            ("and the kernel of this tree mounts it with every file",
             lambda c: c.kernel["mounts"] is True and c.kernel["files"] == len(content.files))]),
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
                  lambda c: c.mount.counts["next_fid"] == content.next_fid),
                 ("and the kernel of this tree mounts it with the range rotten and the blocks held",
                  lambda c: c.kernel["mounts"] is True and [r["tree"] for r in c.kernel["rotten"]] == ["FILES"] and
                  c.kernel["held"] == ondisk.RunsCount(held))]),
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
        Case("B6c a FILES leaf and the first DIGESTS leaf rotten", lambda o: RotFiles(o, digests=True, first=True),
             must={"RANGE_ROTTEN", "HELD_BLOCKS"}, checks=[
                 ("the first DIGESTS leaf is a rotten range from the start, and ground past it is free",
                  lambda c: Found(c, "RANGE_ROTTEN", "DIGESTS") and c.kernel["free"] > 0)]),
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

    def Oversheet(o, reach):
        """The inner FILES root again, its last child's pointer naming as many
        Sheets as lie from the farther of its copies to block `reach` — a
        length the medium could hold and no node of this book has: each copy
        is a unit of one Sheet, so neither is the node the pointer names."""
        Lay(o)
        last, items = leaves[-1], []
        for k, body in inner.items:
            c = bookread.Cursor(body)
            n, p = c.V(), c.Ptr()
            if list(p["blocks"]) == list(last.blocks):
                body = tally.InnerEncode(n, bookread.PtrPut(p["seq"], reach - max(last.blocks), p["blocks"]))
            items.append((k, body))
        binder.NodeWrite(o, g, plan.seq, inner.blocks, inner.sheets, inner.tree, inner.level, items)

    # The ribbon's next Quire, past the Tally-Quires and the ribbon's own: a
    # rotten leaf's pointer that reaches onto its first block lays the Tally's
    # ground over that block and leaves the rest of it no one's.
    nxt = tuple(plan.next)
    if not nxt[1] or nxt[0] <= max(leaves[-1].blocks):
        raise Unreadable("the padded book's next Quire %u+%u does not lie past its last FILES leaf" % nxt)
    # The blocks a rotten range holds are its files' that no one else claims:
    # a pointer that reaches to the far Deed lays the Tally's claim over every
    # block past the leaf, so the range holds blocks only where a file of the
    # last leaf names ground short of it — which the layout of the image's own
    # files decides, not the landing.
    short = any(s < min(leaves[-1].blocks) for k, _b in leaves[-1].items for s, _n in content.files[k[0]].Ground())
    far = {"RANGE_ROTTEN", "DOUBLE_OWNER", "RIBBON_TORN"} | ({"HELD_BLOCKS"} if short else set())
    nxt_rest = lambda c: all(any(s <= b < s + n for x in c.claims if x.kind == "tally" for s, n in x.runs)
                             for b in range(nxt[0] + 1, sum(nxt)))

    def SlotOver(o, index, seq, files_root=None):
        """The slot the padded book was bound with, sealed again as slot
        `index` at `seq` — its FILES root, when given, a pointer elsewhere."""
        slot = tally.SlotRead(BookGround(o), 0)
        if files_root is not None:
            root = slot.roots[tally.FILES]
            slot.roots[tally.FILES] = tally.Root(root.flags, root.level, root.items, files_root)
        o.Put(g.base + tally.SlotBlock(g, index) * SECTORS_PER_BLOCK,
              b"".join(tally.SlotSectors(g, index, seq, slot)))

    def NewerUnread(o):
        """Slot 1 at seq 3, whose FILES root — an inner node — names a Sheet
        no node was written to: the mount falls back to slot 0, and slot 1
        is walked for its ground, the root that does not read with it."""
        Lay(o)
        SlotOver(o, 1, 3, bookread.PtrPut(plan.seq, inner.sheets, plan.FreeSheet()))

    def OlderCut(o):
        """Slot 1 at seq 3 is the book as bound; slot 0 names, for FILES, the
        inner root written again at a free Sheet with its second child's
        pointer cut off after its count — a slot not stood on, walked for
        its ground past a pointer that does not read."""
        Lay(o)
        SlotOver(o, 1, 3)
        free, items = plan.FreeSheet(), list(inner.items)
        items[1] = (items[1][0], tally.InnerEncode(bookread.Cursor(items[1][1]).V(), b""))
        binder.NodeWrite(o, g, plan.seq, free, inner.sheets, tally.FILES, inner.level, items)
        SlotOver(o, 0, 2, bookread.PtrPut(plan.seq, inner.sheets, free))

    leaf = bt.Node(tally.FILES)
    births = lambda c: {r.birth for fs in c.mount.files.values() for r in fs.runs if not r.hole}
    padded = lambda c: set(c.mount.files) == set(content.files)
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
        Case("B38b a FILES leaf's pointer names Sheets to the far Deed", lambda o: Oversheet(o, g.data_end),
             must=far, checks=[
                 ("the leaf is a rotten range: neither copy is a unit that long",
                  lambda c: Found(c, "RANGE_ROTTEN", "FILES")),
                 ("and the ground its parent names for it — every block to the far Deed — is the Tally's, "
                  "said where a file names it too", lambda c: Found(c, "DOUBLE_OWNER", "the Tallies' nodes")),
                 ("so the next Quire the slot names lies on the book, and the ribbon is not taken there",
                  lambda c: Found(c, "RIBBON_TORN", "Quire %u+%u" % nxt)),
                 ("and the kernel of this tree mounts it with the range's files locked",
                  lambda c: c.kernel["mounts"] is True and c.kernel["rotten"] != [])]),
        Case("B38c a FILES leaf's pointer names Sheets onto the first block of the next Quire",
             lambda o: Oversheet(o, nxt[0] + 1), must={"RANGE_ROTTEN", "HELD_BLOCKS", "DOUBLE_OWNER", "RIBBON_TORN"},
             checks=[
                 ("the leaf is a rotten range, its ground the Tally's", lambda c: Found(c, "RANGE_ROTTEN", "FILES")),
                 ("the next Quire the slot names lies on that ground by its first block: the ribbon is not "
                  "taken there, as the kernel's census does not take it (book_ground.c BookGroundRibbon)",
                  lambda c: Found(c, "RIBBON_TORN", "Quire %u+%u" % nxt)),
                 ("and the rest of that Quire is the Tallies' too: a Tally-Quire is held whole while one node "
                  "in it lives, and a dead node's ground or a tail no Tally reached is given to no one (§16.5 "
                  "п.17, Р-d3; mount/ground_census.c GroundQuiresHeld)", nxt_rest),
                 ("and the kernel of this tree mounts it with the range's files locked",
                  lambda c: c.kernel["mounts"] is True and c.kernel["rotten"] != [])]),
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
        Case("B42 the newest slot's inner FILES root does not read", NewerUnread, must={"SLOT_FELL_BACK"},
             checks=[
                 ("slot 1's FILES root holds in neither copy, and slot 0 is stood on",
                  lambda c: Found(c, "SLOT_FELL_BACK", "slot 1", "FILES") and c.mount.slot.index == 0),
                 ("every file reads", padded),
                 ("and the kernel of this tree mounts it on slot 0 with every file",
                  lambda c: c.kernel["mounts"] is True and c.kernel["slot"] == 0 and
                  c.kernel["files"] == len(content.files))]),
        Case("B42b a slot not stood on names a child by a pointer cut short", OlderCut, checks=[
            ("the newest slot, 1 at seq 3, is stood on, and nothing is said",
             lambda c: c.mount.slot.index == 1 and not c.findings),
            ("every file reads", padded),
            ("and the kernel of this tree mounts it on slot 1 with every file",
             lambda c: c.kernel["mounts"] is True and c.kernel["slot"] == 1 and
             c.kernel["files"] == len(content.files))]),
    ]


ELEMENT_ROOM = 64                 # bytes free in the target's leaf of FILES the element landings need


def FilesLeaves(bt):
    """The leaves of FILES the book is laid in, and the bytes each has free in
    its Sheets."""
    leaves = [n for n in bt.plan.nodes if n.tree == tally.FILES and n.level == 0]
    free = {}
    for n in leaves:
        stream, _starts = binder.NodeStream(n.tree, n.level, list(n.items))
        free[id(n)] = n.sheets * tally.SHEET_PAYLOAD - len(stream)
    return leaves, free


def ElementTarget(bt):
    """The file the element landings aim at, and its leaf of FILES: the
    smallest live file whose leaf has ELEMENT_ROOM bytes free. A landing here
    lays an item of the target's again in place, and B7c's two runs past 2^62
    in place of a one-block file's Parcel grow its leaf by 28 bytes — the most
    of any. A book's leaves are laid as full as their items go, so the
    smallest file alone (bt.S) may sit in a leaf with fewer bytes free than
    that; which file has room is the image's, never a reason to refuse."""
    leaves, free = FilesLeaves(bt)
    for fs in sorted((fs for fs in bt.content.files.values() if fs.Blocks() >= 1 and fs.fid != bt.c0.boot_fid
                      and fs is not bt.L), key=lambda fs: (fs.Blocks(), fs.fid)):
        leaf = next((n for n in leaves if any(k == (fs.fid, 1) for k, _b in n.items)), None)
        if leaf is not None and free[id(leaf)] >= ELEMENT_ROOM:
            return fs, leaf
    raise Unreadable("no file but the largest has %u bytes free in its leaf of FILES for the element landings"
                     % ELEMENT_ROOM)


def MeetTarget(bt):
    """The file whose one run B45 cuts in two that meet on a block, and its
    leaf of FILES: the largest live file of one run of at least two blocks
    whose leaf has ELEMENT_ROOM bytes free — the second run is written in the
    Parcel's place, and the leaf is as full as its items go, so the largest
    file (bt.L) can sit in a leaf with no room for it (a book of more files
    than the first images had did). Which file has room is the image's, never
    a reason to refuse."""
    leaves, free = FilesLeaves(bt)
    for fs in sorted((fs for fs in bt.content.files.values() if fs.fid != bt.c0.boot_fid and len(fs.runs) == 1 and
                      not fs.runs[0].hole and fs.runs[0].count >= 2), key=lambda fs: (-fs.Blocks(), fs.fid)):
        leaf = next((n for n in leaves if any(k == (fs.fid, 1) for k, _b in n.items)), None)
        if leaf is not None and free[id(leaf)] >= ELEMENT_ROOM:
            return fs, leaf
    raise Unreadable("no file of one run of two blocks or more has %u bytes free in its leaf of FILES for B45"
                     % ELEMENT_ROOM)


def ElementCases(bt):
    (S, sleaf), L = ElementTarget(bt), bt.L
    run = S.runs[0]

    def Parcel(o, runs, key=(S.fid, 1)):
        bt.Reitem(o, sleaf, lambda items: [(key, tally.ParcelEncode(runs, bt.plan.seq))
                                           if k == (S.fid, 1) else (k, b) for k, b in items])

    def Longer(o):
        bt.Reitem(o, sleaf, lambda items: [(k, b + b"\0") if k == (S.fid, 0) else (k, b) for k, b in items])

    def Wide(o):
        wide = S.Copy()
        wide.flags |= 1 << 40
        bt.Reitem(o, sleaf, lambda items: [(k, tally.FileHeadEncode(wide)) if k == (S.fid, 0) else (k, b)
                                           for k, b in items])

    M, mleaf = MeetTarget(bt)
    mrun = M.runs[0]
    half = max(1, mrun.count // 2)

    def SelfMeet(o):
        """M's one run cut in two that meet on a block: [a, a + k) and
        [a + k - 1, a + n - 1) — as many blocks as its size takes, its last
        block named by no one."""
        runs = [tally.Run(False, mrun.start, half, mrun.birth),
                tally.Run(False, mrun.start + half - 1, mrun.count - half, mrun.birth)]
        bt.Reitem(o, mleaf, lambda items: [(k, tally.ParcelEncode(runs, bt.plan.seq)) if k == (M.fid, 1)
                                           else (k, b) for k, b in items])

    return [
        Case("B7 a Parcel with a hole before it", lambda o: Parcel(o, S.runs, key=(S.fid, 2)),
             must={"PARCEL_GAP", "HELD_BLOCKS"}, checks=[
                 ("%s is locked: its Parcels do not cover block 0" % Label(S),
                  lambda c: S.fid in c.mount.locked and Found(c, "PARCEL_GAP", "fid %u" % S.fid)),
                 ("the blocks it names are held", lambda c: c.held == [(run.start, run.count)])]),
        Case("B7b a run born before seq 0", lambda o: Parcel(o, [tally.Run(False, run.start, run.count, -1)]),
             must={"PARCEL_GAP"}, checks=[
                 ("%s is locked: its Parcel does not read, as the kernel's codec refuses it (TallyRunGet)" % Label(S),
                  lambda c: S.fid in c.mount.locked and Found(c, "PARCEL_GAP", "fid %u" % S.fid, "before seq 0"))]),
        Case("B7c a run past block 2^64 - 1",
             lambda o: Parcel(o, [tally.Run(False, 1 << 62, (1 << 62) - 1, run.birth),
                                  tally.Run(False, bookread.U64 - 1, 3, run.birth)]),
             must={"PARCEL_GAP"}, checks=[
                 ("%s is locked: its Parcel does not read, as the kernel's codec refuses it (TallyRunGet)" % Label(S),
                  lambda c: S.fid in c.mount.locked and Found(c, "PARCEL_GAP", "fid %u" % S.fid, "2^64 - 1"))]),
        Case("B57 a run past the far Deed",
             lambda o: Parcel(o, [tally.Run(False, bt.ground.data_end + 1, S.Blocks(), run.birth)]),
             must={"EXTENT_OUTSIDE"}, checks=[
                 ("%s is locked by the Tally for the ground past the far Deed it names" % Label(S),
                  lambda c: c.mount.locked.get(S.fid) == tally.OUTSIDE and
                  Found(c, "EXTENT_OUTSIDE", "fid %u" % S.fid, "%u+%u" % (bt.ground.data_end + 1, S.Blocks()))),
                 ("and the volume is not put under glass for it, as the kernel's Tally does not",
                  lambda c: not c.mount.glass)]),
        Case("B8 two files name one block",
             lambda o: Parcel(o, [tally.Run(False, L.runs[0].start, S.Blocks(), run.birth)]),
             must={"DOUBLE_OWNER"}, checks=[
                 ("the double names %s and %s" % (Label(S), Label(L)),
                  lambda c: Found(c, "DOUBLE_OWNER", Label(S), Label(L)))]),
        Case("B45 two runs of one file meet on a block", SelfMeet, must={"DOUBLE_OWNER"}, checks=[
            ("the block both of %s's runs name is said to have two owners, %s twice" % (Label(M), Label(M)),
             lambda c: any(x.code == "DOUBLE_OWNER" and x.runs == [(mrun.start + half - 1, 1)] and
                           x.text.count(Label(M)) == 2 for x in c.findings)),
            ("and the kernel of this tree names that block, and how many",
             lambda c: c.kernel["double"] == [[mrun.start + half - 1, 1]])]),
        Case("B9 a FILES header one byte longer than known", Longer, must={"UNDER_GLASS"}, checks=[
            ("the volume is read only: written by a kernel that knows more",
             lambda c: Glass(c, "knows more", "FILES")),
            ("and every file still reads", lambda c: set(c.mount.files) == set(bt.content.files))]),
        Case("B9b a FILES header whose flags reach past 32 bits", Wide, must={"UNDER_GLASS"}, checks=[
            ("the volume is read only: %s carries flags this kernel cannot hold" % Label(S),
             lambda c: Glass(c, "fid %u" % S.fid, "flags past the 32 bits")),
            ("and every file still reads", lambda c: set(c.mount.files) == set(bt.content.files))]),
        Case("B31 a block of a file rotten", lambda o: bt.Rot(o, run.start * SECTORS_PER_BLOCK + 3),
             must={"DIGEST_MISMATCH"}, checks=[
                 ("%s's block %u does not match its digest" % (Label(S), run.start),
                  lambda c: Found(c, "DIGEST_MISMATCH", Label(S), "%u+1" % run.start))]),
    ] + SharedCases(bt, S, Parcel)


def SharedCases(bt, S, Parcel):
    """A run that carries SHARED (§8.2): ground it names that other runs may
    name too. Its file reads through it as through DATA, its blocks held to
    the block's one digest; the census takes it for an owner beside the one
    DATA run of the block, or alone when the DATA owner has gone — never on a
    node of the book, and never on ground no one ever wrote, which has no
    digest to hold a read to. A run whose list of extensions names anything
    but one empty SHARED is not carried on."""
    run, L = S.runs[0], bt.L
    head = min(S.size, BLOCK)

    def Shared(start, ext=None):
        """S's runs, its first block a run of one SHARED block at `start`."""
        one = tally.Run(False, start, 1, run.birth, ext=bookread.EntryPut(tally.EXT_SHARED, b"") if ext is None
                        else ext)
        rest = [tally.Run(False, run.start + 1, run.count - 1, run.birth)] if run.count > 1 else []
        return lambda o: Parcel(o, [one] + rest + S.runs[1:])

    node = next(x for x in bt.plan.nodes if x.tree == tally.TAGS).blocks[0]
    mounts = ("and the kernel of this tree mounts it, not under glass, two owners of a block said nowhere",
              lambda c: c.kernel["mounts"] is True and c.kernel["glass"] == 0 and c.kernel["double"] == [])
    return [
        Case("B12 a run carries SHARED, its file the one owner of its ground", Shared(run.start), checks=[
            ("the volume is not put under glass: SHARED is carried on", lambda c: not c.mount.glass),
            ("%s still reads, and nothing is said" % Label(S),
             lambda c: c.Sha256(S.fid) == bt.c0.Sha256(S.fid) and not c.findings),
            ("its first block is claimed as shared by it",
             lambda c: any(x.kind == "shared" and x.runs == [(run.start, 1)] for x in c.claims)),
            ("and the files the census prints name that run shared and the rest as they were",
             lambda c: [r[0] for r in next(f for f in c.Json()["files"] if f["fid"] == S.fid)["runs"]] ==
             ["shared"] + ["hole" if r.hole else "data" for r in ([run] if run.count > 1 else []) + S.runs[1:]]),
            mounts]),
        Case("B12b a run SHARED over the DATA of another live file", Shared(L.runs[0].start), checks=[
            ("nothing is said: one DATA owner and one shared on the block", lambda c: not c.findings),
            ("%s reads the block of %s it shares, and its own past it" % (Label(S), Label(L)),
             lambda c: c.Content(S.fid)[:head] == bt.c0.Content(L.fid)[:head] and
             c.Content(S.fid)[head:] == bt.c0.Content(S.fid)[head:]),
            ("its own first block is no one's now",
             lambda c: not any(s <= run.start < s + k for x in c.claims for s, k in x.runs)), mounts]),
        Case("B12c a run SHARED on ground no one wrote", Shared(bt.F), must={"DIGEST_MISMATCH"}, checks=[
            ("%s's block carries no digest: a read of it is refused" % Label(S),
             lambda c: Found(c, "DIGEST_MISMATCH", Label(S), "%u+1" % bt.F, "carry no digest")),
            ("and no two owners of a block are said", lambda c: not Found(c, "DOUBLE_OWNER")), mounts]),
        Case("B12d a run SHARED on a node of the Tally", Shared(node), must={"DOUBLE_OWNER", "DIGEST_MISMATCH"},
             checks=[("the block is said to be %s's and the Tallies' nodes' at once, and carries no digest"
                      % Label(S), lambda c: Found(c, "DOUBLE_OWNER", Label(S), "the Tallies' nodes")),
                     ("and the kernel of this tree names that block", lambda c: [node, 1] in c.kernel["double"])]),
        Case("B12e a run SHARED and CODEC", Shared(run.start, bookread.EntryPut(tally.EXT_SHARED, b"") +
                                                 bookread.EntryPut(tally.EXT_CODEC, b"\x01\x01")),
             must={"UNDER_GLASS"}, checks=[("the volume is read only: a run's extension this kernel cannot carry on",
                                            lambda c: Glass(c, "extension"))]),
        Case("B12f a run SHARED with a body", Shared(run.start, bookread.EntryPut(tally.EXT_SHARED, b"\x00")),
             must={"UNDER_GLASS"}, checks=[("the volume is read only: SHARED's body is empty in this reader's book",
                                            lambda c: Glass(c, "extension"))]),
    ]


def PlantBookmark(bt, o):
    """Slot 1 at seq 3, slot 0's Tally with a BOOKMARKS root of one item."""
    g = bt.ground
    free = bt.plan.FreeSheet()
    binder.NodeWrite(o, g, bt.plan.seq, free, 1, tally.BOOKMARKS, 0, [(1, b"\x01\x02\x03")])
    slot = tally.SlotRead(BookGround(o), 0)
    slot.roots[tally.BOOKMARKS] = tally.Root(0, 0, 1, bookread.PtrPut(bt.plan.seq, 1, free))
    o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, 3, slot)))


def SlotRootCases(bt):
    g = bt.ground

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

    def RootOver(o, tree, blocks):
        """Slot 1 at seq 3, the bound slot sealed again with its root of
        `tree` named by a pointer to `blocks` — its seq and Sheets kept."""
        slot = tally.SlotRead(BookGround(o), 0)
        root = slot.roots[tree]
        ptr = root.Pointer()
        slot.roots[tree] = tally.Root(root.flags, root.level, root.items,
                                      bookread.PtrPut(ptr["seq"], ptr["sheets"], blocks))
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, 3, slot)))

    files = bt.Node(tally.FILES, level=bt.plan.roots[tally.FILES].level, index=0)
    # Copies in the landings' room, a Sheet run apart, where nothing was
    # written as a node: copies that do not hold, and ground all the same.
    blank = [bt.F + k * files.sheets for k in range(11)]
    if (len(blank) + 1) * files.sheets > LANDING_ROOM:
        raise Unreadable("twelve copies of the FILES root, %u Sheet(s) each, do not fit the landings' room of "
                         "%u blocks" % (files.sheets, LANDING_ROOM))

    def RootFar(o, failing):
        """Slot 1 at seq 3 naming its FILES root by `failing` blank copies
        and, after them, one more where the root is written whole — a block
        no other slot names, so that only a reader that claims every copy
        the pointer names counts it taken."""
        whole = bt.F + failing * files.sheets
        binder.NodeWrite(o, g, bt.plan.seq, [whole], files.sheets, tally.FILES, files.level, files.items)
        RootOver(o, tally.FILES, blank[:failing] + [whole])
        return whole
    tags = bt.Node(tally.TAGS, level=bt.plan.roots[tally.TAGS].level, index=0)
    twin = next((n for n in bt.plan.nodes if n.tree != tally.TAGS and n.sheets == tags.sheets), None)
    if twin is None:
        raise Unreadable("the Tally has no node of %u Sheet(s) beside the TAGS root" % tags.sheets)

    def QuiresTaken(c, blocks):
        """The free ground of the book before (c0) the Quires a node of
        `files.sheets` at each of `blocks` lies in take — a Tally-Quire held
        whole while one node in it lives (§16.5 п.17), the stood slot's ribbon
        Quire long, on the grid of the volume's blocks."""
        q = c.mount.slot.ribbon["quire_blocks"]
        quires = RunsNormal([(b // q * q, -(-(b + files.sheets) // q) * q - b // q * q) for b in blocks])
        return sum(n for _s, n in RunsIntersect(bt.c0.free_runs, quires))

    def Tallied(c, blocks):
        runs = [r for x in c.claims if x.kind == "tally" for r in x.runs]
        return all(any(s <= b < s + n for s, n in runs) for b in blocks)

    def Wide(o, frame_bytes=32, slots=None):
        """The Deed sealed again: its FRAME `frame_bytes` long, or its ring of
        `slots` — what a kernel that knows more, or another maker, wrote."""
        deed = BookGround(o).deed
        frame = dict(deed["frame"], spine_slots=slots or deed["frame"]["spine_slots"])
        boot = tuple(deed["boot"][k] for k in ondisk.VOLUME_BOOT.names)
        for role, vlba in ((ondisk.DEED_ROLE_HEAD, 0), (ondisk.DEED_ROLE_TAIL, deed["tail_sector"])):
            o.Put(g.base + vlba, FrameDeed(deed, frame, boot, role, frame_bytes=frame_bytes))

    return [
        Case("B13 BOOKMARKS not empty", lambda o: PlantBookmark(bt, o), must={"UNDER_GLASS"}, checks=[
            ("the newest slot, 1 at seq 3, is stood on", lambda c: c.mount.slot.index == 1),
            ("and the volume is read only: a bookmark this kernel cannot hold",
             lambda c: Glass(c, "BOOKMARKS")),
            ("and its free blocks are not counted, by this reader or the kernel of this tree: the ground a "
             "bookmark holds is derived by neither",
             lambda c: c.Json()["census"]["free"] is None and c.kernel["mounts"] is True and
             c.kernel["free"] is None)]),
        Case("B25 a FRAME of 32 bytes", Wide, must={"UNDER_GLASS"}, checks=[
            ("the volume is read only: its FRAME was written by a kernel that knows more",
             lambda c: Glass(c, "FRAME", "knows more"))]),
        Case("B41 a Spine ring of two slots", lambda o: Wide(o, frame_bytes=24, slots=2), must={"FRAME_FOREIGN"},
             checks=[("a volume of another maker, said; it mounts on slot 0 and is not under glass",
                      lambda c: Found(c, "FRAME_FOREIGN", "2 slots") and c.mount.slot.index == 0 and
                      not c.mount.glass)]),
        Case("B43 a FILES root named by five copies, the first four not holding",
             lambda o: RootFar(o, 4), must={"NODE_COPY_ROTTEN"}, checks=[
                 ("slot 1 is stood on, its FILES root read off its fifth copy",
                  lambda c: c.mount.slot.index == 1 and
                  sorted(k for n, k, _w in c.mount.tally.mended if n.tree == tally.FILES) == [0, 1, 2, 3]),
                 ("every copy the pointer names is the Tally's ground — the fifth, which no other slot names, "
                  "among them", lambda c: Tallied(c, blank[:5])),
                 ("and every file reads", lambda c: set(c.mount.files) == set(bt.content.files)),
                 ("and the kernel of this tree mounts it on slot 1, four copies read past and each named, the "
                  "Quires its five Sheets lie in taken whole",
                  lambda c: c.kernel["mounts"] is True and c.kernel["slot"] == 1 and c.kernel["mended"] == 4 and
                  c.kernel["mended_said"] == 4 and
                  c.kernel["free"] == bt.c0.kernel["free"] - QuiresTaken(c, blank[:4] + [bt.F + 4 * files.sheets]))]),
        Case("B43c a FILES root named by twelve copies, the first eleven not holding",
             lambda o: RootFar(o, 11), must={"NODE_COPY_ROTTEN"}, checks=[
                 ("slot 1 is stood on, its FILES root read off its twelfth copy",
                  lambda c: c.mount.slot.index == 1 and
                  sorted(k for n, k, _w in c.mount.tally.mended if n.tree == tally.FILES) == list(range(11))),
                 ("every copy the pointer names is the Tally's ground", lambda c: Tallied(c, blank + [bt.F + 11 *
                                                                                                  files.sheets])),
                 ("and the kernel of this tree mounts it on slot 1, eleven copies read past — the first four "
                  "named one by one, the rest by their count — and the Quires its twelve Sheets lie in taken whole",
                  lambda c: c.kernel["mounts"] is True and c.kernel["slot"] == 1 and c.kernel["mended"] == 11 and
                  c.kernel["mended_said"] == 4 and
                  c.kernel["free"] == bt.c0.kernel["free"] - QuiresTaken(c, blank + [bt.F + 11 * files.sheets]))]),
        Case("B43b a FILES root named by three copies, the third never read",
             lambda o: RootOver(o, tally.FILES, list(files.blocks) + blank[:1]), checks=[
                 ("slot 1 is stood on, and nothing is said", lambda c: c.mount.slot.index == 1 and not c.findings),
                 ("the third copy is the Tally's ground, though no read reaches it",
                  lambda c: Tallied(c, blank[:1])),
                 ("and the kernel of this tree counts it taken, and the Quire it lies in",
                  lambda c: c.kernel["free"] == bt.c0.kernel["free"] - QuiresTaken(c, blank[:1]))]),
        Case("B44 two nodes of one Tally on one block",
             lambda o: RootOver(o, tally.TAGS, [tags.blocks[0], twin.blocks[1]]), must={"DOUBLE_OWNER"}, checks=[
                 ("slot 1 is stood on, its TAGS root read off its first copy",
                  lambda c: c.mount.slot.index == 1 and not c.mount.tally.mended),
                 ("the block its second copy shares with the %s node of level %u is said to have two owners"
                  % (tally.TREE_NAMES[twin.tree], twin.level),
                  lambda c: any(x.code == "DOUBLE_OWNER" and x.runs == [(twin.blocks[1], twin.sheets)]
                                for x in c.findings)),
                 ("and the kernel of this tree names that block, and how many",
                  lambda c: c.kernel["double"] == [[twin.blocks[1], twin.sheets]])]),
        Case("B34 the newest slot's Tally does not read", Unread, must={"SLOT_FELL_BACK"}, checks=[
            ("slot 1's TAGS root does not parse in either copy, and slot 0 is stood on",
             lambda c: Found(c, "SLOT_FELL_BACK", "slot 1", "TAGS", "do not rise") and c.mount.slot.index == 0),
            ("and every file reads", lambda c: set(c.mount.files) == set(bt.content.files))]),
        Case("B35 no slot holds", lambda o: bt.Zero(o, tally.SlotBlock(g, 0) * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK),
             must={"SPINE_NONE"}, checks=[
                 ("the book does not mount: none of the slots holds",
                  lambda c: c.mount.refusal is not None and "none of the 3 slots" in c.mount.refusal),
                 ("and no file is listed", lambda c: not c.mount.files),
                 ("and the kernel of this tree refuses it in the same words",
                  lambda c: c.kernel["mounts"] is False and
                  c.kernel["refusal"] == "none of the 3 slots of its Spine holds")]),
    ]
