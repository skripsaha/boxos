"""selftest_book_ribbon — the book's landings on its ribbon: every kind of
record a cycle sews and the rule each is replayed by (§3.7), and how the
ribbon goes on and ends (§2.7, §3.9) — torn, lied about, rotten, of another
history, what settled lost, an Undone, a Slip, a Quire run into its next, a
retry at the start of the next. Each a fork of the base selftest_book.py
lays; what the book's census must say of it. The landings of a ribbon no
writer leaves are selftest_book_ribbon_damage.py's."""

from . import bookread, ondisk, stitch, stitch_ribbon, tally
from .bookground import BookGround
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK, RunsNormal
from .selftest_book import Born, Case, Errata, Found, FrameDeed, Glass, Label
from .selftest_book_tally import PlantBookmark

# A growth past the image the base was read from, by 4096 blocks: ground no
# head Deed of the base names.
GROWTH = 4096 * SECTORS_PER_BLOCK


def LandingRecord(c, base, late=(), also=()):
    """The sectors the landing laid over the book `base`, as one machine
    writes them (§3.3): its data but at blocks `late`, a flush, the book's
    units, a flush — then its data at blocks `late`, which no stitch after it
    names, and `also`: (lba, sectors) written again as they lie, a Tally
    node, a slot, a file's block of the base. The entries of a record
    (selftest_book_medium.SyntheticLog)."""
    from .blklog import LOG_FLUSH
    g = c.g
    laid = {x: raw for x, raw in c.medium.over.items() if base.over.get(x) != raw}

    def Data(lba):
        raw = laid[lba]
        return raw[4:bookread.HEAD_BYTES] == bytes(bookread.HEAD_BYTES - 4) or \
            not bookread.SectorCheck(raw, g.uuid, lba - g.base)

    lbas = sorted(laid)
    lately = {x for x in lbas if Data(x) and (x - g.base) // SECTORS_PER_BLOCK in late}
    return [(x, 0, laid[x]) for x in lbas if Data(x) and x not in lately] + [(0, LOG_FLUSH, b"")] + \
        [(x, 0, laid[x]) for x in lbas if not Data(x)] + [(0, LOG_FLUSH, b"")] + \
        [(x, 0, laid[x]) for x in sorted(lately)] + \
        [(lba + k, 0, c.medium.sectors(lba + k, 1)) for lba, n in also for k in range(n)]


def RecordKinds(g, base, entries, sectors):
    """A machine's record of `entries` over the book `base`, of a medium
    `sectors` long (None: past the image, where a growth takes its ground),
    classified (blklog_book.py): {volume block: the kinds its sectors are
    named}."""
    from . import blklog
    from .selftest_book_medium import SyntheticLog
    log = blklog.BlkLog(SyntheticLog(entries, len(entries)), sectors)
    named = blklog.Classifier(log, base)
    kinds = {}
    for w in log.Writes():
        for t in named.Classify(w):
            kinds.setdefault((t.first_lba - g.base) // SECTORS_PER_BLOCK, set()).add(t.kind)
    return kinds


def RecordCases(bt):
    f1, f2, f3 = bt.fid, bt.fid + 1, bt.fid + 2

    def Foreign(kind, ignorable):
        def Plant(o):
            d = bt.Fresh(o, 0, 1, 5)
            bt.Pen(o).Sew([Born(bt.F, f1, d, b"kind%u" % kind), stitch.Unknown(kind, ignorable, b"\x07\x07")],
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

    def Shared(both):
        """f1 born on a block and f2 born SHARED on it, then f1 quitclaimed —
        and, `both`, f2 after it: the block is f2's while f2 lives, and lies
        fallow once when its last owner goes (§8.2)."""
        def Plant(o):
            d = bt.Fresh(o, 0, 1, 4)
            sharer = stitch.Born(fid=f2, flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1, modified_ns=1,
                                 name=b"sharer", tags=(), digests=[],
                                 runs=[stitch.Data(bt.F, 1, bookread.EntryPut(tally.EXT_SHARED, b""))])
            gone = [stitch.Quitclaim(fid=f1, base=0)] + ([stitch.Quitclaim(fid=f2, base=0)] if both else [])
            bt.Pen(o).Sew([Born(bt.F, f1, d, b"owner"), sharer] + gone, flags=stitch.DATA_FLUSHED)
        return Plant

    def Fallow(c):
        return [x.runs for x in c.claims if x.kind == "fallow"]

    def Unlanded(o, torn):
        """A Born whose block holds what its digest does not name, in a stitch
        sewn without DATA_FLUSHED — and, `torn`, a stitch after it torn."""
        bt.Fresh(o, 0, 1, 9)
        pen = bt.Pen(o)
        pen.Sew([Born(bt.F, f2, [0x1234], b"unlanded")], flags=0)
        if torn:
            s = pen.Sew([stitch.UseContext(b"next")], flags=0)
            bt.Zero(o, s.vlbas[4], s.of - 4)

    return [
        Case("B10 a record of kind 31 not to be stepped over", Foreign(31, 0), must={"UNDER_GLASS"}, checks=[
            ("the volume is read only and the stitch is not applied",
             lambda c: Glass(c, "kind 31") and f1 not in c.mount.files),
            ("its file id is never given again", lambda c: c.mount.counts["next_fid"] == f1 + 1)]),
        Case("B11 a record of kind 31 to step over", Foreign(31, 1), checks=[
            ("the stitch is applied: %u is born" % f1, lambda c: f1 in c.mount.files and not c.mount.glass),
            ("and nothing is said", lambda c: not c.findings)]),
        Case("B11c a record of kind 0 to step over: no kind is 0, a tag of 0 being PAD (§2.3)", Foreign(0, 1),
             checks=[("the stitch is applied: %u is born" % f1, lambda c: f1 in c.mount.files and not c.mount.glass),
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
             must={"NOT_LANDED"}, checks=[
                 ("%u is rolled back by its splice, a verdict the session's first stitch lays down"
                  % f2, lambda c: f2 not in c.mount.files and Errata(c, f2, 1)),
                 ("its id is never given again, and no lie is said", lambda c: c.mount.counts["next_fid"] ==
                  f2 + 1 and not c.mount.ribbon.lied)]),
        Case("B26b the newest stitch's data never landed, a torn stitch after it", lambda o: Unlanded(o, True),
             must={"NOT_LANDED", "RIBBON_TORN"}, checks=[
                 ("the medium did not keep what it confirmed: MEDIUM_LIED, said",
                  lambda c: c.mount.ribbon.lied and Found(c, "NOT_LANDED", "MEDIUM_LIED")),
                 ("and %u is rolled back by the verdict" % f2, lambda c: Errata(c, f2, 1))]),
        Case("B33 a Born of a file that exists", One([again]), must={"UNDER_GLASS"}, checks=[
                 ("the stitch is not applied: it gives birth to %s, which exists" % Label(bt.S),
                  lambda c: Glass(c, "gives birth to fid %u" % bt.S.fid) and
                  c.mount.files[bt.S.fid].name == bt.S.name)]),
        Case("B27 a record that does not match its base",
             One([stitch.Labeled(fid=bt.S.fid, base=1, name=b"stray")]),
             must={"BASE_MISMATCH"}, checks=[
                 ("%s stays as the tally has it" % Label(bt.S),
                  lambda c: c.mount.files[bt.S.fid].name == bt.S.name),
                 ("its roll-back is a verdict, why 2 — the volume written on",
                  lambda c: Errata(c, bt.S.fid, 2) and not c.mount.glass)]),
        Case("B28 two files born and the second quitclaimed in one stitch", Quitclaimed,
             checks=[("%u lives and %u is gone, its block lying fallow (M-1)" % (f1, f2),
                      lambda c: f1 in c.mount.files and f2 not in c.mount.files and
                      [x.runs for x in c.claims if x.kind == "fallow"] == [[(bt.F + 1, 1)]]),
                     ("and nothing is said", lambda c: not c.findings),
                     ("%u is never given again: the next file id is %u" % (f2, f3),
                      lambda c: c.mount.counts["next_fid"] == f3)]),
        Case("B28s a block shared, its DATA owner quitclaimed", Shared(False), checks=[
            ("%u is gone and %u lives, the block its own: it lies fallow for no one" % (f1, f2),
             lambda c: f1 not in c.mount.files and f2 in c.mount.files and not Fallow(c) and
             any(x.kind == "shared" and x.runs == [(bt.F, 1)] for x in c.claims)),
            ("%u reads the block's bytes, held to its one digest, and nothing is said" % f2,
             lambda c: c.Content(f2) == bt.Pattern(4) and not c.findings),
            ("and the kernel of this tree mounts it, not under glass",
             lambda c: c.kernel["mounts"] is True and c.kernel["glass"] == 0 and c.kernel["fallow"] == 0)]),
        Case("B28t a block shared, both its owners quitclaimed", Shared(True), checks=[
            ("the block lies fallow once (M-1)", lambda c: not c.mount.files.get(f2) and Fallow(c) == [[(bt.F, 1)]]),
            ("and nothing is said", lambda c: not c.findings),
            ("and the kernel of this tree mounts it, the block fallow once",
             lambda c: c.kernel["mounts"] is True and c.kernel["glass"] == 0 and c.kernel["fallow"] == 1)]),
    ]


def MachineRecordCases(bt):
    """The landings as one machine's record writes them, every write named by
    blklog_book.py: a stitch and its data, and data no stitch of the record
    names — on ground free as of its epoch, where a book's census counts it
    (§3.4 L-8), and no one's where that census cannot say it is free: a
    bookmark keeps the count, a growth's ground before the head Deed names
    it, a book that does not mount."""
    from .blklog import LOG_FLUSH
    f1, g, F, size = bt.fid, bt.ground, bt.F, bt.base.SizeSectors()
    leaf, held = bt.Node(tally.FILES), bt.S.Ground()[0][0]

    def Again(block, blocks=1):
        return [(g.base + block * SECTORS_PER_BLOCK, blocks * SECTORS_PER_BLOCK)]

    def Recorded(o):
        d = bt.Fresh(o, 0, 2, 5)
        bt.Pen(o).Sew([Born(F, f1, d, b"recorded")], flags=stitch.DATA_FLUSHED)

    def Named(c):
        k = RecordKinds(g, bt.base, LandingRecord(c, bt.base, also=Again(leaf.blocks[0], leaf.sheets) +
                                                  Again(tally.SlotBlock(g, 0))), size)
        return k.get(F) == k.get(F + 1) == {"file"} and not any(v & {"unowned", "free"} for v in k.values()) \
            and {"stitch", "sheet", "slot"} <= set().union(*k.values())

    def Unnamed(o):
        """F written and let go before any stitch, F + 1 written and named by
        the stitch, F + 2 written with no stitch after it."""
        bt.Fresh(o, 0, 1, 21)
        d = bt.Fresh(o, 1, 1, 22)
        bt.Pen(o).Sew([Born(F + 1, f1, d, b"unnamed")], flags=stitch.DATA_FLUSHED)
        bt.Fresh(o, 2, 1, 23)

    def UnnamedKinds(c):
        k = RecordKinds(g, bt.base, LandingRecord(c, bt.base, {F + 2}, Again(held)), size)
        return k.get(F) == k.get(F + 2) == {"free"} and k.get(F + 1) == k.get(held) == {"file"} and \
            not any("unowned" in v for v in k.values()) and any("stitch" in v for v in k.values())

    def Alone(plant):
        """`plant`, and F written after it, named by no stitch."""
        return lambda o: (plant(o), bt.Fresh(o, 0, 1, 24))

    def NoOnes(c):
        k = RecordKinds(g, bt.base, LandingRecord(c, bt.base, {F}), size)
        return k.get(F) == {"unowned"}

    deed = g.deed
    grown = dict(deed, sectors=deed["sectors"] + GROWTH, tail_sector=deed["tail_sector"] + GROWTH)
    far = FrameDeed(grown, deed["frame"], tuple(deed["boot"][k] for k in ondisk.VOLUME_BOOT.names),
                    ondisk.DEED_ROLE_TAIL)
    past = g.data_end + 7

    def Grown(c):
        k = RecordKinds(g, bt.base, [(g.base + grown["tail_sector"], 0, far), (0, LOG_FLUSH, b""),
                                     (g.base + past * SECTORS_PER_BLOCK, 0, b"\x5a" * BLOCK)], None)
        return k.get(grown["tail_sector"] // SECTORS_PER_BLOCK) == {"deed"} and k.get(past) == {"unowned"}

    return [
        Case("B56 a stitch and its data as a machine's record writes them", Recorded, checks=[
            ("every write of the record is named — the data at F and F + 1 a file's, block by block, by the "
             "stitch after it; the stitch, a Tally node and a slot by their sectors' heads — none unowned, none "
             "on free ground", Named)]),
        Case("B58 data no stitch of the record names", Unnamed, checks=[
            ("the data written over before a stitch named it, and the data no stitch follows, are on ground "
             "free as of their epoch; the data the stitch names the file's, and %s's block written again as "
             "it lies the file's of the census — none unowned" % Label(bt.S), UnnamedKinds)]),
        Case("B58b data no stitch names, on a book a bookmark holds", Alone(lambda o: PlantBookmark(bt, o)),
             must={"UNDER_GLASS"}, checks=[
                 ("F is no one's: where a Tally names a bookmark, whose ground neither census derives, what is "
                  "free is not counted (B13)", NoOnes)]),
        Case("B58c data no stitch names, on ground a growth took before the head Deed names it", lambda o: None,
             checks=[("the far copy of a growth by %u blocks, a flush, and data at block %u, past the end the "
                      "head Deed names: the far copy the growth's, the data no one's — the census of its epoch "
                      "ends at block %u" % (GROWTH // SECTORS_PER_BLOCK, past, g.data_end), Grown)]),
        Case("B58d data no stitch names, on a book that does not mount",
             Alone(lambda o: bt.Zero(o, tally.SlotBlock(g, 0) * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK)),
             must={"SPINE_NONE"}, checks=[
                 ("F is no one's: a book none of whose slots holds names no owner, and no ground free (B35)",
                  NoOnes)]),
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

    def Hole(o):
        """C17: stitch 3 sewn, 4 and 5 never landed though their flushes were
        answered, 6 and 7 laid over the hole — 6 naming 5."""
        d = bt.Fresh(o, 0, 5, 8)
        pen = bt.Pen(o)
        for k in range(5):
            pen.Sew([Born(bt.F + k, bt.fid + k, d[k:k + 1], b"h%u" % k)], flags=stitch.DATA_FLUSHED,
                    write=k not in (1, 2))

    def SlotBetween(o):
        """C17: stitch 3 sewn, then a Tally at seq 4 standing on it bound in
        slot 1 at seq 5 — both seqs the ribbon's own counter gave — and 6 and
        7 never landed, 8 laid over the hole naming 7."""
        g, d = bt.ground, bt.Fresh(o, 0, 4, 8)
        pen = bt.Pen(o)
        s3 = pen.Sew([Born(bt.F, f1, d[:1], b"t0")], flags=stitch.DATA_FLUSHED)
        slot = tally.SlotRead(g, 0)
        slot.tally_seq, slot.stitch_seq, slot.stitch_clasp = 4, s3.seq, s3.clasp
        slot.ribbon = dict(slot.ribbon, at=s3.vlbas[-1] // SECTORS_PER_BLOCK + 1)
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, 5, slot)))
        pen.tally_seq = 4
        for k, seq in enumerate((6, 7, 8)):
            pen.Sew([Born(bt.F + 1 + k, bt.fid + 1 + k, d[1 + k:2 + k], b"t%u" % (k + 1))],
                    flags=stitch.DATA_FLUSHED, seq=seq, write=seq == 8)

    def Island(o):
        """C17: stitch 3 sewn; 4 never landed; 5 landed whole, naming it; 6
        torn to half its sectors; 7 landed whole, naming 6 — one branch."""
        d = bt.Fresh(o, 0, 5, 8)
        pen = bt.Pen(o)
        out = [pen.Sew([Born(bt.F + k, bt.fid + k, d[k:k + 1], b"i%u" % k)], flags=stitch.DATA_FLUSHED,
                       write=k != 1) for k in range(5)]
        bt.Zero(o, out[3].vlbas[4], out[3].of - 4)

    def Comb(o):
        """C17: stitch 3 sewn; 4 never landed; past it 5 to 41, every odd one
        whole, naming the one before, every even one torn to half its
        sectors — one branch, nineteen lost, each alone."""
        d = bt.Fresh(o, 0, 39, 8)
        pen = bt.Pen(o)
        out = [pen.Sew([Born(bt.F + k, bt.fid + k, d[k:k + 1], b"c%u" % k)], flags=stitch.DATA_FLUSHED,
                       write=k != 1) for k in range(39)]
        for s in out[3::2]:
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
            ("the line counts the two that settled and do not read whole, 4 and 5, at the most — a seq its "
             "counter gave may have settled as no stitch — and the largest seq found",
             lambda c: Found(c, "DEAD_BRANCH", "what settled is lost: at most 2 stitch(es), seq 4..5",
                             "no seq up to 6, the largest found, is given again")),
            ("no number seen past the end is given again",
             lambda c: c.mount.counts["next_seq"] == 7 and c.mount.counts["next_fid"] == bt.fid + 4)]),
        Case("B61 the lie proven over a hole of two (C17)", Hole, must={"DEAD_BRANCH"}, checks=[
            ("the ribbon ends on stitch 3; 6 and 7 are not applied",
             lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3] and bt.fid + 3 not in c.mount.files),
            ("the lie is proven, and what settled is lost is counted: at most 2 stitch(es), seq 4 and 5",
             lambda c: Found(c, "DEAD_BRANCH", "the lie is proven",
                             "what settled is lost: at most 2 stitch(es), seq 4..5") and c.mount.ribbon.lied),
            ("the line says the next seq: none up to 7, the largest found, is given again — 8 is the next",
             lambda c: Found(c, "DEAD_BRANCH", "no seq up to 7, the largest found, is given again") and
             c.mount.counts["next_seq"] == 8)]),
        Case("B62 a Tally and its slot bound between the end and what settled is lost (C17)", SlotBetween,
             must={"DEAD_BRANCH"}, checks=[
                 ("the mount stands on slot 1, whose Tally stood on stitch 3; 8 is not applied",
                  lambda c: c.mount.slot.index == 1 and bt.fid + 3 not in c.mount.files),
                 ("seq 4 and 5, the Tally's and the slot's the Spine names, are not counted: at most 2 stitch(es), "
                  "seq 6 and 7",
                  lambda c: Found(c, "DEAD_BRANCH", "the lie is proven",
                                  "what settled is lost: at most 2 stitch(es), seq 6..7;") and c.mount.ribbon.lied),
                 ("none up to 8, the largest found, is given again",
                  lambda c: Found(c, "DEAD_BRANCH", "no seq up to 8, the largest found") and
                  c.mount.counts["next_seq"] == 9)]),
        Case("B63 a stitch past the end found whole among what settled is lost (C17)", Island,
             must={"DEAD_BRANCH"}, checks=[
                 ("the ribbon ends on stitch 3; 5 and 7, whole, are not applied",
                  lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3] and bt.fid + 2 not in c.mount.files),
                 ("5, found whole, is not counted lost: at most 2 stitch(es), seq 4 and 6",
                  lambda c: Found(c, "DEAD_BRANCH", "what settled is lost: at most 2 stitch(es), seq 4, 6;")),
                 ("none up to 7, the largest found, is given again",
                  lambda c: Found(c, "DEAD_BRANCH", "no seq up to 7, the largest found") and
                  c.mount.counts["next_seq"] == 8)]),
        Case("B64 what settled is lost in more runs than the line has room for (C17)", Comb,
             must={"DEAD_BRANCH"}, checks=[
                 ("the ribbon ends on stitch 3", lambda c: [s.seq for s in c.mount.ribbon.stitches] == [3]),
                 ("all nineteen counted, the runs past the line's room said as \", ...\"",
                  lambda c: Found(c, "DEAD_BRANCH", "what settled is lost: at most 19 stitch(es), seq 4, 6, 8, 10, "
                                  "12, 14, 16, 18, 20, 22, 24, 26, ...;")),
                 ("none up to 41, the largest found, is given again",
                  lambda c: Found(c, "DEAD_BRANCH", "no seq up to 41, the largest found") and
                  c.mount.counts["next_seq"] == 42)]),
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


LOG_LINES, LOG_LINE = 100, 40


def LogLine(i):
    """The logger's line i: 40 bytes."""
    return (b"line %03u of the logger " % i).ljust(LOG_LINE, b".")


def LogSlips(c, fid):
    """The Shaped records of `fid` the ribbon carries, in order."""
    shaped = [stitch.RecordDecode(kind, body, s.seq) for s in c.mount.ribbon.stitches
              for kind, _ign, body, _at in s.records if kind == stitch.SHAPED]
    return [d for d in shaped if d["fid"] == fid]


def PromisedLess(c, promised):
    """The free blocks the census counts, less `promised` — a block for each
    file a Slip lies over (§16.6 п.1); None where it counts none."""
    free = c.Json()["census"]["free"]
    return None if free is None else free - promised


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

    def Logger(o):
        """A logger's lines of 40 bytes, a stitch each, on a file born empty:
        the first puts in its hole, and each lays its line alone — the change
        since the stitch before, by the kernel's own rule (stitch.SlipDelta,
        §16.5 п.13), not the tail the lines have grown."""
        pen = bt.Pen(o)
        pen.Sew([stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=0, created_ns=1, modified_ns=1,
                             name=b"logger", tags=(), runs=[], digests=[])], flags=stitch.DATA_FLUSHED)
        tail, size = {}, 0
        for i in range(LOG_LINES):
            now = dict(tail)
            now.update(zip(range(size, size + LOG_LINE), LogLine(i)))
            slip = stitch.SlipDelta(tail, now, size + LOG_LINE)
            splices = [] if size else [(0, 0, [stitch.Hole(1)])]
            sewn.setdefault("log", []).append(pen.Sew([stitch.Shaped(
                fid=f1, base=1, size=size + LOG_LINE, mtime_ns=2 + i, splices=splices, digests=[], slip=slip)],
                flags=stitch.DATA_FLUSHED))
            tail, size = now, size + LOG_LINE

    def CutGrown(o):
        """100 bytes born a hole and a Slip, cut to 50 by a Shaped of no
        splice, grown to 80 by another: the overlay is cut by the size, and
        what grew reads as the hole's zeros (§16.5 п.13)."""
        pen = bt.Pen(o)
        pen.Sew([stitch.Born(fid=f1, flags=ondisk.FILE_ACTIVE, size=100, created_ns=1, modified_ns=1,
                             name=b"cut", tags=(), runs=[stitch.Hole(1)], digests=[],
                             slip=(0, b"a" * 100))], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=f1, base=1, size=50, mtime_ns=2, splices=[], digests=[])],
                flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=f1, base=1, size=80, mtime_ns=3, splices=[], digests=[])],
                flags=stitch.DATA_FLUSHED)

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
        Case("B22 a Slip", Slip, checks=[
            ("%u reads its tail out of the ribbon: 100 bytes then 50" % f1,
             lambda c: c.Content(f1) == b"a" * 100 + b"b" * 50),
            ("and the volume goes on: a Slip is folded by this kernel (§16.5 п.13)",
             lambda c: not c.mount.glass and not c.findings),
            ("the kernel tells one block fewer free than the census counts: the one promised to the tail "
             "its Slips lie over (§16.6 п.1)", lambda c: c.kernel["free"] == PromisedLess(c, 1))]),
        Case("B22c a logger's Slips, each its stitch's change", Logger, checks=[
            ("%u reads every line out of the ribbon" % f1,
             lambda c: c.Content(f1) == b"".join(LogLine(i) for i in range(LOG_LINES))),
            ("each stitch one atom, its Slip the line alone",
             lambda c: all(len(s.vlbas) <= SECTORS_PER_BLOCK for s in sewn["log"]) and
             all(r["slip"] == (i * LOG_LINE, LogLine(i)) for i, r in enumerate(LogSlips(c, f1)))),
            ("and nothing is said", lambda c: not c.mount.glass and not c.findings)]),
        Case("B22d a Slip cut by the size, then grown inside its block", CutGrown, checks=[
            ("%u reads 50 bytes and 30 zeros, not the bytes the cut took" % f1,
             lambda c: c.Content(f1) == b"a" * 50 + bytes(30)),
            ("and nothing is said", lambda c: not c.mount.glass and not c.findings),
            ("the 50 bytes the cut left a Slip of hold their block promised (§16.6 п.1)",
             lambda c: c.kernel["free"] == PromisedLess(c, 1))]),
        Case("B32 a Slip let go by a splice of its block", Folded, checks=[
            ("%u reads its new block, the overlay gone with the hole it lay on" % f1,
             lambda c: c.Content(f1) == bt.Pattern(10)[:150]),
            ("and nothing is said: no Slip is left to fold", lambda c: not c.findings),
            ("and no block is promised: the kernel tells the free the census counts",
             lambda c: c.kernel["free"] == PromisedLess(c, 0))]),
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
        Case("B22b a Slip past the file's size", Sewn(past), must={"BASE_MISMATCH"}, checks=[
            ("the file does not match its Slip and is not born", lambda c: Found(c, "BASE_MISMATCH", "Slip") and
             f1 not in c.mount.files and Errata(c, f1, 2))]),
        Case("B26c on a volume that lied, the stitch before the newest did not land", Lied,
             must={"NOT_LANDED"}, checks=[
                 ("both newest stitches are held to their data, DATA_FLUSHED or not (M-2): %u is rolled back"
                  % f1, lambda c: Found(c, "NOT_LANDED", "fid %u" % f1, "stitch 3") and
                  f1 not in c.mount.files and f2 in c.mount.files and Errata(c, f1, 1))]),
        Case("B27b a Shaped whose runs do not cover its size", Sewn(lambda q, d: [
            stitch.Shaped(fid=S.fid, base=q - S.gen, size=S.size + 10 * BLOCK, mtime_ns=1, splices=[],
                          digests=[])]), must={"BASE_MISMATCH"}, checks=[
            ("%s does not match: its runs do not cover its size" % Label(S),
             lambda c: Found(c, "BASE_MISMATCH", "fid %u" % S.fid, "runs cover") and Errata(c, S.fid, 2))]),
        Case("B27c a Labeled that puts on a tag already worn", Sewn(lambda q, d: [
            stitch.Labeled(fid=worn.fid, base=q - worn.gen, add=[worn.tags[0]])]),
             must={"BASE_MISMATCH"}, checks=[
                 ("%s does not match: the tag is worn" % Label(worn),
                  lambda c: Found(c, "BASE_MISMATCH", "fid %u" % worn.fid, "already worn") and
                  Errata(c, worn.fid, 2))]),
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
        stitch.Unit = stitch_ribbon.Unit = Counted
        try:
            stitch_ribbon.Ribbon(r.g, r.slot, r.taken)
        finally:
            stitch.Unit = stitch_ribbon.Unit = real
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
