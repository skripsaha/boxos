"""selftest_book_shown — the landings of a program's file that shows its tags to other programs
(volume_ribbon.h, Born and Labeled SHOWN; volume_tally.h, the FILES header's shown). Whom a
program shows its own tags to is the card of its file — the bit TAGFS_FILE_SHOWN and the rising
list of the programs' files, 0 first for every program — laid by a Born, laid anew whole by a
Labeled SHOWN, and carried by every other change; a list of none takes the bit off. A header or
a Born that carries the bit and names no program does not read, nor does a Labeled that names the
bit in its FLAGS. A file that shows nothing carries neither the bit nor the list, its header the
bytes it was before the list was. Each a fork of the base selftest_book.py lays."""

from . import bookread, ondisk, stitch, tally
from .selftest_book import Case

PROGRAM = 4242
PLAYER, EDITOR = 77, 900
EVERY = 0
TERM_NS = 1700000000123456789
SHOWN = ondisk.FILE_ACTIVE | ondisk.FILE_SHOWN


def ShownCases(bt):
    S = bt.S
    fid = bt.fid

    def BornShown(shown=(PLAYER, EDITOR), flags=SHOWN, made_by=0):
        return stitch.Born(fid=fid, flags=flags, size=0, created_ns=1, modified_ns=1, name=b"shows", tags=(),
                           runs=[], digests=[], made_by=made_by, shown=shown)

    def Born(o):
        bt.Pen(o).Sew([BornShown()], flags=stitch.DATA_FLUSHED)

    def ShownAnew(o):
        pen = bt.Pen(o)
        pen.Sew([BornShown()], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, shown=[EVERY, PLAYER])], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, name=b"renamed")], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, keep=TERM_NS)], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, keep=0)], flags=stitch.DATA_FLUSHED)

    def ShownNone(o):
        pen = bt.Pen(o)
        pen.Sew([BornShown()], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, shown=[])], flags=stitch.DATA_FLUSHED)

    def ShownOnPlain(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, shown=[PLAYER])], flags=stitch.DATA_FLUSHED)

    def BitInFlags(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, flags=(ondisk.FILE_SHOWN, 0))],
                flags=stitch.DATA_FLUSHED)

    def Shows(c, f, name, shown):
        fs = c.mount.files[f]
        return fs.name == name and fs.shown == shown and bool(fs.flags & ondisk.FILE_SHOWN) == bool(shown)

    def Refused(read, bad):
        try:
            read(bad)
        except bookread.Short:
            return True
        return False

    def HeadRoundTrip(c):
        """A header's list follows its made_by, and is read back whatever else it carries; a header
        of a file that shows nothing is the bytes it always was, and one that names the bit and no
        program, or is cut short, does not read."""
        made = ondisk.FILE_MADE_BY | ondisk.FILE_DELETED
        plain = tally.FileState(7, 3, ondisk.FILE_ACTIVE, 9, 1, 2, b"file", [4, 6], [])
        shown = tally.FileState(7, 3, SHOWN, 9, 1, 2, b"file", [4, 6], [], shown=[EVERY, PLAYER, EDITOR])
        every = tally.FileState(7, 3, SHOWN | made, 9, 1, 2, b"file", [4, 6], [], keep_ns=TERM_NS,
                                made_by=PROGRAM, shown=[EVERY])
        V = bookread.VarintPut

        def Laid(flags):
            return (V(3) + V(flags) + V(9) + V(1) + V(bookread.Zigzag(1)) + V(4) + b"file" +
                    bookread.IdsPut([4, 6]))
        read = lambda body: tally.FileHeadDecode(7, body, [])
        body_every = tally.FileHeadEncode(every)
        unnamed = tally.FileState(7, 3, SHOWN, 9, 1, 2, b"file", [4, 6], [], shown=[])
        return (tally.FileHeadEncode(plain) == Laid(ondisk.FILE_ACTIVE) and
                tally.FileHeadEncode(shown) == Laid(SHOWN) + V(3) + V(EVERY) + V(PLAYER - 1) +
                V(EDITOR - PLAYER - 1) and
                body_every == Laid(SHOWN | made) + V(TERM_NS) + V(PROGRAM) + V(1) + V(EVERY) and
                read(tally.FileHeadEncode(shown)).shown == [EVERY, PLAYER, EDITOR] and
                read(tally.FileHeadEncode(plain)).shown == [] and
                read(body_every).made_by == PROGRAM and read(body_every).shown == [EVERY] and
                Refused(read, body_every[:-1]) and Refused(read, tally.FileHeadEncode(unnamed)))

    def BornRoundTrip(c):
        """A Born carries its list after made_by, the bytes of one that shows nothing are as they
        were, and one that names the bit and no program does not read."""
        def Read(rec):
            return stitch.RecordDecode(stitch.BORN, rec.body, 1)
        both = BornShown(flags=SHOWN | ondisk.FILE_MADE_BY, made_by=PROGRAM)
        bare = BornShown(shown=(), flags=ondisk.FILE_ACTIVE)
        return (Read(both)["shown"] == [PLAYER, EDITOR] and Read(both)["made_by"] == PROGRAM and
                Read(bare)["shown"] == [] and
                BornShown().body.endswith(bookread.IdsPut([PLAYER, EDITOR])) and
                both.body.endswith(bookread.VarintPut(PROGRAM) + bookread.IdsPut([PLAYER, EDITOR])) and
                Refused(Read, BornShown(shown=())) and
                Refused(Read, stitch.Record(stitch.BORN, BornShown().body[:-1])))

    def LabeledCircle(c):
        """A Labeled SHOWN, and one that shows none, written again from what it reads are the bytes
        they were — with a term beside it too."""
        def Circle(rec):
            return stitch.RecordAgain(stitch.LABELED, stitch.RecordDecode(stitch.LABELED, rec.body, 9)).body == rec.body
        return (Circle(stitch.Labeled(fid=fid, base=2, shown=[EVERY, EDITOR])) and
                Circle(stitch.Labeled(fid=fid, base=2, shown=[])) and
                Circle(stitch.Labeled(fid=fid, base=2, keep=TERM_NS, shown=[PLAYER])))

    return [
        Case("SH1 a Born that names the programs a program's file shows its tags to puts them in the record",
             Born, checks=[
                 ("the file shows its tags to both, its bit on", lambda c: Shows(c, fid, b"shows", [PLAYER, EDITOR])),
                 ("the volume is written on", lambda c: not c.mount.glass)]),
        Case("SH2 a Labeled SHOWN lays the list anew, and a rename, a deletion and a restore leave it",
             ShownAnew, checks=[
                 ("the file shows its tags to every program and the player",
                  lambda c: Shows(c, fid, b"renamed", [EVERY, PLAYER])),
                 ("and is not deleted", lambda c: not c.mount.files[fid].flags & ondisk.FILE_DELETED)]),
        Case("SH3 a Labeled SHOWN of none takes the bit off", ShownNone, checks=[
            ("the file shows nothing", lambda c: Shows(c, fid, b"shows", []))]),
        Case("SH4 a Labeled SHOWN on a file that showed nothing puts the bit on", ShownOnPlain, checks=[
            ("the file shows its tags to the player", lambda c: Shows(c, S.fid, c.mount.files[S.fid].name, [PLAYER]))]),
        Case("SH5 the shown bit in a Labeled's FLAGS does not read", BitInFlags, checks=[
            ("the file shows nothing for it", lambda c: not c.mount.files[S.fid].flags & ondisk.FILE_SHOWN
             and c.mount.files[S.fid].shown == [])]),
        Case("SH6 a FILES header carries its list after made_by, and a file that shows nothing none",
             lambda o: None, checks=[("a header's list is read back and none is the old bytes", HeadRoundTrip)]),
        Case("SH7 a Born carries its list after made_by", lambda o: None, checks=[
            ("a Born's list is read back, and a Born naming the bit and none does not read", BornRoundTrip)]),
        Case("SH8 a Labeled SHOWN is written again byte for byte", lambda o: None, checks=[
            ("the list, none, and a list beside a term", LabeledCircle)]),
    ]
