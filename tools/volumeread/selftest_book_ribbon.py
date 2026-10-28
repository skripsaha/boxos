"""selftest_book_ribbon — the book's landings on its ribbon: every kind of
record a cycle sews and the rule each is replayed by (§3.7), and how the
ribbon goes on and ends (§2.7, §3.9) — torn, lied about, rotten, of another
history, what settled lost, an Undone, a Slip, a Quire run into its next, a
retry at the start of the next. Each a fork of the base selftest_book.py
lays; what the book's census must say of it."""

from . import bookread, ondisk, stitch, tally
from .bookground import BookGround
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK, RunsNormal
from .selftest_book import Born, Found, Glass, Label
from .selftest_cases import Case


def RecordNamed(c, base, also=()):
    """The sectors the landing laid over the book `base`, as one machine
    writes them (§3.3): its data, a flush, then the book's units, and after a
    flush `also` — (lba, sectors) of units written again as they lie, a Tally
    node and a slot — a record of them classified (blklog_book.py): a file's
    data named by the stitch after it, every unit by its sectors' heads, and
    none unowned."""
    from . import blklog
    from .selftest_cases import SyntheticLog
    g = c.g
    laid = {x: raw for x, raw in c.medium.over.items() if base.over.get(x) != raw}

    def Unit(lba):
        raw = laid[lba]
        return raw[4:bookread.HEAD_BYTES] != bytes(bookread.HEAD_BYTES - 4) and \
            bookread.SectorCheck(raw, g.uuid, lba - g.base)

    lbas = sorted(laid)
    entries = [(x, 0, laid[x]) for x in lbas if not Unit(x)] + [(0, blklog.LOG_FLUSH, b"")] + \
        [(x, 0, laid[x]) for x in lbas if Unit(x)] + [(0, blklog.LOG_FLUSH, b"")] + \
        [(lba + k, 0, c.medium.sectors(lba + k, 1)) for lba, n in also for k in range(n)]
    log = blklog.BlkLog(SyntheticLog(entries, len(entries)), base.SizeSectors())
    named = blklog.Classifier(log, base)
    kinds = [t.kind for w in log.Writes() for t in named.Classify(w)]
    need = {"stitch", "file"} | ({"sheet", "slot"} if also else set())
    return bool(kinds) and "unowned" not in kinds and need <= set(kinds)


def RecordCases(bt):
    f1, f2, f3 = bt.fid, bt.fid + 1, bt.fid + 2

    def Kind31(ignorable):
        def Plant(o):
            d = bt.Fresh(o, 0, 1, 5)
            bt.Pen(o).Sew([Born(bt.F, f1, d, b"kind31"), stitch.Unknown(31, ignorable, b"\x07\x07")],
                          flags=stitch.DATA_FLUSHED)
        return Plant

    def One(records):
        return lambda o: bt.Pen(o).Sew(records, flags=stitch.DATA_FLUSHED)

    again = stitch.Born(fid=bt.S.fid, flags=ondisk.FILE_ACTIVE, size=0, created_ns=1, modified_ns=1,
                        name=b"again", tags=(), runs=[], digests=[])

    def Quitclaimed(o):
        d = bt.Fresh(o, 0, 2, 2)
        bt.Pen(o).Sew([Born(bt.F, f1, d[:1], b"a"), Born(bt.F + 1, f2, d[1:], b"b"),
                       stitch.Quitclaim(fid=f2, base=0)], flags=stitch.DATA_FLUSHED)

    def Unlanded(o, torn):
        """A Born whose block holds what its digest does not name, in a stitch
        sewn without DATA_FLUSHED — and, `torn`, a stitch after it torn."""
        bt.Fresh(o, 0, 1, 9)
        pen = bt.Pen(o)
        pen.Sew([Born(bt.F, f2, [0x1234], b"unlanded")], flags=0)
        if torn:
            s = pen.Sew([stitch.UseContext(b"next")], flags=0)
            bt.Zero(o, s.vlbas[4], s.of - 4)

    leaf = bt.Node(tally.FILES)

    def Recorded(o):
        d = bt.Fresh(o, 0, 2, 5)
        bt.Pen(o).Sew([Born(bt.F, f1, d, b"recorded")], flags=stitch.DATA_FLUSHED)

    return [
        Case("B56 a stitch and its data as a machine's record writes them", Recorded, checks=[
            ("every write of the record is named — the data a file's, by the stitch after it; the stitch, a "
             "Tally node and a slot by their sectors' heads — none unowned",
             lambda c: RecordNamed(c, bt.base, also=[(c.g.base + leaf.blocks[0] * SECTORS_PER_BLOCK,
                                                      leaf.sheets * SECTORS_PER_BLOCK),
                                                     (c.g.base + tally.SlotBlock(c.g, 0) * SECTORS_PER_BLOCK,
                                                      SECTORS_PER_BLOCK)]))]),
        Case("B10 a record of kind 31 not to be stepped over", Kind31(0), must={"UNDER_GLASS"}, checks=[
            ("the volume is read only and the stitch is not applied",
             lambda c: Glass(c, "kind 31") and f1 not in c.mount.files),
            ("its file id is never given again", lambda c: c.mount.counts["next_fid"] == f1 + 1)]),
        Case("B11 a record of kind 31 to step over", Kind31(1), checks=[
            ("the stitch is applied: %u is born" % f1, lambda c: f1 in c.mount.files and not c.mount.glass),
            ("and nothing is said", lambda c: not c.findings)]),
        Case("B14 a Frozen in a stitch", One([stitch.Frozen(1, 0, [], b"snap")]), must={"UNDER_GLASS"},
             checks=[("the volume is read only: a bookmark", lambda c: Glass(c, "Frozen"))]),
        Case("B15 a Coined RETIRED", One([stitch.Coined(bt.tag, retired=True)]), must={"UNDER_GLASS"},
             checks=[("the volume is read only: an id retired", lambda c: Glass(c, "RETIRED"))]),
        Case("B16 a tag id of 2^31", One([stitch.Coined(1 << 31, b"wide")]), must={"UNDER_GLASS"}, checks=[
            ("the volume is read only: an id wider than the kernel gives", lambda c: Glass(c, "2^31")),
            ("and that id is never given again", lambda c: c.mount.counts["next_tag"] == (1 << 31) + 1)]),
        Case("B16b a tag key of 300 bytes", One([stitch.Coined(bt.tag, b"k" * 300)]), must={"UNDER_GLASS"},
             checks=[("the volume is read only: a tag wider than the kernel holds one",
                      lambda c: Glass(c, "wider than this kernel holds"))]),
        Case("B23 a stitch with no record", One([]), checks=[
            ("the stitch is taken, seq 3", lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3]),
            ("the next seq is 4, and nothing is said",
             lambda c: c.mount.counts["next_seq"] == 4 and not c.findings),
            ("and the kernel of this tree mounts it, the stitch sewn on and applied, seq 4 next",
             lambda c: c.kernel["mounts"] is True and (c.kernel["sewn"], c.kernel["last"], c.kernel["applied"],
                                                        c.kernel["next_seq"]) == (1, 3, 1, 4))]),
        Case("B26 the newest stitch's data never landed", lambda o: Unlanded(o, False),
             must={"NOT_LANDED", "UNDER_GLASS"}, checks=[
                 ("%u is rolled back by its splice, and the roll-back cannot be written by this kernel"
                  % f2, lambda c: f2 not in c.mount.files and Glass(c, "roll")),
                 ("its id is never given again, and no lie is said", lambda c: c.mount.counts["next_fid"] ==
                  f2 + 1 and not c.mount.ribbon.lied)]),
        Case("B26b the newest stitch's data never landed, a torn stitch after it", lambda o: Unlanded(o, True),
             must={"NOT_LANDED", "UNDER_GLASS", "RIBBON_TORN"}, checks=[
                 ("the medium did not keep what it confirmed: MEDIUM_LIED, said",
                  lambda c: c.mount.ribbon.lied and Found(c, "NOT_LANDED", "MEDIUM_LIED"))]),
        Case("B33 a Born of a file that exists", One([again]), must={"UNDER_GLASS"}, checks=[
                 ("the stitch is not applied: it gives birth to %s, which exists" % Label(bt.S),
                  lambda c: Glass(c, "gives birth to fid %u" % bt.S.fid) and
                  c.mount.files[bt.S.fid].name == bt.S.name)]),
        Case("B27 a record that does not match its base",
             One([stitch.Labeled(fid=bt.S.fid, base=1, name=b"stray")]),
             must={"BASE_MISMATCH", "UNDER_GLASS"}, checks=[
                 ("%s stays as the tally has it" % Label(bt.S),
                  lambda c: c.mount.files[bt.S.fid].name == bt.S.name),
                 ("its roll-back is a verdict this kernel cannot write", lambda c: Glass(c, "roll"))]),
        Case("B28 two files born and the second quitclaimed in one stitch", Quitclaimed,
             checks=[("%u lives and %u is gone, its block lying fallow (M-1)" % (f1, f2),
                      lambda c: f1 in c.mount.files and f2 not in c.mount.files and
                      [x.runs for x in c.claims if x.kind == "fallow"] == [[(bt.F + 1, 1)]]),
                     ("and nothing is said", lambda c: not c.findings),
                     ("%u is never given again: the next file id is %u" % (f2, f3),
                      lambda c: c.mount.counts["next_fid"] == f3)]),
    ]


