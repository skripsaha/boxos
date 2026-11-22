"""selftest_book_head — the landings of a file's first bytes (volume_tally.h, the FILES header's head;
volume_ribbon.h, Born and Labeled HEAD). What a file is is told by its first bytes — up to HEAD_MOST,
all of a shorter file — which its card keeps exact, the book knowing no format: laid by a Born, laid anew
by a Labeled HEAD after a change of what the file holds, none taking the bit off. A header or a Born
that carries the bit and no byte, or more bytes than the file or HEAD_MOST, does not read, nor does a
Labeled that names the bit in its FLAGS; and a card whose bytes are not the file's is damage — every
question of what the file is answers wrong (HEAD_MISMATCH). Each a fork of the base selftest_book.py
lays."""

from . import bookread, ondisk, stitch, tally
from .ondisk import BLOCK
from .selftest_book import Case, Found

HEADED = ondisk.FILE_ACTIVE | ondisk.FILE_HEAD


def HeadCases(bt):
    S = bt.S
    fid = bt.fid

    def Headed(o):
        d = bt.Fresh(o, 0, 1, 7)
        bt.Pen(o).Sew([bt.Born(bt.F, fid, d, b"headed")], flags=stitch.DATA_FLUSHED)

    def Rewritten(o):
        d = bt.Fresh(o, 0, 2, 3)
        pen = bt.Pen(o)
        pen.Sew([bt.Born(bt.F, fid, d[:1], b"rewritten")], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=fid, base=1, size=BLOCK, mtime_ns=2, splices=[(0, 1, [stitch.Data(bt.F + 1, 1)])],
                               digests=d[1:]),
                 stitch.Labeled(fid=fid, base=0, head=bt.Block(bt.F + 1)[:ondisk.HEAD_MOST])],
                flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=fid, base=1, size=0, mtime_ns=3, splices=[(0, 1, [])], digests=[]),
                 stitch.Labeled(fid=fid, base=0, head=b"")], flags=stitch.DATA_FLUSHED)

    def Stale(o):
        d = bt.Fresh(o, 0, 2, 4)
        pen = bt.Pen(o)
        pen.Sew([bt.Born(bt.F, fid, d[:1], b"stale")], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=fid, base=1, size=BLOCK, mtime_ns=2, splices=[(0, 1, [stitch.Data(bt.F + 1, 1)])],
                               digests=d[1:])], flags=stitch.DATA_FLUSHED)

    def Headless(o):
        d = bt.Fresh(o, 0, 1, 5)
        bt.Pen(o).Sew([stitch.Born(fid=fid, flags=ondisk.FILE_ACTIVE, size=BLOCK, created_ns=1, modified_ns=1,
                                   name=b"headless", tags=(), runs=[stitch.Data(bt.F, 1)], digests=d)],
                      flags=stitch.DATA_FLUSHED)

    def BitInFlags(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, flags=(0, ondisk.FILE_HEAD))],
                flags=stitch.DATA_FLUSHED)

    def Card(c, f):
        fs = c.mount.files.get(f)
        return fs.head if fs is not None and bool(fs.flags & ondisk.FILE_HEAD) == bool(fs.head) else None

    def Refused(read, bad):
        try:
            read(bad)
        except bookread.Short:
            return True
        return False

    def HeadRoundTrip(c):
        """A header's first bytes close it, as many as the file holds up to HEAD_MOST; one of no bytes is the
        bytes it always was; the bit with none, more than the file holds or more than HEAD_MOST does not read."""
        V = bookread.VarintPut
        read = lambda body: tally.FileHeadDecode(7, body, [])
        full = tally.FileState(7, 3, HEADED, 9000, 1, 2, b"file", [4], [], head=b"0123456789abcdef")
        short = tally.FileState(7, 3, HEADED, 5, 1, 2, b"file", [4], [], head=b"hello")
        empty = tally.FileState(7, 3, ondisk.FILE_ACTIVE, 0, 1, 2, b"file", [4], [])

        def Laid(fs):
            return (V(3) + V(fs.flags) + V(fs.size) + V(1) + V(bookread.Zigzag(1)) + V(4) + b"file" +
                    bookread.IdsPut([4]))
        past = tally.FileState(7, 3, HEADED, 4, 1, 2, b"file", [4], [], head=b"hello")
        wide = tally.FileState(7, 3, HEADED, 9000, 1, 2, b"file", [4], [], head=b"x" * (ondisk.HEAD_MOST + 1))
        bare = tally.FileState(7, 3, HEADED, 9000, 1, 2, b"file", [4], [], head=b"")
        return (tally.FileHeadEncode(full) == Laid(full) + V(16) + full.head and
                read(tally.FileHeadEncode(full)).head == full.head and
                read(tally.FileHeadEncode(short)).head == b"hello" and
                tally.FileHeadEncode(empty) == Laid(empty) and read(tally.FileHeadEncode(empty)).head == b"" and
                Refused(read, tally.FileHeadEncode(past)) and Refused(read, tally.FileHeadEncode(wide)) and
                Refused(read, tally.FileHeadEncode(bare)) and Refused(read, tally.FileHeadEncode(full)[:-1]))

    def RecordsRoundTrip(c):
        """A Born carries its first bytes last, a Labeled HEAD its new ones — none reads, a list past
        HEAD_MOST does not — and both are written again byte for byte."""
        born = stitch.Born(fid=fid, flags=HEADED, size=20, created_ns=1, modified_ns=1, name=b"b", tags=(),
                           runs=[stitch.Hole(1)], digests=[], head=b"abcdefghijklmnop")
        d = stitch.RecordDecode(stitch.BORN, born.body, 1)
        labels = [stitch.Labeled(fid=fid, base=2, head=b"xyz"), stitch.Labeled(fid=fid, base=2, head=b"")]
        circle = all(stitch.RecordAgain(stitch.LABELED, stitch.RecordDecode(stitch.LABELED, r.body, 9)).body == r.body
                     for r in labels)
        wide = stitch.Labeled(fid=fid, base=2, head=b"y" * (ondisk.HEAD_MOST + 1))
        return (d["head"] == b"abcdefghijklmnop" and stitch.RecordAgain(stitch.BORN, d).body == born.body and
                circle and stitch.RecordDecode(stitch.LABELED, labels[1].body, 9)["head"] == b"" and
                Refused(lambda body: stitch.RecordDecode(stitch.LABELED, body, 9), wide.body))

    return [
        Case("HD1 a Born carries the first bytes of the file it gives birth to", Headed, checks=[
            ("the card holds them, its bit on", lambda c: Card(c, fid) == bt.Pattern(7)[:ondisk.HEAD_MOST]),
            ("and nothing is said", lambda c: not c.findings)]),
        Case("HD2 a Labeled HEAD lays them anew after the change, and a file cut to nothing holds none",
             Rewritten, checks=[
                 ("the card holds none, its bit off, the file empty",
                  lambda c: Card(c, fid) == b"" and c.mount.files[fid].size == 0),
                 ("and nothing is said", lambda c: not c.findings)]),
        Case("HD3 a change of what a file holds with no Labeled HEAD leaves a card that lies", Stale,
             must={"HEAD_MISMATCH"}, checks=[
                 ("its old bytes are said against the new", lambda c: Found(c, "HEAD_MISMATCH", "stale"))]),
        Case("HD4 a Born of a file that holds bytes and carries none lies too", Headless, must={"HEAD_MISMATCH"},
             checks=[("it is said to begin with nothing", lambda c: Found(c, "HEAD_MISMATCH", "headless", "nothing"))]),
        Case("HD5 the head bit in a Labeled's FLAGS does not read", BitInFlags, checks=[
            ("the file's card is as it was", lambda c: Card(c, S.fid) == S.head)]),
        Case("HD6 a FILES header carries the first bytes last", lambda o: None, checks=[
            ("read back, none for an empty file, refused past the file, HEAD_MOST or with none", HeadRoundTrip)]),
        Case("HD7 a Born and a Labeled HEAD carry them, written again byte for byte", lambda o: None, checks=[
            ("a Born's last field, a Labeled's new bytes and none", RecordsRoundTrip)]),
    ]
