"""selftest_book_ribbon_damage — the book's landings on a ribbon no writer
leaves: what a torn or rotten stitch costs (§3.9), a unit the format does not
allow (§1.5, §2.1, §2.2), and what the medium may say that no writer does, held
to a bound. Each a fork of the base selftest_book.py lays; what the book's
census must say of it. The landings of a ribbon as writers leave it are
selftest_book_ribbon.py's."""

from . import bookread, ondisk, stitch, tally
from .bookground import BookGround
from .medium import Unreadable
from .ondisk import BLOCK, SECTORS_PER_BLOCK, RunsNormal
from .selftest_book import Born, Found, Glass, Label
from .selftest_cases import Case


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