def RibbonEndCases(bt):
    """How the ribbon ends (§3.9): torn, lied about, rotten, of another history,
    or what settled lost."""
    f1, f2, f3 = bt.fid, bt.fid + 1, bt.fid + 2
    sewn = {}

    def Two(o, tear=None, lose=None):
        """Stitches 3, 4 and 5 born f1, f2, f3 at F, F+1, F+2: 4 torn to its
        first four sectors (`tear`), or never landed at all (`lose`)."""
        d = bt.Fresh(o, 0, 3, 3)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"one")], flags=stitch.DATA_FLUSHED)
        s4 = pen.Sew([Born(bt.F + 1, f2, d[1:2], b"two")], flags=stitch.DATA_FLUSHED, write=not lose)
        s5 = pen.Sew([Born(bt.F + 2, f3, d[2:], b"three")], flags=stitch.DATA_FLUSHED) if lose else None
        if tear:
            bt.Zero(o, s4.vlbas[4], s4.of - 4)
        sewn.update(s3=s3, s4=s4, s5=s5)

    def Rot(o):
        d = bt.Fresh(o, 0, 2, 4)
        pen = bt.Pen(o)
        s3 = pen.Sew([stitch.UseContext(b"x" * 1000), Born(bt.F, f1, d[:1], b"kept")],
                     flags=stitch.DATA_FLUSHED)
        pen.Sew([Born(bt.F + 1, f2, d[1:], b"after")], flags=stitch.DATA_FLUSHED)
        bt.Rot(o, s3.vlbas[1])
        sewn.update(s3=s3)

    def Foreign(o):
        """Stitch 4's first sector sealed where stitch 3's second lies: a unit
        of another seq inside a settled one, and a later stitch landed."""
        d = bt.Fresh(o, 0, 2, 4)
        pen = bt.Pen(o)
        s3 = pen.Sew([stitch.UseContext(b"x" * 1000), Born(bt.F, f1, d[:1], b"kept")], flags=stitch.DATA_FLUSHED)
        s4 = pen.Sew([Born(bt.F + 1, f2, d[1:], b"after")], flags=stitch.DATA_FLUSHED)
        o.Put(bt.ground.base + s3.vlbas[1], bookread.SectorSeal(s4.sectors[0], bt.ground.uuid, s3.vlbas[1]))

    def Stranger(o):
        d = bt.Fresh(o, 0, 2, 6)
        pen = bt.Pen(o)
        pen.Sew([Born(bt.F, f1, d[:1], b"sewn")], flags=stitch.DATA_FLUSHED)
        pen.Sew([Born(bt.F + 1, f2, d[1:], b"stranger")], flags=stitch.DATA_FLUSHED, seq=9,
                prev_clasp=0xDEADBEEF)

    def Settled(o):
        """Stitches 3 to 6, 4 and 5 torn to half their sectors: 6 began only
        once 5 had settled, and 5 once 4 had."""
        d = bt.Fresh(o, 0, 4, 8)
        pen = bt.Pen(o)
        out = [pen.Sew([Born(bt.F + k, bt.fid + k, d[k:k + 1], b"s%u" % k)], flags=stitch.DATA_FLUSHED)
               for k in range(4)]
        for s in out[1:3]:
            bt.Zero(o, s.vlbas[4], s.of - 4)

    return [*CutCases(bt),
        Case("B17 a torn stitch", lambda o: Two(o, tear=True), must={"RIBBON_TORN"}, checks=[
            ("the ribbon ends on stitch 3, %u born and %u not" % (f1, f2),
             lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3] and f1 in c.mount.files and
             f2 not in c.mount.files),
            ("the torn stitch's seq and file id are never given again",
             lambda c: c.mount.counts["next_seq"] == 5 and c.mount.counts["next_fid"] == f2 + 1)]),
        Case("B18 a stitch lost and the one after it landed", lambda o: Two(o, lose=True),
             must={"DEAD_BRANCH"}, checks=[
                 ("the ribbon ends on stitch 3", lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3]),
                 ("the lie is proven: stitch 5 names stitch 4, which the medium does not hold",
                  lambda c: Found(c, "DEAD_BRANCH", "seq 5", "proven") and c.mount.ribbon.lied),
                 ("nothing past it is applied, and its numbers are never given again",
                  lambda c: f3 not in c.mount.files and c.mount.counts["next_seq"] == 6 and
                  c.mount.counts["next_fid"] == f3 + 1)]),
        Case("B19 a sector of a settled stitch rotten", Rot, must={"STITCH_TORN"}, checks=[
            ("stitch 3 is taken as closed and damaged, sector 1 named, its lost bytes counted",
             lambda c: Found(c, "STITCH_TORN", "stitch 3 ", "sector(s) 1 of", "%u byte" % sewn["s3"].LostAt(1))),
            ("the record wholly in its other sectors stands: %u is born, and so is %u" % (f1, f2),
             lambda c: f1 in c.mount.files and f2 in c.mount.files),
            ("the Use Context that crossed the rotten sector is lost", lambda c: c.mount.use == b"")]),
        Case("B19b a sector of another unit inside a settled stitch", Foreign, must={"STITCH_TORN"}, checks=[
            ("stitch 3 is closed and damaged, its sector 1 another unit's: MEDIUM_LIED",
             lambda c: Found(c, "STITCH_TORN", "stitch 3 ", "another unit's") and c.mount.ribbon.lied),
            ("what reads stands: %u and %u are born" % (f1, f2),
             lambda c: f1 in c.mount.files and f2 in c.mount.files)]),
        Case("B20 a dead branch: a foreign Clasp", Stranger, must={"CLASP_STRANGER"}, checks=[
            ("the ribbon ends on stitch 3; seq 9 is not sewn",
             lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3] and f2 not in c.mount.files),
            ("seq 9 is never given again", lambda c: c.mount.counts["next_seq"] == 10)]),
        Case("B30 what settled is lost", Settled, must={"DEAD_BRANCH"}, checks=[
            ("the ribbon ends on stitch 3", lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3]),
            ("stitch 6 names 5, begun only after 4 settled: what settled is lost, and said",
             lambda c: Found(c, "DEAD_BRANCH", "settled is lost", "4, 5, 6") and c.mount.ribbon.lied),
            ("no number seen past the end is given again",
             lambda c: c.mount.counts["next_seq"] == 7 and c.mount.counts["next_fid"] == bt.fid + 4)]),
    ]


CUT_SECTORS = (1, 2, 3, 4, 7)


