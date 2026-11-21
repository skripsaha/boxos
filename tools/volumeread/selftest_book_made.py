"""selftest_book_made — the landings of a file made by a program (volume_ribbon.h, Born;
volume_tally.h, the FILES header's made_by). Who made a file is the record's own — the
bit TAGFS_FILE_MADE_BY and the id of the program's file — laid once by the Born and
carried by every change after; a header or a Born that carries the bit and names no
program does not read, nor does a Labeled that names the bit in its FLAGS. A file no
program made carries neither the bit nor the field, its header the bytes it was before
the field was. Each a fork of the base selftest_book.py lays."""

from . import bookread, ondisk, stitch, tally
from .selftest_book import Case

PROGRAM = 4242
TERM_NS = 1700000000123456789
MADE = ondisk.FILE_ACTIVE | ondisk.FILE_MADE_BY


def MadeCases(bt):
    S = bt.S
    fid = bt.fid

    def BornMade(name=b"made", made_by=PROGRAM):
        return stitch.Born(fid=fid, flags=MADE, size=0, created_ns=1, modified_ns=1, name=name, tags=(),
                           runs=[], digests=[], made_by=made_by)

    def Made(o):
        bt.Pen(o).Sew([BornMade()], flags=stitch.DATA_FLUSHED)

    def MadeThenChanged(o):
        pen = bt.Pen(o)
        pen.Sew([BornMade()], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, name=b"renamed")], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, keep=TERM_NS)], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, keep=0)], flags=stitch.DATA_FLUSHED)

    def BitInFlags(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, flags=(ondisk.FILE_MADE_BY, 0))],
                flags=stitch.DATA_FLUSHED)

    def MadeBy(c, name, made_by):
        fs = c.mount.files[fid]
        return fs.name == name and fs.made_by == made_by and bool(fs.flags & ondisk.FILE_MADE_BY)

    def Refused(read, bad):
        try:
            read(bad)
        except bookread.Short:
            return True
        return False

    def HeadRoundTrip(c):
        """A header's made_by follows its keep_ns, and is read back whatever else it carries; a
        header of a file no program made is the bytes it always was, and one that names the bit and
        no program, or is cut short, does not read."""
        plain = tally.FileState(7, 3, ondisk.FILE_ACTIVE, 9, 1, 2, b"file", [4, 6], [])
        made = tally.FileState(7, 3, MADE, 9, 1, 2, b"file", [4, 6], [], made_by=PROGRAM)
        both = tally.FileState(7, 3, MADE | ondisk.FILE_DELETED, 9, 1, 2, b"file", [4, 6], [], keep_ns=TERM_NS,
                               made_by=PROGRAM)
        V = bookread.VarintPut

        def Laid(flags):
            return (V(3) + V(flags) + V(9) + V(1) + V(bookread.Zigzag(1)) + V(4) + b"file" +
                    bookread.IdsPut([4, 6]))
        read = lambda body: tally.FileHeadDecode(7, body, [])
        body_both = tally.FileHeadEncode(both)
        unnamed = tally.FileState(7, 3, MADE, 9, 1, 2, b"file", [4, 6], [], made_by=0)
        return (tally.FileHeadEncode(plain) == Laid(ondisk.FILE_ACTIVE) and
                tally.FileHeadEncode(made) == Laid(MADE) + V(PROGRAM) and
                body_both == Laid(MADE | ondisk.FILE_DELETED) + V(TERM_NS) + V(PROGRAM) and
                read(tally.FileHeadEncode(made)).made_by == PROGRAM and
                read(tally.FileHeadEncode(plain)).made_by == 0 and
                read(body_both).keep_ns == TERM_NS and read(body_both).made_by == PROGRAM and
                Refused(read, body_both[:-1]) and Refused(read, tally.FileHeadEncode(unnamed)))

    def BornRoundTrip(c):
        """A Born carries made_by after its slip, the bytes of one of a file no program made are as
        they were, and one that names the bit and no program does not read."""
        def Read(rec):
            return stitch.RecordDecode(stitch.BORN, rec.body, 1)
        made, plain = BornMade(), BornMade().body
        bare = stitch.Born(fid=fid, flags=ondisk.FILE_ACTIVE, size=0, created_ns=1, modified_ns=1, name=b"made",
                           tags=(), runs=[], digests=[])
        return (Read(made)["made_by"] == PROGRAM and Read(bare)["made_by"] == 0 and
                len(plain) == len(bare.body) + len(bookread.VarintPut(PROGRAM)) and
                Refused(Read, BornMade(made_by=0)) and Refused(Read, stitch.Record(stitch.BORN, plain[:-1])))

    return [
        Case("MB1 a Born that names the program that made the file puts it in the record", Made, checks=[
            ("the file is made by the program, its bit on", lambda c: MadeBy(c, b"made", PROGRAM)),
            ("the volume is written on", lambda c: not c.mount.glass)]),
        Case("MB2 a rename, a deletion and a restore leave who made the file", MadeThenChanged, checks=[
            ("the file is still made by the program", lambda c: MadeBy(c, b"renamed", PROGRAM)),
            ("and is not deleted", lambda c: not c.mount.files[fid].flags & ondisk.FILE_DELETED)]),
        Case("MB3 the made-by bit in a Labeled's FLAGS does not read", BitInFlags, checks=[
            ("the file is not made by anyone for it", lambda c: not c.mount.files[S.fid].flags & ondisk.FILE_MADE_BY
             and c.mount.files[S.fid].made_by == 0)]),
        Case("MB4 a FILES header carries made_by after the term, and a file no program made none",
             lambda o: None, checks=[("a header's program is read back and none is the old bytes", HeadRoundTrip)]),
        Case("MB5 a Born carries made_by after its slip", lambda o: None, checks=[
            ("a Born's program is read back, and a Born naming the bit and none does not read", BornRoundTrip)]),
    ]