def CutCases(bt):
    """C2, a sector at a time: stitch 3 — two Coineds and the Born of a file
    born empty wearing their tags, a kernel's stitch of a create — torn at its
    sector k, the sectors from k on never landed. The ribbon ends before it
    (m-1) whatever k, and every number the sectors that landed name — its seq,
    each record's that lies whole in them — is never given again; a record cut
    by the tear names nothing."""
    t1, t2, fid = bt.tag, bt.tag + 1, bt.fid
    sewn = {}

    def Cut(o, k):
        pen = bt.Pen(o)
        s3 = pen.Sew([stitch.Coined(t1, b"c2key", b"v" * 600), stitch.Coined(t2, b"c2kin", b"w" * 600),
                      stitch.Born(fid=fid, flags=ondisk.FILE_ACTIVE, size=0, created_ns=1, modified_ns=1,
                                  name=b"c2.n", tags=(t1, t2), runs=[], digests=[])],
                     flags=stitch.DATA_FLUSHED)
        bt.Zero(o, s3.vlbas[k], s3.of - k)
        sewn[k] = s3

    def Whole(k, i):
        """Whether record i of the torn stitch lies whole in its first k sectors."""
        s3 = sewn[k]
        return s3.starts[i + 1] <= k * bookread.PAYLOAD_BYTES

    def Numbers(c, k):
        want_fid = fid + 1 if Whole(k, 3) else fid
        want_tag = t2 + 1 if Whole(k, 2) else t1 + 1 if Whole(k, 1) else t1
        return (c.mount.counts["next_seq"] == 4 and c.mount.counts["next_fid"] == want_fid and
                c.mount.counts["next_tag"] == want_tag)

    return [Case("B55%s a create's stitch torn at its sector %u" % ("abcde"[n], k),
                 lambda o, k=k: Cut(o, k), must={"RIBBON_TORN"}, checks=[
                     ("the ribbon ends before it: no stitch sewn on, %u not born" % fid,
                      lambda c: not c.mount.ribbon.stitches and fid not in c.mount.files),
                     ("seq 3 and every number a record wholly landed names are never given again, and none a "
                      "record cut by the tear names", lambda c, k=k: Numbers(c, k))])
            for n, k in enumerate(CUT_SECTORS)]


def RibbonCases(bt):
    """What the ribbon carries on (§2.7, §2.8): an Undone, a Slip, a Quire run
    into its next, a retry at the start of the next."""
    f1, f2, f3 = bt.fid, bt.fid + 1, bt.fid + 2
    sewn = {}

    def Undone(o):
        d = bt.Fresh(o, 0, 2, 7)
        pen = bt.Pen(o)
        pen.Sew([Born(bt.F, f1, d[:1], b"undone"), Born(bt.F + 1, f2, d[1:], b"stands")],
                flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Undone(seq_back=1, why=1, fids=[f1])], flags=stitch.DATA_FLUSHED)

    def Slip(o):
        """A file of 100 bytes born a hole and a Slip, and 50 more by a second
        Slip — its stem coined and worn as every file of the volume wears its
        own, so the guest finds it by name."""
        pen = bt.Pen(o)
        pen.Sew([stitch.Coined(bt.tag, b"slip"),
                 stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=100, created_ns=1, modified_ns=1,
                             name=b"slip", tags=(bt.tag,), runs=[stitch.Hole(1)], digests=[],
                             slip=(0, b"a" * 100))], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=f1, base=1, size=150, mtime_ns=2, splices=[], digests=[],
                               slip=(100, b"b" * 50))], flags=stitch.DATA_FLUSHED)

    def Folded(o):
        """A file born a hole and a Slip, then its last block written whole:
        the splice that changes the block lets the overlay go (§2.8)."""
        d = bt.Fresh(o, 0, 1, 10)
        pen = bt.Pen(o)
        pen.Sew([stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=100, created_ns=1, modified_ns=1,
                             name=b"folded", tags=(), runs=[stitch.Hole(1)], digests=[],
                             slip=(0, b"a" * 100))], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=f1, base=1, size=150, mtime_ns=2, splices=[(0, 1, [stitch.Data(bt.F, 1)])],
                               digests=d)], flags=stitch.DATA_FLUSHED)

    def Crossing(o):
        """A first stitch that leaves one atom of the ribbon Quire, a second of
        two atoms that runs on into the next_quire, and a third after it."""
        pen = bt.Pen(o)
        room = pen.Room() - SECTORS_PER_BLOCK
        pen.Sew([stitch.UseContext(b"u" * (room * bookread.PAYLOAD_BYTES - 100))], flags=stitch.DATA_FLUSHED)
        sewn["cross"] = pen.Sew([stitch.UseContext(b"v" * 6000)], flags=stitch.DATA_FLUSHED)
        sewn["after"] = pen.Sew([stitch.UseContext(b"w")], flags=stitch.DATA_FLUSHED)

    def Retry(o):
        """Stitch 3; stitch 4 refused by the medium; the cycle again, seq 5, at
        the start of the next_quire (M-6), naming 3."""
        d = bt.Fresh(o, 0, 2, 9)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"before")], flags=stitch.DATA_FLUSHED)
        pen.Sew([Born(bt.F + 1, f2, d[1:], b"refused")], flags=stitch.DATA_FLUSHED, write=False)
        pen.Hop(back_to=s3)
        pen.Sew([Born(bt.F + 1, f3, d[1:], b"again")], flags=stitch.DATA_FLUSHED)

    def OnTally(o):
        """A stitch whose StitchHead names Tally-Quire A as its next_quire."""
        d = bt.Fresh(o, 0, 1, 16)
        pen = bt.Pen(o)
        pen.next = (bt.plan.a, bt.ground.grain)
        pen.Sew([Born(bt.F, f1, d, b"named")], flags=stitch.DATA_FLUSHED)

    def Fallen(o):
        """Stitch 3 born f1; slot 1 at seq 4, a Tally that took stitch 3 in —
        its `at` past it — whose FILES root names a Sheet no node was written
        to: the mount falls back to slot 0, whose ribbon is longer by stitch
        3, and replays it (§4.5 п.5: the older ribbon is whole while a slot
        names it)."""
        d = bt.Fresh(o, 0, 1, 12)
        s3 = bt.Pen(o).Sew([Born(bt.F, f1, d, b"fallen")], flags=stitch.DATA_FLUSHED)
        g = BookGround(o)
        slot = tally.SlotRead(g, 0)
        slot.stitch_seq, slot.stitch_clasp = s3.seq, s3.clasp
        slot.ribbon = dict(slot.ribbon, at=s3.vlbas[-1] // SECTORS_PER_BLOCK + 1)
        root = slot.roots[tally.FILES]
        slot.roots[tally.FILES] = tally.Root(root.flags, root.level, root.items,
                                             bookread.PtrPut(bt.plan.seq, 1, bt.plan.FreeSheet()))
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, s3.seq + 1, slot)))

    ribbon, nxt = tuple(bt.plan.ribbon), tuple(bt.plan.next)
    return [
        Case("B21c a newer slot that does not read, over a stitch it took in", Fallen, must={"SLOT_FELL_BACK"},
             checks=[("the book stands on slot 0, and its longer ribbon bears %u" % f1,
                      lambda c: c.mount.slot.index == 0 and [s.seq for s in c.mount.ribbon.stitches] == [3] and
                      f1 in c.mount.files),
                     ("and the kernel of this tree mounts it so", lambda c: c.kernel["mounts"] and
                      c.kernel["slot"] == 0 and c.kernel["fell_back"] == 1 and c.kernel["sewn"] == 1)]),
        Case("B21 Undone", Undone, checks=[
            ("stitch 3's record of %u is passed over and %u stands" % (f1, f2),
             lambda c: f1 not in c.mount.files and f2 in c.mount.files),
            ("the block %u named only lies fallow" % bt.F,
             lambda c: [x.runs for x in c.claims if x.kind == "fallow"] == [[(bt.F, 1)]])]),
        Case("B22 a Slip", Slip, must={"UNDER_GLASS"}, checks=[
            ("%u reads its tail out of the ribbon: 100 bytes then 50" % f1,
             lambda c: c.Content(f1) == b"a" * 100 + b"b" * 50),
            ("the volume is read only: a Slip this kernel cannot fold", lambda c: Glass(c, "Slip"))]),
        Case("B32 a Slip let go by a splice of its block", Folded, checks=[
            ("%u reads its new block, the overlay gone with the hole it lay on" % f1,
             lambda c: c.Content(f1) == bt.Pattern(10)[:150]),
            ("and nothing is said: no Slip is left to fold", lambda c: not c.findings)]),
        Case("B24 the ribbon crosses into its next Quire", Crossing, checks=[
            ("three stitches, the second running from block %u's Quire into block %u's" % (ribbon[0], nxt[0]),
             lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3, 4, 5] and
             [p.quire for p in c.mount.ribbon.stitches[1].places[::15]] == [ribbon, nxt]),
            ("the last Use Context stands", lambda c: c.mount.use == b"w"),
            ("both Quires are the ribbon's",
             lambda c: [x.runs for x in c.claims if x.kind == "ribbon"] == [RunsNormal([ribbon, nxt])]),
            ("and nothing is said", lambda c: not c.findings)]),
        Case("B24b a next_quire on the Tally", OnTally, must={"RIBBON_TORN"}, checks=[
            ("the ribbon is not taken there (§3.7's table), said; the stitch itself stands",
             lambda c: Found(c, "RIBBON_TORN", "Quire %u+%u" % (bt.plan.a, bt.ground.grain), "on the book") and
             f1 in c.mount.files and (bt.plan.a, bt.ground.grain) not in c.mount.ribbon.quires)]),
        Case("B29 a retry at the start of the next Quire", Retry, checks=[
            ("stitches 3 and 5, the second from block %u, where the next_quire begins" % nxt[0],
             lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3, 5] and
             c.mount.ribbon.stitches[1].start.quire == nxt and c.mount.ribbon.stitches[1].start.off == 0),
            ("%u and %u are born, %u — whose stitch never landed — is not" % (f1, f3, f2),
             lambda c: f1 in c.mount.files and f3 in c.mount.files and f2 not in c.mount.files),
            ("and nothing is said", lambda c: not c.findings)]),
    ]


def RecordRuleCases(bt):
    """Each rule a record is replayed by (§2.8, §3.7's table), broken once."""
    f1, f2, S = bt.fid, bt.fid + 1, bt.S
    worn = next((fs for _f, fs in sorted(bt.content.files.items()) if fs.tags and fs.fid != bt.c0.boot_fid), None)
    if worn is None or not bt.content.tags:
        raise Unreadable("the image has no file that wears a tag")
    known = min(bt.content.tags)
    ts = bt.content.tags[known]
    flushed = stitch.DATA_FLUSHED

    def Sewn(records, **kw):
        """One stitch of `records(seq, digests)`, its seq the pen's next and
        F, F+1 holding fresh blocks whose digests it is handed."""
        def Plant(o):
            d = bt.Fresh(o, 0, 2, 11)
            pen = bt.Pen(o)
            pen.Sew(records(pen.next_seq, d), flags=flushed, **kw)
        return Plant

    def Lied(o):
        """Two stitches on a volume that lied, both with DATA_FLUSHED: the
        first's block holds what its digest does not name (M-2)."""
        d = bt.Fresh(o, 0, 2, 13)
        pen = bt.Pen(o)
        pen.Sew([Born(bt.F, f1, [0x1234], b"unlanded")], flags=flushed | stitch.MEDIUM_LIED)
        pen.Sew([Born(bt.F + 1, f2, d[1:], b"landed")], flags=flushed | stitch.MEDIUM_LIED)

    inactive = lambda q, d: [stitch.Born(fid=f1, flags=0, size=BLOCK, created_ns=1, modified_ns=1,
                                         name=b"inactive", tags=(), runs=[stitch.Data(bt.F, 1)], digests=d[:1])]
    past = lambda q, d: [stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=100, created_ns=1, modified_ns=1,
                                     name=b"past", tags=(), runs=[stitch.Hole(1)], digests=[], slip=(50, b"a" * 100))]
    front = bt.ground.data_block - 1
    return [
        Case("B11b a Born not ACTIVE", Sewn(inactive), must={"UNDER_GLASS"}, checks=[
            ("the stitch is not applied: a Born whose ACTIVE bit is not set does not read (§2.8)",
             lambda c: Glass(c, "not ACTIVE") and f1 not in c.mount.files)]),
        Case("B15b a Coined of a known id with another text",
             Sewn(lambda q, d: [stitch.Coined(known, ts.key + b"-other", ts.value)]), must={"UNDER_GLASS"},
             checks=[("the stitch is not applied: it gives id %u a second text" % known,
                      lambda c: Glass(c, "gives id %u" % known) and c.mount.tags[known].key == ts.key)]),
        Case("B15c a Coined of a known id with its own text",
             Sewn(lambda q, d: [stitch.Coined(known, ts.key, ts.value)]), checks=[
                 ("it is taken, and nothing is said", lambda c: not c.findings and not c.mount.glass)]),
        Case("B21b an Undone after another record", Sewn(lambda q, d: [
            Born(bt.F, f1, d[:1], b"before"), stitch.Undone(seq_back=1, why=1, fids=[S.fid])]),
             must={"UNDER_GLASS"}, checks=[
                 ("the stitch is not applied: an Undone stands first in its stitch (§2.8)",
                  lambda c: Glass(c, "an Undone after another record") and f1 not in c.mount.files and
                  S.fid in c.mount.files)]),
        Case("B22b a Slip past the file's size", Sewn(past), must={"BASE_MISMATCH", "UNDER_GLASS"}, checks=[
            ("the file does not match its Slip and is not born", lambda c: Found(c, "BASE_MISMATCH", "Slip") and
             f1 not in c.mount.files)]),
        Case("B26c on a volume that lied, the stitch before the newest did not land", Lied,
             must={"NOT_LANDED", "UNDER_GLASS"}, checks=[
                 ("both newest stitches are held to their data, DATA_FLUSHED or not (M-2): %u is rolled back"
                  % f1, lambda c: Found(c, "NOT_LANDED", "fid %u" % f1, "stitch 3") and
                  f1 not in c.mount.files and f2 in c.mount.files)]),
        Case("B27b a Shaped whose runs do not cover its size", Sewn(lambda q, d: [
            stitch.Shaped(fid=S.fid, base=q - S.gen, size=S.size + 10 * BLOCK, mtime_ns=1, splices=[],
                          digests=[])]), must={"BASE_MISMATCH", "UNDER_GLASS"}, checks=[
            ("%s does not match: its runs do not cover its size" % Label(S),
             lambda c: Found(c, "BASE_MISMATCH", "fid %u" % S.fid, "runs cover"))]),
        Case("B27c a Labeled that puts on a tag already worn", Sewn(lambda q, d: [
            stitch.Labeled(fid=worn.fid, base=q - worn.gen, add=[worn.tags[0]])]),
             must={"BASE_MISMATCH", "UNDER_GLASS"}, checks=[
                 ("%s does not match: the tag is worn" % Label(worn),
                  lambda c: Found(c, "BASE_MISMATCH", "fid %u" % worn.fid, "already worn"))]),
        Case("B27d a Labeled that takes ACTIVE off", Sewn(lambda q, d: [
            stitch.Labeled(fid=S.fid, base=q - S.gen, flags=(0, ondisk.FILE_ACTIVE))]), must={"UNDER_GLASS"},
             checks=[("the stitch is not applied: a file leaves by Quitclaim, not by losing ACTIVE (§2.8)",
                      lambda c: Glass(c, "takes ACTIVE off") and c.mount.files[S.fid].flags & ondisk.FILE_ACTIVE)]),
        Case("B28b a Born on the front of the volume", Sewn(lambda q, d: [
            stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1, modified_ns=1, name=b"front",
                        tags=(), runs=[stitch.Data(front, 1)], digests=d[:1])]), must={"EXTENT_OUTSIDE", "UNDER_GLASS"},
             checks=[
            ("%u names block %u, before the data run, and is locked" % (f1, front),
             lambda c: Found(c, "EXTENT_OUTSIDE", "fid %u" % f1) and f1 in c.mount.locked),
            ("and the volume is under glass: no Tally names it as it lay",
             lambda c: Glass(c, "locked by the replay of the ribbon"))]),
        Case("B23e a Clasp that counts fresh blocks its records do not name",
             Sewn(lambda q, d: [Born(bt.F, f1, d[:1], b"fresh")], fresh=9), must={"UNDER_GLASS"}, checks=[
                 ("the stitch is not applied: its Clasp names 9 fresh blocks and its records 1",
                  lambda c: Glass(c, "fresh") and f1 not in c.mount.files)]),
    ]


def RibbonBranchCases(bt):
    """Whose a unit past the end of the chain is (§2.2, §2.7, §3.9): a
    closed, damaged stitch of another history, one the next does not name, a
    dead branch of two, a retry whose refused try landed whole, a retry that
    names another Quire it came from, a stitch naming one before the end."""
    f1, f2, f3, f4 = bt.fid, bt.fid + 1, bt.fid + 2, bt.fid + 3
    flushed = stitch.DATA_FLUSHED

    def Chain(o, foreign=False, rot=None, f_names=None, seed=3):
        """3 (f1), U = 4 (f2, a Use Context first so it spans sectors) and
        F = 5 (f3): U of another history (`foreign`), its sector `rot`
        ("one" or "clasp") rotten, F naming `f_names` ("s3") in place of U."""
        d = bt.Fresh(o, 0, 3, seed)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"own")], flags=flushed)
        u = pen.Sew([stitch.UseContext(b"x" * 1000), Born(bt.F + 1, f2, d[1:2], b"u")], flags=flushed,
                    prev_clasp=0xDEADBEEF if foreign else None)
        pen.Sew([Born(bt.F + 2, f3, d[2:], b"f")], flags=flushed, names=s3 if f_names else None)
        if rot:
            bt.Rot(o, u.vlbas[1 if rot == "one" else u.starts[-1] // bookread.PAYLOAD_BYTES])

    def Kept(o, came=True):
        """3; 4 refused by the medium yet landed whole (or, `came` False,
        never landed); the cycle again as 5 at the start of the next_quire
        naming 3 — its from_quire this Quire, or none — and 6 after it."""
        d = bt.Fresh(o, 0, 3, 9)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"before")], flags=flushed)
        pen.Sew([Born(bt.F + 1, f2, d[1:2], b"refused")], flags=flushed, write=came)
        pen.Hop(back_to=s3)
        if not came:
            pen.came = None
        pen.Sew([Born(bt.F + 1, f3, d[1:2], b"again")], flags=flushed)
        pen.Sew([Born(bt.F + 2, f4, d[2:], b"after")], flags=flushed)

    def Fork(o):
        """3 and 4 sewn on; 5, whole, names 3 by its Clasp and lies right
        after 4 — two stitches name one, and 5 is not where the ribbon goes."""
        d = bt.Fresh(o, 0, 3, 11)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"base")], flags=flushed)
        pen.Sew([Born(bt.F + 1, f2, d[1:2], b"one")], flags=flushed)
        pen.Sew([Born(bt.F + 2, f3, d[2:], b"other")], flags=flushed, names=s3)

    chain = lambda c: [s.seq for s in c.mount.ribbon.stitches]
    return [
        Case("B19c a closed, damaged stitch of another history", lambda o: Chain(o, foreign=True, rot="one"),
             must={"CLASP_STRANGER"}, checks=[
                 ("its StitchHead names stitch 3 with a Clasp not 3's: not sewn on, rot or no rot",
                  lambda c: chain(c) == [3] and f2 not in c.mount.files and f3 not in c.mount.files),
                 ("a dead branch of seq 4, 5 — no lie", lambda c: Found(c, "CLASP_STRANGER", "4, 5") and
                  not c.mount.ribbon.lied)]),
        Case("B19d a rotten stitch and the next naming the one before it",
             lambda o: Chain(o, rot="clasp", f_names=True), must={"RIBBON_TORN", "DEAD_BRANCH"}, checks=[
                 ("stitch 4 is not taken as closed: the stitch after it does not name it",
                  lambda c: chain(c) == [3] and f2 not in c.mount.files and f3 not in c.mount.files),
                 ("seq 5 names the end of the ribbon from where it does not go on",
                  lambda c: Found(c, "DEAD_BRANCH", "seq 5", "does not go on"))]),
        Case("B20b a dead branch of two stitches", lambda o: Chain(o, foreign=True),
             must={"CLASP_STRANGER"}, checks=[
                 ("seq 4 and seq 5, which names it, are one branch of another history — no lie",
                  lambda c: Found(c, "CLASP_STRANGER", "4, 5") and not c.mount.ribbon.lied and chain(c) == [3])]),
        Case("B29b a retry after a write refused and kept", Kept, must={"DEAD_BRANCH"}, checks=[
            ("rules (а) and (б) both give a whole stitch naming 3 by its Clasp: the larger seq, the retry 5, is "
             "taken, and 6 after it (§16.3 п.11)",
             lambda c: chain(c) == [3, 5, 6] and not c.mount.ribbon.lied),
            ("the refused try 4 is said as the pieces of a retry — a note, no damage, no lie",
             lambda c: Found(c, "DEAD_BRANCH", "pieces of a retry", "seq 4") and
             all(x.severity == "note" for x in c.findings)),
            ("%u, %u and %u stand, %u — the try the retry stands for — is not taken" % (f1, f3, f4, f2),
             lambda c: f1 in c.mount.files and f3 in c.mount.files and f4 in c.mount.files and
             f2 not in c.mount.files)]),
        Case("B29c a retry that does not name the Quire it came from", lambda o: Kept(o, came=False),
             must={"DEAD_BRANCH"}, checks=[
                 ("rule (б) refuses it (§4.5 п.5): the ribbon ends on stitch 3",
                  lambda c: chain(c) == [3] and f3 not in c.mount.files),
                 ("seq 5 names the end of the ribbon from where it does not go on",
                  lambda c: Found(c, "DEAD_BRANCH", "seq 5", "does not go on"))]),
        Case("B29d a stitch naming one before the end of the ribbon", Fork, must={"DEAD_BRANCH"}, checks=[
            ("the ribbon is 3 and 4; seq 5, which names 3 by its Clasp as 4 does, is not taken",
             lambda c: chain(c) == [3, 4] and f2 in c.mount.files and f3 not in c.mount.files),
            ("said loud as the ribbon forking at 3 (§16.3 п.10) — no lie, no retry",
             lambda c: Found(c, "DEAD_BRANCH", "forks at stitch 3", "seq 5", "§16.3 п.10") and
             not any("M-6" in x.text for x in c.findings) and
             all(x.severity == "damage" for x in c.findings if x.code == "DEAD_BRANCH") and
             not c.mount.ribbon.lied)]),
    ]


def RibbonFurtherCases(bt):
    """Rule (б′) (§16.4 п.4): a retry refused inside the next_quire and taken
    further in it, a try refused that landed whole and the retry past it, the
    next_quire run out by tries."""
    f1, f2, f3 = bt.fid, bt.fid + 1, bt.fid + 2
    flushed = stitch.DATA_FLUSHED

    def Further(o, tries, landed=False):
        """3; 4 refused in its Quire (and, `landed`, kept by the medium
        whole); the cycle again at the start of the next_quire and on past
        each try refused there (§16.4 п.4) — `tries` says how each landed:
        "none", "whole" or "first" (its first sector alone) — then the retry
        that lands, naming 3, and 2 more after it."""
        d = bt.Fresh(o, 0, 3, 13)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"before")], flags=flushed)
        pen.Sew([Born(bt.F + 1, f2, d[1:2], b"again")], flags=flushed, write=landed)
        pen.Hop(back_to=s3)
        for landed in tries:
            t = pen.Sew([Born(bt.F + 1, f2, d[1:2], b"again")], flags=flushed, write=landed != "none")
            if landed == "first":
                bt.Zero(o, t.vlbas[1], t.of - 1)
            pen.Again(back_to=s3)
        pen.Sew([Born(bt.F + 1, f2, d[1:2], b"again")], flags=flushed)
        pen.Sew([Born(bt.F + 2, f3, d[2:], b"after")], flags=flushed)

    def RunOut(o):
        """3; 4 refused in its Quire; every try after it refused in the
        next_quire to its last atom, where the last one landed its first
        sector alone: the next_quire has run out."""
        d = bt.Fresh(o, 0, 2, 15)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"before")], flags=flushed)
        pen.Sew([Born(bt.F + 1, f2, d[1:], b"again")], flags=flushed, write=False)
        pen.Hop(back_to=s3)
        while True:
            last = pen.place.Left() <= SECTORS_PER_BLOCK
            t = pen.Sew([Born(bt.F + 1, f2, d[1:], b"again")], flags=flushed, write=last)
            if last:
                bt.Zero(o, t.vlbas[1], t.of - 1)
                return
            pen.Again(back_to=s3)

    def Long(o):
        """A stitch an atom long on every atom of the Quire, and 40 more on
        into the next_quire: a Quire's worth of stitches before it, and the
        next_quire well into its own."""
        pen = bt.Pen(o)
        grid = pen.g.atom // bookread.SECTOR_BYTES
        while pen.Room() > 2 * grid:
            pen.Sew([stitch.UseContext(b"q")], flags=flushed)
        for k in range(41):
            pen.Sew([stitch.UseContext(b"n%u" % k)], flags=flushed)

    def UnitsRead(c):
        """How many units a walk of the ribbon the census read builds, its
        direct scan past the end with it (stitch.Unit counted)."""
        r, built, real = c.mount.ribbon, [0], stitch.Unit

        class Counted(real):
            __slots__ = ()

            def __init__(self, *a):
                built[0] += 1
                real.__init__(self, *a)
        stitch.Unit = Counted
        try:
            stitch.Ribbon(r.g, r.slot, r.taken)
        finally:
            stitch.Unit = real
        return built[0]

    chain = lambda c: [s.seq for s in c.mount.ribbon.stitches]
    walked = lambda c, seqs: Found(c, "DEAD_BRANCH", "walked over", seqs) and \
        all(x.severity == "note" for x in c.findings) and not c.mount.ribbon.lied
    return [
        Case("B29e a retry refused at the start of the next_quire and taken further in it",
             lambda o: Further(o, ["none"]), must={"DEAD_BRANCH"}, checks=[
                 ("rule (б′): the ribbon is 3, then 6 — on the atom past the try refused at the start of the "
                  "next_quire, naming 3 and the Quire the first try came from — and 7 (§16.4 п.4)",
                  lambda c: chain(c) == [3, 6, 7] and c.mount.ribbon.stitches[1].start.off == SECTORS_PER_BLOCK),
                 ("%u, %u and %u stand" % (f1, f2, f3),
                  lambda c: all(f in c.mount.files for f in (f1, f2, f3))),
                 ("what was walked over is said as the pieces of a retry, none of it read — a note, no lie",
                  lambda c: walked(c, "none that reads"))]),
        Case("B29f two retries refused in a row inside the next_quire, the second landing a sector",
             lambda o: Further(o, ["none", "first"]), must={"DEAD_BRANCH"}, checks=[
                 ("the ribbon is 3, then 7, on the atom past 6's, and 8",
                  lambda c: chain(c) == [3, 7, 8] and c.mount.ribbon.stitches[1].start.off == 2 * SECTORS_PER_BLOCK),
                 ("6, a unit whose first sector alone holds, is stepped over by its `of` and said walked over",
                  lambda c: walked(c, "seq 6") and f2 in c.mount.files and f3 in c.mount.files)]),
        Case("B29g a retry refused at the start of the next_quire that landed whole, and the one after it",
             lambda o: Further(o, ["whole"]), must={"DEAD_BRANCH"}, checks=[
                 ("5, whole and naming 3, is passed over for 6 after it, which names 3 with the larger seq "
                  "(§16.3 п.11, §16.4 п.4): the ribbon is 3, 6, 7",
                  lambda c: chain(c) == [3, 6, 7] and f2 in c.mount.files and f3 in c.mount.files),
                 ("5 is said walked over as the pieces of a retry — a note, no lie", lambda c: walked(c, "seq 5"))]),
        Case("B29i a try refused that landed whole in its Quire, its retry refused at the start of the "
             "next_quire, and the one past it", lambda o: Further(o, ["none"], landed=True),
             must={"DEAD_BRANCH"}, checks=[
                 ("nothing past 4 names it, so rule (б′) is weighed: 6, past the try at the next_quire's start, "
                  "names 3 with the larger seq and is taken, then 7 (§16.3 п.11, §16.4 п.4)",
                  lambda c: chain(c) == [3, 6, 7] and f2 in c.mount.files and f3 in c.mount.files),
                 ("4 is said as the pieces of a retry, and the start of the next_quire walked over — notes, no lie",
                  lambda c: Found(c, "DEAD_BRANCH", "pieces of a retry", "seq 4") and walked(c, "none that reads"))]),
        Case("B29j a Quire full of stitches and the next_quire well into its own", Long, must=set(), checks=[
            ("every stitch is sewn on: the Quire's, and the 41 past it", lambda c: len(chain(c)) > 41 and
             c.mount.ribbon.stitches[-1].start.quire != c.mount.ribbon.stitches[0].start.quire),
            ("each unit of the chain is read once — rule (б′) is weighed for no stitch a later one names "
             "(§16.4 п.4: its cost only after a refusal)",
             lambda c: UnitsRead(c) <= len(chain(c)) + 8)]),
        Case("B29h the next_quire run out by retries refused", RunOut, must={"DEAD_BRANCH"}, checks=[
            ("no stitch past 3 is taken: the walk ends at the next_quire's end", lambda c: chain(c) == [3] and
             f2 not in c.mount.files),
            ("the last try, cut, is said as the pieces of a retry — a note, no lie",
             lambda c: Found(c, "DEAD_BRANCH", "pieces of", "cut while it was written") and
             all(x.severity == "note" for x in c.findings) and not c.mount.ribbon.lied)]),
    ]


def RibbonLossCases(bt):
    """What a torn or rotten stitch costs (§3.9): the last cycle whole by its
    sectors and one sum broken, what settled lost behind a torn stitch whose
    successor is torn too, and the rule of rot rolling a file back onto
    another's blocks."""
    f1, f2, S = bt.fid, bt.fid + 1, bt.S
    flushed = stitch.DATA_FLUSHED

    def Torn(o, whole):
        """3 and 4; 4 torn — its sectors 4 on gone, or (`whole`) all there and
        one byte of sector 2 flipped; or 4 and 5 torn, 5's StitchHead naming 4."""
        d = bt.Fresh(o, 0, 3, 8)
        pen = bt.Pen(o)
        out = [pen.Sew([Born(bt.F + k, bt.fid + k, d[k:k + 1], b"t%u" % k)], flags=flushed)
               for k in range(2 if whole else 3)]
        if whole:
            bt.Rot(o, out[1].vlbas[2])
        else:
            for s in out[1:]:
                bt.Zero(o, s.vlbas[4], s.of - 4)

    def Taken(o):
        """3 moves S onto F (a Shaped), and the sector its Shaped begins in
        rots; 4 bears f1 on S's old blocks; 5 renames S from the gen 3 gave
        it — which no longer stands: S is rolled back onto f1's blocks."""
        n = S.Blocks()
        d = bt.Fresh(o, 0, n, 14)
        old = [r.start + i for r in S.runs for i in range(r.count)]
        mine = [bookread.Integrity(bt.ground.Block(b), bt.lanes) for b in old]
        pen = bt.Pen(o)
        q = pen.next_seq
        s3 = pen.Sew([stitch.UseContext(b"y" * 1000),
                      stitch.Shaped(fid=S.fid, base=q - S.gen, size=S.size, mtime_ns=2,
                                    splices=[(0, n, [stitch.Data(bt.F, n)])], digests=d)], flags=flushed)
        pen.Sew([stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=n * BLOCK, created_ns=1, modified_ns=1,
                             name=b"taker", tags=(), runs=[stitch.Data(old[0], n)], digests=mine)], flags=flushed)
        pen.Sew([stitch.Labeled(fid=S.fid, base=2, name=b"renamed")], flags=flushed)
        bt.Rot(o, s3.vlbas[s3.starts[2] // bookread.PAYLOAD_BYTES])

    chain = lambda c: [s.seq for s in c.mount.ribbon.stitches]
    return [
        Case("B17b a torn stitch whole by its sectors, one sum broken", lambda o: Torn(o, True),
             must={"RIBBON_TORN"}, checks=[
                 ("the last cycle does not read whole, said loud: a cut or rot (M-4)",
                  lambda c: Found(c, "RIBBON_TORN", "all 8 sectors", "cut or rot") and
                  any(x.severity == "damage" for x in c.findings if x.code == "RIBBON_TORN")),
                 ("the ribbon ends on stitch 3", lambda c: chain(c) == [3] and f2 not in c.mount.files)]),
        Case("B19e the rule of rot rolls a file back onto another's blocks", Taken,
             must={"STITCH_TORN", "BASE_MISMATCH", "UNDER_GLASS"}, checks=[
                 ("%s is locked: the blocks it last matched on are %u's now" % (Label(S), f1),
                  lambda c: c.mount.locked.get(S.fid, "").find("another file") >= 0 and f1 in c.mount.files),
                 ("and no block has two owners", lambda c: not Found(c, "DOUBLE_OWNER"))]),
        Case("B30b what settled is lost, the stitch naming it torn too", lambda o: Torn(o, False),
             must={"DEAD_BRANCH"}, checks=[
                 ("seq 5's StitchHead reads and names 4: what settled is lost — MEDIUM_LIED",
                  lambda c: Found(c, "DEAD_BRANCH", "settled is lost", "4, 5") and c.mount.ribbon.lied)]),
    ]


def RibbonShapeCases(bt):
    """A unit of the ribbon the format does not allow (§1.5, §2.1, §2.2): a
    stitch of one sector, one that says it is longer than the ribbon, and a
    whole one whose sectors' leads lie."""
    f1, g = bt.fid, bt.ground

    def Long(o):
        """One sector at the ribbon's `at`, sealed as part 0 of a stitch of
        0xFFFFFFF0 sectors."""
        pen = bt.Pen(o)
        stream, starts, _c = stitch.StitchStream(stitch.HeadPut(stitch.DATA_FLUSHED, 0, 0, 2, 0, (0, 0), (0, 0)),
                                                 [])
        vlba = pen.place.Vlba()
        head = bookread.SECTOR_HEAD.struct.pack(0, bookread.HEAD_BYTES, bookread.UNIT_STITCH, 0, 0xFFFFFFF0,
                                                pen.next_seq)
        o.Put(g.base + vlba, bookread.SectorSeal(head + stream.ljust(bookread.PAYLOAD_BYTES, b"\0"), g.uuid, vlba))

    def Leads(o):
        d = bt.Fresh(o, 0, 1, 15)
        s = bt.Pen(o).Sew([Born(bt.F, f1, d, b"leads")], flags=stitch.DATA_FLUSHED)
        bt.BumpLead(o, s.vlbas[0])

    return [
        Case("B23b a stitch of one sector", lambda o: bt.Pen(o).Sew([], flags=stitch.DATA_FLUSHED, of=1),
             must={"RECORD_SHAPE"}, checks=[
                 ("no stitch is one sector long: it is not sewn on, and said",
                  lambda c: Found(c, "RECORD_SHAPE", "seq 3", "1 sector") and not c.mount.ribbon.stitches)]),
        Case("B23c a stitch longer than the ribbon", Long, must={"RECORD_SHAPE"}, checks=[
            ("it is not sewn on, and said", lambda c: Found(c, "RECORD_SHAPE", "seq 3", "4294967280 sectors") and
             not c.mount.ribbon.stitches)]),
        Case("B23d a whole stitch whose lead lies", Leads, must={"RECORD_SHAPE"}, checks=[
            ("its sector 0 names a lead its records do not give it, said loud",
             lambda c: Found(c, "RECORD_SHAPE", "stitch 3", "lead")),
            ("and the stitch, whole, is applied: %u is born" % f1, lambda c: f1 in c.mount.files)]),
    ]


def RibbonBoundCases(bt):
    """What the medium may say that no writer does, held to a bound before a
    byte of it is trusted: a StitchHead naming a seq past its own, two units
    naming each other, a unit whose own StitchHead names a Quire past the
    volume, a slot whose ribbon lies past the volume, a CODEC run of 2^62
    blocks in the stitch the mount checks, a Clasp counting other fresh blocks
    than its records over data that did not land, a hole of 2^62 blocks born
    and put in, a tag the kernel cannot hold worn by a file born after it is
    coined, a block of the newest stitch past the volume's end; an older
    slot's ribbon run on through a Quire only its StitchHeads name (§16.2
    п.12); and two that hold what memory is left with to the print a mount
    gives of it: a ribbon of two thousand files born, in fids that fall, and a
    file renamed and given a tag coined beside it."""
    f1, g, S = bt.fid, bt.ground, bt.S
    flushed = stitch.DATA_FLUSHED
    huge = (1 << 62) - 1      # the longest run a record can name: its count is shifted by 2 in a u64

    def Raw(o, pen, seq, prev, prev_clasp, records=(), nxt=(0, 0), of=SECTORS_PER_BLOCK):
        """One whole unit at the pen's place whose StitchHead says what no
        pen would: `prev`, `prev_clasp`, `nxt`; sealed at `of` sectors, only
        the first of them written when it is longer than one block."""
        head = stitch.HeadPut(flushed, prev, prev_clasp, seq - pen.tally_seq, 0, nxt, (0, 0))
        stream, starts, clasp = stitch.StitchStream(head, list(records))
        vlbas = [pen.place.On(k, pen.next).Vlba() for k in range(min(of, SECTORS_PER_BLOCK))]
        sectors = bookread.UnitSeal(stream, starts, bookread.UNIT_STITCH, seq, vlbas, g.uuid)
        if of != len(vlbas):
            raw = bytearray(sectors[0])
            raw[12:16] = of.to_bytes(4, "little")
            sectors = [bookread.SectorSeal(bytes(raw), g.uuid, vlbas[0])]
        for v, s in zip(vlbas, sectors):
            o.Put(g.base + v, s)
        pen.place = pen.place.On(len(vlbas), pen.next)
        return seq, clasp

    def Above(o):
        pen = bt.Pen(o)
        Raw(o, pen, pen.next_seq, pen.next_seq + 5, 0xDEADBEEF)

    def Each(o):
        pen = bt.Pen(o)
        a = pen.next_seq
        _s, ca = Raw(o, pen, a, (1 << 64) - 1, 0xDEADBEEF)
        Raw(o, pen, a + 1, 1, ca)

    def Vast(o):
        pen = bt.Pen(o)
        Raw(o, pen, pen.next_seq, 0, 0, nxt=(g.data_block, 1 << 40), of=8801168)

    def Beyond(o):
        s = tally.SlotRead(g, 0)
        q = -(-(g.data_end + 1) // 256) * 256
        s.ribbon = dict(s.ribbon, quire_block=q, quire_blocks=256, at=q)
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK,
              b"".join(tally.SlotSectors(g, 1, bt.Pen(o).next_seq, s)))

    def Coded(o):
        d = bt.Fresh(o, 0, 1, 21)
        codec = bookread.EntryPut(tally.EXT_CODEC, bookread.VarintPut(1) + bookread.VarintPut(1))
        bt.Pen(o).Sew([stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1, modified_ns=1,
                                   name=b"coded", tags=(), runs=[stitch.Data(bt.F, huge, codec)], digests=d)])

    def Miscounted(o):
        bt.Fresh(o, 0, 1, 22)
        bt.Pen(o).Sew([Born(bt.F, f1, [0x1234], b"miscounted")], fresh=9)

    def Gap(q, d):
        return [stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1, modified_ns=1, name=b"gap",
                            tags=(), runs=[stitch.Hole(huge)], digests=[])]

    def Widened(q, d):
        return [stitch.Shaped(fid=S.fid, base=q - S.gen, size=S.size, mtime_ns=1,
                              splices=[(S.Blocks(), 0, [stitch.Hole(huge)])], digests=[])]

    def Sewn(records, flags=flushed):
        def Plant(o):
            d = bt.Fresh(o, 0, 1, 23)
            pen = bt.Pen(o)
            pen.Sew(records(pen.next_seq, d), flags=flags)
        return Plant

    wide = 1 << 31
    worn = lambda q, d: [stitch.Coined(wide, b"wide"),
                         stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1, modified_ns=1,
                                     name=b"worn", tags=(wide,), runs=[stitch.Data(bt.F, 1)], digests=d)]
    past = g.data_end + 1
    gone = lambda q, d: [Born(past, f1, d, b"past")]

    grain = bt.ground.grain
    q1, q2 = tuple(bt.plan.ribbon), tuple(bt.plan.next)
    q3, q4 = (q2[0] + grain, grain), (q2[0] + 2 * grain, grain)

    def Older(o):
        """Slot 0's ribbon run from its Quire into its next_quire and on into a
        third its stitches name; slot 1, newer, puts its ribbon on in a fourth.
        The third is slot 0's by its StitchHeads alone — the slot names it
        nowhere, and slot 1's ribbon does not run through it."""
        if ondisk.RunsSubtract([q3, q4], bt.c0.free_runs):
            raise Unreadable("the two grains past the ribbon's next_quire are not free")
        pen = bt.Pen(o)
        for nxt in (q2, q3):
            room = pen.Room() - SECTORS_PER_BLOCK
            pen.Sew([stitch.UseContext(b"f" * (room * bookread.PAYLOAD_BYTES - 100))], flags=flushed)
            cross = pen.Sew([stitch.UseContext(b"c" * 6000)], flags=flushed)
            pen.next = q3 if nxt == q2 else None
        g2 = BookGround(o)
        slot = tally.SlotRead(g2, 0)
        slot.stitch_seq, slot.stitch_clasp = cross.seq, cross.clasp
        slot.ribbon = dict(slot.ribbon, quire_block=q4[0], quire_blocks=q4[1], at=q4[0], next_block=0,
                           next_blocks=0)
        o.Put(g2.base + tally.SlotBlock(g2, 1) * SECTORS_PER_BLOCK,
              b"".join(tally.SlotSectors(g2, 1, cross.seq + 1, slot)))

    renamed = lambda q, d: [stitch.Coined(bt.tag, b"renamed-tag"),
                            stitch.Labeled(fid=S.fid, base=q - S.gen, name=b"renamed", add=[bt.tag])]

    def Many(o):
        """Forty stitches of fifty Borns, each stitch's fids falling, the last
        renaming the last file it bore — found among the stitch's own."""
        pen = bt.Pen(o)
        for s in range(40):
            pen.Sew([stitch.Born(fid=f1 + 1999 - (s * 50 + i), flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1,
                                 modified_ns=1, name=b"m%04u" % (s * 50 + i), tags=(), runs=[stitch.Hole(1)],
                                 digests=[]) for i in range(50)] +
                    ([stitch.Labeled(fid=f1, base=0, name=b"relabeled")] if s == 39 else []), flags=flushed)

    return [
        Case("B46 a unit past the end naming a seq past its own", Above, must={"CLASP_STRANGER"}, checks=[
            ("its StitchHead names no stitch this ribbon holds: a stranger, and no lie",
             lambda c: Found(c, "CLASP_STRANGER", "does not hold") and not c.mount.ribbon.lied)]),
        Case("B46b two units past the end naming each other", Each, must={"CLASP_STRANGER"}, checks=[
            ("one branch of two, a stranger — the walk to its root ends",
             lambda c: Found(c, "CLASP_STRANGER", "seq %u, %u" % (c.mount.ribbon.past[0].seq,
                                                                 c.mount.ribbon.past[1].seq))
             and not c.mount.ribbon.lied)]),
        Case("B47 a unit of 8801168 sectors whose StitchHead names a Quire past the volume", Vast,
             must={"RECORD_SHAPE"}, checks=[
                 ("the Quire its own StitchHead names is not one: the unit is longer than its Quire and the next",
                  lambda c: Found(c, "RECORD_SHAPE", "8801168 sectors long, more than") and
                  not c.mount.ribbon.stitches)]),
        Case("B47b a slot whose ribbon lies past the volume", Beyond, must={"RIBBON_TORN"}, checks=[
            ("no stitch past its Tally is read", lambda c: Found(c, "RIBBON_TORN", "outside the data run") and
             c.mount.slot.index == 1 and not c.mount.ribbon.quires)]),
        Case("B48 a CODEC run of 2^62 blocks in the stitch the mount checks", Coded,
             must={"BASE_MISMATCH", "UNDER_GLASS"}, checks=[
                 ("one block is checked against its one digest; the record does not cover its size",
                  lambda c: not Found(c, "NOT_LANDED") and Found(c, "BASE_MISMATCH", "fid %u" % f1, "runs cover")
                  and f1 not in c.mount.files)]),
        Case("B48b a Clasp counting other fresh blocks over data that did not land", Miscounted,
             must={"UNDER_GLASS"}, checks=[
                 ("its data is not read: the stitch is not applied for its Clasp, and nothing is said not landed",
                  lambda c: not Found(c, "NOT_LANDED") and Glass(c, "fresh") and f1 not in c.mount.files)]),
        Case("B49 a Born of a hole of 2^62 blocks", Sewn(Gap), must={"BASE_MISMATCH", "UNDER_GLASS"}, checks=[
            ("its runs do not cover its size", lambda c: Found(c, "BASE_MISMATCH", "fid %u" % f1, "runs cover"))]),
        Case("B49b a Shaped putting in a hole of 2^62 blocks", Sewn(Widened), must={"BASE_MISMATCH", "UNDER_GLASS"},
             checks=[("%s does not match: its runs do not cover its size" % Label(S),
                      lambda c: Found(c, "BASE_MISMATCH", "fid %u" % S.fid, "runs cover") and
                      c.mount.files[S.fid].runs == S.runs)]),
        Case("B50 a file born wearing a tag of id 2^31 coined before it", Sewn(worn), must={"UNDER_GLASS"},
             checks=[("the tag is known — coined — and the file born; the kernel cannot hold the id, said",
                      lambda c: f1 in c.mount.files and Glass(c, "past 2^31") and
                      not Found(c, "BASE_MISMATCH"))]),
        Case("B51 the newest stitch's data past the volume's end", Sewn(gone, flags=0),
             must={"NOT_LANDED", "UNDER_GLASS"}, checks=[
                 ("a block the medium does not give did not land: %u is rolled back" % f1,
                  lambda c: Found(c, "NOT_LANDED", "fid %u" % f1, "block %u" % past) and f1 not in c.mount.files)]),
        Case("B52 two thousand files born in forty stitches, their fids falling", Many, checks=[
            ("every one is born, and nothing is said",
             lambda c: all(f in c.mount.files for f in range(f1, f1 + 2000)) and not c.findings and
             len(c.mount.applied) == 40 and c.mount.files[f1].name == b"relabeled")]),
        Case("B54 an older slot whose ribbon runs on through a Quire it does not name", Older, checks=[
            ("the mount stands on slot 1; the third Quire is the ribbon's by slot 0's StitchHeads",
             lambda c: c.mount.slot.index == 1 and
             [x.runs for x in c.claims if x.kind == "ribbon"] == [RunsNormal([q1, q2, q3, q4])])]),
        Case("B53 a file renamed and given a tag coined beside it", Sewn(renamed), checks=[
            ("%s is 'renamed' and wears tag %u, and nothing is said" % (Label(S), bt.tag),
             lambda c: c.mount.files[S.fid].name == b"renamed" and bt.tag in c.mount.files[S.fid].tags and
             not c.findings)]),
    ]
